#!/usr/bin/env python3
"""Builds the dashboard's offline address file (.addr) from OpenStreetMap data.

Input: three files of GeoJSON features, one feature per line (a "GeoJSON sequence"), written by
osmium-tool in Docker (tools/address/build_address.sh does that step):

    areas    boundary=administrative outlines; osmium joins the ways of each boundary relation
             into one multipolygon (an outline made of several rings, holes included)
    roads    highway=* ways as lines
    places   place=* nodes as points

Output: a Montero address file (format and reference lookup in mtaddr.py), by default
dist/map/Map/luzon.addr, for the SD card's Map folder.

How the Philippines is tagged in OpenStreetMap (checked in the Geofabrik extract with data up to
2026-11-13; boundary=administrative relations):

    admin_level 2   country
    admin_level 3   region (Metro Manila is one of them)
    admin_level 4   province                                    -> province
    admin_level 5   the four districts of Metro Manila (Capital, Eastern, Northern and Southern
                    Manila District) and a few provincial legislative districts
    admin_level 6   city or municipality, Pateros included       -> city or municipality
    admin_level 7   legislative districts of some cities (Quezon City, Paranaque, ...)
    admin_level 8   the districts of Manila (Tondo, Sampaloc, Malate, ...) and some other named
                    parts of cities
    admin_level 9   the zones of Manila (Zone 1, ...)
    admin_level 10  barangay, the numbered ones of Manila too    -> barangay
    admin_level 11, 12  sitio, purok or subdivision (parts of a barangay)

Metro Manila has no province: the dashboard's "province" there is the outline of the region
itself (admin_level 3, named "Metro Manila"), because people write addresses there as
"Makati, Metro Manila". The province outlines around it (Rizal, Cavite, Bulacan) do not cover it.
Five independent cities in Luzon lie outside every province outline (Baguio, Angeles, Olongapo,
Lucena, Naga): each one's outline is added again as an area of the province people write with
it (see INDEPENDENT_CITY_PROVINCES), so "Baguio, Benguet" instead of no province.

Only relations count for provinces and cities. A closed way (a single way forming a ring) is
used only at admin_level 10: some barangays are mapped that way, while closed ways at other
levels are mostly tagging mistakes (in Laoag, numbered barangays are closed ways tagged
admin_level 6; taken as cities they would replace "Laoag City").

Names: the name tag, else name:en, as OpenStreetMap has them; control characters are removed
and runs of spaces become one space (a zero byte would end a name early in the file).

Python 3.9, standard library only.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
import unicodedata
from pathlib import Path
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mtaddr  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WORK = ROOT / "dist" / "map-build" / "address"
DEFAULT_OUTPUT = ROOT / "dist" / "map" / "Map" / "luzon.addr"

# Luzon, as for the map (minimum longitude, minimum latitude, maximum longitude, maximum latitude).
LUZON = (119.4, 12.4, 124.5, 18.8)

# Grid and radii chosen for Luzon with check_address.py (2026-11-13 data): 0.02 degree cells
# (about 2.2 km) and a 3 km place radius give a 29 MB file whose busiest cell (in Manila) holds
# about 1500 items; 0.01 degree cells would double the file for no better answers.
DEFAULT_CELL_DEG = 0.02
DEFAULT_ROAD_RADIUS_M = 75     # more than mtaddr.ROAD_MATCH_M (50 m): roads are listed with room to spare
DEFAULT_PLACE_RADIUS_M = 3000  # 97% of points on drivable roads have a named place this close
DEFAULT_AREA_TOLERANCE_M = 10.0  # outlines may move this far when simplified
DEFAULT_ROAD_TOLERANCE_M = 4.0
MAX_CELL_ITEMS = 4096          # the C reader's limit for one grid cell
MAX_RINGS = 0xFFFF             # the ring count is a 16-bit number in the item record

Bounds = Tuple[float, float, float, float]
Point = Tuple[float, float]

# admin_level -> kind. Metro Manila is handled in admin_kind().
ADMIN_LEVEL_KINDS = {"4": mtaddr.KIND_PROVINCE, "6": mtaddr.KIND_CITY, "10": mtaddr.KIND_BARANGAY}
# Regions (admin_level 3) that have no provinces and stand in for one.
PROVINCE_LEVEL_REGIONS = frozenset({"Metro Manila"})
# Independent cities (highly urbanized, or independent component cities) that OpenStreetMap
# leaves out of their province's outline (a hole, or a notch at the coast), with the province
# people still write after them ("Baguio City, Benguet"). Used only while the city's outline
# lies in no province. Checked for Luzon in the 2026-11-13 data; Olongapo shares more border
# with Bataan but belongs to Zambales, so this is a list, not a guess from the shapes.
INDEPENDENT_CITY_PROVINCES = {"Baguio": "Benguet", "Angeles": "Pampanga", "Olongapo": "Zambales",
                              "Lucena": "Quezon", "Naga City": "Camarines Sur"}

# highway=* -> road rank (mtaddr: 1 motorway or trunk, 2 primary, 3 secondary, 4 tertiary,
# 5 other). Paths, footways, steps, cycleways, pedestrian streets, busways, roads under
# construction and planned roads are left out: a car is not on them, and near a real road
# they could win as the nearest name.
ROAD_RANKS = {
    "motorway": 1, "motorway_link": 1, "trunk": 1, "trunk_link": 1,
    "primary": 2, "primary_link": 2,
    "secondary": 3, "secondary_link": 3,
    "tertiary": 4, "tertiary_link": 4,
    "unclassified": 5, "residential": 5, "living_street": 5, "service": 5, "track": 5,
    "road": 5,
}

# place=* -> place rank (mtaddr: 1 city, 2 town, 3 suburb, 4 village, 5 neighbourhood,
# 6 hamlet). In the Philippines place=quarter marks a barangay in a town or city (most carry
# admin_level=10, like the place=village barangays in the country), so it ranks as a village.
PLACE_RANKS = {"city": 1, "town": 2, "suburb": 3, "quarter": 4, "village": 4,
               "neighbourhood": 5, "hamlet": 6}

KIND_NAMES = {mtaddr.KIND_PROVINCE: "province", mtaddr.KIND_CITY: "city or municipality",
              mtaddr.KIND_BARANGAY: "barangay", mtaddr.KIND_ROAD: "road", mtaddr.KIND_PLACE: "place"}


# ---------------------------------------------------------------------------------------------
# Tags

# Characters the dashboard's font (Barlow) has no glyph for, written the way people would type
# them: the non-breaking and other hyphens, the numero sign and the Roman numeral characters
# (as in "Barangay Ⅲ").
_ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI", "XII"]
FONT_SUBSTITUTES = {"‐": "-", "‑": "-", "‒": "-", "№": "No."}
FONT_SUBSTITUTES.update({chr(0x2160 + i): r for i, r in enumerate(_ROMAN)})
FONT_SUBSTITUTES.update({chr(0x2170 + i): r.lower() for i, r in enumerate(_ROMAN)})


def clean_name(text: object) -> str:
    """A name as OpenStreetMap has it, minus control characters and doubled spaces, with accents
    joined to their letters and the characters in FONT_SUBSTITUTES replaced."""
    if not isinstance(text, str):
        return ""
    out = []
    for ch in unicodedata.normalize("NFC", text):
        ch = FONT_SUBSTITUTES.get(ch, ch)
        cat = unicodedata.category(ch[0])
        if cat[0] == "Z" or ch in "\t\n\r":
            out.append(" ")
        elif cat[0] != "C" and cat != "Mn":  # a mark left on its own cannot be drawn
            out.append(ch)
    return " ".join("".join(out).split())


def feature_name(props: Dict[str, object]) -> str:
    return clean_name(props.get("name")) or clean_name(props.get("name:en"))


def admin_kind(props: Dict[str, object]) -> Optional[int]:
    """The area kind (mtaddr.KIND_PROVINCE, _CITY, _BARANGAY) of a boundary, or None."""
    if props.get("boundary") != "administrative" or not feature_name(props):
        return None
    level = str(props.get("admin_level") or "").strip()
    if props.get("@type", "relation") == "way":
        return mtaddr.KIND_BARANGAY if level == "10" else None
    if level == "3" and feature_name(props) in PROVINCE_LEVEL_REGIONS:
        return mtaddr.KIND_PROVINCE
    return ADMIN_LEVEL_KINDS.get(level)


def road_rank(props: Dict[str, object]) -> Optional[int]:
    return ROAD_RANKS.get(str(props.get("highway") or ""))


def place_rank(props: Dict[str, object]) -> Optional[int]:
    return PLACE_RANKS.get(str(props.get("place") or ""))


# ---------------------------------------------------------------------------------------------
# Simplification (Douglas-Peucker: keep the point farthest from the straight line between two
# kept points while it is farther than the tolerance, then repeat on both halves)

def _scales(points: Sequence[Point]) -> Tuple[float, float]:
    """Metres per degree of longitude and latitude around these points."""
    lat = (min(p[1] for p in points) + max(p[1] for p in points)) / 2.0
    return mtaddr.M_PER_DEG_LON_EQUATOR * max(math.cos(math.radians(lat)), 0.01), mtaddr.M_PER_DEG_LAT


def _mark(xs: List[float], ys: List[float], first: int, last: int, tol2: float,
          keep: List[bool]) -> None:
    stack = [(first, last)]
    while stack:
        a, b = stack.pop()
        if b - a < 2:
            continue
        ax, ay = xs[a], ys[a]
        dx, dy = xs[b] - ax, ys[b] - ay
        length2 = dx * dx + dy * dy
        far, far_d = -1, tol2
        for i in range(a + 1, b):
            px, py = xs[i] - ax, ys[i] - ay
            if length2 > 0.0:
                t = (px * dx + py * dy) / length2
                if t > 1.0:
                    t = 1.0
                elif t < 0.0:
                    t = 0.0
                px -= t * dx
                py -= t * dy
            d = px * px + py * py
            if d > far_d:
                far, far_d = i, d
        if far >= 0:
            keep[far] = True
            stack.append((a, far))
            stack.append((far, b))


def _dedupe(points: Sequence[Point]) -> List[Point]:
    out: List[Point] = []
    for p in points:
        p = (float(p[0]), float(p[1]))
        if not out or p != out[-1]:
            out.append(p)
    return out


def simplify_line(points: Sequence[Point], tolerance_m: float) -> List[Point]:
    """The line with only the points needed to stay within tolerance_m of the original."""
    pts = _dedupe(points)
    if len(pts) < 3 or tolerance_m <= 0:
        return pts
    kx, ky = _scales(pts)
    xs = [p[0] * kx for p in pts]
    ys = [p[1] * ky for p in pts]
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    _mark(xs, ys, 0, len(pts) - 1, tolerance_m * tolerance_m, keep)
    return [p for p, k in zip(pts, keep) if k]


def simplify_ring(ring: Sequence[Point], tolerance_m: float) -> List[Point]:
    """A closed ring simplified like a line, split at its first point and the point farthest
    from it. Returns a closed ring with at least three corners, or [] for a ring with no area
    (all points on one line)."""
    pts = _dedupe(ring)
    if len(pts) > 1 and pts[0] == pts[-1]:
        pts.pop()
    if len(pts) < 3:
        return []
    kx, ky = _scales(pts)
    xs = [p[0] * kx for p in pts]
    ys = [p[1] * ky for p in pts]
    # Close the ring in the projected lists so the last half ends on the first point.
    xs.append(xs[0])
    ys.append(ys[0])
    n = len(xs)
    far = max(range(1, n - 1), key=lambda i: (xs[i] - xs[0]) ** 2 + (ys[i] - ys[0]) ** 2)
    keep = [False] * n
    keep[0] = keep[far] = keep[n - 1] = True
    if tolerance_m > 0:
        tol2 = tolerance_m * tolerance_m
        _mark(xs, ys, 0, far, tol2, keep)
        _mark(xs, ys, far, n - 1, tol2, keep)
    else:
        keep = [True] * n
    if sum(keep) < 4:
        # A sliver thinner than the tolerance: keep it as the triangle with the most area.
        ax, ay, bx, by = xs[0], ys[0], xs[far], ys[far]
        best, best_i = 0.0, -1
        for i in range(1, n - 1):
            if i != far:
                area2 = abs((bx - ax) * (ys[i] - ay) - (by - ay) * (xs[i] - ax))
                if area2 > best:
                    best, best_i = area2, i
        if best_i < 0:
            return []
        keep[best_i] = True
    out = [pts[i] for i in range(n - 1) if keep[i]]
    out.append(out[0])
    return out


# ---------------------------------------------------------------------------------------------
# GeoJSON

def read_features(path: Path, problems: Optional[Dict[str, int]] = None) -> Iterator[dict]:
    """Features of a GeoJSON sequence file (one feature per line, with or without the record
    separator character osmium puts in front). Lines that are not a feature are skipped and
    counted in problems["bad lines"]."""
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip("\x1e \t\r\n")
            if not line:
                continue
            try:
                feature = json.loads(line)
            except ValueError:
                feature = None
            if (not isinstance(feature, dict) or not isinstance(feature.get("geometry"), dict)
                    or not isinstance(feature.get("properties", {}), dict)):
                if problems is not None:
                    problems["bad lines"] = problems.get("bad lines", 0) + 1
                continue
            yield feature


def _is_point(p: object) -> bool:
    return (isinstance(p, (list, tuple)) and len(p) >= 2 and all(isinstance(v, (int, float)) for v in p[:2])
            and math.isfinite(p[0]) and math.isfinite(p[1]))


def _points(coords: object) -> List[Point]:
    if not isinstance(coords, list):
        return []
    return [(float(p[0]), float(p[1])) for p in coords if _is_point(p)]


def bbox_of(points: Sequence[Point]) -> Bounds:
    return (min(p[0] for p in points), min(p[1] for p in points),
            max(p[0] for p in points), max(p[1] for p in points))


def overlaps(box: Bounds, bounds: Bounds) -> bool:
    return not (box[2] < bounds[0] or box[0] > bounds[2] or box[3] < bounds[1] or box[1] > bounds[3])


def feature_rings(geometry: dict) -> List[List[Point]]:
    """All rings of a Polygon or MultiPolygon (outer rings and holes alike)."""
    kind, coords = geometry.get("type"), geometry.get("coordinates")
    if not isinstance(coords, list):
        return []
    if kind == "Polygon":
        polygons = [coords]
    elif kind == "MultiPolygon":
        polygons = coords
    else:
        return []
    rings = []
    for polygon in polygons:
        if isinstance(polygon, list):
            for ring in polygon:
                pts = _points(ring)
                if len(pts) >= 3:
                    rings.append(pts)
    return rings


def area_from_feature(feature: dict, bounds: Bounds,
                      tolerance_m: float) -> Optional[Tuple[int, str, List[List[Point]]]]:
    """(kind, name, simplified rings) of an administrative area that reaches into bounds."""
    props = feature.get("properties") or {}
    kind = admin_kind(props)
    if kind is None:
        return None
    rings = feature_rings(feature["geometry"])
    if not rings or not overlaps(bbox_of([p for r in rings for p in r]), bounds):
        return None
    simple = [r for r in (simplify_ring(r, tolerance_m) for r in rings) if len(r) >= 4]
    if not simple:
        return None
    if len(simple) > MAX_RINGS:
        # Never seen in the data; keep the rings with the most points (the big ones).
        simple = sorted(simple, key=len, reverse=True)[:MAX_RINGS]
    return kind, feature_name(props), simple


def road_from_feature(feature: dict, bounds: Bounds,
                      tolerance_m: float) -> Optional[Tuple[str, int, List[Point]]]:
    """(name, rank, simplified points) of a named road that reaches into bounds."""
    props = feature.get("properties") or {}
    rank = road_rank(props)
    name = feature_name(props)
    geometry = feature["geometry"]
    if rank is None or not name or geometry.get("type") != "LineString":
        return None
    pts = _points(geometry.get("coordinates"))
    if len(pts) < 2 or not overlaps(bbox_of(pts), bounds):
        return None
    pts = simplify_line(pts, tolerance_m)
    return (name, rank, pts) if len(pts) >= 2 else None


def place_from_feature(feature: dict, bounds: Bounds) -> Optional[Tuple[str, int, float, float]]:
    """(name, rank, lon, lat) of a named place inside bounds."""
    props = feature.get("properties") or {}
    rank = place_rank(props)
    name = feature_name(props)
    geometry = feature["geometry"]
    if rank is None or not name or geometry.get("type") != "Point" or not _is_point(geometry.get("coordinates")):
        return None
    lon, lat = float(geometry["coordinates"][0]), float(geometry["coordinates"][1])
    if not (bounds[0] <= lon <= bounds[2] and bounds[1] <= lat <= bounds[3]):
        return None
    return name, rank, lon, lat


# ---------------------------------------------------------------------------------------------
# Independent cities

def interior_point(rings: Sequence[Sequence[Point]]) -> Optional[Point]:
    """A point inside an outline (even-odd rule over all rings): the middle of the widest
    inside stretch of a horizontal line across its box."""
    ys = [p[1] for r in rings for p in r]
    if not ys:
        return None
    y0, y1 = min(ys), max(ys)
    for frac in (0.5, 0.35, 0.65, 0.2, 0.8, 0.1, 0.9):
        y = y0 + (y1 - y0) * frac
        xs = []
        for r in rings:
            for (ax, ay), (bx, by) in zip(r, r[1:]):
                if (ay > y) != (by > y):
                    xs.append(ax + (y - ay) * (bx - ax) / (by - ay))
        xs.sort()
        best = None
        for j in range(0, len(xs) - 1, 2):
            if best is None or xs[j + 1] - xs[j] > best[0]:
                best = (xs[j + 1] - xs[j], (xs[j] + xs[j + 1]) / 2)
        if best is not None and best[0] > 0:
            return best[1], y
    return None


def _inside_deg(rings: Sequence[Sequence[Point]], point: Point) -> bool:
    return mtaddr.inside([[(mtaddr.e6(x), mtaddr.e6(y)) for x, y in r] for r in rings],
                         mtaddr.e6(point[0]), mtaddr.e6(point[1]))


def independent_city_provinces(areas: Sequence[Tuple[int, str, List[List[Point]]]]
                               ) -> List[Tuple[str, Tuple[int, str, List[List[Point]]]]]:
    """For each city of INDEPENDENT_CITY_PROVINCES whose outline lies in no province outline,
    the city's outline once more as an area of its province. Returns (city, area) pairs.

    A separate area rather than extra rings of the province: where the two outlines overlap a
    little after simplification, both still answer with the same province."""
    provinces = {}
    for kind, name, rings in areas:
        if kind == mtaddr.KIND_PROVINCE:
            provinces.setdefault(name, []).append((bbox_of([p for r in rings for p in r]), rings))
    out = []
    for kind, name, rings in areas:
        province = INDEPENDENT_CITY_PROVINCES.get(name)
        if kind != mtaddr.KIND_CITY or province not in provinces:
            continue
        point = interior_point(rings)
        if point is None:
            continue
        if any(box[0] <= point[0] <= box[2] and box[1] <= point[1] <= box[3] and _inside_deg(r, point)
               for parts in provinces.values() for box, r in parts):
            continue
        out.append((name, (mtaddr.KIND_PROVINCE, province, rings)))
    return out


# ---------------------------------------------------------------------------------------------
# Build

def build(areas: Path, roads: Path, places: Path, output: Path, bounds: Bounds = LUZON,
          cell_deg: float = DEFAULT_CELL_DEG, road_radius_m: int = DEFAULT_ROAD_RADIUS_M,
          place_radius_m: int = DEFAULT_PLACE_RADIUS_M,
          area_tolerance_m: float = DEFAULT_AREA_TOLERANCE_M,
          road_tolerance_m: float = DEFAULT_ROAD_TOLERANCE_M,
          max_cell_items: int = MAX_CELL_ITEMS, exact_cells: bool = True,
          log: Callable[[str], None] = print) -> dict:
    """Writes the address file and returns numbers about it. The file is written under a
    temporary name and renamed when complete; it is removed again if a cell holds more than
    max_cell_items items."""
    started = time.time()
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_name(output.name + ".part")
    writer = mtaddr.AddrWriter(temp, bounds, cell_deg=cell_deg, road_radius_m=road_radius_m,
                               place_radius_m=place_radius_m, exact_cells=exact_cells)
    problems: Dict[str, int] = {}
    points_in = {"areas": 0, "roads": 0}
    points_out = {"areas": 0, "roads": 0}
    largest_shape = (0, "")
    long_names = set()
    characters = set()

    def note_name(name: str) -> None:
        if len(name.encode("utf-8")) > 95:
            long_names.add(name)
        characters.update(ch for ch in name if ord(ch) > 0x7E)

    log("reading areas from %s" % areas)
    found_areas = []
    for feature in read_features(areas, problems):
        found = area_from_feature(feature, bounds, area_tolerance_m)
        if found is not None:
            found_areas.append(found)
            points_in["areas"] += sum(len(r) for r in feature_rings(feature["geometry"]))
    extra = independent_city_provinces(found_areas)
    for kind, name, rings in found_areas + [area for _, area in extra]:
        if writer.add_area(kind, name, rings):
            n = sum(len(r) for r in rings)
            points_out["areas"] += n
            size = 4 * len(rings) + 8 * n
            if size > largest_shape[0]:
                largest_shape = (size, name)
            note_name(name)
    log("reading roads from %s" % roads)
    for feature in read_features(roads, problems):
        found = road_from_feature(feature, bounds, road_tolerance_m)
        if found is None:
            continue
        name, rank, pts = found
        points_in["roads"] += len(_points(feature["geometry"].get("coordinates")))
        points_out["roads"] += len(pts)
        writer.add_road(name, pts, rank)
        note_name(name)
    log("reading places from %s" % places)
    for feature in read_features(places, problems):
        found = place_from_feature(feature, bounds)
        if found is None:
            continue
        name, rank, lon, lat = found
        writer.add_place(name, lon, lat, rank)
        note_name(name)
    log("writing %s" % output)
    info = writer.close()
    cell_items, col, row = info["largest_cell"]
    if cell_items > max_cell_items:
        temp.unlink()
        raise ValueError("a grid cell (column %d, row %d) would hold %d items, more than %d; use a "
                         "smaller --cell-deg or --place-radius-m" % (col, row, cell_items, max_cell_items))
    os.replace(temp, output)
    info.update({
        "output": str(output), "bounds": list(bounds), "cell_deg": cell_deg,
        "road_radius_m": road_radius_m, "place_radius_m": place_radius_m,
        "area_tolerance_m": area_tolerance_m, "road_tolerance_m": road_tolerance_m,
        "exact_cells": exact_cells,
        "largest_cell": {"items": cell_items, "col": col, "row": row,
                         "lon": bounds[0] + (col + 0.5) * cell_deg, "lat": bounds[1] + (row + 0.5) * cell_deg},
        "points_in": points_in, "points_out": points_out,
        "largest_shape_bytes": largest_shape[0], "largest_shape": largest_shape[1],
        "names_over_95_bytes": len(long_names), "non_ascii": "".join(sorted(characters)),
        "independent_cities": ["%s (%s)" % (city, area[1]) for city, area in extra],
        "problems": problems,
        "seconds": round(time.time() - started, 1),
    })
    return info


def print_summary(info: dict, log: Callable[[str], None] = print) -> None:
    counts = info["counts"]
    for kind in (mtaddr.KIND_PROVINCE, mtaddr.KIND_CITY, mtaddr.KIND_BARANGAY, mtaddr.KIND_ROAD, mtaddr.KIND_PLACE):
        label = KIND_NAMES[kind] + ("s" if kind != mtaddr.KIND_ROAD else " pieces")
        if kind == mtaddr.KIND_CITY:
            label = "cities and municipalities"
        log("  %-26s %d" % (label, counts[kind]))
    cell = info["largest_cell"]
    log("  file size                  %d bytes (%.1f MB)" % (info["bytes"], info["bytes"] / 1e6))
    log("  items (copies per cell)    %d" % info["items"])
    log("  largest cell               %d items, near %.3f, %.3f" % (cell["items"], cell["lon"], cell["lat"]))
    log("  outline points             %d read, %d kept" % (info["points_in"]["areas"], info["points_out"]["areas"]))
    log("  road points                %d read, %d kept" % (info["points_in"]["roads"], info["points_out"]["roads"]))
    log("  largest outline            %d bytes (%s)" % (info["largest_shape_bytes"], info["largest_shape"]))
    log("  names over 95 bytes        %d" % info["names_over_95_bytes"])
    log("  independent cities         %s" % (", ".join(info["independent_cities"]) or "none"))
    log("  characters beyond ASCII    %s" % info["non_ascii"])
    if info["problems"]:
        log("  skipped input              %s" % info["problems"])
    log("  time                       %.1f s" % info["seconds"])


def parse_bounds(text: str) -> Bounds:
    parts = [float(p) for p in text.split(",")]
    if len(parts) != 4 or not (parts[0] < parts[2] and parts[1] < parts[3]):
        raise argparse.ArgumentTypeError("bounds need minlon,minlat,maxlon,maxlat with min < max")
    return parts[0], parts[1], parts[2], parts[3]


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Write the dashboard's offline address file from "
                                "OpenStreetMap GeoJSON exports (made by build_address.sh).")
    p.add_argument("--areas", type=Path, default=DEFAULT_WORK / "areas.geojsonseq")
    p.add_argument("--roads", type=Path, default=DEFAULT_WORK / "roads.geojsonseq")
    p.add_argument("--places", type=Path, default=DEFAULT_WORK / "places.geojsonseq")
    p.add_argument("--output", type=Path, default=DEFAULT_OUTPUT,
                   help="address file to write (default dist/map/Map/luzon.addr)")
    p.add_argument("--bounds", type=parse_bounds, default=LUZON,
                   help="minlon,minlat,maxlon,maxlat (default Luzon, 119.4,12.4,124.5,18.8)")
    p.add_argument("--cell-deg", type=float, default=DEFAULT_CELL_DEG,
                   help="grid cell size in degrees (default %g)" % DEFAULT_CELL_DEG)
    p.add_argument("--road-radius-m", type=int, default=DEFAULT_ROAD_RADIUS_M,
                   help="roads are listed in every cell this close (default %d)" % DEFAULT_ROAD_RADIUS_M)
    p.add_argument("--place-radius-m", type=int, default=DEFAULT_PLACE_RADIUS_M,
                   help="how far the nearest place may be (default %d)" % DEFAULT_PLACE_RADIUS_M)
    p.add_argument("--area-tolerance-m", type=float, default=DEFAULT_AREA_TOLERANCE_M,
                   help="outline simplification in metres (default %g)" % DEFAULT_AREA_TOLERANCE_M)
    p.add_argument("--road-tolerance-m", type=float, default=DEFAULT_ROAD_TOLERANCE_M,
                   help="road simplification in metres (default %g)" % DEFAULT_ROAD_TOLERANCE_M)
    p.add_argument("--bbox-cells", action="store_true",
                   help="list shapes in every cell of their bounding box (larger file, same answers)")
    p.add_argument("--stats", type=Path, help="also write the numbers as JSON to this file")
    p.add_argument("--check", action="store_true",
                   help="then print the coverage report of check_address.py (about a minute)")
    return p.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    for path in (args.areas, args.roads, args.places):
        if not path.is_file():
            print("error: %s is missing; run tools/address/build_address.sh" % path, file=sys.stderr)
            return 1
    try:
        info = build(args.areas, args.roads, args.places, args.output, args.bounds, args.cell_deg,
                     args.road_radius_m, args.place_radius_m, args.area_tolerance_m, args.road_tolerance_m,
                     exact_cells=not args.bbox_cells)
    except ValueError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    print("wrote %s" % args.output)
    print_summary(info)
    if args.stats:
        args.stats.parent.mkdir(parents=True, exist_ok=True)
        args.stats.write_text(json.dumps(info, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.check:
        import check_address
        print("")
        check_address.check(args.output, args.roads, args.places)
    return 0


if __name__ == "__main__":
    sys.exit(main())
