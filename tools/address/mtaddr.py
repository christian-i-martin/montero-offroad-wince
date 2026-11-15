"""Montero address file (.addr): offline "where am I" lookup for the dashboard.

Holds area outlines (province, city or municipality, barangay), named roads and named places,
with a grid index, so the head unit can turn a GPS position into "road, barangay, city,
province" without a network. The C reader is dashboard/src/addr.c; lookup() below is the
reference for what it must answer. Python 3.9, standard library only.

Coordinates are whole micro-degrees (degrees x 1,000,000), longitude first. All numbers are
little-endian. Layout:

    offset  field (u32 unless noted)
    0       magic, the 4 bytes "MTAD"
    4       version, 1
    8       lon0, i32: west edge of the grid
    12      lat0, i32: south edge of the grid
    16      cell size in micro-degrees
    20      cols
    24      rows
    28      cells_offset: cols * rows + 1 item indexes; the items of cell (col, row) are
            items[start[row * cols + col]] up to items[start[row * cols + col + 1]]
    32      items_offset: item records, 32 bytes each (below)
    36      item_count
    40      coords_offset: the coordinate area
    44      names_offset: names, UTF-8, each ending with a zero byte
    48      names_size
    52      road_radius_m: roads are listed in every cell within this distance
    56      place_radius_m: places are listed in every cell within this distance
    60      zero

Item record (a copy per cell the shape touches, so one read gets a whole cell):

    0   u8   kind: 1 province, 2 city or municipality, 3 barangay, 4 road, 5 place
    1   u8   rank: for places 1 city, 2 town, 3 suburb, 4 village, 5 neighbourhood, 6 hamlet;
             for roads 1 motorway or trunk, 2 primary, 3 secondary, 4 tertiary, 5 other
    2   u16  ring count (areas), 0 otherwise
    4   u32  name: offset into the names
    8   i32  bounding box: min lon, min lat, max lon, max lat (16 bytes)
    24  u32  coords: byte offset into the coordinate area
    28  u32  point count (all rings together)

Coordinate area: an area starts with one u32 point count per ring, then its points; a road
(at most MAX_ROAD_POINTS points, longer roads are split) or a place is just its points. A point
is two i32: lon, lat. Rings are closed (last point = first point).

Lookup: an area contains the point when the point is inside an odd number of its rings (so
holes and several outer rings work); when several areas of one kind contain it, the one with
the smallest bounding box wins. The road is the nearest within ROAD_MATCH_M, the place the
nearest within the file's place radius. Distances are metres on a flat projection around the
point (fine at these distances).
"""
from __future__ import annotations

import math
import struct
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

MAGIC = b"MTAD"
VERSION = 1
HEADER = struct.Struct("<4sIiiIIIIIIIIIII4x")
ITEM = struct.Struct("<BBHIiiiiII")
POINT = struct.Struct("<ii")
KIND_PROVINCE, KIND_CITY, KIND_BARANGAY, KIND_ROAD, KIND_PLACE = 1, 2, 3, 4, 5
AREA_KINDS = (KIND_PROVINCE, KIND_CITY, KIND_BARANGAY)
MAX_ROAD_POINTS = 32
ROAD_MATCH_M = 50.0
M_PER_DEG_LAT = 110574.0
M_PER_DEG_LON_EQUATOR = 111320.0
MAX_FILE_BYTES = 2_000_000_000

Point = Tuple[float, float]  # lon, lat in degrees


def e6(v: float) -> int:
    return int(math.floor(v * 1e6 + 0.5))


class AddrWriter:
    """Collects shapes, then writes the file on close()."""

    def __init__(self, path: Path, bounds: Sequence[float], cell_deg: float = 0.01,
                 road_radius_m: int = 75, place_radius_m: int = 1500, exact_cells: bool = False) -> None:
        """exact_cells: list each shape only in the cells it really reaches (an outline's edges
        and inside, a road's segments plus the radius, a place's circle) instead of every cell of
        its bounding box plus the radius. Same format and same lookup answers, fewer items."""
        minlon, minlat, maxlon, maxlat = bounds
        if not (minlon < maxlon and minlat < maxlat):
            raise ValueError("bounds must be minlon,minlat,maxlon,maxlat")
        if road_radius_m < ROAD_MATCH_M:
            raise ValueError("the road radius must be at least ROAD_MATCH_M")
        self.exact_cells = bool(exact_cells)
        self.path = Path(path)
        self.cell = e6(cell_deg)
        self.lon0, self.lat0 = e6(minlon), e6(minlat)
        self.cols = (e6(maxlon) - self.lon0 + self.cell - 1) // self.cell
        self.rows = (e6(maxlat) - self.lat0 + self.cell - 1) // self.cell
        self.road_radius_m = int(road_radius_m)
        self.place_radius_m = int(place_radius_m)
        self._names: Dict[str, int] = {}
        self._name_blob = bytearray()
        self._coords = bytearray()
        self._shapes: List[tuple] = []  # (kind, rank, rings, name_offset, bbox, coord_offset, points, pad_m)
        self.counts = {k: 0 for k in (KIND_PROVINCE, KIND_CITY, KIND_BARANGAY, KIND_ROAD, KIND_PLACE)}

    def _name(self, name: str) -> int:
        found = self._names.get(name)
        if found is None:
            found = len(self._name_blob)
            self._name_blob += name.encode("utf-8") + b"\0"
            self._names[name] = found
        return found

    def _add(self, kind: int, rank: int, name: str, rings: List[List[Tuple[int, int]]],
             is_area: bool, pad_m: float) -> None:
        pts = [p for r in rings for p in r]
        bbox = (min(p[0] for p in pts), min(p[1] for p in pts), max(p[0] for p in pts), max(p[1] for p in pts))
        offset = len(self._coords)
        if is_area:
            self._coords += b"".join(struct.pack("<I", len(r)) for r in rings)
        self._coords += b"".join(POINT.pack(x, y) for x, y in pts)
        self._shapes.append((kind, rank, len(rings) if is_area else 0, self._name(name), bbox, offset,
                             len(pts), pad_m))
        self.counts[kind] += 1

    def add_area(self, kind: int, name: str, rings: Iterable[Sequence[Point]]) -> bool:
        """An outline (rings in degrees, any orientation, holes as extra rings). False when
        nothing usable was given."""
        if kind not in AREA_KINDS or not name:
            raise ValueError("an area needs a kind 1 to 3 and a name")
        clean = []
        for ring in rings:
            r = [(e6(lon), e6(lat)) for lon, lat in ring]
            r = [p for i, p in enumerate(r) if i == 0 or p != r[i - 1]]
            if r and r[0] != r[-1]:
                r.append(r[0])
            if len(r) >= 4:
                clean.append(r)
        if not clean:
            return False
        self._add(kind, 0, name, clean, True, 0.0)
        return True

    def add_road(self, name: str, points: Sequence[Point], rank: int = 5) -> int:
        """A named road (a line). Long roads are split; returns the number of pieces."""
        if not name:
            raise ValueError("a road needs a name")
        pts = [(e6(lon), e6(lat)) for lon, lat in points]
        pts = [p for i, p in enumerate(pts) if i == 0 or p != pts[i - 1]]
        if len(pts) < 2:
            return 0
        pieces = 0
        start = 0
        while start < len(pts) - 1:
            chunk = pts[start:start + MAX_ROAD_POINTS]
            self._add(KIND_ROAD, rank, name, [chunk], False, float(self.road_radius_m))
            pieces += 1
            start += MAX_ROAD_POINTS - 1  # pieces share their end point
        return pieces

    def add_place(self, name: str, lon: float, lat: float, rank: int) -> None:
        if not name:
            raise ValueError("a place needs a name")
        self._add(KIND_PLACE, rank, name, [[(e6(lon), e6(lat))]], False, float(self.place_radius_m))

    def _cells(self, bbox: Tuple[int, int, int, int], pad_m: float) -> Iterable[int]:
        minx, miny, maxx, maxy = bbox
        if pad_m > 0:
            lat = max(abs(miny), abs(maxy)) / 1e6
            dy = pad_m / M_PER_DEG_LAT * 1e6
            dx = pad_m / (M_PER_DEG_LON_EQUATOR * max(math.cos(math.radians(lat)), 0.01)) * 1e6
            minx, maxx, miny, maxy = minx - dx, maxx + dx, miny - dy, maxy + dy
        c0 = max(0, int((minx - self.lon0) // self.cell))
        c1 = min(self.cols - 1, int((maxx - self.lon0) // self.cell))
        r0 = max(0, int((miny - self.lat0) // self.cell))
        r1 = min(self.rows - 1, int((maxy - self.lat0) // self.cell))
        for row in range(r0, r1 + 1):
            for col in range(c0, c1 + 1):
                yield row * self.cols + col

    # Exact cell lists (exact_cells=True). Each helper may list a few cells too many (it works
    # with padded rectangles), never one too few: a lookup in a cell that does not list a shape
    # could not have matched it.

    def _shape_points(self, s: tuple) -> List[List[Tuple[int, int]]]:
        kind, _, nrings, _, _, coord, npts, _ = s
        sizes = list(struct.unpack_from("<%dI" % nrings, self._coords, coord)) if nrings else [npts]
        at = coord + 4 * nrings
        out = []
        for n in sizes:
            flat = struct.unpack_from("<%di" % (2 * n), self._coords, at)
            out.append(list(zip(flat[0::2], flat[1::2])))
            at += 8 * n
        return out

    def _pad(self, bbox: Tuple[int, int, int, int], pad_m: float) -> Tuple[float, float]:
        """The radius in micro-degrees of longitude and latitude (longitude at the shape's
        highest latitude, where a degree is shortest, so the padding is never too small)."""
        lat = max(abs(bbox[1]), abs(bbox[3])) / 1e6
        return (pad_m / (M_PER_DEG_LON_EQUATOR * max(math.cos(math.radians(lat)), 0.01)) * 1e6,
                pad_m / M_PER_DEG_LAT * 1e6)

    def _segment_cells(self, ax: float, ay: float, bx: float, by: float, padx: float, pady: float,
                       out: set) -> None:
        """Adds every cell whose rectangle, grown by padx and pady, meets the segment."""
        cell = self.cell
        r0 = max(0, int((min(ay, by) - pady - self.lat0) // cell))
        r1 = min(self.rows - 1, int((max(ay, by) + pady - self.lat0) // cell))
        for row in range(r0, r1 + 1):
            y0 = self.lat0 + row * cell - pady
            y1 = self.lat0 + (row + 1) * cell + pady
            if ay == by:
                t0, t1 = 0.0, 1.0
            else:
                ta, tb = (y0 - ay) / (by - ay), (y1 - ay) / (by - ay)
                t0, t1 = max(0.0, min(ta, tb)), min(1.0, max(ta, tb))
                if t0 > t1:
                    continue
            xa, xb = ax + t0 * (bx - ax), ax + t1 * (bx - ax)
            c0 = max(0, int((min(xa, xb) - padx - self.lon0) // cell))
            c1 = min(self.cols - 1, int((max(xa, xb) + padx - self.lon0) // cell))
            base = row * self.cols
            out.update(range(base + c0, base + c1 + 1))

    def _area_cells(self, rings: List[List[Tuple[int, int]]]) -> set:
        """Cells an outline's edges pass through, plus the cells whose centre is inside it (by
        the even-odd rule of inside()). A cell no edge passes through is wholly inside or wholly
        outside, so its centre decides."""
        out: set = set()
        margin = 2.0  # micro-degrees, against rounding at cell borders
        for ring in rings:
            for i in range(len(ring) - 1):
                (ax, ay), (bx, by) = ring[i], ring[i + 1]
                self._segment_cells(ax, ay, bx, by, margin, margin, out)
        ys = [p[1] for ring in rings for p in ring]
        r0 = max(0, int((min(ys) - self.lat0) // self.cell))
        r1 = min(self.rows - 1, int((max(ys) - self.lat0) // self.cell))
        crossings: Dict[int, List[float]] = {}
        half = self.cell / 2.0
        for ring in rings:
            for i in range(len(ring) - 1):
                (x1, y1), (x2, y2) = ring[i], ring[i + 1]
                if y1 == y2:
                    continue
                lo, hi = min(y1, y2), max(y1, y2)
                # rows whose centre line y = lat0 + row * cell + half lies in [lo, hi]
                ra = max(r0, int(math.ceil((lo - half - self.lat0) / self.cell)))
                rb = min(r1, int(math.floor((hi - half - self.lat0) / self.cell)))
                for row in range(ra, rb + 1):
                    yc = self.lat0 + row * self.cell + half
                    if (y1 > yc) != (y2 > yc):
                        crossings.setdefault(row, []).append(x1 + (yc - y1) * (x2 - x1) / (y2 - y1))
        for row, xs in crossings.items():
            xs.sort()
            base = row * self.cols
            for j in range(0, len(xs) - 1, 2):
                # centres x with xs[j] <= x < xs[j + 1] have an odd number of crossings to their right
                c0 = max(0, int(math.ceil((xs[j] - self.lon0) / self.cell - 0.5)))
                c1 = min(self.cols - 1, int(math.ceil((xs[j + 1] - self.lon0) / self.cell - 0.5)) - 1)
                if c0 <= c1:
                    out.update(range(base + c0, base + c1 + 1))
        return out

    def _place_cells(self, x: int, y: int, pad_m: float) -> set:
        """Cells with a point within pad_m of the place (distance as in lookup(), using the
        cell's highest latitude so it is never overestimated)."""
        out = set()
        for cell in self._cells((x, y, x, y), pad_m):
            row, col = divmod(cell, self.cols)
            cx0, cy0 = self.lon0 + col * self.cell, self.lat0 + row * self.cell
            cx1, cy1 = cx0 + self.cell, cy0 + self.cell
            lat = max(abs(cy0), abs(cy1)) / 1e6
            kx = M_PER_DEG_LON_EQUATOR * max(math.cos(math.radians(lat)), 0.01) / 1e6
            ky = M_PER_DEG_LAT / 1e6
            dx = max(cx0 - x, 0, x - cx1) * kx
            dy = max(cy0 - y, 0, y - cy1) * ky
            if math.hypot(dx, dy) <= pad_m + 1.0:
                out.add(cell)
        return out

    def _exact(self, s: tuple) -> Iterable[int]:
        kind, bbox, pad_m = s[0], s[4], s[7]
        rings = self._shape_points(s)
        if kind in AREA_KINDS:
            return self._area_cells(rings)
        if kind == KIND_PLACE:
            return self._place_cells(rings[0][0][0], rings[0][0][1], pad_m)
        out: set = set()
        padx, pady = self._pad(bbox, pad_m)
        pts = rings[0]
        for i in range(len(pts) - 1):
            self._segment_cells(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], padx, pady, out)
        return out

    def close(self) -> dict:
        per_cell: List[List[int]] = [[] for _ in range(self.cols * self.rows)]
        for i, s in enumerate(self._shapes):
            for cell in (sorted(self._exact(s)) if self.exact_cells else self._cells(s[4], s[7])):
                per_cell[cell].append(i)
        largest = max(range(len(per_cell)), key=lambda c: len(per_cell[c]))
        starts = [0]
        for lst in per_cell:
            starts.append(starts[-1] + len(lst))
        item_count = starts[-1]
        cells_offset = HEADER.size
        items_offset = cells_offset + 4 * len(starts)
        coords_offset = items_offset + ITEM.size * item_count
        names_offset = coords_offset + len(self._coords)
        total = names_offset + len(self._name_blob)
        if total > MAX_FILE_BYTES:
            raise ValueError("address file would be %d bytes; use a smaller area" % total)
        records = []
        for s in self._shapes:
            kind, rank, rings, name, bbox, coord, npts, _ = s
            records.append(ITEM.pack(kind, rank, rings, name, bbox[0], bbox[1], bbox[2], bbox[3], coord, npts))
        with open(self.path, "wb") as f:
            f.write(HEADER.pack(MAGIC, VERSION, self.lon0, self.lat0, self.cell, self.cols, self.rows,
                                cells_offset, items_offset, item_count, coords_offset, names_offset,
                                len(self._name_blob), self.road_radius_m, self.place_radius_m))
            f.write(struct.pack("<%dI" % len(starts), *starts))
            for lst in per_cell:
                f.write(b"".join(records[i] for i in lst))
            f.write(self._coords)
            f.write(self._name_blob)
        return {"bytes": total, "items": item_count, "shapes": len(self._shapes),
                "cells": self.cols * self.rows, "counts": dict(self.counts),
                "largest_cell": (len(per_cell[largest]), largest % self.cols, largest // self.cols)}


class AddrReader:
    """Reads a .addr file and answers lookups (the reference for the C reader)."""

    def __init__(self, path: Path) -> None:
        self.data = Path(path).read_bytes()
        (magic, version, self.lon0, self.lat0, self.cell, self.cols, self.rows, self.cells_offset,
         self.items_offset, self.item_count, self.coords_offset, self.names_offset, self.names_size,
         self.road_radius_m, self.place_radius_m) = HEADER.unpack_from(self.data)
        if magic != MAGIC or version != VERSION:
            raise ValueError("not a version %d Montero address file" % VERSION)

    def name(self, offset: int) -> str:
        start = self.names_offset + offset
        return self.data[start:self.data.index(b"\0", start)].decode("utf-8")

    def items(self, col: int, row: int) -> List[tuple]:
        if not (0 <= col < self.cols and 0 <= row < self.rows):
            return []
        a, b = struct.unpack_from("<II", self.data, self.cells_offset + 4 * (row * self.cols + col))
        return [ITEM.unpack_from(self.data, self.items_offset + ITEM.size * i) for i in range(a, b)]

    def rings(self, item: tuple) -> List[List[Tuple[int, int]]]:
        kind, _, nrings, _, _, _, _, _, coord, npts = item
        at = self.coords_offset + coord
        sizes = [npts]
        if nrings:
            sizes = list(struct.unpack_from("<%dI" % nrings, self.data, at))
            at += 4 * nrings
        out = []
        for n in sizes:
            out.append([POINT.unpack_from(self.data, at + 8 * k) for k in range(n)])
            at += 8 * n
        return out

    def lookup(self, lon: float, lat: float, road_match_m: float = ROAD_MATCH_M) -> dict:
        x, y = e6(lon), e6(lat)
        result = {"province": None, "city": None, "barangay": None, "road": None, "road_m": None,
                  "place": None, "place_m": None}
        if x < self.lon0 or y < self.lat0:
            return result
        col, row = (x - self.lon0) // self.cell, (y - self.lat0) // self.cell
        kx = M_PER_DEG_LON_EQUATOR * math.cos(math.radians(y / 1e6)) / 1e6
        ky = M_PER_DEG_LAT / 1e6
        best_area = {}
        best_road = (road_match_m, None)
        best_place = (float(self.place_radius_m), None)
        for item in self.items(col, row):
            kind, _, _, name, bx0, by0, bx1, by1, _, _ = item
            if kind in AREA_KINDS:
                if not (bx0 <= x <= bx1 and by0 <= y <= by1):
                    continue
                if inside(self.rings(item), x, y):
                    size = (bx1 - bx0) * (by1 - by0)
                    if kind not in best_area or size < best_area[kind][0]:
                        best_area[kind] = (size, name)
            elif kind == KIND_ROAD:
                d = polyline_distance(self.rings(item)[0], x, y, kx, ky)
                if d < best_road[0]:
                    best_road = (d, name)
            elif kind == KIND_PLACE:
                px, py = self.rings(item)[0][0]
                d = math.hypot((px - x) * kx, (py - y) * ky)
                if d < best_place[0]:
                    best_place = (d, name)
        for kind, key in ((KIND_PROVINCE, "province"), (KIND_CITY, "city"), (KIND_BARANGAY, "barangay")):
            if kind in best_area:
                result[key] = self.name(best_area[kind][1])
        if best_road[1] is not None:
            result["road"], result["road_m"] = self.name(best_road[1]), best_road[0]
        if best_place[1] is not None:
            result["place"], result["place_m"] = self.name(best_place[1]), best_place[0]
        return result


def inside(rings: List[List[Tuple[int, int]]], x: int, y: int) -> bool:
    """Even-odd rule over all rings."""
    odd = False
    for ring in rings:
        n = len(ring)
        for i in range(n - 1):
            x1, y1 = ring[i]
            x2, y2 = ring[i + 1]
            if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
                odd = not odd
    return odd


def polyline_distance(points: List[Tuple[int, int]], x: int, y: int, kx: float, ky: float) -> float:
    best = float("inf")
    for i in range(len(points) - 1):
        ax, ay = (points[i][0] - x) * kx, (points[i][1] - y) * ky
        bx, by = (points[i + 1][0] - x) * kx, (points[i + 1][1] - y) * ky
        dx, dy = bx - ax, by - ay
        length2 = dx * dx + dy * dy
        t = 0.0 if length2 == 0 else max(0.0, min(1.0, -(ax * dx + ay * dy) / length2))
        best = min(best, math.hypot(ax + t * dx, ay + t * dy))
    return best
