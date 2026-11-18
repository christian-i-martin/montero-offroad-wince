"""The holiday list: its sources, the built dashboard/holidays.txt, and the C reader against the
reference reader in tools/holidays/build_holidays.py (town lookup from coordinates and the
holidays of a day)."""
from __future__ import annotations

import datetime as dt
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "holidays"))
import build_holidays as bh  # noqa: E402

COMPILER = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
OUTPUT = ROOT / "dashboard" / "holidays.txt"


def town_index(towns, provinces, province, name):
    return next(i for i, t in enumerate(towns) if t.name == name and provinces[t.province] == province)


class SourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.span, cls.provinces, cls.towns, cls.holidays = bh.read(OUTPUT.read_text(encoding="utf-8"))

    def test_output_is_up_to_date(self):
        provinces, towns, holidays = bh.load()
        self.assertEqual(OUTPUT.read_text(encoding="utf-8"), bh.render(provinces, towns, holidays),
                         "run python3 tools/holidays/build_holidays.py")

    def test_towns(self):
        self.assertEqual(len(self.provinces), 39)
        self.assertEqual(len(self.towns), 771)
        for t in self.towns:   # inside the Luzon group (Palawan reaches south to 8 N; Kalayaan is the Spratlys)
            self.assertTrue(750000 <= t.lat_e5 <= 2150000 and 11400000 <= t.lon_e5 <= 12450000, t)
            self.assertGreaterEqual(t.radius_m, bh.MIN_RADIUS_M)

    def test_national_lists(self):
        national = [h for h in self.holidays if not h.places]
        regular = {y: sum(1 for h in national if h.kind == "R" and h.first.year == y) for y in (2026, 2027)}
        self.assertEqual(regular, {2026: 12, 2027: 10})   # 2027: Eid'l Fitr and Eid'l Adha not proclaimed yet
        by_day = {(h.first, h.name): h.kind for h in national}
        self.assertEqual(by_day[(dt.date(2026, 11, 30), "Bonifacio Day")], "R")
        self.assertEqual(by_day[(dt.date(2027, 3, 27), "Black Saturday")], "S")
        self.assertEqual(by_day[(dt.date(2027, 2, 25), "EDSA People Power Revolution anniversary")], "W")
        self.assertEqual(by_day[(dt.date(2026, 1, 26), "National Bible Day")], "W")   # last Monday of January
        self.assertEqual(by_day[(dt.date(2027, 1, 25), "National Bible Day")], "W")

    def test_records(self):
        self.assertEqual(self.span, (bh.FIRST_DAY, bh.LAST_DAY))
        last = None
        for h in self.holidays:
            self.assertIn(h.kind, bh.KINDS)
            self.assertTrue(bh.FIRST_DAY <= h.first <= h.last <= bh.LAST_DAY, h)
            self.assertLessEqual(len(h.name.encode("utf-8")), bh.NAME_MAX)
            for p in h.places:
                n = int(p[1:])
                self.assertLess(n, len(self.provinces) if p[0] == "P" else len(self.towns), h)
            if last:
                self.assertLessEqual(last, h.first)
            last = h.first
        keys = [(h.first, h.name, h.places) for h in self.holidays]
        self.assertEqual(len(keys), len(set(keys)), "a holiday is listed twice")

    def test_proclamation_replaces_law(self):
        das = "T%d" % town_index(self.towns, self.provinces, "Cavite", "Dasmariñas")
        days = [(h.first, h.kind) for h in self.holidays if das in h.places and h.first.month == 11 and h.first.day == 26]
        self.assertEqual(days, [(dt.date(2026, 11, 26), "S"), (dt.date(2027, 11, 26), "W")])
        occ = "P%d" % self.provinces.index("Occidental Mindoro")
        nov = sorted(h.first for h in self.holidays if occ in h.places and h.first.month == 11)
        self.assertEqual(nov, [dt.date(2026, 11, 16), dt.date(2027, 11, 15)])   # 2026: moved off the Sunday

    def test_known_days(self):
        def rows(province, name, day):
            return bh.today(self.provinces, self.towns, self.holidays, day,
                            town_index(self.towns, self.provinces, province, name))
        self.assertEqual(rows("Quezon", "Lucban", dt.date(2026, 5, 15)), [("Pahiyas Festival", "S", 1, "Lucban")])
        # Quezon Day: a working day nationwide, a day off in Quezon City; shown once, as the day off
        self.assertEqual(rows("Metro Manila", "Quezon City", dt.date(2026, 8, 19)),
                         [("Quezon Day", "S", 1, "Quezon City")])
        self.assertEqual(rows("Cavite", "Dasmariñas", dt.date(2026, 8, 19)), [("Quezon Day", "W", 0, "")])
        self.assertEqual(rows("Pampanga", "Angeles", dt.date(2026, 6, 15)),
                         [("Mt. Pinatubo eruption anniversary", "S", 1, "Angeles")])
        self.assertEqual(rows("Batangas", "Lipa", dt.date(2026, 11, 30)), [("Bonifacio Day", "R", 0, "")])
        self.assertEqual(rows("Metro Manila", "Pasig", dt.date(2026, 11, 18)), [("ASEAN Summit", "S", 1, "Metro Manila")])

    def test_town_centres_find_their_town(self):
        centres = {}
        for t in self.towns:
            centres.setdefault((t.lat_e5, t.lon_e5), []).append(t)
        for i, t in enumerate(self.towns):
            if len(centres[(t.lat_e5, t.lon_e5)]) == 1:
                self.assertEqual(bh.locate(self.towns, t.lat_e5, t.lon_e5), i, t.name)
        self.assertEqual(bh.locate(self.towns, 1030000, 12390000), -1)   # Cebu: not Luzon
        self.assertEqual(bh.locate(self.towns, 1450000, 11700000), -1)   # the South China Sea


@unittest.skipIf(COMPILER is None, "no C compiler on this machine")
class CReaderTests(unittest.TestCase):
    def test_c_reader_matches_reference(self):
        span, provinces, towns, holidays = bh.read(OUTPUT.read_text(encoding="utf-8"))
        rng = random.Random(7)
        queries, expected = [], []
        points = [(t.lat_e5 + rng.randint(-3000, 3000), t.lon_e5 + rng.randint(-3000, 3000)) for t in towns]
        points += [(rng.randint(750000, 2150000), rng.randint(11600000, 12450000)) for _ in range(3000)]
        for lat, lon in points:
            queries.append("L %d %d" % (lat, lon))
            expected.append(str(bh.locate(towns, lat, lon)))
        busy = sorted({h.first for h in holidays if h.places})
        for _ in range(3000):
            day = rng.choice(busy) if rng.random() < 0.8 else bh.FIRST_DAY + dt.timedelta(days=rng.randint(0, 729))
            town = rng.randint(-1, len(towns) - 1)
            queries.append("D %d %d %d %d" % (day.year, day.month, day.day, town))
            rows = bh.today(provinces, towns, holidays, day, town)
            expected.append("\x1e".join("%s\x1f%s\x1f%d\x1f%s" % r for r in rows))
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "holiday_check"
            subprocess.run([COMPILER, "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "dashboard/src"),
                            "-o", str(binary), str(ROOT / "tests/c/holiday_check.c"),
                            str(ROOT / "dashboard/src/holiday.c"), str(ROOT / "dashboard/src/sun.c"), "-lm"],
                           check=True)
            result = subprocess.run([str(binary), str(OUTPUT)], input="\n".join(queries) + "\n",
                                    capture_output=True, text=True, encoding="utf-8", check=True)
        self.assertIn("0 bad lines", result.stderr)
        got = result.stdout.split("\n")[:-1]
        self.assertEqual(len(got), len(expected))
        mismatches = [(q, e, g) for q, e, g in zip(queries, expected, got) if e != g]
        self.assertEqual(mismatches[:5], [])


if __name__ == "__main__":
    unittest.main()
