#!/bin/sh
# Build the head-unit probe (probe/) with CeGCC in Docker.
# Output: dist/probe/Probe/Probe.exe and README.txt (copy the Probe folder to the SD card root).
set -eu

# ENLYZE CeGCC 9.3.0 for arm-mingw32ce, pinned by digest. Published for linux/amd64 only;
# Docker Desktop on Apple Silicon runs it under emulation.
IMAGE="ghcr.io/enlyze/windows-ce-build-environment-arm@sha256:22ea23d697accaab2b98fb12d81ccc1faade90f9fe490f554ae7c5c194dfe27c"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$ROOT/dist/probe"
docker run --rm --platform linux/amd64 \
    -v "$ROOT/probe:/src:ro" -v "$ROOT/dist/probe:/out" -w /src \
    "$IMAGE" make OUT=/out/Probe
echo "Built $ROOT/dist/probe/Probe/Probe.exe"
