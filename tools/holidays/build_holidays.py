"""Build the dashboard's holiday list (dashboard/holidays.txt) from the tables in tools/holidays/.

Sources (UTF-8, tab-separated, # starts a comment):
    national.tsv     national holidays by date (the yearly proclamations)
    local-YYYY.tsv   local special days proclaimed for a year (one proclamation each); every
                     local-*.tsv file is read
    laws.tsv         holidays fixed by law, every year (national and local)
    towns.tsv        every Luzon city and municipality with its centre and land area (Wikidata,
                     fetch_towns.py); local holidays name their places by these names

Output, read by dashboard/src/holiday.c (one record a line, fields separated by tabs):
    V  first_day  last_day
        the dates the list covers (YYYY-MM-DD); outside them the dashboard shows no holiday
    P  index  name
        a province, numbered from 0 in file order
    T  province  lat_e5  lon_e5  radius_m  name
        a city or municipality, numbered from 0 in file order: its centre in 1/100,000 degrees and
        the radius of a circle of its land area
    H  first_day  last_day  kind  name  places
        a holiday; kind R regular holiday, S special non-working day (both days off), W special
        working holiday (not a day off); places empty for the whole country, otherwise tokens
        Pn (province n) and Tn (town n) separated by spaces

Where the car is (locate below, matched by holiday_locate in holiday.c): the town whose distance
from the position, divided by its radius, is smallest, so a big municipality reaches further
than a small one; none when even that one is more than NEAR_LIMIT radii away (at sea, outside
Luzon). Towns more than BOX_E5 away in latitude or longitude are skipped. The distance is worked
out on a flat projection with whole numbers and doubles in a fixed order, so the C reader gives
exactly the same answer. It is the nearest town centre, not the town's outline: near a border the
neighbouring town can be picked. A local holiday applies when its places hold that town or its
province.

    python3 tools/holidays/build_holidays.py          # -> dashboard/holidays.txt
    python3 tools/holidays/build_holidays.py --check  # fail when holidays.txt is out of date
    python3 tools/holidays/build_holidays.py --extra TEST.tsv --out dist/dashboard/Navigation/holidays.txt
                                                      # a copy with test rows, for trying it on the car

Python 3.9, standard library only.
"""
from __future__ import annotations

import argparse
import datetime as dt
import math
import sys
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUTPUT = ROOT / "dashboard" / "holidays.txt"
FIRST_DAY = dt.date(2026, 1, 1)
LAST_DAY = dt.date(2027, 12, 31)
KINDS = ("R", "S", "W")
MOVED_WITHIN_DAYS = 7     # a proclamation this close to a law's date, for the same place, replaces it
NEAR_LIMIT = 3.0          # radii
BOX_E5 = 300000           # 3 degrees
COS_SCALE = 4096
M_PER_E5 = 1.1132         # metres in 1/100,000 degree of latitude
MIN_RADIUS_M = 500
NAME_MAX = 79             # bytes, the reader's limit (HOLIDAY_NAME_MAX - 1)


class Town(NamedTuple):
    province: int
    lat_e5: int
    lon_e5: int
    radius_m: int
    name: str


class Holiday(NamedTuple):
    first: dt.date
    last: dt.date
    kind: str
    name: str
    places: Tuple[str, ...]   # "P3", "T120"; empty for the whole country
    source: str


def rows(path: Path) -> List[List[str]]:
    out = []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        out.append(line.split("\t") + ["%s:%d" % (path.name, n)])
    return out


def load_towns(path: Path) -> Tuple[List[str], List[Town], Dict[Tuple[str, str], int]]:
    provinces: List[str] = []
    towns: List[Town] = []
    index: Dict[Tuple[str, str], int] = {}
    for prov, name, lat, lon, area, _q, _where in rows(path):
        if prov not in provinces:
            provinces.append(prov)
        radius = max(MIN_RADIUS_M, int(round(math.sqrt(float(area) / math.pi) * 1000)))
        index[(prov, name)] = len(towns)
        towns.append(Town(provinces.index(prov), int(round(float(lat) * 1e5)), int(round(float(lon) * 1e5)),
                          radius, name))
    return provinces, towns, index


def place_tokens(text: str, provinces: Sequence[str], index: Dict[Tuple[str, str], int], where: str) -> Tuple[str, ...]:
    tokens = []
    for item in filter(None, (s.strip() for s in text.split(";"))):
        if "/" in item:
            prov, town = item.split("/", 1)
            if (prov, town) not in index:
                raise SystemExit("%s: no town %r in %r in towns.tsv" % (where, town, prov))
            tokens.append("T%d" % index[(prov, town)])
        elif item in provinces:
            tokens.append("P%d" % provinces.index(item))
        else:
            raise SystemExit("%s: no province %r in towns.tsv" % (where, item))
    return tuple(tokens)


def parse_dates(text: str, where: str) -> Tuple[dt.date, dt.date]:
    try:
        a, _, b = text.partition("..")
        first = dt.date.fromisoformat(a)
        last = dt.date.fromisoformat(b) if b else first
    except ValueError:
        raise SystemExit("%s: bad date %r" % (where, text))
    if last < first:
        raise SystemExit("%s: %r ends before it starts" % (where, text))
    return first, last


def check(kind: str, name: str, where: str) -> None:
    if kind not in KINDS:
        raise SystemExit("%s: kind %r is not one of %s" % (where, kind, ", ".join(KINDS)))
    if not name or len(name.encode("utf-8")) > NAME_MAX or "\t" in name:
        raise SystemExit("%s: name %r is empty or longer than %d bytes" % (where, name, NAME_MAX))


def rule_date(rule: str, year: int, where: str) -> dt.date:
    month, _, day = rule.partition("-")
    try:
        if day == "last-Mon":
            d = dt.date(year, int(month) % 12 + 1, 1) if int(month) < 12 else dt.date(year + 1, 1, 1)
            d -= dt.timedelta(days=1)
            return d - dt.timedelta(days=d.weekday())
        return dt.date(year, int(month), int(day))
    except ValueError:
        raise SystemExit("%s: bad rule %r" % (where, rule))


def load(tables: Path = HERE, extra: Optional[Path] = None):
    provinces, towns, index = load_towns(tables / "towns.tsv")
    proclaimed: List[Holiday] = []
    if extra is not None:   # test rows, same columns as local-*.tsv; empty places: the whole country
        for date, kind, places, name, source, where in rows(extra):
            first, last = parse_dates(date, where)
            check(kind, name, where)
            proclaimed.append(Holiday(first, last, kind, name, place_tokens(places, provinces, index, where), source))
    for date, kind, name, source, where in rows(tables / "national.tsv"):
        first, last = parse_dates(date, where)
        check(kind, name, where)
        proclaimed.append(Holiday(first, last, kind, name, (), source))
    for path in sorted(tables.glob("local-*.tsv")):
        for date, kind, places, name, source, where in rows(path):
            first, last = parse_dates(date, where)
            check(kind, name, where)
            tokens = place_tokens(places, provinces, index, where)
            if not tokens:
                raise SystemExit("%s: a local day needs a place" % where)
            proclaimed.append(Holiday(first, last, kind, name, tokens, source))
    years = sorted({h.first.year for h in proclaimed if h.places})   # years with proclaimed local days
    by_law: List[Holiday] = []
    for rule, kind, places, name, law, where in rows(tables / "laws.tsv"):
        check(kind, name, where)
        tokens = place_tokens(places, provinces, index, where)
        for year in range(FIRST_DAY.year, LAST_DAY.year + 1):
            day = rule_date(rule, year, where)
            if year in years and tokens and any(
                    set(tokens) & set(h.places) and abs((h.first - day).days) <= MOVED_WITHIN_DAYS
                    for h in proclaimed):
                continue   # that year's proclamation for the same place replaces it
            by_law.append(Holiday(day, day, kind, name, tokens, law))
    holidays = [h for h in proclaimed + by_law if h.last >= FIRST_DAY and h.first <= LAST_DAY]
    order = {id(h): i for i, h in enumerate(holidays)}
    holidays.sort(key=lambda h: (h.first, bool(h.places), order[id(h)]))
    return provinces, towns, holidays


def render(provinces: Sequence[str], towns: Sequence[Town], holidays: Sequence[Holiday]) -> str:
    out = ["# Montero dashboard holidays: built by tools/holidays/build_holidays.py from the tables in",
           "# tools/holidays/ (format there); read by dashboard/src/holiday.c. Do not edit by hand.",
           "V\t%s\t%s" % (FIRST_DAY.isoformat(), LAST_DAY.isoformat())]
    out += ["P\t%d\t%s" % (i, p) for i, p in enumerate(provinces)]
    out += ["T\t%d\t%d\t%d\t%d\t%s" % (t.province, t.lat_e5, t.lon_e5, t.radius_m, t.name) for t in towns]
    out += ["H\t%s\t%s\t%s\t%s\t%s" % (h.first.isoformat(), h.last.isoformat(), h.kind, h.name, " ".join(h.places))
            for h in holidays]
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------- reference reader (for the tests)

def read(text: str):
    """Parses holidays.txt as holiday.c does: (first, last), provinces, towns, holidays."""
    span, provinces, towns, holidays = None, [], [], []
    for line in text.splitlines():
        f = line.split("\t")
        if f[0] == "V":
            span = (dt.date.fromisoformat(f[1]), dt.date.fromisoformat(f[2]))
        elif f[0] == "P":
            provinces.append(f[2])
        elif f[0] == "T":
            towns.append(Town(int(f[1]), int(f[2]), int(f[3]), int(f[4]), f[5]))
        elif f[0] == "H":
            holidays.append(Holiday(dt.date.fromisoformat(f[1]), dt.date.fromisoformat(f[2]), f[3], f[4],
                                    tuple(f[5].split()) if len(f) > 5 else (), ""))
    return span, provinces, towns, holidays


def cos_k(lat_e5: int) -> int:
    return int(math.floor(math.cos(lat_e5 / 1e5 * math.pi / 180.0) * COS_SCALE + 0.5))


def locate(towns: Sequence[Town], lat_e5: int, lon_e5: int) -> int:
    """Index of the town the position is in, or -1 (see the module notes)."""
    k = cos_k(lat_e5)
    best, best_ratio, best_d2 = -1, 0.0, 0.0
    for i, t in enumerate(towns):
        dlat, dlon = lat_e5 - t.lat_e5, lon_e5 - t.lon_e5
        if abs(dlat) > BOX_E5 or abs(dlon) > BOX_E5:
            continue
        dx = float(dlon) * k
        dy = float(dlat) * COS_SCALE
        d2 = dx * dx + dy * dy
        ratio = d2 / (float(t.radius_m) * float(t.radius_m))
        if best < 0 or ratio < best_ratio:
            best, best_ratio, best_d2 = i, ratio, d2
    if best >= 0:
        limit = NEAR_LIMIT * towns[best].radius_m / M_PER_E5 * COS_SCALE
        if best_d2 > limit * limit:
            return -1
    return best


def today(provinces, towns, holidays, day: dt.date, town: int, rows_max: int = 3):
    """The rows the dashboard shows for day at town (-1: unknown): (name, kind, local, place).

    National holidays first, then local ones, in file order. A holiday held under one name at
    several levels (Quezon Day nationwide and in Quezon City, a day for Pampanga and for Angeles)
    is shown once, at the most specific level: the town, else the province, else the country."""
    found = []   # (level, name, kind, place): level 0 national, 1 province, 2 town
    tokens = set()
    if town >= 0:
        tokens = {"T%d" % town, "P%d" % towns[town].province}
    for h in holidays:
        if not (h.first <= day <= h.last):
            continue
        if not h.places:
            found.append((0, h.name, h.kind, ""))
        elif "T%d" % town in h.places and town >= 0:
            found.append((2, h.name, h.kind, towns[town].name))
        elif tokens & set(h.places):
            found.append((1, h.name, h.kind, provinces[towns[town].province]))
    best = {}
    for level, name, _, _ in found:
        best[name] = max(best.get(name, -1), level)
    out, seen = [], set()
    for level in (0, 1, 2):
        for lv, name, kind, place in found:
            if lv == level and best[name] == level and name not in seen:
                seen.add(name)
                out.append((name, kind, int(level > 0), place))
    return out[:rows_max]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, default=OUTPUT)
    ap.add_argument("--check", action="store_true", help="only check that the output is up to date")
    ap.add_argument("--extra", type=Path, help="test rows to add (columns of local-*.tsv; empty places: national); "
                    "write them to a copy with --out, never to dashboard/holidays.txt")
    args = ap.parse_args(argv)
    if args.extra and args.out.resolve() == OUTPUT.resolve():
        raise SystemExit("--extra needs --out: test rows must not go into dashboard/holidays.txt")
    provinces, towns, holidays = load(extra=args.extra)
    text = render(provinces, towns, holidays)
    if args.check:
        if not args.out.exists() or args.out.read_text(encoding="utf-8") != text:
            print("%s is out of date: run python3 tools/holidays/build_holidays.py" % args.out)
            return 1
        print("%s is up to date" % args.out)
        return 0
    args.out.write_text(text, encoding="utf-8")
    kinds = {k: sum(1 for h in holidays if h.kind == k) for k in KINDS}
    print("%d holidays (%d national, %d local; %d regular, %d special non-working, %d working), %d towns -> %s"
          % (len(holidays), sum(1 for h in holidays if not h.places), sum(1 for h in holidays if h.places),
             kinds["R"], kinds["S"], kinds["W"], len(towns), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
