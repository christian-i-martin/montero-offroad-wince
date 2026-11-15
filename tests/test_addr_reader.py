"""The dashboard's address reader (dashboard/src/addr.c) against the reference in
tools/address/mtaddr.py.

Writes a made-up address file (overlapping areas, holes, a two-part area, long roads that get
split, places, non-ASCII names), then asks both readers about thousands of random points and a
few chosen ones and expects the same answers.
"""
from __future__ import annotations

from pathlib import Path
import math
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "address"))
import mtaddr  # noqa: E402

COMPILER = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
BOUNDS = (121.0, 14.0, 121.2, 14.2)


def square(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def blob(cx, cy, r, n, rng):
    """A wobbly ring around (cx, cy)."""
    return [(cx + r * (0.7 + 0.3 * rng.random()) * math.cos(2 * math.pi * k / n),
             cy + r * (0.7 + 0.3 * rng.random()) * math.sin(2 * math.pi * k / n)) for k in range(n)]


def write_sample(path: Path) -> None:
    rng = random.Random(7)
    w = mtaddr.AddrWriter(path, BOUNDS, cell_deg=0.02, road_radius_m=75, place_radius_m=1500)
    w.add_area(mtaddr.KIND_PROVINCE, "Rizal", [square(121.0, 14.0, 121.12, 14.2)])
    w.add_area(mtaddr.KIND_PROVINCE, "Laguna", [square(121.12, 14.0, 121.2, 14.2)])
    # a city with a hole and a second part
    w.add_area(mtaddr.KIND_CITY, "Antipolo", [square(121.02, 14.02, 121.1, 14.1), square(121.05, 14.05, 121.06, 14.06),
                                              square(121.14, 14.14, 121.16, 14.16)])
    w.add_area(mtaddr.KIND_CITY, "Parañaque", [square(121.0, 14.1, 121.08, 14.2)])  # overlaps: smaller box wins
    for i in range(40):
        cx, cy = 121.0 + rng.random() * 0.2, 14.0 + rng.random() * 0.2
        w.add_area(mtaddr.KIND_BARANGAY, "Barangay %d" % i if i % 3 else "San Isidro %d" % i,
                   [blob(cx, cy, 0.008 + rng.random() * 0.01, 9 + i % 7, rng)])
    for i in range(60):
        x, y = 121.0 + rng.random() * 0.2, 14.0 + rng.random() * 0.2
        pts = []
        for _ in range(5 + i % 80):  # some longer than one piece
            pts.append((x, y))
            x += (rng.random() - 0.5) * 0.004
            y += (rng.random() - 0.5) * 0.004
        w.add_road("Road %d" % (i % 25) if i % 4 else "Calle Niño %d" % i, pts, rank=1 + i % 5)
    for i in range(30):
        w.add_place("Sitio %d" % i, 121.0 + rng.random() * 0.2, 14.0 + rng.random() * 0.2, 1 + i % 6)
    w.close()


def query_points():
    rng = random.Random(11)
    pts = [(mtaddr.e6(121.0 + rng.random() * 0.2), mtaddr.e6(14.0 + rng.random() * 0.2)) for _ in range(3000)]
    pts += [(mtaddr.e6(121.055), mtaddr.e6(14.055)),   # in Antipolo's hole
            (mtaddr.e6(121.15), mtaddr.e6(14.15)),     # Antipolo's second part, in Laguna
            (mtaddr.e6(121.03), mtaddr.e6(14.12)),     # both cities: Parañaque's box is smaller (Antipolo's second part widens its box)
            (mtaddr.e6(120.9), mtaddr.e6(14.1)),       # west of the grid
            (mtaddr.e6(121.1), mtaddr.e6(14.3))]       # north of the grid
    return pts


def reference(reader: mtaddr.AddrReader, lon_e6: int, lat_e6: int) -> tuple:
    inside_grid = (reader.lon0 <= lon_e6 < reader.lon0 + reader.cols * reader.cell
                   and reader.lat0 <= lat_e6 < reader.lat0 + reader.rows * reader.cell)
    r = reader.lookup(lon_e6 / 1e6, lat_e6 / 1e6)
    return (1 if inside_grid else 0, r["province"] or "", r["city"] or "", r["barangay"] or "", r["road"] or "",
            -1 if r["road_m"] is None else int(math.floor(r["road_m"] + 0.5)), r["place"] or "",
            -1 if r["place_m"] is None else int(math.floor(r["place_m"] + 0.5)))


@unittest.skipIf(COMPILER is None, "no C compiler on this machine")
class AddrReaderTests(unittest.TestCase):
    def test_c_matches_reference(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "sample.addr"
            write_sample(path)
            binary = Path(temp) / "addr_tool"
            subprocess.run([COMPILER, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "dashboard/src"),
                            "-o", str(binary), str(ROOT / "tests/c/addr_tool.c"),
                            str(ROOT / "dashboard/src/addr.c"), "-lm"], check=True)
            pts = query_points()
            out = subprocess.run([str(binary), str(path)], input="".join("%d %d\n" % p for p in pts),
                                 capture_output=True, text=True, check=True).stdout.splitlines()
            reader = mtaddr.AddrReader(path)
        self.assertEqual(out[-1].split()[2:], ["0", "0"], "read errors or skipped shapes")
        found = {"road": 0, "barangay": 0, "place": 0}
        for p, line in zip(pts, out[:-1]):
            got = line.split("\t")
            want = reference(reader, *p)
            self.assertEqual(got[:5] + [got[6]], [str(v) for v in want[:5]] + [want[6]], p)
            for i in (5, 7):  # distances: C rounds a float that may differ in the last digit
                self.assertLessEqual(abs(int(got[i]) - want[i]), 1, p)
            found["road"] += bool(want[4])
            found["barangay"] += bool(want[3])
            found["place"] += bool(want[6])
        self.assertTrue(all(v > 100 for v in found.values()), found)  # the sample exercises every kind
        hole, second, overlap, west, north = [line.split("\t") for line in out[-6:-1]]
        self.assertEqual(hole[1:3], ["Rizal", ""])
        self.assertEqual(second[1:3], ["Laguna", "Antipolo"])
        self.assertEqual(overlap[2], "Parañaque")
        self.assertEqual(west[0], "0")
        self.assertEqual(north[0], "0")


REAL_FILE = ROOT / "dist" / "map" / "Map" / "luzon.addr"


@unittest.skipIf(COMPILER is None or not REAL_FILE.exists(), "no C compiler or no built luzon.addr")
class RealFileTests(unittest.TestCase):
    """The real Luzon file (tools/address/build_address.sh), when it has been built."""

    def test_c_matches_reference_on_luzon(self):
        rng = random.Random(3)
        pts = [(mtaddr.e6(120.5 + rng.random()), mtaddr.e6(14.0 + 2 * rng.random())) for _ in range(4000)]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "addr_tool"
            subprocess.run([COMPILER, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "dashboard/src"),
                            "-o", str(binary), str(ROOT / "tests/c/addr_tool.c"),
                            str(ROOT / "dashboard/src/addr.c"), "-lm"], check=True)
            out = subprocess.run([str(binary), str(REAL_FILE)], input="".join("%d %d\n" % p for p in pts),
                                 capture_output=True, text=True, check=True).stdout.splitlines()
        reader = mtaddr.AddrReader(REAL_FILE)
        self.assertEqual(out[-1].split()[2:], ["0", "0"], "read errors or skipped shapes")
        roads = 0
        for p, line in zip(pts, out[:-1]):
            got = line.split("\t")
            want = reference(reader, *p)
            self.assertEqual(got[:4] + [got[6]], [str(v) for v in want[:4]] + [want[6]], p)
            self.assertLessEqual(abs(int(got[5]) - want[5]), 1, p)
            if got[4] != want[4]:  # two roads at the same distance (a junction): either is right
                self.assertEqual(int(got[5]), want[5], p)
            roads += bool(want[4])
        self.assertGreater(roads, 200)


class FormatTests(unittest.TestCase):
    def test_roads_split_and_names_shared(self):
        with tempfile.TemporaryDirectory() as temp:
            w = mtaddr.AddrWriter(Path(temp) / "a.addr", BOUNDS)
            pieces = w.add_road("Long Road", [(121.0 + i * 0.001, 14.05) for i in range(100)])
            self.assertEqual(pieces, 4)  # 32 points each, sharing end points
            self.assertEqual(w.add_road("Dot", [(121.0, 14.0)]), 0)
            self.assertFalse(w.add_area(mtaddr.KIND_BARANGAY, "Tiny", [[(121.0, 14.0), (121.001, 14.0)]]))
            info = w.close()
            self.assertEqual(info["counts"][mtaddr.KIND_ROAD], 4)
            r = mtaddr.AddrReader(Path(temp) / "a.addr")
            self.assertEqual(r.lookup(121.05, 14.0503)["road"], "Long Road")
            self.assertEqual(r.names_size, len("Long Road") + 1)

    def test_rejects_bad_input(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError):
                mtaddr.AddrWriter(Path(temp) / "b.addr", (121.0, 14.0, 120.0, 14.1))
            with self.assertRaises(ValueError):
                mtaddr.AddrWriter(Path(temp) / "c.addr", BOUNDS, road_radius_m=20)
            w = mtaddr.AddrWriter(Path(temp) / "d.addr", BOUNDS)
            with self.assertRaises(ValueError):
                w.add_area(mtaddr.KIND_ROAD, "X", [square(121.0, 14.0, 121.1, 14.1)])
            with self.assertRaises(ValueError):
                w.add_place("", 121.0, 14.0, 1)


if __name__ == "__main__":
    unittest.main()
