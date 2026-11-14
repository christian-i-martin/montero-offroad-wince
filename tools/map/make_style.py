#!/usr/bin/env python3
"""Writes tools/map/style/montero-dark.json, the map style the tile renderer uses.

A "style" is a MapLibre GL style file: it says which layers of the vector tiles to draw, in which
order, with which colours and line widths. The vector tiles follow the OpenMapTiles schema
(https://openmaptiles.org/schema/): layers such as water, landcover, transportation (roads) and
place (town names). This style is written from that schema for the dashboard's small map panel:
a dark map in the dashboard's colours, main roads in warm colours, unpaved tracks dashed in an
earthy colour, paths dotted, and large labels with dark outlines ("halos").

Zoom numbers in this file are dashboard tile zooms (256 pixel tiles, the zoom stored in the
.pack files). MapLibre counts zoom for 512 pixel tiles, so the style file gets each number minus
one (function sz below): dashboard zoom 14 is style zoom 13.

Run it after changing a colour or a width:

    python3 tools/map/make_style.py           # rewrite the style file
    python3 tools/map/make_style.py --check   # exit 1 if the style file is out of date

tests/test_map_tools.py also checks that the committed style file matches this script.
Python 3.9 compatible, standard library only.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Dict, List

STYLE_PATH = Path(__file__).resolve().parent / "style" / "montero-dark.json"
STYLE_ID = "montero-dark"

# The only font tileserver-gl ships (in its tileserver-gl-styles package).
FONT = ["Noto Sans Regular"]

# Colours. The first group comes from the dashboard theme.
DASH_BG = "#0B0E11"
TEXT = "#F1F4F6"
TEXT_MUTED = "#A3ADB7"
ACCENT = "#F6A623"
COOL = "#5BC0EB"

LAND = "#1E242A"
WATER = "#0E2C42"         # build_map.py reads this colour back from the style file
COAST = "#2D6A8F"         # thin line along the water's edge
FOREST = "#1B2B23"
WETLAND = "#1A2B2D"
SAND = "#2D2B23"
PARK_FILL = "#1F3A2A"
PARK_LINE = "#2E5C40"
RESIDENTIAL = "#262C33"
INDUSTRIAL = "#292C31"
BUILDING = "#30373F"
RUNWAY = "#4A535C"

MOTORWAY = ACCENT
TRUNK = "#F4B547"
PRIMARY = "#E8C86A"
SECONDARY = "#D2C79C"
TERTIARY = "#BCC3C9"
MINOR = "#8E98A1"
SERVICE = "#6F7983"
TRACK = "#D08F4E"         # earthy ochre, dashed
UNPAVED = "#B78C5C"       # unpaved minor and service roads, long dashes
PATH = "#B4BEC7"          # dotted
RAIL = "#77818B"
FERRY = COOL
CASING = DASH_BG
TUNNEL_OPACITY = 0.45

RIVER = "#2C7AA8"
STREAM = "#245E80"
COUNTRY = "#A98ED1"
PROVINCE = "#58616B"
WATER_TEXT = "#8CCBEA"
PEAK = "#D9B98C"
FUEL = ACCENT

HALO = DASH_BG
NAME = ["coalesce", ["get", "name:latin"], ["get", "name"]]


def sz(tile_zoom: float) -> float:
    """Dashboard tile zoom (256 pixel tiles) to MapLibre style zoom (512 pixel tiles)."""
    return tile_zoom - 1


def ramp(stops: List[tuple], base: float = 1.4) -> list:
    """A value that changes smoothly with zoom; stops are (dashboard zoom, value) pairs."""
    expr: list = ["interpolate", ["exponential", base], ["zoom"]]
    for zoom, value in stops:
        expr += [sz(zoom), value]
    return expr


def steps(first: Any, stops: List[tuple]) -> list:
    """A value that jumps at the given dashboard zooms."""
    expr: list = ["step", ["zoom"], first]
    for zoom, value in stops:
        expr += [sz(zoom), value]
    return expr


def layer(layer_id: str, kind: str, source_layer: str = "", min_zoom: float = 0,
          max_zoom: float = 0, filt: list = None, paint: Dict[str, Any] = None,
          layout: Dict[str, Any] = None) -> Dict[str, Any]:
    out: Dict[str, Any] = {"id": layer_id, "type": kind}
    if source_layer:
        out["source"] = "openmaptiles"
        out["source-layer"] = source_layer
    if min_zoom:
        out["minzoom"] = sz(min_zoom)
    if max_zoom:
        out["maxzoom"] = sz(max_zoom)
    if filt is not None:
        out["filter"] = filt
    if layout:
        out["layout"] = layout
    out["paint"] = paint or {}
    return out


def is_class(*names: str) -> list:
    return ["match", ["get", "class"], list(names), True, False]


def text_layout(size: Any, transform: str = "none", **extra: Any) -> Dict[str, Any]:
    out: Dict[str, Any] = {"text-field": NAME, "text-font": FONT, "text-size": size,
                           "text-transform": transform, "text-max-width": 8}
    out.update(extra)
    return out


def text_paint(colour: str, halo: float = 1.8) -> Dict[str, Any]:
    return {"text-color": colour, "text-halo-color": HALO, "text-halo-width": halo,
            "text-halo-blur": 0.4}


# Road classes, drawn from least to most important so main roads end up on top.
# Each entry: class names, first dashboard zoom, colour, widths per dashboard zoom, casing extra.
ROADS = [
    ("service", ["service"], 14, SERVICE, [(14, 0.8), (15, 1.6)], 0.0),
    ("minor", ["minor"], 13, MINOR, [(13, 0.9), (14, 1.8), (15, 2.8)], 1.0),
    ("tertiary", ["tertiary"], 11, TERTIARY,
     [(11, 0.7), (12, 1.2), (13, 1.8), (14, 2.8), (15, 4.0)], 1.2),
    ("secondary", ["secondary"], 9, SECONDARY,
     [(9, 0.6), (10, 1.0), (12, 1.8), (14, 3.4), (15, 4.8)], 1.4),
    ("primary", ["primary"], 8, PRIMARY,
     [(8, 0.7), (10, 1.4), (12, 2.2), (14, 4.0), (15, 5.6)], 1.6),
    ("trunk", ["trunk"], 8, TRUNK,
     [(8, 0.9), (10, 1.6), (12, 2.6), (14, 4.6), (15, 6.4)], 1.8),
    ("motorway", ["motorway"], 8, MOTORWAY,
     [(8, 1.0), (10, 1.8), (12, 2.8), (14, 5.0), (15, 7.0)], 1.8),
]

PAVED_OR_UNKNOWN = ["!=", ["get", "surface"], "unpaved"]
UNPAVED_FILTER = ["==", ["get", "surface"], "unpaved"]
NOT_TUNNEL = ["!=", ["get", "brunnel"], "tunnel"]
TUNNEL = ["==", ["get", "brunnel"], "tunnel"]


def widen(stops: List[tuple], extra: float) -> List[tuple]:
    return [(z, round(w + extra * min(1.0, 0.5 + (z - 8) / 8.0), 2)) for z, w in stops]


def road_layers() -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    # Tunnels: faded, no casing, underneath everything else.
    for name, classes, first, colour, widths, _ in ROADS:
        out.append(layer("tunnel-" + name, "line", "transportation", first,
                         filt=["all", TUNNEL, is_class(*classes)],
                         layout={"line-cap": "butt", "line-join": "round"},
                         paint={"line-color": colour, "line-width": ramp(widths),
                                "line-opacity": TUNNEL_OPACITY,
                                "line-dasharray": [2, 1]}))

    # Paths: dotted, light grey. Tracks: dashed, earthy, with a dark casing so they stand out
    # on forest and farmland.
    out.append(layer("path", "line", "transportation", 14,
                     filt=["all", NOT_TUNNEL, is_class("path")],
                     layout={"line-cap": "round", "line-join": "round"},
                     paint={"line-color": PATH, "line-width": ramp([(14, 1.3), (15, 1.8)]),
                            "line-dasharray": [0.1, 2.2]}))
    out.append(layer("track-casing", "line", "transportation", 13,
                     filt=["all", NOT_TUNNEL, is_class("track")],
                     layout={"line-cap": "butt", "line-join": "round"},
                     paint={"line-color": CASING, "line-opacity": 0.8,
                            "line-width": ramp([(13, 2.8), (14, 3.8), (15, 4.8)])}))
    out.append(layer("track", "line", "transportation", 13,
                     filt=["all", NOT_TUNNEL, is_class("track")],
                     layout={"line-cap": "butt", "line-join": "round"},
                     paint={"line-color": TRACK,
                            "line-width": ramp([(13, 1.5), (14, 2.2), (15, 2.9)]),
                            "line-dasharray": [2.2, 1.2]}))

    # Casings of the normal roads, then the roads themselves.
    for name, classes, first, colour, widths, extra in ROADS:
        if not extra:
            continue
        out.append(layer("casing-" + name, "line", "transportation", max(first, 10),
                         filt=["all", NOT_TUNNEL, is_class(*classes)],
                         layout={"line-cap": "round", "line-join": "round"},
                         paint={"line-color": CASING, "line-opacity": 0.9,
                                "line-width": ramp(widen(widths, extra))}))
    for name, classes, first, colour, widths, _ in ROADS:
        if name in ("minor", "service"):
            # Paved (or unknown surface) minor roads solid; unpaved ones dashed in earth colour.
            out.append(layer("road-" + name, "line", "transportation", first,
                             filt=["all", NOT_TUNNEL, PAVED_OR_UNKNOWN, is_class(*classes)],
                             layout={"line-cap": "round", "line-join": "round"},
                             paint={"line-color": colour, "line-width": ramp(widths)}))
            out.append(layer("road-" + name + "-unpaved", "line", "transportation", first,
                             filt=["all", NOT_TUNNEL, UNPAVED_FILTER, is_class(*classes)],
                             layout={"line-cap": "butt", "line-join": "round"},
                             paint={"line-color": UNPAVED, "line-width": ramp(widths),
                                    "line-dasharray": [4, 1.5]}))
        else:
            out.append(layer("road-" + name, "line", "transportation", first,
                             filt=["all", NOT_TUNNEL, is_class(*classes)],
                             layout={"line-cap": "round", "line-join": "round"},
                             paint={"line-color": colour, "line-width": ramp(widths)}))

    out.append(layer("rail", "line", "transportation", 10,
                     filt=["all", NOT_TUNNEL, is_class("rail", "transit")],
                     paint={"line-color": RAIL, "line-width": ramp([(10, 0.8), (15, 2.0)])}))
    out.append(layer("rail-hatch", "line", "transportation", 13,
                     filt=["all", NOT_TUNNEL, is_class("rail", "transit")],
                     paint={"line-color": LAND, "line-width": ramp([(13, 0.6), (15, 1.2)]),
                            "line-dasharray": [2, 2]}))
    out.append(layer("ferry", "line", "transportation", 11, filt=is_class("ferry"),
                     paint={"line-color": FERRY, "line-opacity": 0.4,
                            "line-width": ramp([(11, 0.7), (15, 1.4)]),
                            "line-dasharray": [3, 2]}))
    return out


def area_layers() -> List[Dict[str, Any]]:
    return [
        layer("background", "background", paint={"background-color": LAND}),
        layer("landcover-wood", "fill", "landcover", filt=is_class("wood", "forest"),
              paint={"fill-color": FOREST, "fill-antialias": False}),
        layer("landcover-wetland", "fill", "landcover", 10, filt=is_class("wetland"),
              paint={"fill-color": WETLAND, "fill-antialias": False}),
        layer("landcover-sand", "fill", "landcover", 11, filt=is_class("sand"),
              paint={"fill-color": SAND, "fill-antialias": False}),
        layer("landuse-residential", "fill", "landuse", 9,
              filt=is_class("residential", "suburb", "quarter", "neighbourhood"),
              paint={"fill-color": RESIDENTIAL, "fill-antialias": False}),
        layer("landuse-industrial", "fill", "landuse", 11,
              filt=is_class("industrial", "commercial", "retail", "railway", "garages",
                            "quarry"),
              paint={"fill-color": INDUSTRIAL, "fill-antialias": False}),
        layer("park", "fill", "park", 9,
              paint={"fill-color": PARK_FILL, "fill-opacity": 0.35, "fill-antialias": False}),
        layer("park-outline", "line", "park", 10,
              paint={"line-color": PARK_LINE, "line-width": ramp([(10, 0.6), (15, 1.4)]),
                     "line-dasharray": [3, 2]}),
        layer("water", "fill", "water", filt=["!=", ["get", "brunnel"], "tunnel"],
              paint={"fill-color": WATER, "fill-antialias": True}),
        layer("waterway-river", "line", "waterway", 9,
              filt=["all", NOT_TUNNEL, is_class("river", "canal")],
              layout={"line-cap": "round", "line-join": "round"},
              paint={"line-color": RIVER,
                     "line-width": ramp([(9, 0.5), (11, 0.7), (13, 1.1), (15, 2.4)])}),
        layer("waterway-stream", "line", "waterway", 13,
              filt=["all", NOT_TUNNEL, is_class("stream", "drain", "ditch")],
              layout={"line-cap": "round", "line-join": "round"},
              paint={"line-color": STREAM, "line-width": ramp([(13, 0.5), (15, 1.1)])}),
        layer("coastline", "line", "water", 9, filt=["!=", ["get", "brunnel"], "tunnel"],
              paint={"line-color": COAST, "line-width": ramp([(9, 0.6), (15, 1.2)])}),
        layer("aeroway-runway", "line", "aeroway", 11, filt=is_class("runway"),
              paint={"line-color": RUNWAY, "line-width": ramp([(11, 1.0), (15, 8.0)], 1.6)}),
        layer("building", "fill", "building", 15,
              paint={"fill-color": BUILDING, "fill-antialias": False}),
    ]


def boundary_layers() -> List[Dict[str, Any]]:
    on_land = ["==", ["get", "maritime"], 0]
    return [
        layer("boundary-province", "line", "boundary", 10,
              filt=["all", on_land, ["match", ["get", "admin_level"], [3, 4], True, False]],
              paint={"line-color": PROVINCE, "line-width": ramp([(10, 0.6), (15, 1.2)]),
                     "line-dasharray": [3, 2]}),
        layer("boundary-country", "line", "boundary",
              filt=["all", on_land, ["==", ["get", "admin_level"], 2]],
              paint={"line-color": COUNTRY, "line-width": ramp([(8, 0.8), (15, 1.8)]),
                     "line-dasharray": [3, 2]}),
    ]


def label_layers() -> List[Dict[str, Any]]:
    return [
        layer("water-name", "symbol", "water_name", 9,
              filt=["!=", ["get", "class"], "lake"],
              layout=text_layout(ramp([(9, 12), (15, 14)], 1.2), "none",
                                 **{"text-letter-spacing": 0.1, "symbol-placement": "point"}),
              paint=text_paint(WATER_TEXT, 1.4)),
        layer("lake-name", "symbol", "water_name", 12, filt=is_class("lake"),
              layout=text_layout(12),
              paint=text_paint(WATER_TEXT, 1.4)),
        layer("waterway-name", "symbol", "waterway", 13, filt=is_class("river", "canal"),
              layout=text_layout(12, **{"symbol-placement": "line", "symbol-spacing": 300,
                                       "text-max-angle": 30}),
              paint=text_paint(WATER_TEXT, 1.4)),
        layer("road-name-minor", "symbol", "transportation_name", 14,
              filt=is_class("tertiary", "minor", "service", "track"),
              layout=text_layout(12, **{"symbol-placement": "line", "symbol-spacing": 350,
                                       "text-max-angle": 30}),
              paint=text_paint("#D9DEE2")),
        layer("road-name-major", "symbol", "transportation_name", 12,
              filt=is_class("motorway", "trunk", "primary", "secondary"),
              layout=text_layout(ramp([(12, 12), (15, 13)], 1.2),
                                 **{"symbol-placement": "line", "symbol-spacing": 400,
                                    "text-max-angle": 30}),
              paint=text_paint("#F1EBDD")),
        layer("fuel", "circle", "poi", 14, filt=is_class("fuel"),
              paint={"circle-color": FUEL, "circle-radius": ramp([(14, 3), (15, 4)]),
                     "circle-stroke-color": HALO, "circle-stroke-width": 1.2}),
        layer("fuel-name", "symbol", "poi", 15, filt=is_class("fuel"),
              layout=text_layout(12, **{"text-anchor": "left", "text-offset": [0.7, 0]}),
              paint=text_paint(ACCENT, 1.6)),
        layer("peak", "circle", "mountain_peak", 11, filt=is_class("peak", "volcano"),
              paint={"circle-color": PEAK, "circle-radius": ramp([(11, 2.2), (15, 3.2)]),
                     "circle-stroke-color": HALO, "circle-stroke-width": 1}),
        layer("peak-name", "symbol", "mountain_peak", 12, filt=is_class("peak", "volcano"),
              layout={"text-field": ["case", ["has", "ele"],
                                     ["concat", NAME, "\n", ["to-string", ["get", "ele"]], " m"],
                                     NAME],
                      "text-font": FONT, "text-size": 12, "text-anchor": "top",
                      "text-offset": [0, 0.5], "text-max-width": 8},
              paint=text_paint(PEAK, 1.6)),
        layer("place-island", "symbol", "place", 10, filt=is_class("island"),
              layout=text_layout(ramp([(10, 12), (15, 14)], 1.2),
                                 **{"text-letter-spacing": 0.05}),
              paint=text_paint(TEXT_MUTED)),
        layer("place-minor", "symbol", "place", 13,
              filt=is_class("hamlet", "suburb", "quarter", "neighbourhood",
                            "isolated_dwelling"),
              layout=text_layout(12, **{"symbol-sort-key": ["get", "rank"]}),
              paint=text_paint(TEXT_MUTED)),
        layer("place-village", "symbol", "place", 12, filt=is_class("village"),
              layout=text_layout(ramp([(12, 12), (15, 14)], 1.2),
                                 **{"symbol-sort-key": ["get", "rank"]}),
              paint=text_paint("#DCE1E5")),
        layer("place-town", "symbol", "place", 10, filt=is_class("town"),
              layout=text_layout(ramp([(10, 12.5), (15, 16)], 1.2),
                                 **{"symbol-sort-key": ["get", "rank"]}),
              paint=text_paint(TEXT)),
        layer("place-city", "symbol", "place", 8, filt=is_class("city"),
              layout=text_layout(ramp([(8, 13), (12, 16), (15, 18)], 1.2),
                                 **{"symbol-sort-key": ["get", "rank"]}),
              paint=text_paint(TEXT, 2.0)),
        layer("place-country", "symbol", "place", 0, 9, filt=is_class("country"),
              layout=text_layout(13, "uppercase", **{"text-letter-spacing": 0.15}),
              paint=text_paint(TEXT_MUTED)),
    ]


def build_style() -> Dict[str, Any]:
    layers = area_layers() + road_layers() + boundary_layers() + label_layers()
    return {
        "version": 8,
        "name": "Montero dark",
        "metadata": {
            "montero:note": "Written for the OpenMapTiles schema by tools/map/make_style.py. "
                            "Style zoom = dashboard tile zoom - 1 (256 pixel tiles).",
        },
        "sources": {
            "openmaptiles": {
                "type": "vector",
                "url": "mbtiles://{openmaptiles}",
                "attribution": "© OpenStreetMap contributors, © OpenMapTiles",
            },
        },
        "glyphs": "{fontstack}/{range}.pbf",
        "layers": layers,
    }


def render() -> str:
    return json.dumps(build_style(), indent=1, ensure_ascii=False) + "\n"


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true",
                        help="only check that the style file is up to date")
    args = parser.parse_args(argv)
    text = render()
    if args.check:
        current = STYLE_PATH.read_text(encoding="utf-8") if STYLE_PATH.exists() else ""
        if current != text:
            print("%s is out of date; run python3 tools/map/make_style.py" % STYLE_PATH)
            return 1
        return 0
    STYLE_PATH.parent.mkdir(parents=True, exist_ok=True)
    STYLE_PATH.write_text(text, encoding="utf-8")
    print("wrote %s (%d layers)" % (STYLE_PATH, len(build_style()["layers"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
