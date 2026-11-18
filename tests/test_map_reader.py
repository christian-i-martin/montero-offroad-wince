"""Compile and run the dashboard's map reader (dashboard/src/map.c) on the build machine.

Writes small packs with tools/map/mtpack.py (RGB, palette, shared, corrupt and missing tiles),
then runs tests/c/map_selftest.c against them: decoding, exact pixel placement across tile
borders, background and outside colours, night dimming, the tile cache and bottom-up bitmaps.
(The dashboard draws the vector map since 0.9; its previews with a map are in tests/test_vmap.py.)
"""
from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "map"))
import mtpack  # noqa: E402

COMPILER = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
FLAGS = ["-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
         "-I", str(ROOT / "dashboard/src"), "-I", str(ROOT / "probe/src")]
MAP_SOURCES = ["dashboard/src/map.c", "dashboard/src/pngdec.c"]
ADDR_SOURCES = ["dashboard/src/addr.c"]
A, B, C, D, E, F = 0xC82828, 0x00C800, 0x0000C8, 0xF0F010, 0xC8C8C8, 0x640064


def rgb(c: int) -> bytes:
    return bytes(((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF))


def solid_rgb_png(c: int) -> bytes:
    return mtpack.encode_png(256, 256, rgb(c) * (256 * 256))


def quadrant_png() -> bytes:
    rows = []
    for y in range(256):
        left, right = (B, C) if y < 128 else (E, F)
        rows.append(rgb(left) * 128 + rgb(right) * 128)
    return mtpack.encode_png(256, 256, b"".join(rows))


def write_test_packs(folder: Path) -> tuple:
    p14, p15, junk = folder / "z14.pack", folder / "z15.pack", folder / "junk.pack"
    shared = solid_rgb_png(A)
    with mtpack.PackWriter(p14, 14, 1000, 2000, 3, 2, background=0x102030) as w:
        w.add(1000, 2000, shared)
        w.add(1001, 2000, quadrant_png())
        w.add(1002, 2000, mtpack.solid_png(D))
        w.add(1000, 2001, shared)
        w.add(1001, 2001, b"not a png at all")
    with mtpack.PackWriter(p15, 15, 5, 5, 1, 1) as w:
        w.add(5, 5, shared)
    junk.write_bytes(b"MTPK" + b"\xff" * 60)
    return p14, p15, junk


def compile_c(main: str, sources: list, binary: Path) -> None:
    subprocess.run([COMPILER] + FLAGS + ["-o", str(binary), str(ROOT / main)]
                   + [str(ROOT / s) for s in sources] + ["-lm"], check=True)


class PackFormatTests(unittest.TestCase):
    def test_writer_dedupes_and_reader_round_trips(self):
        with tempfile.TemporaryDirectory() as temp:
            p14, _, _ = write_test_packs(Path(temp))
            r = mtpack.PackReader(p14)
            self.assertEqual((r.zoom, r.x0, r.y0, r.cols, r.rows, r.background), (14, 1000, 2000, 3, 2, 0x102030))
            self.assertEqual(r.entry(1000, 2000), r.entry(1000, 2001))
            self.assertEqual(r.entry(1002, 2001), (0, 0))
            self.assertIsNone(r.entry(1003, 2000))
            self.assertTrue(r.tile(1001, 2000).startswith(b"\x89PNG"))

    def test_size_limit(self):
        with tempfile.TemporaryDirectory() as temp:
            w = mtpack.PackWriter(Path(temp) / "small.pack", 10, 0, 0, 2, 1, max_bytes=200)
            w.add(0, 0, b"x" * 50)
            with self.assertRaises(mtpack.PackFull):
                w.add(1, 0, b"y" * 100)
            w.add(1, 0, b"x" * 50)  # a tile already stored costs nothing
            w.close()
            self.assertLessEqual((Path(temp) / "small.pack").stat().st_size, 200)

    def test_rejects_tiles_outside(self):
        with tempfile.TemporaryDirectory() as temp:
            with mtpack.PackWriter(Path(temp) / "a.pack", 3, 0, 0, 2, 2) as w:
                with self.assertRaises(ValueError):
                    w.add(2, 0, b"x")
            with self.assertRaises(ValueError):
                mtpack.PackWriter(Path(temp) / "b.pack", 3, 7, 0, 2, 1)


@unittest.skipIf(COMPILER is None, "no C compiler on this machine")
class MapReaderTests(unittest.TestCase):
    def test_selftest(self):
        with tempfile.TemporaryDirectory() as temp:
            packs = write_test_packs(Path(temp))
            binary = Path(temp) / "map_selftest"
            compile_c("tests/c/map_selftest.c", MAP_SOURCES, binary)
            result = subprocess.run([str(binary)] + [str(p) for p in packs], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
