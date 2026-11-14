"""Montero tile pack (.pack): the offline map file format the dashboard reads.

One pack holds the map pictures (tiles) of one zoom level for a rectangle of tiles. Tiles are
256 x 256 PNG images numbered like OpenStreetMap's ("slippy map" scheme, Web Mercator): at zoom z
the world is 2^z by 2^z tiles, x grows east from 180 W, y grows south from about 85 N.

Layout, all numbers little-endian 32-bit unsigned:

    offset  field
    0       magic, the 4 bytes "MTPK"
    4       version, 1
    8       zoom
    12      x0, first tile column in the pack
    16      y0, first tile row in the pack
    20      cols
    24      rows
    28      tile_size, 256
    32      format, 1 = PNG
    36      index_offset, 64
    40      background, 0x00RRGGBB: drawn for entries without a tile
    44..63  zero
    64      index: cols * rows entries in row order (entry = (y - y0) * cols + (x - x0)),
            each "offset, length" of the tile's PNG bytes from the start of the file;
            length 0 means no tile (the dashboard draws the background colour)
    ...     tile data; identical tiles are stored once and shared by several entries

PNG tiles may be 8-bit RGB, RGBA (alpha is ignored) or palette images. A pack stays below
MAX_PACK_BYTES so that it fits the SD card's FAT32 file system and 31-bit file offsets on
Windows CE. A zoom level that would be larger is split into several packs (column bands).
The dashboard reads every *.pack file in the SD card's Map folder.

The C reader is dashboard/src/map.c. Python 3.9 compatible, standard library only.
"""
from __future__ import annotations

import hashlib
import struct
import zlib
from pathlib import Path
from typing import BinaryIO, Dict, Optional

MAGIC = b"MTPK"
VERSION = 1
HEADER_SIZE = 64
TILE_SIZE = 256
FORMAT_PNG = 1
MAX_PACK_BYTES = 2_000_000_000
_HEADER = struct.Struct("<4s10I")
_ENTRY = struct.Struct("<II")


class PackFull(Exception):
    """Adding the tile would make the pack larger than MAX_PACK_BYTES."""


class PackWriter:
    """Writes one pack. Use add() for each tile, then close().

    The index is reserved up front and written on close(), so tiles can be added in any order.
    Identical tile bytes are stored once.
    """

    def __init__(self, path: Path, zoom: int, x0: int, y0: int, cols: int, rows: int,
                 background: int = 0x000000, max_bytes: int = MAX_PACK_BYTES) -> None:
        if cols <= 0 or rows <= 0:
            raise ValueError("a pack needs at least one tile column and row")
        if not 0 <= zoom <= 20:
            raise ValueError("zoom must be 0 to 20 (the dashboard reads no higher levels)")
        side = 1 << zoom
        if x0 < 0 or y0 < 0 or x0 + cols > side or y0 + rows > side:
            raise ValueError("tile rectangle is outside the world at this zoom")
        self.path = Path(path)
        self.zoom, self.x0, self.y0, self.cols, self.rows = zoom, x0, y0, cols, rows
        self.background = background & 0xFFFFFF
        self.max_bytes = max_bytes
        self._index = [(0, 0)] * (cols * rows)
        self._seen: Dict[bytes, tuple] = {}
        self._size = HEADER_SIZE + _ENTRY.size * cols * rows
        if self._size > max_bytes:
            raise PackFull("the index alone is larger than the size limit")
        self.tiles = 0
        self.unique = 0
        self._file: Optional[BinaryIO] = open(self.path, "wb")
        self._file.write(b"\0" * self._size)

    @property
    def size(self) -> int:
        return self._size

    def add(self, x: int, y: int, data: bytes) -> None:
        if self._file is None:
            raise ValueError("pack is closed")
        if not (self.x0 <= x < self.x0 + self.cols and self.y0 <= y < self.y0 + self.rows):
            raise ValueError("tile %d/%d/%d is outside this pack" % (self.zoom, x, y))
        if not data:
            raise ValueError("empty tile data; leave the entry out instead")
        key = hashlib.sha1(data).digest()
        found = self._seen.get(key)
        if found is None:
            if self._size + len(data) > self.max_bytes:
                raise PackFull("pack would exceed %d bytes" % self.max_bytes)
            found = (self._size, len(data))
            self._file.write(data)
            self._size += len(data)
            self._seen[key] = found
            self.unique += 1
        self._index[(y - self.y0) * self.cols + (x - self.x0)] = found
        self.tiles += 1

    def close(self) -> None:
        if self._file is None:
            return
        f = self._file
        f.seek(0)
        f.write(_HEADER.pack(MAGIC, VERSION, self.zoom, self.x0, self.y0, self.cols, self.rows,
                             TILE_SIZE, FORMAT_PNG, HEADER_SIZE, self.background))
        f.write(b"\0" * (HEADER_SIZE - _HEADER.size))
        f.write(b"".join(_ENTRY.pack(o, n) for o, n in self._index))
        f.close()
        self._file = None

    def __enter__(self) -> "PackWriter":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


class PackReader:
    """Reads a pack written by PackWriter (for checks and tests)."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        with open(self.path, "rb") as f:
            raw = f.read(HEADER_SIZE)
        if len(raw) < HEADER_SIZE:
            raise ValueError("%s: too short for a pack header" % self.path)
        (magic, version, self.zoom, self.x0, self.y0, self.cols, self.rows, tile_size, fmt,
         self.index_offset, self.background) = _HEADER.unpack_from(raw)
        if magic != MAGIC or version != VERSION or tile_size != TILE_SIZE or fmt != FORMAT_PNG:
            raise ValueError("%s: not a version %d Montero tile pack" % (self.path, VERSION))

    def entry(self, x: int, y: int) -> Optional[tuple]:
        if not (self.x0 <= x < self.x0 + self.cols and self.y0 <= y < self.y0 + self.rows):
            return None
        with open(self.path, "rb") as f:
            f.seek(self.index_offset + _ENTRY.size * ((y - self.y0) * self.cols + (x - self.x0)))
            return _ENTRY.unpack(f.read(_ENTRY.size))

    def tile(self, x: int, y: int) -> Optional[bytes]:
        found = self.entry(x, y)
        if not found or found[1] == 0:
            return None
        with open(self.path, "rb") as f:
            f.seek(found[0])
            return f.read(found[1])


def _chunk(kind: bytes, body: bytes) -> bytes:
    return (struct.pack(">I", len(body)) + kind + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))


def encode_png(width: int, height: int, pixels: bytes, palette: Optional[bytes] = None) -> bytes:
    """Minimal PNG encoder for tests and solid tiles.

    pixels: rows of RGB bytes (3 per pixel), or palette indexes (1 per pixel) when palette
    (RGB triplets) is given.
    """
    per_pixel = 1 if palette is not None else 3
    if len(pixels) != width * height * per_pixel:
        raise ValueError("pixel data does not match the size")
    stride = width * per_pixel
    raw = b"".join(b"\0" + pixels[r * stride:(r + 1) * stride] for r in range(height))
    colour_type = 3 if palette is not None else 2
    out = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8,
                                                              colour_type, 0, 0, 0))
    if palette is not None:
        out += _chunk(b"PLTE", palette)
    return out + _chunk(b"IDAT", zlib.compress(raw, 9)) + _chunk(b"IEND", b"")


def solid_png(rgb: int, size: int = TILE_SIZE) -> bytes:
    """A one-colour tile as a small palette PNG."""
    palette = bytes(((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF))
    return encode_png(size, size, b"\0" * (size * size), palette)
