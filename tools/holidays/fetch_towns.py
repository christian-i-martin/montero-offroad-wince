"""Fetch the Luzon cities and municipalities with their centres and land areas from Wikidata.

Writes tools/holidays/towns.tsv: province, town, latitude, longitude, area in km2 and the
Wikidata item, one town a line. build_holidays.py turns it into the town table of the dashboard's
holiday file, where the dashboard picks the town whose centre is nearest the GPS position (each
town's distance divided by its size, so a big municipality reaches further than a small one).
Local holidays name their places by these town and province names.

Wikidata (CC0, no attribution needed) is used instead of the map's OpenStreetMap extract on
purpose: the holiday list must not depend on the map, which will be replaced. The table is
committed, so building the holiday file needs no network; run this again only to refresh it.

    python3 tools/holidays/fetch_towns.py            # -> tools/holidays/towns.tsv
    python3 tools/holidays/fetch_towns.py --out FILE

Python 3.9, standard library only.
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import math
import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Dict, List, Tuple

ENDPOINT = "https://query.wikidata.org/sparql"
USER_AGENT = "holiday-list-builder/1.0 (offline car dashboard)"

# The Luzon-group regions, by province (Metro Manila stands in for NCR).
REGIONS = {
    "NCR": ["Metro Manila"],
    "CAR": ["Abra", "Apayao", "Benguet", "Ifugao", "Kalinga", "Mountain Province"],
    "I": ["Ilocos Norte", "Ilocos Sur", "La Union", "Pangasinan"],
    "II": ["Batanes", "Cagayan", "Isabela", "Nueva Vizcaya", "Quirino"],
    "III": ["Aurora", "Bataan", "Bulacan", "Nueva Ecija", "Pampanga", "Tarlac", "Zambales"],
    "IV-A": ["Batangas", "Cavite", "Laguna", "Quezon", "Rizal"],
    "MIMAROPA": ["Marinduque", "Occidental Mindoro", "Oriental Mindoro", "Palawan", "Romblon"],
    "V": ["Albay", "Camarines Norte", "Camarines Sur", "Catanduanes", "Masbate", "Sorsogon"],
}
PROVINCES = [p for ps in REGIONS.values() for p in ps]

# Cities and municipalities (municipality of the Philippines, city of the Philippines and their
# subclasses), with centre, land area and the area they are located in.
TOWNS_QUERY = """
SELECT ?item ?itemLabel ?coord ?area ?parent ?parentLabel WHERE {
  VALUES ?type { wd:Q24764 wd:Q104157 }
  ?item wdt:P31/wdt:P279* ?type .
  ?item wdt:P17 wd:Q928 .
  OPTIONAL { ?item wdt:P625 ?coord . }
  OPTIONAL { ?item wdt:P2046 ?area . }
  OPTIONAL { ?item wdt:P131 ?parent . }
  SERVICE wikibase:label { bd:serviceParam wikibase:language "en,mul,tl". }
}"""

# Towns that no longer exist (dissolved or abolished date).
DISSOLVED_QUERY = """
SELECT ?item WHERE {
  VALUES ?type { wd:Q24764 wd:Q104157 }
  ?item wdt:P31/wdt:P279* ?type .
  ?item wdt:P17 wd:Q928 .
  ?item wdt:P576 ?dissolved .
}"""

# Corrections, checked by hand on 2026-11-18. Highly urbanised cities are located in their region
# on Wikidata, not in a province; a few towns list a second, wrong province; two former towns
# carry no dissolution date.
PROVINCE_OF = {
    "Q1822": "Benguet",     # Baguio
    "Q55741": "Pampanga",   # Angeles
    "Q56759": "Zambales",   # Olongapo
    "Q104125": "Quezon",    # Lucena
    "Q111739": "Palawan",   # Puerto Princesa
}
ONLY_PROVINCE = {"Hermosa": "Bataan", "Flora": "Apayao", "San Pablo": "Laguna"}
FORMER = {"Tablas", "Dupax"}      # Dupax was split into Dupax del Norte and del Sur in 1971
DEFAULT_AREA_KM2 = 100.0          # for the few towns without an area


def sparql(query: str) -> List[Dict[str, str]]:
    url = ENDPOINT + "?" + urllib.parse.urlencode({"query": query})
    request = urllib.request.Request(url, headers={"Accept": "text/tab-separated-values", "User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        text = response.read().decode("utf-8")
    rows = list(csv.reader(io.StringIO(text), delimiter="\t", quoting=csv.QUOTE_NONE))
    head = [h.lstrip("?") for h in rows[0]]
    return [dict(zip(head, r + [""] * (len(head) - len(r)))) for r in rows[1:]]


def literal(value: str) -> str:
    m = re.match(r'^"(.*)"(?:@[\w-]+|\^\^.*)?$', value)
    return m.group(1) if m else value


def qid(value: str) -> str:
    return value.rsplit("/", 1)[-1].rstrip(">")


def point(value: str) -> Tuple[float, float]:
    m = re.match(r"Point\(([-\d.]+) ([-\d.]+)\)", literal(value))
    if not m:
        raise ValueError("not a point: %s" % value)
    return float(m.group(2)), float(m.group(1))   # latitude, longitude


def collect(rows: List[Dict[str, str]], dissolved: set) -> List[Tuple[str, str, float, float, float, str]]:
    items: Dict[str, dict] = {}
    for r in rows:
        q = qid(r["item"])
        if q in dissolved:
            continue
        d = items.setdefault(q, {"name": literal(r["itemLabel"]), "coord": None, "area": None, "parents": []})
        if r["coord"] and d["coord"] is None:
            d["coord"] = point(r["coord"])
        if r["area"] and d["area"] is None:
            d["area"] = float(literal(r["area"]))
        if r["parentLabel"]:
            d["parents"].append(literal(r["parentLabel"]))
    out = []
    for q, d in items.items():
        if d["name"] in FORMER:
            continue
        provinces = sorted({p for p in d["parents"] if p in PROVINCES})
        if q in PROVINCE_OF:
            provinces = [PROVINCE_OF[q]]
        elif d["name"] in ONLY_PROVINCE and ONLY_PROVINCE[d["name"]] in provinces:
            provinces = [ONLY_PROVINCE[d["name"]]]
        if not provinces:
            continue
        if len(provinces) > 1:
            raise SystemExit("%s (%s) is in several provinces: %s; add it to ONLY_PROVINCE" % (d["name"], q, provinces))
        if d["coord"] is None:
            raise SystemExit("%s (%s) has no coordinates on Wikidata" % (d["name"], q))
        lat, lon = d["coord"]
        out.append((provinces[0], d["name"], lat, lon, d["area"] or DEFAULT_AREA_KM2, q))
    names = {}
    for prov, name, *_ in out:
        if (prov, name) in names:
            raise SystemExit("two towns named %s in %s" % (name, prov))
        names[(prov, name)] = 1
    return sorted(out, key=lambda t: (PROVINCES.index(t[0]), t[1]))


def write(path: Path, towns) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Luzon cities and municipalities: province, town, latitude, longitude, land area (km2),\n"
                "# Wikidata item. Written by tools/holidays/fetch_towns.py from Wikidata (CC0).\n")
        for prov, name, lat, lon, area, q in towns:
            f.write("%s\t%s\t%.5f\t%.5f\t%.2f\t%s\n" % (prov, name, lat, lon, area, q))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", type=Path, default=Path(__file__).with_name("towns.tsv"))
    args = ap.parse_args(argv)
    dissolved = {qid(r["item"]) for r in sparql(DISSOLVED_QUERY)}
    towns = collect(sparql(TOWNS_QUERY), dissolved)
    write(args.out, towns)
    counts = {p: sum(1 for t in towns if t[0] == p) for p in PROVINCES}
    print("%d towns in %d provinces -> %s" % (len(towns), sum(1 for c in counts.values() if c), args.out))
    print(", ".join("%s %d" % kv for kv in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
