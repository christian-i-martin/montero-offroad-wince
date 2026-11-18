"""The vector map pilot: tools/vmap/ (vector tile decoding, the .vmap format, style compiling,
clipping) and dashboard/src/vraster.c, vfont.c and vmap.c, compiled and run on the build machine.

When the Dasmarinas map has been built (python3 tools/vmap/build_vmap.py), the real file is drawn
too; set VMAP_PREVIEW_DIR to a new folder to keep the pictures as PNG files.
"""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "vmap"))
sys.path.insert(0, str(ROOT / "tools" / "map"))
import build_vmap  # noqa: E402
import mtpack  # noqa: E402
import mtvmap  # noqa: E402
import mvt  # noqa: E402

COMPILER = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
FLAGS = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror", "-I", str(ROOT / "dashboard/src")]
SOURCES = ["dashboard/src/vtiles.c", "dashboard/src/vmap.c", "dashboard/src/vicons.c", "dashboard/src/vraster.c", "dashboard/src/vfont.c", "dashboard/src/pngdec.c"]
FONT = ROOT / "dashboard" / "fonts" / "Barlow-Medium.ttf"
REAL_MAP = ROOT / "dist" / "vmap" / "VMap" / "dasmarinas.vmap"


def compile_c(main: str, binary: Path) -> None:
    subprocess.run([COMPILER] + FLAGS + ["-o", str(binary), str(ROOT / main)] + [str(ROOT / s) for s in SOURCES]
                   + ["-lm"], check=True)


def square(x0: int, y0: int, x1: int, y1: int):
    """A clockwise ring on the screen (y down): an outer ring in vector tiles."""
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


class VectorTileTests(unittest.TestCase):
    def test_round_trip(self):
        data = mvt.encode_tile({
            "water": [{"type": mvt.POLYGON, "properties": {"class": "lake"}, "parts": [square(0, 0, 100, 100)]}],
            "transportation": [{"type": mvt.LINE, "properties": {"class": "minor", "layer": 1},
                                "parts": [[(0, 5), (50, 5), (50, -20)]]}],
            "place": [{"type": mvt.POINT, "properties": {"name": "Dasmariñas", "rank": 3}, "parts": [[(7, 8)]]}],
        })
        tile = mvt.decode_tile(data)
        self.assertEqual(tile["water"]["features"][0]["parts"], [square(0, 0, 100, 100)])
        road = tile["transportation"]["features"][0]
        self.assertEqual(road["properties"], {"class": "minor", "layer": 1})
        self.assertEqual(road["parts"], [[(0, 5), (50, 5), (50, -20)]])
        self.assertEqual(tile["place"]["features"][0]["properties"]["name"], "Dasmariñas")
        self.assertEqual(tile["place"]["extent"], 4096)


class FormatTests(unittest.TestCase):
    def test_tile_round_trip(self):
        feats = [mtvmap.Feature(mtvmap.FEATURE_AREA, [square(0, 0, 10, 10)]),
                 mtvmap.Feature(mtvmap.FEATURE_POINT, [[(5, 6)]], "Burol", 7),
                 mtvmap.Feature(mtvmap.FEATURE_LINE, [[(0, 0), (40000, -40000)]], "Burol")]
        raw = mtvmap.encode_tile(feats, {3: [0], 1: [1, 2]})
        back, draws = mtvmap.decode_tile(raw)
        self.assertEqual(draws, {1: [1, 2], 3: [0]})
        self.assertEqual(back[0].parts, [square(0, 0, 10, 10)])
        self.assertEqual((back[1].text, back[1].rank), ("Burol", 7))
        self.assertEqual(back[2].parts, [[(0, 0), (32767, -32768)]], "points are clamped to 16 bits")

    def test_file_round_trip(self):
        zooms = 9
        layer = mtvmap.Layer("x", mtvmap.LINE, 10, 99, (0x123456, 0x654321), opacity=0.5, dashes=[2, 1],
                             size=[float(k) for k in range(zooms)])
        level = mtvmap.Level(14, 100, 200, 2, 1, 14, 99)
        level.tiles[(101, 200)] = mtvmap.encode_tile([mtvmap.Feature(mtvmap.FEATURE_POINT, [[(1, 2)]], "a")], {0: [0]})
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "t.vmap"
            mtvmap.write(path, [layer], [level], (1, 2), (120.0, 14.0, 121.0, 15.0), 8.0, zooms)
            r = mtvmap.Reader(path)
            self.assertEqual(r.land, (1, 2))
            self.assertEqual(r.bounds, (120.0, 14.0, 121.0, 15.0))
            self.assertEqual(r.layers[0]["colour"], (0x123456, 0x654321))
            self.assertEqual(r.layers[0]["dashes"], [2.0, 1.0])
            self.assertEqual(r.layers[0]["size"][3], 3.0)
            self.assertEqual(r.layers[0]["opacity"], 0.5)
            self.assertIsNone(r.tile(0, 100, 200))
            self.assertEqual(mtvmap.decode_tile(r.tile(0, 101, 200))[0][0].text, "a")


class StyleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rules, cls.land = build_vmap.compile_style()
        cls.by_name = {r.layer.name: r.layer for r in cls.rules}
        cls.zooms = build_vmap.zoom_table()

    def size_at(self, name: str, zoom: float, which: str = "size") -> float:
        return getattr(self.by_name[name], which)[self.zooms.index(zoom)]

    def test_colours_come_from_both_palettes(self):
        sys.path.insert(0, str(ROOT / "tools" / "map"))
        import make_style
        self.assertEqual(self.land, (int(make_style.DARK["LAND"][1:], 16), int(make_style.LIGHT["LAND"][1:], 16)))
        self.assertEqual(self.by_name["road-motorway"].colour,
                         (int(make_style.DARK["MOTORWAY"][1:], 16), int(make_style.LIGHT["MOTORWAY"][1:], 16)))

    def test_widths_follow_the_style_and_grow_past_zoom_16(self):
        self.assertAlmostEqual(self.size_at("road-minor", 15.0), 2.8, places=2)
        self.assertAlmostEqual(self.size_at("road-minor", 16.0), 5.6, places=2)  # large print: twice zoom 15
        self.assertAlmostEqual(self.size_at("road-minor", 17.0), 5.6 * build_vmap.GROW_RATE, places=2)
        widths = self.by_name["road-primary"].size
        self.assertTrue(all(a <= b for a, b in zip(widths, widths[1:])))

    def test_large_print_labels_are_folded_in(self):
        self.assertNotIn("road-name-minor-large", self.by_name)
        self.assertEqual(self.size_at("road-name-minor", 15.0), 12)
        self.assertEqual(self.size_at("road-name-minor", 15.5), 18)
        self.assertEqual(self.size_at("road-name-minor", 16.0), 24)
        self.assertEqual(self.by_name["road-name-minor"].kind, mtvmap.LABEL_LINE)
        self.assertEqual(self.by_name["place-town"].kind, mtvmap.LABEL_POINT)

    def test_expressions(self):
        ev = build_vmap.evaluate
        self.assertTrue(ev(["match", ["get", "class"], ["a", "b"], True, False], {"class": "b"}))
        self.assertEqual(ev(["coalesce", ["get", "name:latin"], ["get", "name"]], {"name": "X"}), "X")
        self.assertEqual(ev(["concat", ["get", "n"], " ", ["to-string", ["get", "ele"]], " m"],
                            {"n": "Peak", "ele": 512.0}), "Peak 512 m")
        self.assertEqual(ev(["step", ["zoom"], 1, 10, 2], {}, 12), 2)


class GeometryTests(unittest.TestCase):
    def test_polygon_is_cut_to_the_tile(self):
        ring = build_vmap.clip_polygon(square(-100, 100, 200, 300), 0, 4096)
        self.assertEqual(sorted(ring), sorted(square(0, 100, 200, 300)))
        self.assertEqual(build_vmap.clip_polygon(square(5000, 0, 6000, 10), 0, 4096), [])

    def test_line_is_cut_into_pieces(self):
        pieces = build_vmap.clip_line([(-10, 10), (10, 10), (10, -10), (20, -10), (20, 10), (30, 10)], 0, 4096)
        self.assertEqual(pieces, [[(0, 10), (10, 10), (10, 0)], [(20, 0), (20, 10), (30, 10)]])

    def test_outline_leaves_out_the_cut_edges(self):
        ring = build_vmap.clip_polygon(square(-100, 100, 200, 300), 0, 4096)
        lines = build_vmap.outline(ring, 0, 4096)
        flat = [p for line in lines for p in line]
        self.assertTrue(all(len(line) >= 2 for line in lines))
        for a, b in zip(flat, flat[1:]):
            self.assertFalse(a[0] == b[0] == 0, "a stretch along the tile's edge")
        self.assertEqual(build_vmap.outline(square(10, 10, 20, 20), 0, 4096),
                         [square(10, 10, 20, 20) + [(10, 10)]])

    def test_buckets_keep_holes_with_their_ring(self):
        outer, hole = square(100, 100, 300, 300), list(reversed(square(150, 150, 200, 200)))
        far = square(3000, 3000, 3100, 3100)
        groups = build_vmap.bucketed([outer, hole, far], True)
        self.assertEqual(groups, [[outer, hole], [far]])


class BuildTests(unittest.TestCase):
    def test_tile_from_vector_tile(self):
        rules, _ = build_vmap.compile_style()
        layers = mvt.decode_tile(mvt.encode_tile({
            "transportation": [{"type": mvt.LINE, "properties": {"class": "motorway"},
                                "parts": [[(-200, 2000), (5000, 2000)]]}],
            "place": [{"type": mvt.POINT, "properties": {"class": "city", "name": "Dasmariñas", "rank": 2},
                       "parts": [[(100, 100)]]},
                      {"type": mvt.POINT, "properties": {"class": "city", "name": "Elsewhere"},
                       "parts": [[(-50, 100)]]}],
        }))
        stats: dict = {}
        raw = build_vmap.build_tile(layers, rules, 14.0, 99.0, stats)
        feats, draws = mtvmap.decode_tile(raw)
        names = [f.text for f in feats if f.text]
        self.assertEqual(names, ["Dasmariñas"], "points outside the tile belong to the next one")
        road = [f for f in feats if f.type == mtvmap.FEATURE_LINE]
        self.assertEqual(len(road), 1, "one shape for the casing and the road")
        self.assertEqual(road[0].parts, [[(0, 2000), (4096, 2000)]])
        index = {r.layer.name: k for k, r in enumerate(rules)}
        self.assertIn(index["casing-motorway"], draws)
        self.assertEqual(draws[index["casing-motorway"]], draws[index["road-motorway"]])

    def test_place_markers(self):
        rules, _ = build_vmap.compile_style()
        names = [r.layer.name for r in rules]
        self.assertNotIn("fuel", names, "the fuel dot is replaced by its marker")
        self.assertLess(names.index("peak-name"), names.index("marker-fuel"))
        self.assertLess(names.index("marker-fuel"), names.index("place-island"), "places are placed before markers")
        layers = mvt.decode_tile(mvt.encode_tile({"poi": [
            {"type": mvt.POINT, "properties": {"class": "fuel", "subclass": "fuel", "name": "Petron"},
             "parts": [[(100, 100)]]},
            {"type": mvt.POINT, "properties": {"class": "parking", "subclass": "parking"}, "parts": [[(200, 100)]]},
            {"type": mvt.POINT, "properties": {"class": "shop", "subclass": "mall", "name": "SM"},
             "parts": [[(300, 100)]]},
            {"type": mvt.POINT, "properties": {"class": "shop", "subclass": "bakery", "name": "Bread"},
             "parts": [[(400, 100)]]},
        ]}))
        feats, draws = mtvmap.decode_tile(build_vmap.build_tile(layers, rules, 14.0, 99.0, {}))
        index = {r.layer.name: k for k, r in enumerate(rules)}
        by_rule = {name: [feats[i] for i in draws.get(index[name], [])] for name in index}
        self.assertEqual([f.text for f in by_rule["marker-fuel"]], ["Petron"])
        self.assertEqual([f.text for f in by_rule["marker-parking"]], [None], "markers need no name")
        self.assertEqual([f.text for f in by_rule["marker-mall"]], ["SM"])
        self.assertFalse(any(f.text == "Bread" for f in feats), "bakeries get no marker")
        fuel = rules[index["marker-fuel"]].layer
        self.assertEqual((fuel.kind, fuel.icon), (mtvmap.MARKER, build_vmap.FUEL))
        zooms = build_vmap.zoom_table()
        self.assertEqual(fuel.size[zooms.index(15.5)], 0.0)
        self.assertEqual(fuel.size[zooms.index(16.0)], 24.0)
        self.assertEqual(rules[index["marker-parking"]].layer.size[zooms.index(16.5)], 0.0)

    def test_more_markers(self):
        rules, _ = build_vmap.compile_style()
        index = {r.layer.name: k for k, r in enumerate(rules)}
        cases = [("place_of_worship", "christian", "marker-church"), ("college", "university", "marker-school"),
                 ("town_hall", "community_centre", "marker-town-hall"), ("toll_booth", "toll_booth", "marker-toll"),
                 ("picnic_site", "picnic_site", "marker-campsite"), ("shop", "hardware", "marker-hardware"),
                 ("museum", "museum", "marker-attraction"), ("drinking_water", "drinking_water", "marker-water")]
        for cls, sub, rule in cases:
            props = {"class": cls, "subclass": sub}
            matched = [r.layer.name for r in rules if r.source == "poi" and r.filter is not None
                       and build_vmap.evaluate(r.filter, props)]
            self.assertEqual(matched, [rule], (cls, sub))
        self.assertEqual(rules[index["marker-toll"]].layer.first_zoom, 16.0, "car park pay booths are toll booths too")
        icons = [r.layer.icon for r in rules if r.layer.kind == mtvmap.MARKER]
        self.assertEqual(sorted(set(icons)), list(range(28)), "every pictogram is used")

    def test_refuses_the_picture_map_folder(self):
        code = build_vmap.main(["--output", str(ROOT / "dist" / "map" / "Map")])
        self.assertEqual(code, 2)


@unittest.skipUnless(COMPILER, "no C compiler")
class RendererTests(unittest.TestCase):
    def write_test_map(self, path: Path) -> None:
        zooms = build_vmap.zoom_table()
        n = len(zooms)
        layers = [
            mtvmap.Layer("area", mtvmap.AREA, 0, 99, (0xC82828, 0xC82828), size=[0.0] * n),
            mtvmap.Layer("line", mtvmap.LINE, 0, 99, (0x2040F0, 0x2040F0), size=[4.0] * n),
            mtvmap.Layer("label", mtvmap.LABEL_POINT, 0, 99, (0xFFFFFF, 0xFFFFFF), (0x000000, 0x000000),
                         max_width=8, size=[14.0] * n, size2=[1.5] * n),
            mtvmap.Layer("marker", mtvmap.MARKER, 0, 99, (0x1E6FD9, 0x1E6FD9), (0x1E6FD9, 0x1E6FD9),
                         anchor=mtvmap.ANCHOR_LEFT, max_width=8, size=[0.0] * n, size2=[0.0] * n, icon=1),
        ]
        level = mtvmap.Level(14, 13696, 7533, 1, 1, 14, 99)
        level.tiles[(13696, 7533)] = mtvmap.encode_tile([
            mtvmap.Feature(mtvmap.FEATURE_AREA, [square(1024, 1024, 3072, 3072)]),
            mtvmap.Feature(mtvmap.FEATURE_LINE, [[(0, 2048), (4096, 2048)]]),
            mtvmap.Feature(mtvmap.FEATURE_POINT, [[(2048, 2048)]], "Test"),
            mtvmap.Feature(mtvmap.FEATURE_POINT, [[(1536, 3584)]]),
        ], {0: [0], 1: [1], 2: [2], 3: [3]})
        mtvmap.write(path, layers, [level], (0x000000, 0x00C000), (120.9, 14.3, 121.0, 14.4), zooms[0], n)

    def test_selftest(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "vmap_selftest"
            compile_c("tests/c/vmap_selftest.c", binary)
            path = Path(temp) / "test.vmap"
            self.write_test_map(path)
            result = subprocess.run([str(binary), str(path), str(FONT)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("ok", result.stdout)

    @unittest.skipUnless(REAL_MAP.exists(), "run python3 tools/vmap/build_vmap.py first")
    def test_real_map_previews(self):
        views = ["16,0,1", "16,0,0,1", "14,0,1", "17,35,1", "18.5,0,1", "15.5,120,0,1"]
        keep = os.environ.get("VMAP_PREVIEW_DIR")
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "vmap_preview"
            compile_c("tests/c/vmap_preview.c", binary)
            prefix = Path(temp) / "view"
            out = subprocess.run([str(binary), str(FONT), str(REAL_MAP), str(prefix)] + views,
                                 capture_output=True, text=True, check=True).stdout
            lines = out.strip().splitlines()
            self.assertEqual(len(lines), len(views))
            for line in lines:
                self.assertIn(": drawn,", line)
                placed = int(line.rsplit(", ", 1)[1].split(" of ")[0])
                self.assertGreater(placed, 0, line)
            bench = Path(temp) / "vtiles_bench"
            compile_c("tests/c/vtiles_bench.c", bench)
            out = subprocess.run([str(bench), str(FONT), str(REAL_MAP), str(Path(temp) / "pic"),
                                  "16,0,1", "14,0,0", "17.5,30,1"], capture_output=True, text=True, check=True).stdout
            lines = [line for line in out.strip().splitlines() if "pixels differ" in line]
            self.assertEqual(len(lines), 3)
            self.assertIn("0 missing", lines[0])
            self.assertTrue(lines[0].endswith(" 0.00% pixels differ"), lines[0])
            self.assertTrue(lines[1].endswith(" 0.00% pixels differ"), lines[1])
            turned_differ = float(lines[2].rsplit(" ", 3)[1].rstrip("%"))
            self.assertLess(turned_differ, 6.0, lines[2])
            if keep:
                folder = Path(keep)
                folder.mkdir(parents=True, exist_ok=False)
                for k in range(len(views)):
                    rgb = (Path(temp) / ("view-%d.rgb" % k)).read_bytes()
                    (folder / ("view-%d.png" % k)).write_bytes(mtpack.encode_png(800, 480, rgb))


@unittest.skipUnless(COMPILER, "no C compiler")
class DashboardMapTests(unittest.TestCase):
    """The dashboard (0.9) draws the vector map: its previews with the real Dasmarinas file."""

    @unittest.skipUnless(REAL_MAP.exists(), "run python3 tools/vmap/build_vmap.py first")
    def test_dashboard_preview_with_vector_map(self):
        dash = ["dashboard/src/model.c", "dashboard/src/render.c", "dashboard/src/mapview.c", "dashboard/src/sun.c",
                "dashboard/src/addr.c", "dashboard/src/holiday.c", "dashboard/src/island.c", "probe/src/nmea.c",
                "probe/src/carinfo.c"]
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "dashboard_preview"
            subprocess.run([COMPILER] + FLAGS + ["-I", str(ROOT / "probe/src"), "-o", str(binary),
                            str(ROOT / "tests/c/dashboard_preview.c")] + [str(ROOT / s) for s in dash + SOURCES]
                           + ["-lm"], check=True)
            out = Path(temp) / "previews"
            out.mkdir()
            keep = os.environ.get("VMAP_PREVIEW_DIR")
            result = subprocess.run([str(binary), str(ROOT / "dashboard/fonts"), str(out), str(REAL_MAP.parent),
                                     "14.3010", "120.9555"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            svg = "{http://www.w3.org/2000/svg}"
            for name, has_map in (("driving.svg", True), ("mappage.svg", True), ("parked-night.svg", True),
                                  ("startup.svg", False)):
                root = ET.parse(out / name).getroot()
                images = [i for i in root.iter(svg + "image")]
                texts = " ".join(t.text or "" for t in root.iter(svg + "text"))
                self.assertEqual(len(images) == 1, has_map, name)
                if has_map:
                    self.assertTrue(images[0].get("href").startswith("data:image/png;base64,"))
                    self.assertIn("OpenStreetMap", texts)
                    self.assertNotIn("No map for this area", texts)
                else:
                    self.assertIn("Waiting for GPS", texts)
            if keep:
                folder = Path(keep) / "dashboard"
                folder.mkdir(parents=True, exist_ok=False)
                for svg_file in out.iterdir():
                    shutil.copy(svg_file, folder / svg_file.name)


if __name__ == "__main__":
    unittest.main()
