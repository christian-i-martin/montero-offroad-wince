"""Offline tests of the address tools in tools/address/: no network, no Docker.

Small made-up GeoJSON files stand in for osmium's exports of the OpenStreetMap data.
"""
from __future__ import annotations

import json
import math
import random
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "address"))
import build_address  # noqa: E402
import check_address  # noqa: E402
import mtaddr  # noqa: E402

BOX = (121.0, 14.0, 121.2, 14.2)


def relation(level, name, rings, **tags):
    """An administrative boundary relation as osmium exports it (rings: polygons of rings)."""
    props = {"@type": "relation", "@id": 1, "boundary": "administrative", "admin_level": str(level)}
    if name is not None:
        props["name"] = name
    props.update(tags)
    return {"type": "Feature", "geometry": {"type": "MultiPolygon", "coordinates": rings}, "properties": props}


def square(x0, y0, x1, y1):
    return [[x0, y0], [x1, y0], [x1, y1], [x0, y1], [x0, y0]]


def line(highway, name, points):
    props = {"@type": "way", "@id": 2, "highway": highway}
    if name:
        props["name"] = name
    return {"type": "Feature", "geometry": {"type": "LineString", "coordinates": points}, "properties": props}


def place(kind, name, lon, lat):
    return {"type": "Feature", "geometry": {"type": "Point", "coordinates": [lon, lat]},
            "properties": {"@type": "node", "@id": 3, "place": kind, "name": name}}


def write_seq(path, features, separator=True):
    with open(path, "w", encoding="utf-8") as f:
        for ft in features:
            f.write(("\x1e" if separator else "") + json.dumps(ft, ensure_ascii=False) + "\n")


def distance_to_polyline_m(points, lon, lat):
    kx = mtaddr.M_PER_DEG_LON_EQUATOR * math.cos(math.radians(lat))
    ky = mtaddr.M_PER_DEG_LAT
    return mtaddr.polyline_distance([(x * kx, y * ky) for x, y in points], lon * kx, lat * ky, 1.0, 1.0)


class TagTests(unittest.TestCase):
    def test_admin_levels(self):
        kind = lambda **p: build_address.admin_kind(dict({"boundary": "administrative", "name": "X"}, **p))
        self.assertEqual(kind(admin_level="4"), mtaddr.KIND_PROVINCE)
        self.assertEqual(kind(admin_level="6"), mtaddr.KIND_CITY)
        self.assertEqual(kind(admin_level="10"), mtaddr.KIND_BARANGAY)
        for level in ("2", "3", "5", "7", "8", "9", "11", "12", "", None):
            self.assertIsNone(kind(admin_level=level), level)

    def test_metro_manila_is_the_province(self):
        kind = build_address.admin_kind
        self.assertEqual(kind({"boundary": "administrative", "admin_level": "3", "name": "Metro Manila"}),
                         mtaddr.KIND_PROVINCE)
        self.assertIsNone(kind({"boundary": "administrative", "admin_level": "3", "name": "Calabarzon"}))

    def test_closed_ways_only_count_as_barangays(self):
        kind = build_address.admin_kind
        way = {"@type": "way", "boundary": "administrative", "name": "Brgy. 23"}
        self.assertEqual(kind(dict(way, admin_level="10")), mtaddr.KIND_BARANGAY)
        self.assertIsNone(kind(dict(way, admin_level="6")))  # Laoag's mistagged barangays
        self.assertIsNone(kind(dict(way, admin_level="4")))

    def test_names(self):
        kind = build_address.admin_kind
        self.assertIsNone(kind({"boundary": "administrative", "admin_level": "6"}))
        self.assertIsNone(kind({"boundary": "political", "admin_level": "6", "name": "X"}))
        self.assertEqual(build_address.feature_name({"name:en": "Pasig"}), "Pasig")
        self.assertEqual(build_address.feature_name({"name": " ", "name:en": "Pasig"}), "Pasig")
        self.assertEqual(build_address.clean_name("Parañaque\x00 \t City\n"), "Parañaque City")
        # characters the dashboard's font lacks become plain ones; accents join their letters
        self.assertEqual(build_address.clean_name("Barangay Ⅲ​"), "Barangay III")
        self.assertEqual(build_address.clean_name("Zone Ⅻ‑A № 5 ⅳ"), "Zone XII-A No. 5 iv")
        self.assertEqual(build_address.clean_name("Piñas"), "Piñas")
        self.assertEqual(build_address.clean_name("Sitio ั Ilaya"), "Sitio Ilaya")
        self.assertEqual(build_address.clean_name(None), "")

    def test_ranks(self):
        self.assertEqual(build_address.road_rank({"highway": "trunk_link"}), 1)
        self.assertEqual(build_address.road_rank({"highway": "secondary"}), 3)
        self.assertEqual(build_address.road_rank({"highway": "track"}), 5)
        for h in ("footway", "path", "steps", "pedestrian", "construction", "proposed", "busway", None):
            self.assertIsNone(build_address.road_rank({"highway": h}), h)
        self.assertEqual(build_address.place_rank({"place": "city"}), 1)
        self.assertEqual(build_address.place_rank({"place": "quarter"}), 4)
        self.assertEqual(build_address.place_rank({"place": "hamlet"}), 6)
        for p in ("isolated_dwelling", "locality", "city_block", "state", "island"):
            self.assertIsNone(build_address.place_rank({"place": p}), p)


class SimplifyTests(unittest.TestCase):
    def test_straight_line_keeps_its_ends(self):
        pts = [(121.0 + i * 0.0001, 14.0) for i in range(50)]
        self.assertEqual(build_address.simplify_line(pts, 5.0), [pts[0], pts[-1]])

    def test_bumps_against_tolerance(self):
        # 0.00003 degrees of latitude is about 3.3 m
        small = [(121.0, 14.0), (121.001, 14.00003), (121.002, 14.0)]
        self.assertEqual(len(build_address.simplify_line(small, 5.0)), 2)
        self.assertEqual(len(build_address.simplify_line(small, 2.0)), 3)

    def test_zero_tolerance_only_drops_repeats(self):
        pts = [(121.0, 14.0), (121.0, 14.0), (121.001, 14.0), (121.002, 14.0)]
        self.assertEqual(build_address.simplify_line(pts, 0), [(121.0, 14.0), (121.001, 14.0), (121.002, 14.0)])

    def test_stays_within_tolerance(self):
        rng = random.Random(4)
        pts, x, y = [], 121.0, 14.0
        for _ in range(400):
            pts.append((x, y))
            x += 0.00005 + rng.random() * 0.0001
            y += (rng.random() - 0.5) * 0.0002
        for tol in (2.0, 10.0, 30.0):
            out = build_address.simplify_line(pts, tol)
            self.assertLess(len(out), len(pts))
            self.assertEqual((out[0], out[-1]), (pts[0], pts[-1]))
            worst = max(distance_to_polyline_m(out, lon, lat) for lon, lat in pts)
            self.assertLessEqual(worst, tol + 0.01)

    def test_ring_keeps_corners(self):
        ring = [(121.0 + i * 0.001, 14.0) for i in range(10)] + [(121.01, 14.0 + i * 0.001) for i in range(10)] \
            + [(121.01 - i * 0.001, 14.01) for i in range(10)] + [(121.0, 14.01 - i * 0.001) for i in range(10)]
        out = build_address.simplify_ring(ring, 10.0)
        self.assertEqual(out[0], out[-1])
        self.assertEqual(set(out), {(121.0, 14.0), (121.01, 14.0), (121.01, 14.01), (121.0, 14.01)})

    def test_wobbly_ring_within_tolerance(self):
        rng = random.Random(8)
        ring = [(121.1 + 0.01 * (1 + 0.005 * rng.random()) * math.cos(2 * math.pi * k / 300),
                 14.1 + 0.01 * (1 + 0.005 * rng.random()) * math.sin(2 * math.pi * k / 300)) for k in range(300)]
        out = build_address.simplify_ring(ring, 12.0)
        self.assertEqual(out[0], out[-1])
        self.assertLess(len(out), len(ring) / 2)
        worst = max(distance_to_polyline_m(out, lon, lat) for lon, lat in ring)
        self.assertLessEqual(worst, 12.01)

    def test_slivers_and_flat_rings(self):
        sliver = [(121.0, 14.0), (121.01, 14.0), (121.02, 14.00001), (121.01, 14.00002), (121.0, 14.0)]
        out = build_address.simplify_ring(sliver, 10.0)
        self.assertEqual(len(out), 4)  # a triangle, closed
        self.assertEqual(build_address.simplify_ring([(121.0, 14.0), (121.01, 14.0), (121.02, 14.0)], 1.0), [])
        self.assertEqual(build_address.simplify_ring([(121.0, 14.0), (121.0, 14.0)], 1.0), [])


class BuildTests(unittest.TestCase):
    def build(self, temp, areas, roads, places, **kw):
        paths = [Path(temp) / n for n in ("areas.geojsonseq", "roads.geojsonseq", "places.geojsonseq")]
        write_seq(paths[0], areas)
        write_seq(paths[1], roads, separator=False)
        write_seq(paths[2], places)
        out = Path(temp) / "Map" / "test.addr"
        info = build_address.build(*paths, out, BOX, log=lambda s: None, **kw)
        return out, info

    def sample(self):
        areas = [
            relation(4, "Rizal", [[square(121.0, 14.0, 121.1, 14.2)]]),
            # a province with a hole where an independent city is
            relation(4, "Benguet", [[square(121.1, 14.0, 121.2, 14.2), square(121.13, 14.13, 121.17, 14.17)]]),
            relation(6, "Baguio", [[square(121.13, 14.13, 121.17, 14.17)]]),
            relation(3, "Metro Manila", [[square(121.0, 14.15, 121.05, 14.2)]]),
            relation(3, "Calabarzon", [[square(120.9, 13.9, 121.3, 14.3)]]),
            # a city in two parts, one with a hole
            relation(6, "Antipolo", [[square(121.0, 14.0, 121.05, 14.05), square(121.02, 14.02, 121.03, 14.03)],
                                     [square(121.06, 14.0, 121.08, 14.02)]]),
            relation(10, "San Isidro", [[square(121.01, 14.01, 121.015, 14.015)]]),
            relation(8, "Tondo", [[square(121.0, 14.0, 121.1, 14.1)]]),
            relation(6, None, [[square(121.0, 14.0, 121.1, 14.1)]]),
            relation(6, "Far Away", [[square(125.0, 10.0, 125.1, 10.1)]]),
            dict(relation(6, "Brgy. 5", [[square(121.0, 14.0, 121.01, 14.01)]]),
                 properties={"@type": "way", "boundary": "administrative", "admin_level": "6", "name": "Brgy. 5"}),
            dict(relation(10, "Ward I", [[square(121.04, 14.04, 121.045, 14.045)]]),
                 properties={"@type": "way", "boundary": "administrative", "admin_level": "10", "name": "Ward I"}),
        ]
        roads = [
            line("primary", "Ortigas Avenue", [[121.0, 14.06], [121.05, 14.06], [121.1, 14.0601]]),
            line("residential", "Calle Niño", [[121.01, 14.012], [121.012, 14.012]]),
            line("footway", "Walkway", [[121.0, 14.07], [121.1, 14.07]]),
            line("track", None, [[121.0, 14.08], [121.1, 14.08]]),
        ]
        places = [place("village", "Dalig", 121.03, 14.09), place("quarter", "Bel-Air", 121.14, 14.14),
                  place("isolated_dwelling", "Home", 121.031, 14.091), place("village", "Outside", 121.5, 14.5)]
        return areas, roads, places

    def test_round_trip(self):
        with tempfile.TemporaryDirectory() as temp:
            path, info = self.build(temp, *self.sample(), cell_deg=0.02, place_radius_m=3000)
            reader = mtaddr.AddrReader(path)
            look = reader.lookup
            # 3 provinces (Rizal, Benguet, Metro Manila) + Baguio again as Benguet
            self.assertEqual(info["counts"][mtaddr.KIND_PROVINCE], 4)
            self.assertEqual(info["counts"][mtaddr.KIND_CITY], 2)
            self.assertEqual(info["counts"][mtaddr.KIND_BARANGAY], 2)
            self.assertEqual(info["counts"][mtaddr.KIND_PLACE], 2)
            self.assertEqual(info["independent_cities"], ["Baguio (Benguet)"])
            self.assertLessEqual(info["largest_cell"]["items"], build_address.MAX_CELL_ITEMS)

            r = look(121.012, 14.0121)
            self.assertEqual((r["province"], r["city"], r["barangay"], r["road"]),
                             ("Rizal", "Antipolo", "San Isidro", "Calle Niño"))
            self.assertLess(r["road_m"], 15)
            self.assertEqual(look(121.025, 14.025)["city"], None)          # Antipolo's hole
            self.assertEqual(look(121.07, 14.01)["city"], "Antipolo")      # its second part
            self.assertEqual(look(121.0425, 14.0425)["barangay"], "Ward I")
            self.assertEqual(look(121.005, 14.005)["city"], "Antipolo")    # not the mistagged way
            self.assertEqual(look(121.02, 14.17)["province"], "Metro Manila")
            self.assertEqual(look(121.15, 14.15)["province"], "Benguet")   # in the independent city
            self.assertEqual(look(121.15, 14.15)["city"], "Baguio")
            self.assertEqual(look(121.12, 14.12)["province"], "Benguet")
            r = look(121.05, 14.0603)
            self.assertEqual(r["road"], "Ortigas Avenue")
            self.assertEqual(look(121.05, 14.0705)["road"], None)          # footway and unnamed track
            self.assertEqual(look(121.05, 14.0805)["road"], None)
            r = look(121.031, 14.091)
            self.assertEqual(r["place"], "Dalig")                         # not the isolated dwelling
            self.assertLess(r["place_m"], 200)
            self.assertEqual(look(121.14, 14.141)["place"], "Bel-Air")
            names = reader.data[reader.names_offset:reader.names_offset + reader.names_size]
            for absent in (b"Tondo", b"Calabarzon", b"Far Away", b"Brgy. 5", b"Walkway", b"Home", b"Outside"):
                self.assertNotIn(absent, names)

    def test_bad_lines_are_skipped(self):
        with tempfile.TemporaryDirectory() as temp:
            areas = Path(temp) / "areas.geojsonseq"
            areas.write_text('\x1e{"type": "Feature"\n[1, 2]\n\n' + json.dumps(
                relation(4, "Rizal", [[square(121.0, 14.0, 121.1, 14.2)]])) + "\n", encoding="utf-8")
            problems = {}
            self.assertEqual(len(list(build_address.read_features(areas, problems))), 1)
            self.assertEqual(problems, {"bad lines": 2})

    def test_too_many_items_in_a_cell(self):
        with tempfile.TemporaryDirectory() as temp:
            places = [place("hamlet", "Purok %d" % i, 121.1 + i * 1e-5, 14.1) for i in range(30)]
            with self.assertRaises(ValueError):
                self.build(temp, [], [], places, max_cell_items=20)
            self.assertFalse((Path(temp) / "Map" / "test.addr").exists())
            self.assertFalse((Path(temp) / "Map" / "test.addr.part").exists())

    def test_interior_point_of_a_u_shape(self):
        u = [(0.0, 0.0), (3.0, 0.0), (3.0, 3.0), (2.0, 3.0), (2.0, 1.0), (1.0, 1.0), (1.0, 3.0), (0.0, 3.0), (0.0, 0.0)]
        p = build_address.interior_point([u])
        self.assertTrue(build_address._inside_deg([u], p))


class ExactCellsTests(unittest.TestCase):
    """exact_cells=True lists fewer items but must give the same answers as the bounding box
    listing everywhere."""

    def write(self, path, exact):
        rng = random.Random(21)
        w = mtaddr.AddrWriter(path, BOX, cell_deg=0.005, road_radius_m=75, place_radius_m=900, exact_cells=exact)
        w.add_area(mtaddr.KIND_PROVINCE, "Diagonal", [[(121.0, 14.0), (121.2, 14.19), (121.19, 14.2), (121.0, 14.01)]])
        w.add_area(mtaddr.KIND_PROVINCE, "Ring", [[(121.02, 14.02), (121.18, 14.02), (121.18, 14.18), (121.02, 14.18)],
                                                  [(121.05, 14.05), (121.15, 14.05), (121.15, 14.15), (121.05, 14.15)]])
        # edges exactly on cell borders (multiples of 0.005)
        w.add_area(mtaddr.KIND_CITY, "On the grid", [[(121.05, 14.05), (121.1, 14.05), (121.1, 14.1), (121.05, 14.1)]])
        for i in range(60):
            cx, cy, r = 121.0 + rng.random() * 0.2, 14.0 + rng.random() * 0.2, 0.002 + rng.random() * 0.02
            n = 5 + rng.randrange(20)
            ring = [(cx + r * (0.4 + 0.6 * rng.random()) * math.cos(2 * math.pi * k / n),
                     cy + r * (0.4 + 0.6 * rng.random()) * math.sin(2 * math.pi * k / n)) for k in range(n)]
            w.add_area(mtaddr.KIND_BARANGAY if i % 2 else mtaddr.KIND_CITY, "Area %d" % i, [ring])
        for i in range(80):
            x, y = 121.0 + rng.random() * 0.2, 14.0 + rng.random() * 0.2
            pts = []
            for _ in range(2 + rng.randrange(60)):
                pts.append((x, y))
                x += (rng.random() - 0.5) * 0.01
                y += (rng.random() - 0.5) * 0.01
            w.add_road("Road %d" % i, pts, 1 + i % 5)
        w.add_road("Along the grid", [(121.0, 14.1), (121.2, 14.1)])
        for i in range(120):
            w.add_place("Place %d" % i, 121.0 + rng.random() * 0.2, 14.0 + rng.random() * 0.2, 1 + i % 6)
        w.add_place("On a corner", 121.1, 14.1, 4)
        return w.close()

    def test_same_answers_fewer_items(self):
        with tempfile.TemporaryDirectory() as temp:
            a, b = Path(temp) / "box.addr", Path(temp) / "exact.addr"
            info_a, info_b = self.write(a, False), self.write(b, True)
            self.assertLess(info_b["items"], info_a["items"])
            ra, rb = check_address.CachedReader(a), check_address.CachedReader(b)
            rng = random.Random(5)
            pts = [(121.0 + rng.random() * 0.2, 14.0 + rng.random() * 0.2) for _ in range(6000)]
            # points on and next to cell borders and the shapes' edges
            pts += [(121.0 + i * 0.005, 14.0 + j * 0.005) for i in range(41) for j in range(41)]
            pts += [(121.0 + i * 0.005 + 0.000001, 14.1 - 0.000001 * j) for i in range(40) for j in range(3)]
            for lon, lat in pts:
                self.assertEqual(ra.lookup(lon, lat), rb.lookup(lon, lat), (lon, lat))


class CheckToolTests(unittest.TestCase):
    def test_sample_line_spacing(self):
        # 0.01 degrees of latitude is 1105.74 m
        pts, rest = check_address.sample_line([(121.0, 14.0), (121.0, 14.01)], 400.0, 200.0)
        self.assertEqual(len(pts), 3)
        self.assertAlmostEqual((pts[1][1] - pts[0][1]) * mtaddr.M_PER_DEG_LAT, 400.0, places=3)
        self.assertAlmostEqual(rest, 200.0 + 3 * 400.0 - 1105.74, places=2)
        pts, _ = check_address.sample_line([(121.0, 14.0), (121.0, 14.003)], 400.0, rest)
        self.assertEqual(len(pts), 1)  # sampling runs on across lines
        self.assertAlmostEqual((pts[0][1] - 14.0) * mtaddr.M_PER_DEG_LAT, rest, places=3)


if __name__ == "__main__":
    unittest.main()
