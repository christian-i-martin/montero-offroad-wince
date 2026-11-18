"""The Montero vector map format (*.vmap), shared with dashboard/src/vmap.c.

A .vmap file holds map shapes (areas, lines, points with names) for one rectangle of the world,
plus the drawing rules (layers) compiled from our two map styles, so the head unit draws the map
itself at any zoom and any rotation instead of showing ready-made pictures.

All numbers are little-endian. Layout:

    header (64 bytes)
      0  "MTVM"
      4  u16 version (1)
      6  u16 header size (64)
      8  u32 offset of the layer table     12 u32 layer count
     16  u32 offset of the level table     20 u32 level count
     24  u32 land colour, dark style       28 u32 land colour, light style (0x00RRGGBB)
     32  i32 west, south, east, north in millionths of a degree
     48  u16 zoom table start (zoom x 8)   50 u16 zoom table length (entries, one per 1/8 zoom)
     52  12 bytes reserved (0)

    layer table: one record per layer, in drawing order
      u8 kind (1 area, 2 line, 3 dot, 4 label at a point, 5 label along a line, 6 place marker)
      u8 flags (1 round line ends, 2 upper-case text)
      u8 dash count (0 to 4)        u8 anchor (0 centre, 1 left, 2 top)
      u16 first zoom x 8            u16 end zoom x 8 (drawn while first <= zoom < end)
      u32 colour dark               u32 colour light
      u32 second colour dark        u32 second colour light (label halo, dot outline)
      u16 opacity (0 to 256)        i16 offset x, i16 offset y (labels, in text heights x 64)
      u16 dashes[4] (line widths x 64)
      u16 spacing (labels along lines, text heights x 16)
      u16 max angle (degrees, labels along lines)
      u16 max width (point labels, text heights x 16)
      u16 pictogram (place markers: the number in dashboard/src/vicons.h)
      u16 size[zoom table length]   (pixels x 64: line width, dot radius, text height; for place
                                     markers the height of the name beside them, 0: no name)
      u16 size2[zoom table length]  (pixels x 64: dot outline, halo width)

    level table: one record per data level (data zoom; each covers a grid of tiles)
      u8 data zoom, u8 reserved, u16 columns, u16 rows, u16 reserved
      u32 first tile x, u32 first tile y (OpenStreetMap tile numbers at the data zoom)
      u16 first view zoom x 8, u16 end view zoom x 8 (the view zooms that draw this level)
      u32 offset of the tile index: columns x rows entries of
          u32 offset, u32 compressed length (0: no tile), u32 length after inflating

    tile (zlib-compressed); every section starts 4-byte aligned
      u16 feature count F, u16 draw list count D, u32 point count, u32 part count, u32 text bytes,
      u32 index count
      features[F]: u8 type (1 point, 2 line, 3 area), u8 reserved, u16 part count,
                   u32 first part, u32 first point, u32 text offset (0xFFFFFFFF: none),
                   u16 rank, u16 reserved, i16 min x, min y, max x, max y
      parts[]: u32 points in the part
      draw lists[D]: u16 layer, u16 feature count, u32 first index
      points[]: i16 x, i16 y in tile units (0 to 4096, y down; shapes reach a little past the edges)
      indexes[]: u16 feature numbers, grouped by draw list
      text: UTF-8 names, each ended by a zero byte

Python 3.9 compatible, standard library only.
"""
from __future__ import annotations

import struct
import zlib
from typing import Dict, List, Optional, Sequence, Tuple

MAGIC = b"MTVM"
VERSION = 1
HEADER_SIZE = 64
EXTENT = 4096
NO_TEXT = 0xFFFFFFFF

AREA, LINE, DOT, LABEL_POINT, LABEL_LINE, MARKER = 1, 2, 3, 4, 5, 6
FLAG_ROUND, FLAG_UPPER = 1, 2
ANCHOR_CENTRE, ANCHOR_LEFT, ANCHOR_TOP = 0, 1, 2
FEATURE_POINT, FEATURE_LINE, FEATURE_AREA = 1, 2, 3

ZOOM_STEP = 8  # zoom table: one entry per 1/8 zoom level
LAYER_FIXED = struct.Struct("<BBBBHHIIIIHhh4HHHHH")
LEVEL = struct.Struct("<BBHHHIIHHI")
INDEX_ENTRY = struct.Struct("<III")
TILE_HEAD = struct.Struct("<HHIIII")
FEATURE = struct.Struct("<BBHIIIHHhhhh")
DRAW = struct.Struct("<HHI")


class Layer:
    """One drawing rule. size and size2 hold one value per zoom table entry (pixels)."""

    def __init__(self, name: str, kind: int, first_zoom: float, end_zoom: float,
                 colour: Tuple[int, int], colour2: Tuple[int, int] = (0, 0), opacity: float = 1.0,
                 flags: int = 0, dashes: Sequence[float] = (), anchor: int = ANCHOR_CENTRE,
                 offset: Tuple[float, float] = (0.0, 0.0), spacing: float = 0.0,
                 max_angle: float = 0.0, max_width: float = 0.0,
                 size: Sequence[float] = (), size2: Sequence[float] = (), icon: int = 0):
        self.name, self.kind = name, kind
        self.first_zoom, self.end_zoom = first_zoom, end_zoom
        self.colour, self.colour2, self.opacity = colour, colour2, opacity
        self.flags, self.dashes, self.anchor, self.offset = flags, list(dashes), anchor, offset
        self.spacing, self.max_angle, self.max_width = spacing, max_angle, max_width
        self.size, self.size2 = list(size), list(size2)
        self.icon = icon


class Feature:
    def __init__(self, ftype: int, parts: List[List[Tuple[int, int]]], text: Optional[str] = None,
                 rank: int = 0):
        self.type, self.parts, self.text, self.rank = ftype, parts, text, rank


def _zoom8(z: float) -> int:
    return max(0, min(0xFFFF, int(round(z * ZOOM_STEP))))


def _u16(v: float, scale: float) -> int:
    return max(0, min(0xFFFF, int(round(v * scale))))


def _i16(v: float, scale: float) -> int:
    return max(-0x8000, min(0x7FFF, int(round(v * scale))))


def _clamp16(v: int) -> int:
    return max(-0x8000, min(0x7FFF, v))


def encode_tile(features: Sequence[Feature], draws: Dict[int, List[int]]) -> bytes:
    """The uncompressed bytes of one tile. draws: {layer number: [feature numbers]}."""
    if len(features) > 0xFFFF:
        raise ValueError("too many features in one tile")
    text = bytearray()
    text_offsets: Dict[str, int] = {}
    parts: List[int] = []
    points = bytearray()
    feature_bytes = bytearray()
    npoints = 0
    for f in features:
        t = NO_TEXT
        if f.text:
            if f.text not in text_offsets:
                text_offsets[f.text] = len(text)
                text += f.text.encode("utf-8") + b"\0"
            t = text_offsets[f.text]
        xs = [_clamp16(x) for part in f.parts for x, _ in part]
        ys = [_clamp16(y) for part in f.parts for _, y in part]
        feature_bytes += FEATURE.pack(f.type, 0, len(f.parts), len(parts), npoints, t, min(f.rank, 0xFFFF), 0,
                                      min(xs), min(ys), max(xs), max(ys))
        for part in f.parts:
            parts.append(len(part))
            for x, y in part:
                points += struct.pack("<hh", _clamp16(x), _clamp16(y))
            npoints += len(part)
    draw_bytes = bytearray()
    index_bytes = bytearray()
    count = 0
    for layer in sorted(draws):
        members = draws[layer]
        draw_bytes += DRAW.pack(layer, len(members), count)
        for m in members:
            index_bytes += struct.pack("<H", m)
        count += len(members)
    out = bytearray(TILE_HEAD.pack(len(features), len(draws), npoints, len(parts), len(text), count))
    out += feature_bytes
    out += struct.pack("<%dI" % len(parts), *parts)
    out += draw_bytes
    out += points
    out += index_bytes
    while len(out) % 4:
        out.append(0)
    out += text
    return bytes(out)


def decode_tile(data: bytes) -> Tuple[List[Feature], Dict[int, List[int]]]:
    """The reverse of encode_tile (points come back as stored, clamped to 16 bits)."""
    nf, nd, npoints, nparts, ntext, nindex = TILE_HEAD.unpack_from(data, 0)
    pos = TILE_HEAD.size
    raw = []
    for _ in range(nf):
        raw.append(FEATURE.unpack_from(data, pos))
        pos += FEATURE.size
    parts = list(struct.unpack_from("<%dI" % nparts, data, pos))
    pos += 4 * nparts
    draws_raw = []
    for _ in range(nd):
        draws_raw.append(DRAW.unpack_from(data, pos))
        pos += DRAW.size
    pts = struct.unpack_from("<%dh" % (2 * npoints), data, pos)
    pos += 4 * npoints
    index = struct.unpack_from("<%dH" % nindex, data, pos)
    pos += 2 * nindex
    pos += (-pos) % 4
    text = data[pos:pos + ntext]
    features = []
    for ftype, _, npart, first_part, first_point, toff, rank, _, *_box in raw:
        out_parts = []
        p = first_point
        for k in range(npart):
            n = parts[first_part + k]
            out_parts.append([(pts[2 * i], pts[2 * i + 1]) for i in range(p, p + n)])
            p += n
        name = None
        if toff != NO_TEXT:
            name = text[toff:text.index(b"\0", toff)].decode("utf-8")
        features.append(Feature(ftype, out_parts, name, rank))
    draws = {layer: list(index[first:first + n]) for layer, n, first in draws_raw}
    return features, draws


class Level:
    def __init__(self, zoom: int, x0: int, y0: int, cols: int, rows: int, view_first: float, view_end: float):
        self.zoom, self.x0, self.y0, self.cols, self.rows = zoom, x0, y0, cols, rows
        self.view_first, self.view_end = view_first, view_end
        self.tiles: Dict[Tuple[int, int], bytes] = {}  # (x, y) -> uncompressed tile bytes


def write(path, layers: Sequence[Layer], levels: Sequence[Level], land: Tuple[int, int],
          bounds: Tuple[float, float, float, float], zoom_start: float, zoom_count: int) -> int:
    """Writes a .vmap file; returns its size in bytes."""
    for layer in layers:
        if len(layer.size) != zoom_count or (layer.size2 and len(layer.size2) != zoom_count):
            raise ValueError("layer %s: size tables must have %d entries" % (layer.name, zoom_count))
    layer_bytes = bytearray()
    for layer in layers:
        dashes = (list(layer.dashes) + [0, 0, 0, 0])[:4]
        layer_bytes += LAYER_FIXED.pack(
            layer.kind, layer.flags, len(layer.dashes), layer.anchor,
            _zoom8(layer.first_zoom), _zoom8(layer.end_zoom),
            layer.colour[0], layer.colour[1], layer.colour2[0], layer.colour2[1],
            _u16(layer.opacity, 256), _i16(layer.offset[0], 64), _i16(layer.offset[1], 64),
            *[_u16(d, 64) for d in dashes],
            _u16(layer.spacing, 16), _u16(layer.max_angle, 1), _u16(layer.max_width, 16), layer.icon)
        size2 = layer.size2 or [0.0] * zoom_count
        layer_bytes += struct.pack("<%dH" % zoom_count, *[_u16(v, 64) for v in layer.size])
        layer_bytes += struct.pack("<%dH" % zoom_count, *[_u16(v, 64) for v in size2])
        while len(layer_bytes) % 4:
            layer_bytes.append(0)

    layer_offset = HEADER_SIZE
    level_offset = layer_offset + len(layer_bytes)
    pos = level_offset + LEVEL.size * len(levels)
    index_offsets = []
    for lv in levels:
        index_offsets.append(pos)
        pos += INDEX_ENTRY.size * lv.cols * lv.rows
    data_start = pos

    tile_blobs = bytearray()
    index_bytes = bytearray()
    for lv in levels:
        for row in range(lv.rows):
            for col in range(lv.cols):
                raw = lv.tiles.get((lv.x0 + col, lv.y0 + row))
                if raw is None:
                    index_bytes += INDEX_ENTRY.pack(0, 0, 0)
                    continue
                packed = zlib.compress(raw, 9)
                index_bytes += INDEX_ENTRY.pack(data_start + len(tile_blobs), len(packed), len(raw))
                tile_blobs += packed
                while len(tile_blobs) % 4:
                    tile_blobs.append(0)

    west, south, east, north = bounds
    header = bytearray(MAGIC)
    header += struct.pack("<HHIIIIII", VERSION, HEADER_SIZE, layer_offset, len(layers), level_offset, len(levels),
                          land[0], land[1])
    header += struct.pack("<iiii", *[int(round(v * 1e6)) for v in (west, south, east, north)])
    header += struct.pack("<HH", _zoom8(zoom_start), zoom_count)
    header += bytes(HEADER_SIZE - len(header))
    level_bytes = bytearray()
    for lv, ioff in zip(levels, index_offsets):
        level_bytes += LEVEL.pack(lv.zoom, 0, lv.cols, lv.rows, 0, lv.x0, lv.y0, _zoom8(lv.view_first),
                                  _zoom8(lv.view_end), ioff)
    blob = bytes(header) + bytes(layer_bytes) + bytes(level_bytes) + bytes(index_bytes) + bytes(tile_blobs)
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


class Reader:
    """Reads a .vmap file back (tests and checks)."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != MAGIC:
            raise ValueError("not a vector map file")
        (self.version, _hs, lay_off, lay_n, lev_off, lev_n, land_dark, land_light) = struct.unpack_from(
            "<HHIIIIII", d, 4)
        self.land = (land_dark, land_light)
        self.bounds = tuple(v / 1e6 for v in struct.unpack_from("<iiii", d, 32))
        z0, self.zoom_count = struct.unpack_from("<HH", d, 48)
        self.zoom_start = z0 / ZOOM_STEP
        self.layers: List[dict] = []
        pos = lay_off
        for _ in range(lay_n):
            v = LAYER_FIXED.unpack_from(d, pos)
            pos += LAYER_FIXED.size
            size = struct.unpack_from("<%dH" % self.zoom_count, d, pos)
            pos += 2 * self.zoom_count
            size2 = struct.unpack_from("<%dH" % self.zoom_count, d, pos)
            pos += 2 * self.zoom_count
            pos += (-pos) % 4
            self.layers.append({"kind": v[0], "flags": v[1], "dashes": [x / 64 for x in v[13:13 + v[2]]],
                                "first_zoom": v[4] / ZOOM_STEP, "end_zoom": v[5] / ZOOM_STEP,
                                "colour": (v[6], v[7]), "colour2": (v[8], v[9]), "opacity": v[10] / 256,
                                "size": [s / 64 for s in size], "size2": [s / 64 for s in size2], "icon": v[20]})
        self.levels: List[dict] = []
        for k in range(lev_n):
            zoom, _, cols, rows, _, x0, y0, vf, ve, ioff = LEVEL.unpack_from(d, lev_off + k * LEVEL.size)
            self.levels.append({"zoom": zoom, "cols": cols, "rows": rows, "x0": x0, "y0": y0,
                                "view_first": vf / ZOOM_STEP, "view_end": ve / ZOOM_STEP, "index": ioff})

    def tile(self, level: int, x: int, y: int) -> Optional[bytes]:
        lv = self.levels[level]
        col, row = x - lv["x0"], y - lv["y0"]
        if not (0 <= col < lv["cols"] and 0 <= row < lv["rows"]):
            return None
        off, length, raw = INDEX_ENTRY.unpack_from(self.data, lv["index"] + INDEX_ENTRY.size * (row * lv["cols"] + col))
        if length == 0:
            return None
        out = zlib.decompress(self.data[off:off + length])
        if len(out) != raw:
            raise ValueError("tile length mismatch")
        return out
