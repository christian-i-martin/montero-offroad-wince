#!/usr/bin/env python3
"""Checks a built address file (.addr) against the OpenStreetMap exports it was made from.

Uses the reference lookup (mtaddr.AddrReader.lookup) and prints:

1. The areas in the file by kind.
2. Place coverage: the share of named place nodes (village, hamlet, neighbourhood; and town or
   city for the city outlines) whose position gets a barangay, a city or municipality, a
   province, and any place within the file's place radius.
3. Road coverage: points every --step-m metres along all drivable highway ways (named or not,
   the classes of build_address.ROAD_RANKS), and the share that get a road name within 50 m,
   by class.
4. What a lookup costs the head unit: items in the cell, outlines whose box holds the point
   (each one's points are read and tested) and bytes read, on those road points.
5. Lookups at a few public landmarks.

Positions are only used here on the Mac; nothing is written but the report (stdout, and --json).

Python 3.9, standard library only.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import struct
import sys
import time
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Sequence, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_address  # noqa: E402
import mtaddr  # noqa: E402

# Public landmarks and town centres (approximate positions), for sample lookups.
LANDMARKS = [
    ("Manila, Rizal Park", 120.9796, 14.5831),
    ("Manila, Tondo (numbered barangays)", 120.9668, 14.6122),
    ("Manila, Sampaloc near the University of Santo Tomas", 120.9890, 14.6096),
    ("Makati, Ayala Avenue", 121.0233, 14.5560),
    ("Quezon City, Quezon Memorial Circle", 121.0509, 14.6517),
    ("Pateros, town centre", 121.0688, 14.5441),
    ("Baguio, Session Road", 120.5975, 16.4120),
    ("Vigan, Calle Crisologo", 120.3887, 17.5713),
    ("Laoag, town centre", 120.5930, 18.1975),
    ("Tuguegarao, town centre", 121.7275, 17.6135),
    ("Tarlac City, town centre", 120.5963, 15.4802),
    ("Baler, Aurora, town centre", 121.5612, 15.7590),
    ("Lucena, town centre", 121.6170, 13.9310),
    ("Naga, Camarines Sur, town centre", 123.1857, 13.6235),
    ("Legazpi, Albay, town centre", 123.7533, 13.1435),
    ("Sagada, Mountain Province", 120.9010, 17.0840),
    ("Banaue, Ifugao, town centre", 121.0610, 16.9135),
    ("Sampiro, San Juan, Batangas (rural barangay)", 121.3858, 13.7844),
    ("Telbang, Alaminos, Pangasinan (rural barangay)", 120.0611, 16.1717),
    ("Rice fields of Nueva Ecija (rural)", 120.9000, 15.6200),
    ("Ambaguio trail to Mount Pulag, Nueva Vizcaya (track)", 121.04306, 16.54353),
    ("Montabiong - Banao Road, Ifugao (track)", 121.17486, 16.85113),
    ("Unnamed track in the mountains of Benguet", 120.83862, 16.74243),
    ("Pinatubo trek to the hot springs, Porac, Pampanga (track)", 120.45299, 15.16343),
]

_READER = None


class CachedReader(mtaddr.AddrReader):
    """The reference reader with outlines kept in memory after their first read (same answers,
    much faster for thousands of lookups)."""

    def __init__(self, path: Path) -> None:
        super().__init__(path)
        self._ring_cache: Dict[int, list] = {}

    def rings(self, item: tuple) -> list:
        if item[0] not in mtaddr.AREA_KINDS:
            return super().rings(item)
        found = self._ring_cache.get(item[8])
        if found is None:
            found = self._ring_cache[item[8]] = super().rings(item)
        return found

    def cost(self, lon: float, lat: float) -> Tuple[int, int, int]:
        """(items in the cell, outlines whose box holds the point, bytes of item records and
        coordinates a reader like the C one reads) for one lookup."""
        x, y = mtaddr.e6(lon), mtaddr.e6(lat)
        col, row = (x - self.lon0) // self.cell, (y - self.lat0) // self.cell
        items = self.items(col, row)
        kx = mtaddr.M_PER_DEG_LON_EQUATOR * math.cos(math.radians(y / 1e6)) / 1e6
        ky = mtaddr.M_PER_DEG_LAT / 1e6
        m = mtaddr.ROAD_MATCH_M
        candidates, read = 0, mtaddr.ITEM.size * len(items)
        for item in items:
            kind, _, nrings, _, bx0, by0, bx1, by1, _, npts = item
            if kind in mtaddr.AREA_KINDS:
                if bx0 <= x <= bx1 and by0 <= y <= by1:
                    candidates += 1
                    read += 4 * nrings + 8 * npts
            elif kind == mtaddr.KIND_ROAD:
                # the C reader reads a road only when its box grown by 50 m holds the point
                if not ((x - bx1) * kx > m or (bx0 - x) * kx > m or (y - by1) * ky > m or (by0 - y) * ky > m):
                    read += 8 * npts
        return len(items), candidates, read


def _init(path: str) -> None:
    global _READER
    _READER = CachedReader(Path(path))


def _lookup_many(points: List[Tuple[float, float]]) -> List[tuple]:
    out = []
    for lon, lat in points:
        r = _READER.lookup(lon, lat)
        out.append((r["province"], r["city"], r["barangay"], r["road"], r["place"]) + _READER.cost(lon, lat))
    return out


def lookup_all(addr: Path, points: List[Tuple[float, float]], workers: int) -> List[tuple]:
    """(province, city, barangay, road, place, cell items, outline candidates, bytes read) for
    each point, in order."""
    chunks = [points[i:i + 4000] for i in range(0, len(points), 4000)]
    if workers <= 1:
        _init(str(addr))
        return [r for chunk in chunks for r in _lookup_many(chunk)]
    out: List[tuple] = []
    with ProcessPoolExecutor(max_workers=workers, initializer=_init, initargs=(str(addr),)) as pool:
        for part in pool.map(_lookup_many, chunks):
            out.extend(part)
    return out


def sample_line(points: Sequence[Tuple[float, float]], step_m: float, start_m: float) -> Tuple[List[Tuple[float, float]], float]:
    """Points every step_m metres along a line, the first start_m metres in. Returns the points
    and how far into the next line the next point falls (so sampling runs on across lines)."""
    out = []
    at = start_m
    for (x1, y1), (x2, y2) in zip(points, points[1:]):
        kx = mtaddr.M_PER_DEG_LON_EQUATOR * math.cos(math.radians((y1 + y2) / 2))
        length = math.hypot((x2 - x1) * kx, (y2 - y1) * mtaddr.M_PER_DEG_LAT)
        while at <= length and length > 0:
            t = at / length
            out.append((x1 + t * (x2 - x1), y1 + t * (y2 - y1)))
            at += step_m
        at -= length
    return out, at


def road_samples(roads: Path, bounds: build_address.Bounds,
                 step_m: float) -> Iterator[Tuple[str, bool, float, float]]:
    """(highway class, named, lon, lat) every step_m metres along drivable highway ways."""
    at = step_m / 2
    for feature in build_address.read_features(roads):
        props = feature.get("properties") or {}
        highway = str(props.get("highway") or "")
        geometry = feature["geometry"]
        if highway not in build_address.ROAD_RANKS or geometry.get("type") != "LineString":
            continue
        pts = build_address._points(geometry.get("coordinates"))
        if len(pts) < 2 or not build_address.overlaps(build_address.bbox_of(pts), bounds):
            continue
        named = bool(build_address.feature_name(props))
        found, at = sample_line(pts, step_m, at)
        for lon, lat in found:
            if bounds[0] <= lon <= bounds[2] and bounds[1] <= lat <= bounds[3]:
                yield highway, named, lon, lat


def pct(part: int, whole: int) -> str:
    return "%5.1f%%" % (100.0 * part / whole) if whole else "   n/a"


def check(addr: Path, roads: Path, places: Path, step_m: float = 400.0, workers: int = 0,
          road_sample_limit: int = 0, log=print) -> dict:
    started = time.time()
    workers = workers or max(1, (os.cpu_count() or 2) - 2)
    reader = CachedReader(addr)
    bounds = (reader.lon0 / 1e6, reader.lat0 / 1e6, (reader.lon0 + reader.cols * reader.cell) / 1e6,
              (reader.lat0 + reader.rows * reader.cell) / 1e6)
    report: dict = {"file": str(addr), "bytes": len(reader.data), "items": reader.item_count,
                    "cell_deg": reader.cell / 1e6, "cols": reader.cols, "rows": reader.rows,
                    "road_radius_m": reader.road_radius_m, "place_radius_m": reader.place_radius_m}

    # 1. Areas and shapes by kind (one shape is listed once per cell, so count distinct ones).
    shapes: Dict[int, set] = {k: set() for k in (1, 2, 3, 4, 5)}
    largest = 0
    for i in range(reader.cols * reader.rows):
        a, b = struct.unpack_from("<II", reader.data, reader.cells_offset + 4 * i)
        largest = max(largest, b - a)
    for i in range(reader.item_count):
        item = mtaddr.ITEM.unpack_from(reader.data, reader.items_offset + mtaddr.ITEM.size * i)
        shapes[item[0]].add(item[8])
    report["shapes"] = {build_address.KIND_NAMES[k]: len(v) for k, v in shapes.items()}
    report["largest_cell"] = largest
    log("File %s: %d bytes (%.1f MB), %d items, largest cell %d items, cell %.3f degrees, "
        "road radius %d m, place radius %d m"
        % (addr, len(reader.data), len(reader.data) / 1e6, reader.item_count, largest, reader.cell / 1e6,
           reader.road_radius_m, reader.place_radius_m))
    log("Shapes: " + ", ".join("%s %d" % kv for kv in report["shapes"].items()))

    # 2. Place nodes.
    place_pts: List[Tuple[str, float, float]] = []
    for feature in build_address.read_features(places):
        props = feature.get("properties") or {}
        kind = str(props.get("place") or "")
        found = build_address.place_from_feature(feature, bounds)
        if found and kind in ("village", "hamlet", "neighbourhood", "quarter", "town", "city"):
            place_pts.append((kind, found[2], found[3]))
    results = lookup_all(addr, [(p[1], p[2]) for p in place_pts], workers)
    log("")
    log("Place nodes (named, inside the grid): share whose position gets ...")
    log("  %-14s %7s %9s %9s %9s %9s" % ("place=", "count", "barangay", "city", "province", "any place"))
    report["places"] = {}
    for kinds, label in ((("village",), "village"), (("hamlet",), "hamlet"), (("neighbourhood",), "neighbourhood"),
                         (("village", "hamlet", "neighbourhood"), "all three"), (("quarter",), "quarter"),
                         (("town", "city"), "town or city")):
        rows = [r for p, r in zip(place_pts, results) if p[0] in kinds]
        n = len(rows)
        got = [sum(1 for r in rows if r[i]) for i in (2, 1, 0)]
        log("  %-14s %7d %9s %9s %9s %9s" % (label, n, pct(got[0], n), pct(got[1], n), pct(got[2], n),
                                            pct(sum(1 for r in rows if r[4]), n)))
        report["places"][label] = {"count": n, "barangay": got[0], "city": got[1], "province": got[2]}

    # 3. Road coverage.
    log("")
    log("Sampling drivable roads every %g m ..." % step_m)
    samples = list(road_samples(roads, bounds, step_m))
    if road_sample_limit and len(samples) > road_sample_limit:
        samples = random.Random(1).sample(samples, road_sample_limit)
    results = lookup_all(addr, [(s[2], s[3]) for s in samples], workers)
    by_class: Dict[str, Counter] = {}
    for s, r in zip(samples, results):
        c = by_class.setdefault(s[0], Counter())
        c["n"] += 1
        c["named way"] += s[1]
        c["road"] += bool(r[3])
        c["place"] += bool(r[4])
        c["barangay"] += bool(r[2])
        c["city"] += bool(r[1])
        c["province"] += bool(r[0])
    total = Counter()
    for c in by_class.values():
        total.update(c)
    log("Road points: share that get a road name within %g m (and the share on a named way)"
        % mtaddr.ROAD_MATCH_M)
    log("  %-15s %8s %9s %10s %9s %9s %9s" % ("highway=", "points", "road name", "named way", "place",
                                             "barangay", "city"))
    order = sorted(by_class, key=lambda h: (build_address.ROAD_RANKS[h], -by_class[h]["n"]))
    for h in order + ["all"]:
        c = total if h == "all" else by_class[h]
        n = c["n"]
        log("  %-15s %8d %9s %10s %9s %9s %9s" % (h, n, pct(c["road"], n), pct(c["named way"], n),
                                                pct(c["place"], n), pct(c["barangay"], n), pct(c["city"], n)))
    report["roads"] = {h: dict(c) for h, c in list(by_class.items()) + [("all", total)]}

    # 4. Lookup cost on the road points.
    if results:
        items = [r[5] for r in results]
        cands = [r[6] for r in results]
        read = sorted(r[7] for r in results)
        report["cost"] = {"items_mean": sum(items) / len(items), "items_max": max(items),
                          "outlines_mean": sum(cands) / len(cands), "outlines_max": max(cands),
                          "bytes_mean": sum(read) / len(read), "bytes_p99": read[int(0.99 * (len(read) - 1))],
                          "bytes_max": read[-1]}
        log("")
        log("Lookup cost on these points: %.0f items in the cell on average (most %d), %.1f outlines "
            "tested (most %d), %.0f bytes read on average, %d at the 99th percentile, %d at most"
            % (report["cost"]["items_mean"], report["cost"]["items_max"], report["cost"]["outlines_mean"],
               report["cost"]["outlines_max"], report["cost"]["bytes_mean"], report["cost"]["bytes_p99"],
               report["cost"]["bytes_max"]))

    # 5. Landmarks.
    log("")
    log("Sample lookups (road within %g m, nearest place within %d m):" % (mtaddr.ROAD_MATCH_M, reader.place_radius_m))
    report["samples"] = []
    for label, lon, lat in LANDMARKS:
        r = reader.lookup(lon, lat)
        road = "%s (%.0f m)" % (r["road"], r["road_m"]) if r["road"] else "-"
        place = "%s (%.0f m)" % (r["place"], r["place_m"]) if r["place"] else "-"
        log("  %s\n      road %s | barangay %s | city %s | province %s | place %s"
            % (label, road, r["barangay"] or "-", r["city"] or "-", r["province"] or "-", place))
        report["samples"].append(dict(r, label=label))
    report["seconds"] = round(time.time() - started, 1)
    log("")
    log("check took %.0f s" % report["seconds"])
    return report


def main(argv: Optional[Sequence[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Coverage report of an address file.")
    p.add_argument("--addr", type=Path, default=build_address.DEFAULT_OUTPUT)
    p.add_argument("--roads", type=Path, default=build_address.DEFAULT_WORK / "roads.geojsonseq")
    p.add_argument("--places", type=Path, default=build_address.DEFAULT_WORK / "places.geojsonseq")
    p.add_argument("--step-m", type=float, default=400.0, help="road sample spacing (default 400 m)")
    p.add_argument("--limit", type=int, default=0, help="at most this many road points (random subset)")
    p.add_argument("--workers", type=int, default=0, help="processes (default: CPUs minus two)")
    p.add_argument("--json", type=Path, help="also write the numbers as JSON to this file")
    args = p.parse_args(argv)
    for path in (args.addr, args.roads, args.places):
        if not path.is_file():
            print("error: %s is missing" % path, file=sys.stderr)
            return 1
    report = check(args.addr, args.roads, args.places, args.step_m, args.workers, args.limit)
    if args.json:
        args.json.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
