#!/bin/sh
# Build the dashboard (dashboard/) with CeGCC in Docker.
# Output: dist/dashboard/Navigation/ (Navigation.exe, fonts/, README.txt), a complete SD card
# Navigation folder. If dist/total-commander-direct/ exists (tools/prepare_total_commander.py),
# Total Commander is added as Navigation/TotalCommander/cecmd.exe for the dashboard's menu.
set -eu

# ENLYZE CeGCC 9.3.0 for arm-mingw32ce, pinned by digest (same image as the probe).
IMAGE="ghcr.io/enlyze/windows-ce-build-environment-arm@sha256:22ea23d697accaab2b98fb12d81ccc1faade90f9fe490f554ae7c5c194dfe27c"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/dist/dashboard"
rm -rf "$OUT/Navigation"
mkdir -p "$OUT"
docker run --rm --platform linux/amd64 \
    -v "$ROOT:/src:ro" -v "$OUT:/out" -w /src/dashboard \
    "$IMAGE" make OUT=/out/Navigation

TC="$ROOT/dist/total-commander-direct/Navigation/Navigation.exe"
if [ -f "$TC" ]; then
    mkdir -p "$OUT/Navigation/TotalCommander"
    cp "$TC" "$OUT/Navigation/TotalCommander/cecmd.exe"
    echo "Added Total Commander as Navigation/TotalCommander/cecmd.exe"
else
    echo "No dist/total-commander-direct: the dashboard menu will not offer the file manager"
fi
echo "Built $OUT/Navigation/Navigation.exe"
