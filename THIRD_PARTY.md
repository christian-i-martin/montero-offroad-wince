# Third-party components and provenance

## Current direct replacement

The preparation tool retrieves only Total Commander/CE:

- Reference repository: https://github.com/typlo-gh/Ford-WINCE-Autokit
- Pinned commit: `986cf5464c61eae40d101985027fa24b2e6fde79`
- Path: `TC/cecmd.exe`
- Length: **337,920 bytes**
- Git blob SHA-1: `1925f097d2e2ef09516154b1d9638d38cc97a8c9`
- Publisher information and terms: https://www.ghisler.com/cecmd.htm

Total Commander/CE is copyrighted freeware by Christian Ghisler / C. Ghisler & Co. Its copyright remains with its author. This project does not claim authorship or relicense it. Obtain and use it subject to the publisher's terms.

The executable is not patched. The build creates renamed copies at `Navigation/Navigation.exe` and `LaunchTotalCommander/LaunchTotalCommander.exe`. It does not download maps, vehicle firmware, plugins, fonts, or AutoKit libraries.

The binary is downloaded on the user's computer for local preparation, not committed by the tool. Existing uploaded SD binaries remain a reference snapshot. Hash checks establish equality to the pinned reference, not a security audit or runtime compatibility. A matching CPU family does not prove the required Windows CE APIs are present.

## Dashboard: PNG decoder

`dashboard/third_party/stb_image.h` is stb_image 2.32 by Sean Barrett and contributors, from
https://github.com/nothings/stb at commit `9600522de53973336fa033a289858a4354cbb04c`
(SHA-256 `deb4f18c9ca21b751a1323019031c94c0bf5ca1d2929ef5913cc56f00aab6e91`), unmodified. It is
public domain, or MIT at your choice (the licence text is at the end of the file). The dashboard
compiles only its PNG decoder (`dashboard/src/pngdec.c`).

## Offline map

The map packs in `dist/map/Map/` (copied to `\SD\Map\`) are made on the build machine by
`tools/map/build_map.sh`; none of the tools below goes on the SD card, only the tile pictures.

- Map data: OpenStreetMap contributors, Open Database License 1.0
  (https://www.openstreetmap.org/copyright), from the Geofabrik Philippines extract. The packs are a
  Produced Work and must credit "© OpenStreetMap contributors"; the dashboard shows the credit on
  the map.
- OpenMapTiles schema (the vector tile layout our style is written for): CC-BY 4.0
  (https://github.com/openmaptiles/openmaptiles#license); credit "© OpenMapTiles", shown on the map.
- Water polygons (osmdata.openstreetmap.de) and lake centerlines (github.com/acalcutt/osm-lakelines):
  derived from OpenStreetMap, ODbL. Natural Earth: public domain.
- planetiler 0.10.2 (Apache-2.0; its OpenMapTiles profile BSD-3-Clause), Docker image
  `ghcr.io/onthegomap/planetiler@sha256:cf32202dbc001a9ab4bc11534b642b13de3798179817da8558e567a3d13dd403`.
- tileserver-gl 5.7.0 (BSD-2-Clause; bundles MapLibre Native, sharp, libvips and libimagequant
  under their own licences), Docker image
  `maptiler/tileserver-gl@sha256:24d0364e1d40da0ae3fb3269f617c26e36a883ad4b7454e5c4064a8795bfc807`.
- Labels use Noto Sans Regular from the tileserver-gl image (SIL Open Font License 1.1); it ends up
  only as pixels in the tiles.
- The map style `tools/map/style/montero-dark.json` is this project's own, written from the
  OpenMapTiles schema.

## Offline address

`dist/map/Map/luzon.addr` (copied to `\SD\Map\`) is made by `tools/address/build_address.sh` from
the same Geofabrik OpenStreetMap extract. It is a derived database under the ODbL 1.0: it needs
the "© OpenStreetMap contributors" credit (shown on the map) and, if the file is ever published,
the share-alike terms apply. The tools below only run on the build machine:

- osmium-tool 1.18.0 (GPL-3.0; libosmium under the Boost Software License 1.0), installed as the
  Debian package `osmium-tool=1.18.0-1` in a local image built from
  `debian@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2` (trixie-slim).
- The PSA standard list of local government units (PSGC) was used only to compare counts.

## Superseded MortScript workflow

Commit `7fd8236465c5cf9cdd26dd3e3f49e8e5ec12fa7b` built an overlay using `LaunchTotalCommander/Arthur.exe`, `MortScript.exe`, and a selection-menu script. The direct replacement no longer uses these components. Do not install previously generated overlays for the current workflow.
