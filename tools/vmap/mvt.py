"""Reads Mapbox vector tiles (MVT, the format inside OpenMapTiles .mbtiles files).

A vector tile holds named layers (water, transportation, place, ...). Each layer holds features:
a geometry (points, lines or areas) in tile units (0 to `extent`, usually 4096, y growing down)
plus properties such as class and name. The encoding is Protocol Buffers; this module decodes the
few message types the format uses, so it needs no extra package.

Specification: https://github.com/mapbox/vector-tile-spec/tree/master/2.1
Python 3.9 compatible, standard library only.
"""
from __future__ import annotations

import gzip
import struct
from typing import Any, Dict, List, Tuple

POINT, LINE, POLYGON = 1, 2, 3


def _varint(data: bytes, pos: int) -> Tuple[int, int]:
    result = shift = 0
    while True:
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not b & 0x80:
            return result, pos
        shift += 7


def _fields(data: bytes):
    """Yields (field number, wire type, value) for one message. Length-delimited values are bytes."""
    pos, end = 0, len(data)
    while pos < end:
        key, pos = _varint(data, pos)
        field, wire = key >> 3, key & 7
        if wire == 0:
            value, pos = _varint(data, pos)
        elif wire == 1:
            value = data[pos:pos + 8]
            pos += 8
        elif wire == 2:
            length, pos = _varint(data, pos)
            value = data[pos:pos + length]
            pos += length
        elif wire == 5:
            value = data[pos:pos + 4]
            pos += 4
        else:
            raise ValueError("unsupported wire type %d" % wire)
        yield field, wire, value


def _packed(data: bytes) -> List[int]:
    out, pos = [], 0
    while pos < len(data):
        v, pos = _varint(data, pos)
        out.append(v)
    return out


def _zigzag(v: int) -> int:
    return (v >> 1) ^ -(v & 1)


def _value(data: bytes) -> Any:
    for field, wire, v in _fields(data):
        if field == 1:
            return v.decode("utf-8", "replace")
        if field == 2:
            return struct.unpack("<f", v)[0]
        if field == 3:
            return struct.unpack("<d", v)[0]
        if field in (4, 5):
            return v if v < 1 << 63 else v - (1 << 64)
        if field == 6:
            return _zigzag(v)
        if field == 7:
            return bool(v)
    return None


def decode_geometry(commands: List[int]) -> List[List[Tuple[int, int]]]:
    """The geometry commands as parts: each part a list of (x, y). For areas every ring is one part
    (outer rings clockwise on screen, holes anticlockwise, as the specification requires); a
    closed ring does not repeat its first point."""
    parts: List[List[Tuple[int, int]]] = []
    x = y = 0
    i = 0
    current: List[Tuple[int, int]] = []
    while i < len(commands):
        cmd, count = commands[i] & 7, commands[i] >> 3
        i += 1
        if cmd == 1:  # MoveTo
            for _ in range(count):
                x += _zigzag(commands[i])
                y += _zigzag(commands[i + 1])
                i += 2
                if current:
                    parts.append(current)
                current = [(x, y)]
        elif cmd == 2:  # LineTo
            for _ in range(count):
                x += _zigzag(commands[i])
                y += _zigzag(commands[i + 1])
                i += 2
                current.append((x, y))
        elif cmd == 7:  # ClosePath
            if current:
                parts.append(current)
            current = []
        else:
            raise ValueError("bad geometry command %d" % cmd)
    if current:
        parts.append(current)
    return parts


def decode_tile(data: bytes) -> Dict[str, Dict[str, Any]]:
    """{layer name: {"extent": n, "features": [{"type", "properties", "parts"}, ...]}}.
    data may be gzip-compressed, as in .mbtiles files."""
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    layers: Dict[str, Dict[str, Any]] = {}
    for field, _, layer_bytes in _fields(data):
        if field != 3:
            continue
        name, extent = "", 4096
        keys: List[str] = []
        values: List[Any] = []
        raw_features: List[bytes] = []
        for f, _, v in _fields(layer_bytes):
            if f == 1:
                name = v.decode("utf-8")
            elif f == 2:
                raw_features.append(v)
            elif f == 3:
                keys.append(v.decode("utf-8"))
            elif f == 4:
                values.append(_value(v))
            elif f == 5:
                extent = v
        features = []
        for raw in raw_features:
            ftype, tags, geometry = 0, [], []
            for f, _, v in _fields(raw):
                if f == 2:
                    tags = _packed(v)
                elif f == 3:
                    ftype = v
                elif f == 4:
                    geometry = _packed(v)
            props = {keys[tags[k]]: values[tags[k + 1]] for k in range(0, len(tags) - 1, 2)}
            features.append({"type": ftype, "properties": props, "parts": decode_geometry(geometry)})
        layers[name] = {"extent": extent, "features": features}
    return layers


# ---------------------------------------------------------------- writing (tests only)

def _put_varint(out: bytearray, v: int) -> None:
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


def _put_field(out: bytearray, field: int, payload: bytes) -> None:
    _put_varint(out, field << 3 | 2)
    _put_varint(out, len(payload))
    out += payload


def _zz(v: int) -> int:
    return (v << 1) ^ (v >> 63)


def encode_tile(layers: Dict[str, List[Dict[str, Any]]], extent: int = 4096) -> bytes:
    """A vector tile from {layer: [{"type", "properties", "parts"}]}, for tests. Rings are closed
    with ClosePath; values are strings, integers or floats."""
    out = bytearray()
    for name, features in layers.items():
        layer = bytearray()
        _put_field(layer, 1, name.encode())
        keys: List[str] = []
        values: List[Any] = []
        for feat in features:
            f = bytearray()
            tags: List[int] = []
            for k, v in feat["properties"].items():
                if k not in keys:
                    keys.append(k)
                typed = [(type(x), x) for x in values]
                if (type(v), v) not in typed:
                    values.append(v)
                    typed.append((type(v), v))
                tags += [keys.index(k), typed.index((type(v), v))]
            packed = bytearray()
            for t in tags:
                _put_varint(packed, t)
            _put_field(f, 2, bytes(packed))
            _put_varint(f, 3 << 3)
            _put_varint(f, feat["type"])
            geom: List[int] = []
            x = y = 0
            for part in feat["parts"]:
                px, py = part[0]
                geom += [1 | 1 << 3, _zz(px - x), _zz(py - y)]
                x, y = px, py
                if len(part) > 1:
                    geom.append(2 | (len(part) - 1) << 3)
                    for px, py in part[1:]:
                        geom += [_zz(px - x), _zz(py - y)]
                        x, y = px, py
                if feat["type"] == POLYGON:
                    geom.append(7 | 1 << 3)
            packed = bytearray()
            for g in geom:
                _put_varint(packed, g)
            _put_field(f, 4, bytes(packed))
            _put_field(layer, 2, bytes(f))
        for k in keys:
            _put_field(layer, 3, k.encode())
        for v in values:
            val = bytearray()
            if isinstance(v, str):
                _put_field(val, 1, v.encode())
            elif isinstance(v, float):
                _put_varint(val, 3 << 3 | 1)
                val += struct.pack("<d", v)
            else:
                _put_varint(val, 6 << 3)
                _put_varint(val, _zz(int(v)))
            _put_field(layer, 4, bytes(val))
        _put_varint(layer, 5 << 3)
        _put_varint(layer, extent)
        _put_varint(layer, 15 << 3)
        _put_varint(layer, 2)
        _put_field(out, 3, bytes(layer))
    return bytes(out)
