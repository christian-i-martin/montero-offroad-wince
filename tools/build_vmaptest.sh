#!/bin/sh
# Build the vector map test (vmaptest/) with CeGCC in Docker.
# Output: dist/vmaptest/VMapTest/ (VMapTest.exe, fonts/, README.txt). Copy that folder to the SD
# card root, and dist/vmap/VMap (tools/vmap/build_vmap.py) to the card root as \SD\VMap.
# Also lists the COREDLL functions the exe imports next to the dashboard's, when that is built:
# a function the head unit lacks stops an exe from loading at all.
set -eu

# ENLYZE CeGCC 9.3.0 for arm-mingw32ce, pinned by digest (same image as the probe and dashboard).
IMAGE="ghcr.io/enlyze/windows-ce-build-environment-arm@sha256:22ea23d697accaab2b98fb12d81ccc1faade90f9fe490f554ae7c5c194dfe27c"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/dist/vmaptest"
rm -rf "$OUT/VMapTest"
mkdir -p "$OUT"
docker run --rm --platform linux/amd64 \
    -v "$ROOT:/src:ro" -v "$OUT:/out" -w /src/vmaptest \
    "$IMAGE" sh -c 'make OUT=/out/VMapTest && arm-mingw32ce-objdump -p /out/VMapTest/VMapTest.exe > /out/imports-vmaptest.txt && arm-mingw32ce-objdump -p /out/VMapTest/VMapTest-ARMv4.exe > /out/imports-vmaptest-armv4.txt && if [ -f /src/dist/dashboard/Navigation/Navigation.exe ]; then arm-mingw32ce-objdump -p /src/dist/dashboard/Navigation/Navigation.exe > /out/imports-dashboard.txt; fi'
echo "Built $OUT/VMapTest/VMapTest.exe"
