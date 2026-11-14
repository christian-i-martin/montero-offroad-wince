#!/usr/bin/env python3
"""Builds the dashboard's offline map: renders tiles on a local tile server and writes packs.

Fetches every 256 x 256 map picture (tile) of the chosen area and zoom levels from a tile server
running on this Mac (tileserver-gl in Docker, started by tools/map/build_map.sh) and writes them
into Montero tile packs (*.pack, format in mtpack.py) for the SD card's Map folder.

Only a server on this computer (localhost) is accepted: never point this at public
OpenStreetMap tile servers, their usage policy forbids bulk downloading.

Steps:
1. Render. Tiles go into a cache database (SQLite, one file) in --cache-dir as they arrive, so an
   interrupted run continues where it stopped, and a re-run with the same style and map data
   renders nothing again. The cache file name comes from a fingerprint of the style and data
   files (--cache-key-file), so a new style or new map data starts a fresh cache.
2. Pack. Each zoom level becomes one pack, or several column bands when one pack would pass
   mtpack.MAX_PACK_BYTES. Band edges are planned from the cached tile sizes, so every band fits.
   Files are written under a temporary name and renamed when complete.

Sea shortcut: much of a box around Luzon is open sea. When a tile is one uniform
sea colour, its four children (next zoom) almost always are too, so they reuse a one-colour tile
without being rendered, and so on down. Only tiles in the sea colour count (a tile that is all
forest at zoom 11 can still hold tracks at zoom 14), and only parents at --shortcut-from-zoom or
deeper count, because islands smaller than about one pixel are dropped from the coarser zooms.
Even so, tiny islets or rocks far from any coast can be missed. --no-sea-shortcut renders every
tile.

Python 3.9 compatible, standard library only.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import math
import os
import sqlite3
import struct
import sys
import threading
import time
import urllib.parse
import zlib
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from pathlib import Path
from typing import Callable, Dict, Iterable, Iterator, List, Optional, Sequence, Set, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mtpack  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_STYLE = Path(__file__).resolve().parent / "style" / "montero-dark.json"
DEFAULT_OUTPUT = ROOT / "dist" / "map" / "Map"
DEFAULT_CACHE = ROOT / "dist" / "map-build" / "cache"
DEFAULT_URL = "http://127.0.0.1:8091/styles/montero-dark/256/{z}/{x}/{y}.png"

# Default area: Luzon, from Ilocos Norte and Cagayan to the tip of Bicol, with Catanduanes,
# Polillo, Marinduque and northern Mindoro (minimum longitude, minimum latitude, maximum
# longitude, maximum latitude). The whole country would be about 116.8,4.4,126.7,21.25.
LUZON = (119.4, 12.4, 124.5, 18.8)
MAX_LAT = 85.05112878  # Web Mercator stops here
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
COLOUR_TOLERANCE = 4   # per channel, for palette rounding of the sea colour

Bounds = Tuple[float, float, float, float]
TileRange = Tuple[int, int, int, int]     # x0, y0, x1, y1, all inclusive


# ---------------------------------------------------------------------------------------------
# Tile numbers

def parse_bounds(text: str) -> Bounds:
    parts = [p.strip() for p in text.split(",")]
    if len(parts) != 4:
        raise ValueError("bounds need four numbers: minlon,minlat,maxlon,maxlat")
    minlon, minlat, maxlon, maxlat = (float(p) for p in parts)
    if not (-180 <= minlon < maxlon <= 180 and -90 <= minlat < maxlat <= 90):
        raise ValueError("bounds must be minlon,minlat,maxlon,maxlat with min < max")
    return minlon, minlat, maxlon, maxlat


def bounds_inside(inner: Bounds, outer: Bounds) -> bool:
    return (outer[0] <= inner[0] and outer[1] <= inner[1] and inner[2] <= outer[2]
            and inner[3] <= outer[3])


def lonlat_to_tile(lon: float, lat: float, zoom: int) -> Tuple[int, int]:
    """The tile (x, y) holding this point at this zoom (OpenStreetMap numbering)."""
    side = 1 << zoom
    lat = max(-MAX_LAT, min(MAX_LAT, lat))
    fx = (lon + 180.0) / 360.0 * side
    rad = math.radians(lat)
    fy = (1.0 - math.asinh(math.tan(rad)) / math.pi) / 2.0 * side
    return (min(side - 1, max(0, int(math.floor(fx)))),
            min(side - 1, max(0, int(math.floor(fy)))))


def tile_range(bounds: Bounds, zoom: int) -> TileRange:
    """Inclusive tile rectangle covering the bounds. A maximum edge that falls exactly on a tile
    border does not pull in the next tile."""
    minlon, minlat, maxlon, maxlat = bounds
    x0, y0 = lonlat_to_tile(minlon, maxlat, zoom)
    side = 1 << zoom
    eps = 1e-9 * 360.0
    x1, y1 = lonlat_to_tile(maxlon - eps, minlat + eps, zoom)
    if maxlon >= 180.0:
        x1 = side - 1
    return x0, y0, max(x0, x1), max(y0, y1)


def tile_bounds(zoom: int, x: int, y: int) -> Bounds:
    """Longitude and latitude edges of a tile."""
    side = 1 << zoom

    def lat(row: float) -> float:
        return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * row / side))))
    return (x / side * 360.0 - 180.0, lat(y + 1), (x + 1) / side * 360.0 - 180.0, lat(y))


def range_size(rng: TileRange) -> int:
    x0, y0, x1, y1 = rng
    return (x1 - x0 + 1) * (y1 - y0 + 1)


# ---------------------------------------------------------------------------------------------
# PNG checks

def png_uniform_colour(data: bytes) -> Optional[int]:
    """0xRRGGBB if every pixel of the PNG has the same colour, else None.

    Checks the compressed rows against what PNG's row filters produce for a one-colour picture,
    so it needs no image library and no full decode. Unusual PNGs (interlaced, odd padding)
    count as not uniform, which only costs a render. Alpha is ignored, like on the dashboard.
    """
    if not data.startswith(PNG_SIGNATURE):
        return None
    pos, ihdr, palette, idat = 8, None, b"", []
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length:
            return None
        pos += 12 + length
        if kind == b"IHDR" and length == 13:
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if ihdr is None or not idat:
        return None
    width, height, depth, colour_type, compression, filter_method, interlace = ihdr
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colour_type)
    if channels is None or compression or filter_method or interlace or not width or not height:
        return None
    allowed = (1, 2, 4, 8) if colour_type == 3 else (1, 2, 4, 8, 16) if colour_type == 0 \
        else (8, 16)
    if depth not in allowed:
        return None
    bits = channels * depth
    if bits < 8 and (width * bits) % 8:
        return None
    row_len = (width * bits + 7) // 8
    unit = max(1, bits // 8)        # bytes per pixel as PNG filters see it
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error:
        return None
    stride = row_len + 1
    if len(raw) != height * stride:
        return None
    pixel = raw[1:1 + unit]         # first pixel is stored as is by every filter type
    count = row_len // unit
    zeros = bytes(row_len - unit)
    half = bytes(b - (b >> 1) for b in pixel)
    first_row = {0: pixel * count, 1: pixel + zeros, 2: pixel * count,
                 3: pixel + half * (count - 1), 4: pixel + zeros}
    other_rows = {0: pixel * count, 1: pixel + zeros, 2: bytes(row_len),
                  3: half + zeros, 4: bytes(row_len)}
    for r in range(height):
        expected = (first_row if r == 0 else other_rows).get(raw[r * stride])
        if expected is None or raw[r * stride + 1:(r + 1) * stride] != expected:
            return None
    if bits < 8:
        per_byte = 8 // depth
        mask = (1 << depth) - 1
        samples = {(pixel[0] >> (8 - depth * (i + 1))) & mask for i in range(per_byte)}
        if len(samples) != 1:
            return None
        value = samples.pop()
    else:
        value = pixel[0]
    if colour_type == 3:
        if 3 * value + 3 > len(palette):
            return None
        r, g, b = palette[3 * value:3 * value + 3]
    elif colour_type in (0, 4):
        if depth < 8:
            value = value * 255 // ((1 << depth) - 1)
        r = g = b = value
    else:
        step = depth // 8
        r, g, b = pixel[0], pixel[step], pixel[2 * step]
    return (r << 16) | (g << 8) | b


def colours_close(a: int, b: int, tolerance: int = COLOUR_TOLERANCE) -> bool:
    return all(abs(((a >> s) & 0xFF) - ((b >> s) & 0xFF)) <= tolerance for s in (16, 8, 0))


def parse_colour(text: str) -> int:
    value = text.strip().lstrip("#")
    if len(value) != 6:
        raise ValueError("colour must be RRGGBB, got %r" % text)
    return int(value, 16)


def style_water_colour(style_path: Path) -> int:
    """The fill colour of the style's "water" layer (the sea colour of the rendered tiles)."""
    style = json.loads(Path(style_path).read_text(encoding="utf-8"))
    for item in style.get("layers", []):
        if item.get("id") == "water":
            colour = item.get("paint", {}).get("fill-color")
            if isinstance(colour, str):
                return parse_colour(colour)
    raise ValueError("%s has no water layer with a plain fill colour" % style_path)


# ---------------------------------------------------------------------------------------------
# Tile sources

class TileError(Exception):
    """A tile could not be rendered."""


class HttpTileSource:
    """Fetches PNG tiles from a tile server on this computer, one kept-alive connection per
    worker thread."""

    LOCAL_HOSTS = ("127.0.0.1", "localhost", "::1")

    def __init__(self, url_template: str, timeout: float = 120.0, attempts: int = 4) -> None:
        parts = urllib.parse.urlsplit(url_template)
        if parts.scheme != "http" or parts.hostname not in self.LOCAL_HOSTS:
            raise ValueError("only a local tile server (http://127.0.0.1:...) is allowed, "
                             "never a public tile server")
        for key in ("{z}", "{x}", "{y}"):
            if key not in url_template:
                raise ValueError("the tile URL needs {z}, {x} and {y}")
        self.host, self.port = parts.hostname, parts.port or 80
        self.path = parts.path + ("?" + parts.query if parts.query else "")
        self.url = url_template
        self.timeout, self.attempts = timeout, attempts
        self._local = threading.local()

    def _connection(self) -> http.client.HTTPConnection:
        conn = getattr(self._local, "conn", None)
        if conn is None:
            conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
            self._local.conn = conn
        return conn

    def _drop(self) -> None:
        conn = getattr(self._local, "conn", None)
        if conn is not None:
            conn.close()
        self._local.conn = None

    def fetch(self, zoom: int, x: int, y: int) -> bytes:
        path = self.path.format(z=zoom, x=x, y=y)
        problem = ""
        for attempt in range(self.attempts):
            try:
                conn = self._connection()
                conn.request("GET", path)
                resp = conn.getresponse()
                body = resp.read()
                if resp.status == 200 and body.startswith(PNG_SIGNATURE):
                    return body
                problem = "HTTP %d %s" % (resp.status, body[:200].decode("utf-8", "replace"))
                if resp.status not in (429, 500, 502, 503, 504):
                    break
            except (OSError, http.client.HTTPException) as exc:
                self._drop()
                problem = str(exc) or exc.__class__.__name__
            time.sleep(min(8.0, 0.5 * (2 ** attempt)))
        raise TileError("tile %d/%d/%d: %s" % (zoom, x, y, problem))


# ---------------------------------------------------------------------------------------------
# Cache

class TileCache:
    """Rendered tiles in one SQLite file. Identical tiles are stored once (by SHA-1, as in
    PackWriter). how = 0 rendered, 1 copied from a uniform sea parent."""

    RENDERED, SHORTCUT = 0, 1

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.db = sqlite3.connect(str(self.path))
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=NORMAL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT);
            CREATE TABLE IF NOT EXISTS blobs (hash BLOB PRIMARY KEY, size INTEGER NOT NULL,
                                              rgb INTEGER, data BLOB NOT NULL);
            CREATE TABLE IF NOT EXISTS tiles (z INTEGER, x INTEGER, y INTEGER,
                                              hash BLOB NOT NULL, how INTEGER NOT NULL,
                                              PRIMARY KEY (z, x, y)) WITHOUT ROWID;
        """)
        self.db.commit()

    def close(self) -> None:
        self.db.commit()
        self.db.close()

    def set_meta(self, key: str, value: str) -> None:
        self.db.execute("INSERT OR REPLACE INTO meta VALUES (?, ?)", (key, value))

    def known_rows(self, zoom: int, x: int, y0: int, y1: int) -> Set[int]:
        cur = self.db.execute("SELECT y FROM tiles WHERE z=? AND x=? AND y BETWEEN ? AND ?",
                              (zoom, x, y0, y1))
        return {row[0] for row in cur}

    def column_colours(self, zoom: int, x: int, y0: int, y1: int) -> Dict[int, Optional[int]]:
        """y -> uniform colour (or None) of the cached tiles in one column."""
        cur = self.db.execute(
            "SELECT t.y, b.rgb FROM tiles t JOIN blobs b ON b.hash = t.hash "
            "WHERE t.z=? AND t.x=? AND t.y BETWEEN ? AND ?", (zoom, x, y0, y1))
        return {y: rgb for y, rgb in cur}

    def put(self, zoom: int, x: int, y: int, data: bytes, rgb: Optional[int], how: int) -> None:
        key = hashlib.sha1(data).digest()
        self.db.execute("INSERT OR IGNORE INTO blobs VALUES (?, ?, ?, ?)",
                        (key, len(data), rgb, data))
        self.db.execute("INSERT OR REPLACE INTO tiles VALUES (?, ?, ?, ?, ?)",
                        (zoom, x, y, key, how))

    def commit(self) -> None:
        self.db.commit()

    def count(self, zoom: int, rng: TileRange) -> int:
        x0, y0, x1, y1 = rng
        return self.db.execute("SELECT COUNT(*) FROM tiles WHERE z=? AND x BETWEEN ? AND ? "
                               "AND y BETWEEN ? AND ?", (zoom, x0, x1, y0, y1)).fetchone()[0]

    def columns(self, zoom: int, rng: TileRange) -> Iterator[Tuple[int, List[Tuple[bytes, int]]]]:
        """(x, [(hash, size), ...]) for every column of the range, west to east."""
        x0, y0, x1, y1 = rng
        cur = self.db.execute(
            "SELECT t.x, t.hash, b.size FROM tiles t JOIN blobs b ON b.hash = t.hash "
            "WHERE t.z=? AND t.x BETWEEN ? AND ? AND t.y BETWEEN ? AND ? ORDER BY t.x",
            (zoom, x0, x1, y0, y1))
        current, items = None, []  # type: Optional[int], List[Tuple[bytes, int]]
        for x, key, size in cur:
            if x != current:
                if current is not None:
                    yield current, items
                current, items = x, []
            items.append((bytes(key), size))
        if current is not None:
            yield current, items

    def tiles(self, zoom: int, rng: TileRange) -> Iterator[Tuple[int, int, bytes]]:
        x0, y0, x1, y1 = rng
        cur = self.db.execute(
            "SELECT t.x, t.y, b.data FROM tiles t JOIN blobs b ON b.hash = t.hash "
            "WHERE t.z=? AND t.x BETWEEN ? AND ? AND t.y BETWEEN ? AND ? ORDER BY t.x, t.y",
            (zoom, x0, x1, y0, y1))
        for x, y, data in cur:
            yield x, y, bytes(data)


def cache_key(files: Sequence[Path], url: str) -> str:
    """Fingerprint of the style, server configuration and map data. Small files are hashed;
    big ones (the map data) count by size and modification time, which change on a rebuild."""
    digest = hashlib.sha256(url.encode("utf-8"))
    for path in files:
        path = Path(path)
        stat = path.stat()
        digest.update(path.name.encode("utf-8"))
        if stat.st_size <= 64 * 1024 * 1024:
            digest.update(path.read_bytes())
        else:
            digest.update(("%d:%d" % (stat.st_size, stat.st_mtime_ns)).encode("ascii"))
    return digest.hexdigest()[:16]


# ---------------------------------------------------------------------------------------------
# Rendering

class ZoomStats:
    def __init__(self, zoom: int, total: int) -> None:
        self.zoom, self.total = zoom, total
        self.rendered = self.shortcut = self.cached = 0
        self.render_seconds = 0.0
        self.packs: List[Tuple[Path, int, int, int]] = []   # path, bytes, tiles, unique


Fetch = Callable[[int, int, int], bytes]


def _render_one(fetch: Fetch, zoom: int, x: int, y: int) -> Tuple[int, int, bytes, Optional[int]]:
    data = fetch(zoom, x, y)
    return x, y, data, png_uniform_colour(data)


class Renderer:
    def __init__(self, cache: TileCache, fetch: Fetch, workers: int = 12,
                 sea_colour: Optional[int] = None, shortcut_from_zoom: int = 11,
                 log: Callable[[str], None] = print, progress_seconds: float = 10.0) -> None:
        self.cache, self.fetch, self.workers = cache, fetch, max(1, workers)
        self.sea_colour = sea_colour           # None switches the sea shortcut off
        self.shortcut_from_zoom = shortcut_from_zoom
        self.log, self.progress_seconds = log, progress_seconds
        self._solid: Dict[int, bytes] = {}

    def solid(self, rgb: int) -> bytes:
        if rgb not in self._solid:
            self._solid[rgb] = mtpack.solid_png(rgb)
        return self._solid[rgb]

    def _is_sea(self, rgb: Optional[int]) -> bool:
        return rgb is not None and self.sea_colour is not None and \
            colours_close(rgb, self.sea_colour)

    def _jobs(self, zoom: int, rng: TileRange, stats: ZoomStats) -> Iterator[Tuple[int, int]]:
        """Yields the tiles that need rendering; stores shortcut tiles on the way."""
        x0, y0, x1, y1 = rng
        use_shortcut = self.sea_colour is not None and zoom - 1 >= self.shortcut_from_zoom
        for x in range(x0, x1 + 1):
            known = self.cache.known_rows(zoom, x, y0, y1)
            stats.cached += len(known)
            parents: Dict[int, Optional[int]] = {}
            if use_shortcut and len(known) < y1 - y0 + 1:
                parents = self.cache.column_colours(zoom - 1, x // 2, y0 // 2, y1 // 2)
            for y in range(y0, y1 + 1):
                if y in known:
                    continue
                parent = parents.get(y // 2)
                if use_shortcut and self._is_sea(parent):
                    self.cache.put(zoom, x, y, self.solid(parent), parent, TileCache.SHORTCUT)
                    stats.shortcut += 1
                    continue
                yield x, y

    def render_zoom(self, zoom: int, rng: TileRange) -> ZoomStats:
        stats = ZoomStats(zoom, range_size(rng))
        started = last_log = last_commit = time.monotonic()
        pending: Set[Future] = set()
        jobs = self._jobs(zoom, rng, stats)
        limit = self.workers * 4
        failure: Optional[BaseException] = None
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            try:
                exhausted = False
                while pending or not exhausted:
                    while not exhausted and len(pending) < limit and failure is None:
                        job = next(jobs, None)
                        if job is None:
                            exhausted = True
                            break
                        pending.add(pool.submit(_render_one, self.fetch, zoom, job[0], job[1]))
                    if not pending:
                        break
                    done, pending = wait(pending, timeout=1.0, return_when=FIRST_COMPLETED)
                    for future in done:
                        try:
                            x, y, data, rgb = future.result()
                        except Exception as exc:  # keep the finished tiles, stop new work
                            failure = failure or exc
                            exhausted = True
                            continue
                        if rgb is not None:
                            data = self.solid(rgb)
                        self.cache.put(zoom, x, y, data, rgb, TileCache.RENDERED)
                        stats.rendered += 1
                    now = time.monotonic()
                    if now - last_commit >= 2.0:
                        self.cache.commit()
                        last_commit = now
                    if now - last_log >= self.progress_seconds:
                        last_log = now
                        self._progress(stats, now - started)
            except KeyboardInterrupt:
                for future in pending:
                    future.cancel()
                self.cache.commit()
                raise
        self.cache.commit()
        stats.render_seconds = time.monotonic() - started
        if failure is not None:
            raise TileError("%s (finished tiles are cached; run again to continue)" % failure)
        return stats

    def _progress(self, stats: ZoomStats, seconds: float) -> None:
        done = stats.rendered + stats.shortcut + stats.cached
        rate = stats.rendered / seconds if seconds > 0 else 0.0
        left = stats.total - done
        eta = ""
        if rate > 0 and left > 0:
            eta = ", at most %s left" % format_seconds(left / rate)
        self.log("  z%d: %d of %d tiles (%d rendered, %d sea shortcut, %d cached), "
                 "%.1f rendered/s%s" % (stats.zoom, done, stats.total, stats.rendered,
                                        stats.shortcut, stats.cached, rate, eta))


# ---------------------------------------------------------------------------------------------
# Packing

def plan_bands(columns: Iterable[Tuple[int, List[Tuple[bytes, int]]]], rows: int,
               max_bytes: int = mtpack.MAX_PACK_BYTES) -> List[Tuple[int, int]]:
    """Splits a zoom level into column bands that each fit in one pack.

    columns: (x, [(tile hash, tile size), ...]) west to east, without gaps.
    Returns [(first column, column count), ...]. A pack's size is its header, its index
    (8 bytes per tile) and each different tile once.
    """
    bands: List[Tuple[int, int]] = []
    start, cols, data_bytes = None, 0, 0
    seen: Set[bytes] = set()
    last_x = None
    for x, items in columns:
        if last_x is not None and x != last_x + 1:
            raise ValueError("column %d is missing" % (last_x + 1))
        last_x = x
        new = {}
        for key, size in items:
            if key not in seen:
                new[key] = size
        added = sum(new.values())
        size = mtpack.HEADER_SIZE + 8 * rows * (cols + 1) + data_bytes + added
        if start is not None and size > max_bytes:
            bands.append((start, cols))
            start, cols, data_bytes, seen = None, 0, 0, set()
            new = {}
            for key, size_ in items:
                new[key] = size_
            added = sum(new.values())
        if start is None:
            start = x
            if mtpack.HEADER_SIZE + 8 * rows + added > max_bytes:
                raise ValueError("column %d alone is larger than %d bytes" % (x, max_bytes))
        seen.update(new)
        data_bytes += added
        cols += 1
    if start is not None:
        bands.append((start, cols))
    return bands


def pack_name(prefix: str, zoom: int, x0: int, y0: int, cols: int, rows: int) -> str:
    """Pack file name; it holds the zoom and tile rectangle, so runs over different areas or
    zooms can share one Map folder."""
    return "%s-z%02d-%d-%d-%dx%d.pack" % (prefix, zoom, x0, y0, cols, rows)


def write_packs(cache: TileCache, zoom: int, rng: TileRange, out_dir: Path, prefix: str,
                background: int, max_bytes: int = mtpack.MAX_PACK_BYTES
                ) -> List[Tuple[Path, int, int, int]]:
    x0, y0, x1, y1 = rng
    rows = y1 - y0 + 1
    have = cache.count(zoom, rng)
    if have != range_size(rng):
        raise TileError("zoom %d: %d of %d tiles are rendered; run again to finish"
                        % (zoom, have, range_size(rng)))
    bands = plan_bands(cache.columns(zoom, rng), rows, max_bytes)
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    for bx0, cols in bands:
        final = out_dir / pack_name(prefix, zoom, bx0, y0, cols, rows)
        part = final.with_name(final.name + ".part")
        writer = mtpack.PackWriter(part, zoom, bx0, y0, cols, rows, background, max_bytes)
        try:
            for x, y, data in cache.tiles(zoom, (bx0, y0, bx0 + cols - 1, y1)):
                writer.add(x, y, data)
        finally:
            writer.close()
        if writer.tiles != cols * rows:
            part.unlink()
            raise TileError("%s: only %d of %d tiles" % (final.name, writer.tiles, cols * rows))
        os.replace(str(part), str(final))
        written.append((final, writer.size, writer.tiles, writer.unique))
    return written


def overlapping_packs(out_dir: Path, written: Sequence[Path]) -> List[str]:
    """Other packs in the folder that cover some of the same tiles at the same zoom."""
    mine = []
    for path in written:
        r = mtpack.PackReader(path)
        mine.append((r.zoom, r.x0, r.y0, r.x0 + r.cols, r.y0 + r.rows, path.name))
    notes = []
    names = {p.name for p in written}
    for path in sorted(out_dir.glob("*.pack")):
        if path.name in names:
            continue
        try:
            r = mtpack.PackReader(path)
        except (OSError, ValueError):
            continue
        for zoom, ax0, ay0, ax1, ay1, name in mine:
            if r.zoom == zoom and r.x0 < ax1 and ax0 < r.x0 + r.cols and \
                    r.y0 < ay1 and ay0 < r.y0 + r.rows:
                notes.append("%s overlaps %s (delete it if an older run left it)"
                             % (path.name, name))
                break
    return notes


# ---------------------------------------------------------------------------------------------
# Command line

def format_seconds(seconds: float) -> str:
    seconds = int(round(seconds))
    if seconds < 90:
        return "%d s" % seconds
    if seconds < 5400:
        return "%d min" % round(seconds / 60)
    return "%.1f h" % (seconds / 3600)


def format_bytes(count: float) -> str:
    for unit in ("bytes", "KB", "MB", "GB"):
        if count < 1000 or unit == "GB":
            return ("%d %s" % (count, unit)) if unit == "bytes" else ("%.1f %s" % (count, unit))
        count /= 1000.0
    return str(count)


def build(args: argparse.Namespace, fetch: Optional[Fetch] = None,
          log: Callable[[str], None] = print) -> List[ZoomStats]:
    bounds = args.bounds
    if args.data_bounds and not bounds_inside(bounds, args.data_bounds):
        raise ValueError("the area %s is not inside the area of the vector tiles %s; rebuild "
                         "them with MAP_VECTOR_BOUNDS covering it (see build_map.sh)"
                         % (",".join("%g" % v for v in bounds),
                            ",".join("%g" % v for v in args.data_bounds)))
    zooms = list(range(args.min_zoom, args.max_zoom + 1))
    if args.sea_colour:
        sea = parse_colour(args.sea_colour)
    else:
        sea = style_water_colour(args.style)
    background = parse_colour(args.background) if args.background else sea
    if fetch is None:
        fetch = HttpTileSource(args.url).fetch
    key = cache_key(args.cache_key_file or [], args.url)
    cache = TileCache(Path(args.cache_dir) / ("tiles-%s.sqlite" % key))
    cache.set_meta("url", args.url)
    cache.commit()
    renderer = Renderer(cache, fetch, args.workers, None if args.no_sea_shortcut else sea,
                        args.shortcut_from_zoom, log)
    out_dir = Path(args.output)
    started = time.monotonic()
    all_stats: List[ZoomStats] = []
    written: List[Path] = []
    log("Area %s, zoom %d to %d, cache %s" % (",".join("%g" % v for v in bounds),
                                             args.min_zoom, args.max_zoom, cache.path))
    try:
        for zoom in zooms:
            rng = tile_range(bounds, zoom)
            log("Zoom %d: %d tiles (x %d-%d, y %d-%d)" % (zoom, range_size(rng), rng[0],
                                                         rng[2], rng[1], rng[3]))
            stats = renderer.render_zoom(zoom, rng)
            stats.packs = write_packs(cache, zoom, rng, out_dir, args.name, background,
                                      args.max_pack_bytes)
            written += [p[0] for p in stats.packs]
            all_stats.append(stats)
            log("  %d rendered, %d sea shortcut, %d cached before; %s" % (
                stats.rendered, stats.shortcut, stats.cached,
                ", ".join("%s %s" % (p[0].name, format_bytes(p[1])) for p in stats.packs)))
    finally:
        cache.close()
    elapsed = time.monotonic() - started
    summarise(all_stats, elapsed, log)
    for note in overlapping_packs(out_dir, written):
        log("Note: " + note)
    return all_stats


def summarise(all_stats: Sequence[ZoomStats], elapsed: float,
              log: Callable[[str], None] = print) -> None:
    log("")
    log("zoom      tiles   rendered   shortcut     cached     unique        bytes  packs")
    tot = [0, 0, 0, 0, 0, 0, 0]
    for s in all_stats:
        unique = sum(p[3] for p in s.packs)
        size = sum(p[1] for p in s.packs)
        row = [s.total, s.rendered, s.shortcut, s.cached, unique, size, len(s.packs)]
        tot = [a + b for a, b in zip(tot, row)]
        log("%4d %10d %10d %10d %10d %10d %12d %6d" % ((s.zoom,) + tuple(row)))
    log(" all %10d %10d %10d %10d %10d %12d %6d" % tuple(tot))
    render_time = sum(s.render_seconds for s in all_stats)
    rendered = tot[1]
    log("Total %s in packs (%s per tile), %s." % (
        format_bytes(tot[5]), format_bytes(tot[5] / tot[0]) if tot[0] else "-",
        format_seconds(elapsed)))
    if rendered:
        log("Rendering: %.1f tiles/s (%.0f ms per rendered tile, wall clock with all workers)."
            % (rendered / render_time, 1000.0 * render_time / rendered))


def count_tiles(bounds: Bounds, zooms: Iterable[int], log: Callable[[str], None] = print) -> int:
    total = 0
    for zoom in zooms:
        rng = tile_range(bounds, zoom)
        total += range_size(rng)
        log("zoom %2d: %9d tiles (%d columns x %d rows)" % (
            zoom, range_size(rng), rng[2] - rng[0] + 1, rng[3] - rng[1] + 1))
    log("total:   %9d tiles" % total)
    return total


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Render map tiles on a local tile server and write "
                                            "Montero tile packs (*.pack) for the SD card.")
    p.add_argument("--bounds", type=parse_bounds, default=LUZON,
                   help="minlon,minlat,maxlon,maxlat (default: Luzon, %s)"
                        % ",".join("%g" % v for v in LUZON))
    p.add_argument("--data-bounds", type=parse_bounds,
                   help="area the vector tiles cover (build_map.sh passes it); --bounds must "
                        "lie inside it")
    p.add_argument("--min-zoom", type=int, default=8)
    p.add_argument("--max-zoom", type=int, default=14, help="up to 15 (default 14)")
    p.add_argument("--output", default=str(DEFAULT_OUTPUT),
                   help="folder for the .pack files (default dist/map/Map)")
    p.add_argument("--name", default="ph",
                   help="file name prefix of the packs (default ph)")
    p.add_argument("--url", default=DEFAULT_URL,
                   help="tile URL on the local server, with {z} {x} {y} (default %s)"
                        % DEFAULT_URL)
    p.add_argument("--workers", type=int, default=12, help="parallel requests (default 12)")
    p.add_argument("--cache-dir", default=str(DEFAULT_CACHE),
                   help="where rendered tiles are kept between runs (default dist/map-build/cache)")
    p.add_argument("--cache-key-file", action="append", type=Path,
                   help="file whose change starts a fresh cache (style, server config, map "
                        "data); repeat for several")
    p.add_argument("--style", type=Path, default=DEFAULT_STYLE,
                   help="style file to read the sea colour from")
    p.add_argument("--sea-colour", help="sea colour RRGGBB (default: the style's water colour)")
    p.add_argument("--background", help="pack background colour RRGGBB (default: sea colour)")
    p.add_argument("--no-sea-shortcut", action="store_true",
                   help="render every tile, even below one-colour sea tiles")
    p.add_argument("--shortcut-from-zoom", type=int, default=11,
                   help="only sea tiles at this zoom or deeper pass their colour on to their "
                        "children (default 11)")
    p.add_argument("--max-pack-bytes", type=int, default=mtpack.MAX_PACK_BYTES,
                   help=argparse.SUPPRESS)
    p.add_argument("--count", action="store_true",
                   help="only print how many tiles the area has per zoom")
    args = p.parse_args(argv)
    if not 0 <= args.min_zoom <= args.max_zoom <= 15:
        p.error("zooms must satisfy 0 <= --min-zoom <= --max-zoom <= 15")
    if args.workers < 1:
        p.error("--workers must be at least 1")
    return args


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    if args.count:
        count_tiles(args.bounds, range(args.min_zoom, args.max_zoom + 1))
        return 0
    try:
        build(args)
    except (TileError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("stopped; finished tiles are cached, run the same command again to continue",
              file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
