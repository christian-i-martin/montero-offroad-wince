#!/usr/bin/env python3
"""Builds a Montero vector map (*.vmap, format in mtvmap.py) for a small area, the pilot of a map
that the head unit draws itself (any zoom, any rotation) instead of showing ready-made pictures.

Input: the OpenMapTiles vector tiles that tools/map/build_map.sh already made for Luzon
(dist/map-build/tiles/openmaptiles.mbtiles, zoom 0 to 14). Drawing rules: our two map styles, as
tools/map/make_style.py builds them, compiled into the layer table: colours of both palettes,
line widths, dot sizes and text sizes for every 1/8 zoom level.

Every shape is cut exactly at the edge of its tile, so the head unit can draw neighbouring tiles
together without seams or doubled lines; outlines of areas (the coastline, park edges) leave out
the cut edges. Points are kept only in their own tile, so a name is stored once.

    python3 tools/vmap/build_vmap.py                           # Dasmarinas, Cavite -> dist/vmap/VMap
    python3 tools/vmap/build_vmap.py --bounds W,S,E,N --name NAME

The output folder must not be the picture map's folder (dist/map): the vector map is a separate
trial and never replaces those files. Python 3.9 compatible, standard library only.
"""
from __future__ import annotations

import argparse
import math
import sqlite3
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "map"))
import make_style  # noqa: E402
import mtvmap  # noqa: E402
import mvt  # noqa: E402

DEFAULT_MBTILES = ROOT / "dist" / "map-build" / "tiles" / "openmaptiles.mbtiles"
DEFAULT_OUTPUT = ROOT / "dist" / "vmap" / "VMap"
PICTURE_MAP = ROOT / "dist" / "map"
DASMARINAS = (120.89, 14.23, 121.02, 14.39)  # west, south, east, north: Dasmarinas City, Cavite

ZOOM_START = 8.0     # the zoom table runs from here ...
ZOOM_LAST = 19.0     # ... to here, in 1/8 steps
GROW_BEYOND = 16.0   # line widths and dots keep growing past the style's last stop ...
GROW_RATE = 1.4      # ... by this much per zoom level, so roads stay road-like when close
LARGE_FROM = 15.0    # text grows from its zoom 15 size to the large print size at zoom 16

# Place markers: a pictogram (dashboard/src/vicons.h, same numbers) on a round badge in the place's
# colour, upright at any bearing, with the name beside it (the four most important kinds from zoom
# 16, as the style named fuel stations, the others from zoom 17). Picked from the
# OpenMapTiles poi layer by class and subclass. Listed least important first: the renderer places
# the last ones first, so they win when markers would overlap. These replace the style's fuel dot.
(FUEL, PARKING, MALL, HOSPITAL, POLICE, PHARMACY, BANK, SUPERMARKET, CONVENIENCE, FOOD, CAR_REPAIR, HOTEL, CHURCH, SCHOOL,
 TOWN_HALL, BUS, TOLL, FIRE, POST, TOILETS, CAFE, BAKERY, HARDWARE, MOTORCYCLE, DENTIST, ATTRACTION, CAMPSITE,
 WATER) = range(28)


def _poi(classes: Sequence[str], subclasses: Sequence[str] = ()) -> list:
    test: list = ["match", ["get", "class"], list(classes), True, False]
    if subclasses:
        test = ["all", test, ["match", ["get", "subclass"], list(subclasses), True, False]]
    return test


# Colours by family: transport blue, shopping teal (malls purple), food raspberry, health red
# (pharmacies green), money and post gold, cars slate, landmarks brown, outdoors olive, water cyan.
LANDMARK, WATER_BLUE = 0x6D4C41, 0x0097A7

MARKERS = [
    # (rule name, pictogram, filter, badge colour, first zoom, names from zoom)
    ("marker-water", WATER, _poi(["drinking_water"]), WATER_BLUE, 17.0, 17.0),
    ("marker-toilets", TOILETS, _poi(["toilets"]), WATER_BLUE, 17.0, 17.0),
    ("marker-dentist", DENTIST, _poi(["dentist"]), 0xD32F2F, 17.0, 17.0),
    ("marker-bakery", BAKERY, _poi(["bakery"]), 0xC2185B, 17.0, 17.0),
    ("marker-cafe", CAFE, _poi(["cafe"]), 0xC2185B, 17.0, 17.0),
    ("marker-post", POST, _poi(["post"]), 0x8D6E00, 17.0, 17.0),
    ("marker-hardware", HARDWARE, _poi(["shop"], ["hardware", "doityourself"]), 0x00897B, 17.0, 17.0),
    ("marker-motorcycle", MOTORCYCLE, _poi(["shop"], ["motorcycle"]), 0x546E7A, 17.0, 17.0),
    ("marker-food", FOOD, _poi(["fast_food", "restaurant"]), 0xC2185B, 17.0, 17.0),
    ("marker-convenience", CONVENIENCE, _poi(["shop"], ["convenience"]), 0x00897B, 17.0, 17.0),
    ("marker-church", CHURCH, _poi(["place_of_worship"]), LANDMARK, 16.0, 17.0),
    ("marker-school", SCHOOL, _poi(["school", "college"]), LANDMARK, 16.0, 17.0),
    ("marker-town-hall", TOWN_HALL, _poi(["town_hall"]), LANDMARK, 16.0, 17.0),
    ("marker-attraction", ATTRACTION, _poi(["attraction", "museum", "monument"]), LANDMARK, 16.0, 17.0),
    ("marker-hotel", HOTEL, _poi(["lodging"], ["hotel", "motel", "guest_house", "hostel"]), 0x5E35B1, 16.0, 17.0),
    ("marker-car-repair", CAR_REPAIR, _poi(["car"], ["car_repair", "car_parts"]), 0x546E7A, 16.0, 17.0),
    ("marker-bank", BANK, _poi(["bank", "atm"]), 0x8D6E00, 16.0, 17.0),
    ("marker-pharmacy", PHARMACY, _poi(["pharmacy"]), 0x2E7D32, 16.0, 17.0),
    ("marker-supermarket", SUPERMARKET, _poi(["grocery"], ["supermarket", "marketplace"]), 0x00897B, 16.0, 17.0),
    ("marker-clinic", HOSPITAL, _poi(["hospital"], ["clinic"]), 0xD32F2F, 16.0, 17.0),
    ("marker-bus", BUS, _poi(["bus"], ["bus_station"]), 0x1E6FD9, 16.0, 17.0),
    ("marker-fire", FIRE, _poi(["fire_station"]), 0xD32F2F, 16.0, 17.0),
    ("marker-parking", PARKING, _poi(["parking"]), 0x1E6FD9, 16.0, 17.0),
    ("marker-campsite", CAMPSITE, _poi(["campsite", "picnic_site"]), 0x558B2F, 15.0, 16.0),
    # toll booths: highway toll plazas, but in this data also car park pay booths, hence not before 16
    ("marker-toll", TOLL, _poi(["toll_booth"]), 0x1E6FD9, 16.0, 16.0),
    ("marker-police", POLICE, _poi(["police"]), 0x1A237E, 15.0, 16.0),
    ("marker-mall", MALL, ["any", _poi(["shop"], ["mall"]), _poi(["grocery"], ["department_store"])], 0x8E24AA, 15.0,
     16.0),
    ("marker-hospital", HOSPITAL, _poi(["hospital"], ["hospital"]), 0xD32F2F, 15.0, 16.0),
    ("marker-fuel", FUEL, _poi(["fuel"]), 0xE65100, 15.0, 16.0),
]
REPLACED_BY_MARKERS = ("fuel", "fuel-name")


# ---------------------------------------------------------------- style expressions

def _num(v: Any) -> float:
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else 0.0


def _text(v: Any) -> str:
    if v is None:
        return ""
    if isinstance(v, float) and v.is_integer():
        return str(int(v))
    return str(v)


def evaluate(expr: Any, props: Dict[str, Any], zoom: float = 0.0) -> Any:
    """Evaluates the parts of the MapLibre expression language our styles use. zoom is a style
    zoom (dashboard zoom minus 1)."""
    if not isinstance(expr, list) or not expr or not isinstance(expr[0], str):
        return expr
    op, args = expr[0], expr[1:]
    if op == "get":
        return props.get(args[0])
    if op == "has":
        return args[0] in props
    if op == "zoom":
        return zoom
    if op == "==":
        return evaluate(args[0], props, zoom) == evaluate(args[1], props, zoom)
    if op == "!=":
        return evaluate(args[0], props, zoom) != evaluate(args[1], props, zoom)
    if op == "all":
        return all(evaluate(a, props, zoom) for a in args)
    if op == "any":
        return any(evaluate(a, props, zoom) for a in args)
    if op == "!":
        return not evaluate(args[0], props, zoom)
    if op == "match":
        value = evaluate(args[0], props, zoom)
        pairs, default = args[1:-1], args[-1]
        for k in range(0, len(pairs), 2):
            labels = pairs[k] if isinstance(pairs[k], list) else [pairs[k]]
            if value in labels:
                return evaluate(pairs[k + 1], props, zoom)
        return evaluate(default, props, zoom)
    if op == "coalesce":
        for a in args:
            v = evaluate(a, props, zoom)
            if v is not None:
                return v
        return None
    if op == "concat":
        return "".join(_text(evaluate(a, props, zoom)) for a in args)
    if op == "to-string":
        return _text(evaluate(args[0], props, zoom))
    if op == "case":
        for k in range(0, len(args) - 1, 2):
            if evaluate(args[k], props, zoom):
                return evaluate(args[k + 1], props, zoom)
        return evaluate(args[-1], props, zoom)
    if op == "interpolate":
        kind, stops = args[0], args[2:]
        z = _num(evaluate(args[1], props, zoom))
        base = kind[1] if kind[0] == "exponential" else 1.0
        pts = [(stops[k], _num(evaluate(stops[k + 1], props, zoom))) for k in range(0, len(stops), 2)]
        if z <= pts[0][0]:
            return pts[0][1]
        for (z0, v0), (z1, v1) in zip(pts, pts[1:]):
            if z <= z1:
                t = (z - z0) / (z1 - z0)
                if base != 1.0:
                    t = (base ** (z - z0) - 1) / (base ** (z1 - z0) - 1)
                return v0 + (v1 - v0) * t
        return pts[-1][1]
    if op == "step":
        z = _num(evaluate(args[0], props, zoom))
        value = evaluate(args[1], props, zoom)
        for k in range(2, len(args), 2):
            if z >= args[k]:
                value = evaluate(args[k + 1], props, zoom)
        return value
    raise ValueError("unsupported style expression %r" % op)


def colour(value: str) -> int:
    v = value.lstrip("#")
    if len(v) == 3:
        v = "".join(c * 2 for c in v)
    return int(v[:6], 16)


# ---------------------------------------------------------------- style compiling

def zoom_table() -> List[float]:
    count = int(round((ZOOM_LAST - ZOOM_START) * mtvmap.ZOOM_STEP)) + 1
    return [ZOOM_START + k / mtvmap.ZOOM_STEP for k in range(count)]


class Rule:
    """One compiled layer plus what the builder needs to pick its features."""

    def __init__(self, layer: mtvmap.Layer, source: str, filt: Any, text_field: Any, need_text: bool = True):
        self.layer, self.source, self.filter, self.text_field = layer, source, filt, text_field
        self.need_text = need_text  # labels without a name are left out; markers are drawn anyway


def _table(value: Any, zooms: Sequence[float], grow: bool) -> List[float]:
    out = []
    for z in zooms:
        v = _num(evaluate(value, {}, z - 1)) if isinstance(value, list) else _num(value)
        if grow and z > GROW_BEYOND:
            v *= GROW_RATE ** (z - GROW_BEYOND)
        out.append(v)
    return out


def _large(normal: List[float], large: List[float], zooms: Sequence[float]) -> List[float]:
    out = []
    for z, a, b in zip(zooms, normal, large):
        if z < LARGE_FROM:
            out.append(a)
        elif z < make_style.LARGE_ZOOM:
            k = zooms.index(LARGE_FROM)
            t = (z - LARGE_FROM) / (make_style.LARGE_ZOOM - LARGE_FROM)
            out.append(normal[k] + (large[zooms.index(float(make_style.LARGE_ZOOM))] - normal[k]) * t)
        else:
            out.append(b)
    return out


def _lighter(rgb: int, share: float) -> int:
    return sum(int(((rgb >> s) & 0xFF) + (255 - ((rgb >> s) & 0xFF)) * share) << s for s in (16, 8, 0))


def marker_rules(zooms: Sequence[float]) -> List[Rule]:
    """The place markers, between the road and peak names and the place names."""
    out = []
    for lid, icon, filt, badge, first, names_from in MARKERS:
        names = [0.0 if z < names_from else 12.0 * make_style.LARGE_PRINT if z >= make_style.LARGE_ZOOM else 12.0
                 for z in zooms]
        halos = [0.0 if n == 0 else n * 0.15 for n in names]
        layer = mtvmap.Layer(lid, mtvmap.MARKER, first, 99.0, (badge, badge),
                             (_lighter(badge, 0.55), badge),  # name colour: lighter on the dark map
                             anchor=mtvmap.ANCHOR_LEFT, max_width=8, size=names, size2=halos, icon=icon)
        out.append(Rule(layer, "poi", filt, make_style.NAME, need_text=False))
    return out


def compile_style() -> Tuple[List[Rule], Tuple[int, int]]:
    """The rules in drawing order, and the land colour (dark, light)."""
    dark, light = make_style.build_style("dark"), make_style.build_style("light")
    light_by_id = {item["id"]: item for item in light["layers"]}
    large_by_base = {item["id"][:-len("-large")]: item for item in dark["layers"] if item["id"].endswith("-large")}
    zooms = zoom_table()
    rules: List[Rule] = []
    land = (0, 0)
    for item in dark["layers"]:
        lid, kind = item["id"], item["type"]
        paint, layout = item.get("paint", {}), item.get("layout", {})
        lpaint = light_by_id[lid].get("paint", {})
        if kind == "background":
            land = (colour(paint["background-color"]), colour(lpaint["background-color"]))
            continue
        if lid.endswith("-large") or lid in REPLACED_BY_MARKERS:
            continue  # folded into its normal layer below, or replaced by a place marker
        if lid == "place-island":
            rules.extend(marker_rules(zooms))
        first = item.get("minzoom", 0) + 1
        end = 99.0
        if "maxzoom" in item and lid not in large_by_base:
            end = item["maxzoom"] + 1
        both = lambda key: (colour(paint[key]), colour(lpaint[key]))  # noqa: E731
        text_field = None
        if kind == "fill":
            layer = mtvmap.Layer(lid, mtvmap.AREA, first, end, both("fill-color"),
                                 opacity=_num(paint.get("fill-opacity", 1.0)), size=[0.0] * len(zooms))
        elif kind == "line":
            flags = mtvmap.FLAG_ROUND if layout.get("line-cap") == "round" else 0
            layer = mtvmap.Layer(lid, mtvmap.LINE, first, end, both("line-color"),
                                 opacity=_num(paint.get("line-opacity", 1.0)), flags=flags,
                                 dashes=paint.get("line-dasharray", []),
                                 size=_table(paint.get("line-width", 1.0), zooms, True))
        elif kind == "circle":
            layer = mtvmap.Layer(lid, mtvmap.DOT, first, end, both("circle-color"),
                                 both("circle-stroke-color") if "circle-stroke-color" in paint else (0, 0),
                                 size=_table(paint.get("circle-radius", 3.0), zooms, True),
                                 size2=_table(paint.get("circle-stroke-width", 0.0), zooms, True))
        elif kind == "symbol":
            along = layout.get("symbol-placement") == "line"
            size = _table(layout.get("text-size", 12), zooms, False)
            halo = _table(paint.get("text-halo-width", 0.0), zooms, False)
            if lid in large_by_base:
                big = large_by_base[lid]
                size = _large(size, _table(big["layout"]["text-size"], zooms, False), zooms)
                halo = _large(halo, _table(big["paint"].get("text-halo-width", 0.0), zooms, False), zooms)
            anchor = {"left": mtvmap.ANCHOR_LEFT, "top": mtvmap.ANCHOR_TOP}.get(layout.get("text-anchor"),
                                                                                mtvmap.ANCHOR_CENTRE)
            offset = tuple(layout.get("text-offset", [0.0, 0.0]))
            base_size = _num(layout.get("text-size", 12)) if not isinstance(layout.get("text-size"), list) else 12.0
            flags = mtvmap.FLAG_UPPER if layout.get("text-transform") == "uppercase" else 0
            layer = mtvmap.Layer(lid, mtvmap.LABEL_LINE if along else mtvmap.LABEL_POINT, first, end,
                                 both("text-color"), both("text-halo-color"), flags=flags, anchor=anchor,
                                 offset=offset, spacing=_num(layout.get("symbol-spacing", 250)) / base_size,
                                 max_angle=_num(layout.get("text-max-angle", 45)),
                                 max_width=_num(layout.get("text-max-width", 10)), size=size, size2=halo)
            text_field = layout.get("text-field")
        else:
            raise ValueError("layer %s: unsupported type %s" % (lid, kind))
        rules.append(Rule(layer, item.get("source-layer", ""), item.get("filter"), text_field))
    return rules, land


# ---------------------------------------------------------------- geometry

Point = Tuple[int, int]


def clip_polygon(ring: List[Point], lo: int, hi: int) -> List[Point]:
    """Sutherland-Hodgman: the ring cut to the square lo..hi on both axes."""
    def cut(points: List[Point], axis: int, limit: int, keep_below: bool) -> List[Point]:
        out: List[Point] = []
        if not points:
            return out
        prev = points[-1]
        for cur in points:
            pin = prev[axis] <= limit if keep_below else prev[axis] >= limit
            cin = cur[axis] <= limit if keep_below else cur[axis] >= limit
            if cin != pin:
                t = (limit - prev[axis]) / (cur[axis] - prev[axis])
                other = prev[1 - axis] + (cur[1 - axis] - prev[1 - axis]) * t
                q = (limit, int(round(other))) if axis == 0 else (int(round(other)), limit)
                out.append(q)
            if cin:
                out.append(cur)
            prev = cur
        return out

    pts = ring
    for axis in (0, 1):
        pts = cut(pts, axis, lo, False)
        pts = cut(pts, axis, hi, True)
    clean: List[Point] = []
    for p in pts:
        if not clean or clean[-1] != p:
            clean.append(p)
    if len(clean) > 1 and clean[0] == clean[-1]:
        clean.pop()
    return clean if len(clean) >= 3 else []


def clip_line(line: List[Point], lo: int, hi: int) -> List[List[Point]]:
    """The polyline cut to the square: zero or more pieces (Liang-Barsky per segment)."""
    pieces: List[List[Point]] = []
    current: List[Point] = []
    for (x0, y0), (x1, y1) in zip(line, line[1:]):
        t0, t1 = 0.0, 1.0
        dx, dy = x1 - x0, y1 - y0
        ok = True
        for p, q in ((-dx, x0 - lo), (dx, hi - x0), (-dy, y0 - lo), (dy, hi - y0)):
            if p == 0:
                if q < 0:
                    ok = False
                    break
            else:
                r = q / p
                if p < 0:
                    t0 = max(t0, r)
                else:
                    t1 = min(t1, r)
        if not ok or t0 > t1:
            if len(current) > 1:
                pieces.append(current)
            current = []
            continue
        a = (int(round(x0 + dx * t0)), int(round(y0 + dy * t0)))
        b = (int(round(x0 + dx * t1)), int(round(y0 + dy * t1)))
        if not current or current[-1] != a:
            if len(current) > 1:
                pieces.append(current)
            current = [a]
        if b != current[-1]:
            current.append(b)
        if t1 < 1.0:
            if len(current) > 1:
                pieces.append(current)
            current = []
    if len(current) > 1:
        pieces.append(current)
    return pieces


def outline(ring: List[Point], lo: int, hi: int) -> List[List[Point]]:
    """A clipped ring as lines, leaving out the stretches that run along the tile's edge."""
    def on_edge(a: Point, b: Point) -> bool:
        return (a[0] == b[0] and a[0] in (lo, hi)) or (a[1] == b[1] and a[1] in (lo, hi))

    n = len(ring)
    if n < 2:
        return []
    start = next((k for k in range(n) if on_edge(ring[k - 1], ring[k])), None)
    if start is None:
        return [ring + [ring[0]]]
    pieces: List[List[Point]] = []
    current: List[Point] = []
    for k in range(n):
        a, b = ring[(start + k) % n], ring[(start + k + 1) % n]
        if on_edge(a, b):
            if len(current) > 1:
                pieces.append(current)
            current = []
            continue
        if not current:
            current = [a]
        current.append(b)
    if len(current) > 1:
        pieces.append(current)
    return pieces


BUCKETS = 8  # areas and lines are split into an 8 x 8 grid per tile, so the head unit can skip
             # the parts that are off the screen (the source tiles merge hundreds of buildings or
             # streets into one feature)


def ring_area(ring: List[Point]) -> int:
    """Twice the signed area; positive for outer rings in vector tiles (y down)."""
    return sum(ring[k][0] * ring[(k + 1) % len(ring)][1] - ring[(k + 1) % len(ring)][0] * ring[k][1]
               for k in range(len(ring)))


def bucketed(parts: List[List[Point]], polygon: bool) -> List[List[List[Point]]]:
    """parts grouped by the grid cell of their middle: one list of parts per non-empty cell. An
    area's holes stay with their outer ring."""
    groups: List[List[List[Point]]] = []
    if polygon:
        for ring in parts:
            if ring_area(ring) > 0 or not groups:
                groups.append([ring])
            else:
                groups[-1].append(ring)
    else:
        groups = [[part] for part in parts]
    cells: Dict[Tuple[int, int], List[List[Point]]] = {}
    for group in groups:
        xs = [x for x, _ in group[0]]
        ys = [y for _, y in group[0]]
        cx = min(BUCKETS - 1, max(0, (min(xs) + max(xs)) // 2 * BUCKETS // mtvmap.EXTENT))
        cy = min(BUCKETS - 1, max(0, (min(ys) + max(ys)) // 2 * BUCKETS // mtvmap.EXTENT))
        cells.setdefault((cy, cx), []).extend(group)
    return [cells[k] for k in sorted(cells)]


# ---------------------------------------------------------------- tiles

def tile_range(bounds: Tuple[float, float, float, float], zoom: int) -> Tuple[int, int, int, int]:
    """(x0, y0, x1, y1) inclusive: the tiles covering the bounds at this zoom."""
    west, south, east, north = bounds

    def tx(lon: float) -> int:
        return int((lon + 180.0) / 360.0 * (1 << zoom))

    def ty(lat: float) -> int:
        r = math.radians(lat)
        return int((1.0 - math.log(math.tan(r) + 1.0 / math.cos(r)) / math.pi) / 2.0 * (1 << zoom))

    return tx(west), ty(north), tx(east), ty(south)


def build_tile(layers: Dict[str, Dict[str, Any]], rules: Sequence[Rule], view_first: float,
               view_end: float, stats: Dict[str, int]) -> Optional[bytes]:
    """One .vmap tile from a decoded vector tile: the features the rules draw at view zooms
    view_first to view_end."""
    features: List[mtvmap.Feature] = []
    draws: Dict[int, List[int]] = {}
    shapes: Dict[Tuple[int, int, str], List[int]] = {}  # (source layer, feature, kind) -> feature numbers
    lo, hi = 0, mtvmap.EXTENT

    def add(key: Tuple[int, int, str], make) -> List[int]:
        if key not in shapes:
            made = make()
            shapes[key] = list(range(len(features), len(features) + len(made)))
            features.extend(made)
        return shapes[key]

    def areas(parts) -> List[mtvmap.Feature]:
        rings = [r for r in (clip_polygon(p, lo, hi) for p in parts) if r]
        return [mtvmap.Feature(mtvmap.FEATURE_AREA, group) for group in bucketed(rings, True)] if rings else []

    def lines(pieces) -> List[mtvmap.Feature]:
        return [mtvmap.Feature(mtvmap.FEATURE_LINE, group) for group in bucketed(pieces, False)] if pieces else []

    for number, rule in enumerate(rules):
        lay = rule.layer
        if lay.first_zoom >= view_end or lay.end_zoom <= view_first:
            continue
        src = layers.get(rule.source)
        if src is None:
            continue
        scale = mtvmap.EXTENT / src["extent"]
        for fi, feat in enumerate(src["features"]):
            props = feat["properties"]
            if rule.filter is not None and not evaluate(rule.filter, props):
                continue
            parts = [[(int(round(x * scale)), int(round(y * scale))) for x, y in p] for p in feat["parts"]]
            ftype = feat["type"]
            text = None
            if rule.text_field is not None:
                text = _text(evaluate(rule.text_field, props)).strip()
                if not text and rule.need_text:
                    continue
                text = text or None
            key = (id(src), fi)
            if lay.kind == mtvmap.AREA:
                if ftype != mvt.POLYGON:
                    continue
                made = add(key + ("area",), lambda parts=parts: areas(parts))
            elif lay.kind == mtvmap.LINE:
                if ftype == mvt.POLYGON:
                    made = add(key + ("outline",), lambda parts=parts: lines(
                        [piece for p in parts for piece in outline(clip_polygon(p, lo, hi), lo, hi)]))
                elif ftype == mvt.LINE:
                    made = add(key + ("line",), lambda parts=parts: lines(
                        [piece for p in parts for piece in clip_line(p, lo, hi)]))
                else:
                    continue
            elif lay.kind == mtvmap.LABEL_LINE:
                if ftype != mvt.LINE:
                    continue

                def named_line(parts=parts, text=text) -> List[mtvmap.Feature]:
                    pieces = [piece for p in parts for piece in clip_line(p, lo, hi)]
                    return [mtvmap.Feature(mtvmap.FEATURE_LINE, pieces, text)] if pieces else []
                made = add(key + ("named-line",), named_line)
            else:  # dots and point labels: points inside this tile only
                if ftype != mvt.POINT:
                    continue
                rank = props.get("rank", 0)
                rank = int(rank) if isinstance(rank, (int, float)) and rank > 0 else 0

                def point(parts=parts, text=text, rank=rank) -> List[mtvmap.Feature]:
                    pts = [[q] for p in parts for q in p if lo <= q[0] < hi and lo <= q[1] < hi]
                    return [mtvmap.Feature(mtvmap.FEATURE_POINT, pts, text, rank)] if pts else []
                made = add(key + ("point:" + (text or ""),), point)
            if not made:
                continue
            draws.setdefault(number, []).extend(made)
            stats[lay.name] = stats.get(lay.name, 0) + len(made)
    if not draws:
        return None
    for members in draws.values():
        if lay_sort_needed(members, features):
            members.sort(key=lambda i: features[i].rank)
    return mtvmap.encode_tile(features, draws)


def lay_sort_needed(members: List[int], features: List[mtvmap.Feature]) -> bool:
    return any(features[i].rank for i in members)


def build(mbtiles: Path, bounds: Tuple[float, float, float, float], output: Path,
          min_zoom: int = 10, max_zoom: int = 14) -> Dict[str, Any]:
    rules, land = compile_style()
    db = sqlite3.connect("file:%s?mode=ro" % mbtiles, uri=True)
    levels: List[mtvmap.Level] = []
    stats: Dict[str, int] = {}
    raw_bytes = 0
    for zoom in range(min_zoom, max_zoom + 1):
        x0, y0, x1, y1 = tile_range(bounds, zoom)
        view_first = float(zoom) if zoom > min_zoom else float(min_zoom)
        view_end = float(zoom + 1) if zoom < max_zoom else 99.0
        level = mtvmap.Level(zoom, x0, y0, x1 - x0 + 1, y1 - y0 + 1, view_first, view_end)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                row = db.execute("SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                                 (zoom, x, (1 << zoom) - 1 - y)).fetchone()
                if row is None:
                    continue
                tile = build_tile(mvt.decode_tile(row[0]), rules, view_first, view_end, stats)
                if tile is not None:
                    level.tiles[(x, y)] = tile
                    raw_bytes += len(tile)
        levels.append(level)
    zooms = zoom_table()
    output.parent.mkdir(parents=True, exist_ok=True)
    size = mtvmap.write(output, [r.layer for r in rules], levels, land, bounds, zooms[0], len(zooms))
    return {"layers": len(rules), "levels": [(lv.zoom, lv.cols, lv.rows, len(lv.tiles)) for lv in levels],
            "bytes": size, "raw_bytes": raw_bytes, "features": stats}


def parse_bounds(text: str) -> Tuple[float, float, float, float]:
    parts = [float(v) for v in text.split(",")]
    if len(parts) != 4 or parts[0] >= parts[2] or parts[1] >= parts[3]:
        raise argparse.ArgumentTypeError("bounds must be W,S,E,N")
    return parts[0], parts[1], parts[2], parts[3]


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--mbtiles", type=Path, default=DEFAULT_MBTILES)
    parser.add_argument("--bounds", type=parse_bounds, default=DASMARINAS, help="W,S,E,N in degrees")
    parser.add_argument("--name", default="dasmarinas")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="folder for NAME.vmap")
    parser.add_argument("--min-zoom", type=int, default=10)
    parser.add_argument("--max-zoom", type=int, default=14)
    args = parser.parse_args(argv)
    out_dir = args.output.resolve()
    if out_dir == PICTURE_MAP.resolve() or PICTURE_MAP.resolve() in out_dir.parents:
        print("refusing to write into %s: the picture map stays as it is" % PICTURE_MAP, file=sys.stderr)
        return 2
    if not args.mbtiles.exists():
        print("no vector tiles at %s: run tools/map/build_map.sh first" % args.mbtiles, file=sys.stderr)
        return 1
    start = time.time()
    path = out_dir / (args.name + ".vmap")
    info = build(args.mbtiles, args.bounds, path, args.min_zoom, args.max_zoom)
    print("wrote %s: %.2f MB (%.2f MB before compression), %d layers, %.1f s" % (
        path, info["bytes"] / 1e6, info["raw_bytes"] / 1e6, info["layers"], time.time() - start))
    for zoom, cols, rows, n in info["levels"]:
        print("  zoom %d: %d x %d tiles, %d with data" % (zoom, cols, rows, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
