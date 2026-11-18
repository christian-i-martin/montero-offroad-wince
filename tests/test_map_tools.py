"""Offline tests of the map tools in tools/map/: no network, no Docker.

A fake tile source stands in for the tile server: tiles that touch a small "island" box are
patterned (not one colour), all others are one-colour sea.
"""
import contextlib
import importlib.util
import io
import json
import re
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest
import zlib

MAP_DIR = Path(__file__).resolve().parents[1] / "tools" / "map"


def load(name):
    spec = importlib.util.spec_from_file_location(name, MAP_DIR / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


mtpack = load("mtpack")
build_map = load("build_map")
make_style = load("make_style")

SEA = 0x0E2C42
FOREST = 0x1B2B23


def png_with_filter(width, height, rows, unit, filter_type, colour_type=2, depth=8,
                    palette=None):
    """PNG whose rows are all stored with one filter type (rows: raw pixel bytes per row)."""
    out = b""
    prior = bytes(len(rows[0]))
    for raw in rows:
        line = bytearray()
        for i, value in enumerate(raw):
            left = raw[i - unit] if i >= unit else 0
            up = prior[i]
            up_left = prior[i - unit] if i >= unit else 0
            if filter_type == 0:
                pred = 0
            elif filter_type == 1:
                pred = left
            elif filter_type == 2:
                pred = up
            elif filter_type == 3:
                pred = (left + up) // 2
            else:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                pred = left if pa <= pb and pa <= pc else up if pb <= pc else up_left
            line.append((value - pred) & 0xFF)
        out += bytes([filter_type]) + bytes(line)
        prior = raw

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth,
                                                          colour_type, 0, 0, 0))
    if palette:
        png += chunk(b"PLTE", palette)
    return png + chunk(b"IDAT", zlib.compress(out)) + chunk(b"IEND", b"")


def land_png(seed):
    """A small two-colour picture that differs per seed."""
    pixels = bytearray(b"\x20\x30\x20" * 64)
    pixels[3 * (seed % 64):3 * (seed % 64) + 3] = bytes((seed & 0xFF, (seed >> 8) & 0xFF, 0x99))
    return mtpack.encode_png(8, 8, bytes(pixels))


class FakeSource:
    """Island tiles are patterned, the rest is sea. Counts fetches; can fail after a number."""

    def __init__(self, island=(121.30, 14.52, 121.31, 14.53), fail_after=None,
                 forest=None):
        self.island = island
        self.forest = forest
        self.fail_after = fail_after
        self.calls = []
        self.lock = threading.Lock()

    @staticmethod
    def _touches(box, zoom, x, y):
        w, s, e, n = build_map.tile_bounds(zoom, x, y)
        return w < box[2] and box[0] < e and s < box[3] and box[1] < n

    def __call__(self, zoom, x, y):
        with self.lock:
            if self.fail_after is not None and len(self.calls) >= self.fail_after:
                raise build_map.TileError("fake server stopped")
            self.calls.append((zoom, x, y))
        if self._touches(self.island, zoom, x, y):
            return land_png(zoom * 100000 + x * 7 + y)
        if self.forest and self._touches(self.forest, zoom, x, y):
            return mtpack.solid_png(FOREST)
        return mtpack.solid_png(SEA)


class PackFormatTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def test_round_trip_with_shared_tiles(self):
        path = self.root / "a.pack"
        sea = mtpack.solid_png(SEA)
        with mtpack.PackWriter(path, 10, 850, 470, 3, 2, background=SEA) as w:
            w.add(850, 470, sea)
            w.add(852, 471, sea)
            w.add(851, 470, land_png(1))
        self.assertEqual((w.tiles, w.unique), (3, 2))
        r = mtpack.PackReader(path)
        self.assertEqual((r.zoom, r.x0, r.y0, r.cols, r.rows, r.background),
                         (10, 850, 470, 3, 2, SEA))
        self.assertEqual(r.tile(850, 470), sea)
        self.assertEqual(r.tile(851, 470), land_png(1))
        self.assertEqual(r.entry(850, 470), r.entry(852, 471))
        self.assertIsNone(r.tile(850, 471))
        self.assertIsNone(r.tile(853, 470))
        self.assertEqual(path.stat().st_size, w.size)
        with open(path, "rb") as f:
            head = f.read(64)
        self.assertEqual(head[:4], b"MTPK")
        self.assertEqual(struct.unpack_from("<I", head, 36)[0], 64)

    def test_pack_full(self):
        path = self.root / "b.pack"
        limit = 64 + 8 * 2 + len(land_png(1)) + 10
        with mtpack.PackWriter(path, 5, 0, 0, 2, 1, max_bytes=limit) as w:
            w.add(0, 0, land_png(1))
            with self.assertRaises(mtpack.PackFull):
                w.add(1, 0, land_png(2))


class TileMathTests(unittest.TestCase):
    def test_known_tiles(self):
        self.assertEqual(build_map.lonlat_to_tile(0.0, 0.0, 1), (1, 1))
        self.assertEqual(build_map.lonlat_to_tile(-180.0, 85.0511, 3), (0, 0))
        self.assertEqual(build_map.lonlat_to_tile(180.0, -85.0511, 3), (7, 7))
        self.assertEqual(build_map.lonlat_to_tile(-0.0001, 0.0001, 1), (0, 0))
        # Manila city hall; checked against the tile formula by hand.
        self.assertEqual(build_map.lonlat_to_tile(120.9817, 14.5896, 14), (13698, 7520))

    def test_tile_bounds_round_trip(self):
        for zoom, x, y in ((8, 214, 117), (14, 13698, 7520), (15, 27412, 15060)):
            w, s, e, n = build_map.tile_bounds(zoom, x, y)
            self.assertTrue(w < e and s < n)
            self.assertEqual(build_map.lonlat_to_tile((w + e) / 2, (s + n) / 2, zoom), (x, y))

    def test_range_edges(self):
        self.assertEqual(build_map.tile_range((-180, -85.06, 180, 85.06), 2), (0, 0, 3, 3))
        # A maximum longitude exactly on a tile border does not add the next column.
        w, s, e, n = build_map.tile_bounds(10, 856, 470)
        self.assertEqual(build_map.tile_range((w, s, e, n), 10), (856, 470, 856, 470))
        rng = build_map.tile_range((121.28, 14.50, 121.42, 14.62), 14)
        self.assertEqual(build_map.range_size(rng), 42)

    def test_parse_bounds(self):
        self.assertEqual(build_map.parse_bounds("121.28, 14.5,121.42,14.62"),
                         (121.28, 14.5, 121.42, 14.62))
        for bad in ("1,2,3", "121.4,14.5,121.3,14.6", "a,b,c,d"):
            with self.assertRaises(ValueError):
                build_map.parse_bounds(bad)

    def test_luzon_default(self):
        self.assertEqual(build_map.parse_args([]).bounds, (119.4, 12.4, 124.5, 18.8))
        x0, y0, x1, y1 = build_map.tile_range(build_map.LUZON, 14)
        self.assertEqual((x1 - x0 + 1, y1 - y0 + 1), (233, 304))

    def test_bounds_inside(self):
        self.assertTrue(build_map.bounds_inside((121.28, 14.5, 121.42, 14.62), build_map.LUZON))
        self.assertTrue(build_map.bounds_inside(build_map.LUZON, build_map.LUZON))
        self.assertFalse(build_map.bounds_inside((123.0, 10.0, 124.0, 11.0), build_map.LUZON))


class UniformPngTests(unittest.TestCase):
    def test_solid_palette_tile(self):
        self.assertEqual(build_map.png_uniform_colour(mtpack.solid_png(SEA)), SEA)

    def test_every_filter_type(self):
        width, height = 5, 4
        for colour_type, pixel in ((2, b"\x0e\x2c\x42"), (6, b"\x0e\x2c\x42\xff")):
            unit = len(pixel)
            rows = [pixel * width] * height
            for f in range(5):
                png = png_with_filter(width, height, rows, unit, f, colour_type)
                self.assertEqual(build_map.png_uniform_colour(png), SEA, (colour_type, f))
                odd = [bytearray(r) for r in rows]
                odd[2][unit * 3] ^= 1
                png = png_with_filter(width, height, [bytes(r) for r in odd], unit, f,
                                      colour_type)
                self.assertIsNone(build_map.png_uniform_colour(png), (colour_type, f))

    def test_two_bit_palette(self):
        palette = b"\x00\x00\x00" + bytes((0x0E, 0x2C, 0x42)) + b"\xff\xff\xff" * 2
        rows = [b"\x55\x55"] * 3           # index 1 in every 2-bit sample, 8 pixels wide
        png = png_with_filter(8, 3, rows, 1, 1, colour_type=3, depth=2, palette=palette)
        self.assertEqual(build_map.png_uniform_colour(png), SEA)
        rows = [b"\x55\x56"] + [b"\x55\x55"] * 2
        png = png_with_filter(8, 3, rows, 1, 0, colour_type=3, depth=2, palette=palette)
        self.assertIsNone(build_map.png_uniform_colour(png))

    def test_not_uniform_or_not_png(self):
        self.assertIsNone(build_map.png_uniform_colour(land_png(3)))
        self.assertIsNone(build_map.png_uniform_colour(b"GIF89a"))
        self.assertIsNone(build_map.png_uniform_colour(mtpack.solid_png(SEA)[:40]))

    def test_colour_tolerance(self):
        self.assertTrue(build_map.colours_close(SEA, SEA + 0x030302))
        self.assertFalse(build_map.colours_close(SEA, FOREST))


class BandTests(unittest.TestCase):
    def test_bands_fit_and_cover(self):
        rows = 4
        # Each column: four different 100-byte tiles plus one shared tile.
        columns = [(x, [(b"c%d-%d" % (x, y), 100) for y in range(3)] + [(b"shared", 50)])
                   for x in range(10, 20)]
        limit = 64 + 8 * rows * 3 + 3 * 300 + 50
        bands = build_map.plan_bands(columns, rows, limit)
        self.assertEqual(bands, [(10, 3), (13, 3), (16, 3), (19, 1)])
        for start, cols in bands:
            self.assertLessEqual(64 + 8 * rows * cols + 300 * cols + 50, limit)

    def test_shared_tiles_counted_once(self):
        columns = [(x, [(b"sea", 1000)] * 5) for x in range(50)]
        self.assertEqual(build_map.plan_bands(columns, 5, 64 + 8 * 250 + 1000), [(0, 50)])

    def test_column_too_big(self):
        with self.assertRaises(ValueError):
            build_map.plan_bands([(0, [(b"a", 5000)])], 1, 1000)

    def test_gap_is_an_error(self):
        with self.assertRaises(ValueError):
            build_map.plan_bands([(0, [(b"a", 1)]), (2, [(b"b", 1)])], 1, 1000)


class ShortcutTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def render(self, source, zooms, bounds, sea=SEA, from_zoom=10):
        cache = build_map.TileCache(self.root / "cache.sqlite")
        self.addCleanup(cache.close)
        renderer = build_map.Renderer(cache, source, workers=3, sea_colour=sea,
                                      shortcut_from_zoom=from_zoom, log=lambda s: None)
        return cache, [renderer.render_zoom(z, build_map.tile_range(bounds, z)) for z in zooms]

    def test_sea_children_are_not_rendered(self):
        source = FakeSource()
        bounds = (121.28, 14.50, 121.42, 14.62)
        cache, stats = self.render(source, [10, 11, 12, 13, 14], bounds)
        by_zoom = {s.zoom: s for s in stats}
        self.assertEqual(by_zoom[10].shortcut, 0)        # shortcut starts below --from zoom
        self.assertGreater(by_zoom[14].shortcut, 0)
        for s in stats:
            self.assertEqual(s.rendered + s.shortcut, s.total)
            fetched = [c for c in source.calls if c[0] == s.zoom]
            self.assertEqual(len(fetched), s.rendered)
        # Island tiles along the top row were really rendered.
        for s in stats:
            rng = build_map.tile_range(bounds, s.zoom)
            for x in range(rng[0], rng[2] + 1):
                if FakeSource._touches(source.island, s.zoom, x, rng[1]):
                    self.assertIn((s.zoom, x, rng[1]), source.calls)

    def test_only_sea_colour_counts(self):
        forest_everywhere = (100.0, 0.0, 140.0, 30.0)
        source = FakeSource(island=(0.0, 0.0, 0.001, 0.001), forest=forest_everywhere)
        bounds = (121.28, 14.50, 121.42, 14.62)
        _, stats = self.render(source, [11, 12, 13], bounds)
        self.assertEqual(sum(s.shortcut for s in stats), 0)
        self.assertEqual(len(source.calls), sum(s.total for s in stats))

    def test_switch_off(self):
        source = FakeSource()
        bounds = (121.28, 14.50, 121.42, 14.62)
        _, stats = self.render(source, [11, 12, 13], bounds, sea=None)
        self.assertEqual(sum(s.shortcut for s in stats), 0)
        self.assertEqual(len(source.calls), sum(s.total for s in stats))


class PackerTests(unittest.TestCase):
    BOUNDS = "121.28,14.50,121.42,14.62"

    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def args(self, *extra):
        return build_map.parse_args([
            "--bounds", self.BOUNDS, "--min-zoom", "8", "--max-zoom", "15",
            "--output", str(self.root / "Map"), "--cache-dir", str(self.root / "cache"),
            "--sea-colour", "0E2C42", "--shortcut-from-zoom", "11", "--workers", "4"]
            + list(extra))

    def run_build(self, source, *extra):
        out = io.StringIO()
        stats = build_map.build(self.args(*extra), fetch=source, log=out.write)
        return stats, out.getvalue()

    def test_packs_hold_every_tile(self):
        source = FakeSource()
        stats, text = self.run_build(source)
        self.assertIn("Total", text)
        packs = sorted((self.root / "Map").glob("*.pack"))
        self.assertEqual(len(packs), 8)
        for path in packs:
            r = mtpack.PackReader(path)
            self.assertEqual(r.background, SEA)
            for x in range(r.x0, r.x0 + r.cols):
                for y in range(r.y0, r.y0 + r.rows):
                    tile = r.tile(x, y)
                    self.assertTrue(tile and tile.startswith(b"\x89PNG"))
                    if FakeSource._touches(source.island, r.zoom, x, y):
                        self.assertEqual(tile, land_png(r.zoom * 100000 + x * 7 + y))
                    else:
                        self.assertEqual(build_map.png_uniform_colour(tile), SEA)
            self.assertTrue(path.name.startswith("ph-z%02d-%d-%d-" % (r.zoom, r.x0, r.y0)))
        self.assertFalse(list((self.root / "Map").glob("*.part")))

    def test_rerun_uses_cache(self):
        first = FakeSource()
        self.run_build(first)
        second = FakeSource()
        stats, _ = self.run_build(second)
        self.assertEqual(second.calls, [])
        self.assertEqual(sum(s.cached for s in stats), sum(s.total for s in stats))

    def test_interrupted_run_continues(self):
        broken = FakeSource(fail_after=20)
        with self.assertRaises(build_map.TileError):
            self.run_build(broken)
        resumed = FakeSource()
        self.run_build(resumed)
        self.assertFalse(set(broken.calls) & set(resumed.calls))
        self.assertEqual(len((list((self.root / "Map").glob("*.pack")))), 8)

    def test_small_limit_splits_into_bands(self):
        source = FakeSource(island=(121.28, 14.50, 121.42, 14.62))
        self.run_build(source, "--min-zoom", "15", "--max-pack-bytes", "5000")
        packs = sorted((self.root / "Map").glob("*.pack"))
        self.assertGreater(len(packs), 1)
        columns = []
        for path in packs:
            r = mtpack.PackReader(path)
            self.assertLessEqual(path.stat().st_size, 5000)
            columns += range(r.x0, r.x0 + r.cols)
        rng = build_map.tile_range(build_map.parse_bounds(self.BOUNDS), 15)
        self.assertEqual(sorted(columns), list(range(rng[0], rng[2] + 1)))

    def test_new_style_starts_new_cache(self):
        style = self.root / "style.json"
        style.write_text("{}")
        self.run_build(FakeSource(), "--cache-key-file", str(style))
        style.write_text('{"changed": 1}')
        again = FakeSource()
        self.run_build(again, "--cache-key-file", str(style))
        self.assertTrue(again.calls)
        self.assertEqual(len(list((self.root / "cache").glob("tiles-*.sqlite"))), 2)

    def test_overlap_note(self):
        self.run_build(FakeSource())
        self.run_build(FakeSource(), "--name", "other")
        _, text = self.run_build(FakeSource(), "--name", "third")
        self.assertIn("overlaps", text)

    def test_area_outside_vector_tiles(self):
        source = FakeSource()
        with self.assertRaises(ValueError):
            self.run_build(source, "--data-bounds", "123,10,124,11")
        self.assertEqual(source.calls, [])
        self.run_build(source, "--data-bounds", "119.4,12.4,124.5,18.8")
        self.assertTrue(source.calls)

    def test_only_local_servers(self):
        for url in ("https://tile.openstreetmap.org/{z}/{x}/{y}.png",
                    "http://a.tile.example.org/{z}/{x}/{y}.png"):
            with self.assertRaises(ValueError):
                build_map.HttpTileSource(url)
        build_map.HttpTileSource(build_map.DEFAULT_URL)

    def test_count(self):
        out = io.StringIO()
        total = build_map.count_tiles(build_map.parse_bounds(self.BOUNDS), range(8, 16),
                                      log=out.write)
        self.assertEqual(total, 234)

    def test_zoom_16_is_the_limit(self):
        self.assertEqual(self.args("--min-zoom", "16", "--max-zoom", "16").max_zoom, 16)
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            self.args("--max-zoom", "17")


class StyleTests(unittest.TestCase):
    def setUp(self):
        self.style = json.loads(make_style.STYLE_PATH.read_text(encoding="utf-8"))

    def test_committed_style_matches_generator(self):
        self.assertEqual(make_style.STYLE_PATH.read_text(encoding="utf-8"), make_style.render())

    def test_sea_colour_comes_from_style(self):
        self.assertEqual(build_map.style_water_colour(make_style.STYLE_PATH),
                         int(make_style.WATER[1:], 16))
        self.assertEqual(build_map.style_water_colour(make_style.STYLE_PATH), SEA)

    def test_attribution_and_fonts(self):
        source = self.style["sources"]["openmaptiles"]
        self.assertIn("OpenStreetMap contributors", source["attribution"])
        self.assertIn("OpenMapTiles", source["attribution"])
        for item in self.style["layers"]:
            font = item.get("layout", {}).get("text-font")
            if font is not None:
                self.assertEqual(font, ["Noto Sans Regular"], item["id"])

    def test_text_at_least_12_px(self):
        def numbers(expr):
            if isinstance(expr, (int, float)):
                return [expr]
            if isinstance(expr, list) and expr and expr[0] == "interpolate":
                return [v for v in expr[4::2] if isinstance(v, (int, float))]
            return []
        for item in self.style["layers"]:
            size = item.get("layout", {}).get("text-size")
            if size is not None:
                self.assertTrue(numbers(size), item["id"])
                self.assertGreaterEqual(min(numbers(size)), 12, item["id"])

    def test_tracks_dashed_and_paths_dotted(self):
        layers = {item["id"]: item for item in self.style["layers"]}
        self.assertIn("line-dasharray", layers["track"]["paint"])
        self.assertIn("line-dasharray", layers["path"]["paint"])
        self.assertLess(layers["path"]["paint"]["line-dasharray"][0], 0.5)

    def test_large_print_at_zoom_16(self):
        """Zoom 16 (style zoom 15): every line and dot twice its zoom 15 size, every label layer
        seen there a copy of its zoom 15 layer with twice the text and halo."""
        z15, z16 = make_style.sz(15), make_style.sz(16)
        layers = {item["id"]: item for item in self.style["layers"]}
        for item in self.style["layers"]:
            for key in ("line-width", "circle-radius", "circle-stroke-width"):
                value = item["paint"].get(key)
                if value is None:
                    continue
                self.assertEqual(value[0], "interpolate", item["id"])
                stops = dict(zip(value[3::2], value[4::2]))
                self.assertEqual(max(stops), z16, item["id"])
                below = max(z for z in stops if z < z16)
                self.assertLessEqual(below, z15, item["id"])
                self.assertAlmostEqual(stops[z16], 2 * stops[below], places=2, msg=item["id"])
            if item["type"] != "symbol" or item.get("maxzoom", z16) < z16:
                continue
            if item["id"].endswith("-large"):
                normal = layers[item["id"][:-len("-large")]]
                self.assertEqual(item["minzoom"], max(normal.get("minzoom", 0), z16))
                self.assertEqual(item["layout"]["text-size"],
                                 2 * make_style.at_zoom_15(normal["layout"]["text-size"]))
                self.assertEqual(item["paint"]["text-halo-width"],
                                 round(2 * normal["paint"]["text-halo-width"], 2))
                self.assertEqual(item["filter"] if "filter" in item else None,
                                 normal["filter"] if "filter" in normal else None)
            else:
                self.assertEqual(item.get("maxzoom"), z16, item["id"])
                self.assertIn(item["id"] + "-large", layers)

    def test_label_sizes_stop_at_zoom_15(self):
        """The renderer places labels using their size a zoom level further in, so a label
        layer shown at zoom 15 must not change size after it, or zoom 15 labels would move."""
        for item in self.style["layers"]:
            size = item.get("layout", {}).get("text-size")
            if isinstance(size, list):
                self.assertLessEqual(max(size[3::2]), make_style.sz(15), item["id"])

    def test_tileserver_config_points_at_style(self):
        config = json.loads((MAP_DIR / "tileserver" / "config.json").read_text())
        self.assertEqual(config["styles"]["montero-dark"]["style"], "montero-dark.json")
        self.assertIn("openmaptiles", config["data"])
        self.assertTrue((MAP_DIR / "style" / "montero-dark.json").exists())


class LightStyleTests(unittest.TestCase):
    """The day map: the dark style's layers with the light palette's colours."""

    def setUp(self):
        self.dark = json.loads(make_style.STYLE_PATH.read_text(encoding="utf-8"))
        self.light = json.loads(make_style.LIGHT_STYLE_PATH.read_text(encoding="utf-8"))

    def test_committed_light_style_matches_generator(self):
        self.assertEqual(make_style.LIGHT_STYLE_PATH.read_text(encoding="utf-8"), make_style.render("light"))

    def test_palettes_name_every_colour(self):
        self.assertEqual(sorted(make_style.LIGHT), sorted(make_style.PALETTE_NAMES))
        self.assertEqual(sorted(make_style.DARK), sorted(make_style.PALETTE_NAMES))

    def test_same_layers_other_colours(self):
        def colours(style):
            return set(re.findall(r'"#[0-9A-Fa-f]{6}"', json.dumps(style)))

        def without_colours(style):
            return re.sub(r'"#[0-9A-Fa-f]{6}"', '"#"', json.dumps(style["layers"]))

        self.assertEqual(without_colours(self.dark), without_colours(self.light))
        self.assertFalse(colours(self.dark) & colours(self.light))  # no dark colour left over
        self.assertEqual(self.light["name"], "Montero light")

    def test_light_sea_colour_and_config(self):
        self.assertEqual(build_map.style_water_colour(make_style.LIGHT_STYLE_PATH),
                         int(make_style.LIGHT["WATER"][1:], 16))
        config = json.loads((MAP_DIR / "tileserver" / "config-light.json").read_text())
        self.assertEqual(config["styles"], {"montero-light": {"style": "montero-light.json"}})
        dark_config = json.loads((MAP_DIR / "tileserver" / "config.json").read_text())
        self.assertEqual(config["options"], dark_config["options"])
        self.assertEqual(config["data"], dark_config["data"])


if __name__ == "__main__":
    unittest.main()
