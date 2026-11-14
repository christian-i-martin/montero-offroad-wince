#!/bin/sh
# Builds the dashboard's offline map packs from OpenStreetMap data, all on this Mac.
#
#   tools/map/build_map.sh [build_map.py options]
#
# 1. planetiler (in Docker) downloads the Geofabrik Philippines extract, the OpenStreetMap water
#    polygons and Natural Earth, and turns the part inside MAP_VECTOR_BOUNDS into vector tiles
#    in the OpenMapTiles schema (dist/map-build/tiles/openmaptiles.mbtiles). Skipped when that
#    file exists and was built for the same MAP_VECTOR_BOUNDS.
# 2. tileserver-gl (in Docker) draws PNG tiles from them with tools/map/style/montero-dark.json,
#    on 127.0.0.1 only.
# 3. tools/map/build_map.py fetches the tiles from that local server and writes the packs
#    (default dist/map/Map; options such as --bounds, --min-zoom, --max-zoom, --output pass
#    through). The tile server is stopped at the end.
#
# Environment:
#   MAP_VECTOR_BOUNDS=119.4,12.4,124.5,18.8
#                          area of the vector tiles (default Luzon). The packer's --bounds must
#                          lie inside it. To add another area later, set a box covering
#                          everything wanted (whole country: 116.8,4.4,126.7,21.25); changing it
#                          rebuilds the vector tiles and starts a fresh tile cache.
#   MAP_PORT=8091          local port of the tile server
#   MAP_REBUILD=1          rebuild the vector tiles (after a new download, or to change options)
#   MAP_REFRESH=1          download newer source data if there is any, then rebuild
#   MAP_JAVA_HEAP=2g       memory for planetiler's Java heap
#   MAP_DOCKER_MEMORY=3g   memory limit of each container (protects other containers)
#
# Downloads and intermediate files stay in dist/map-build/ (gitignored). The downloaded files are
# untrusted data: they live in dist/map-build/sources/ and only planetiler reads them.
set -eu

# planetiler 0.10.2 (multi-platform index: linux/amd64 and linux/arm64, runs natively here).
PLANETILER_IMAGE="ghcr.io/onthegomap/planetiler@sha256:cf32202dbc001a9ab4bc11534b642b13de3798179817da8558e567a3d13dd403"
# tileserver-gl 5.7.0 (multi-platform index: linux/amd64 and linux/arm64, runs natively here).
TILESERVER_IMAGE="maptiler/tileserver-gl@sha256:24d0364e1d40da0ae3fb3269f617c26e36a883ad4b7454e5c4064a8795bfc807"

MAP_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$MAP_DIR/../.." && pwd)
BUILD="$ROOT/dist/map-build"
PORT="${MAP_PORT:-8091}"
JAVA_HEAP="${MAP_JAVA_HEAP:-2g}"
DOCKER_MEMORY="${MAP_DOCKER_MEMORY:-3g}"
VECTOR_BOUNDS="${MAP_VECTOR_BOUNDS:-119.4,12.4,124.5,18.8}"
MBTILES="$BUILD/tiles/openmaptiles.mbtiles"
SERVER_NAME="montero-map-tileserver"

for arg in "$@"; do
    if [ "$arg" = "--count" ] || [ "$arg" = "--help" ] || [ "$arg" = "-h" ]; then
        exec python3 "$MAP_DIR/build_map.py" "$@"
    fi
done

if ! python3 "$MAP_DIR/make_style.py" --check; then
    echo "warning: the style file does not match make_style.py; using the file as it is" >&2
fi

mkdir -p "$BUILD/sources" "$BUILD/tiles" "$BUILD/cache"

built_for=$(cat "$MBTILES.bounds" 2>/dev/null || true)
if [ ! -f "$MBTILES" ] || [ "$built_for" != "$VECTOR_BOUNDS" ] || [ "${MAP_REBUILD:-0}" = 1 ] \
        || [ "${MAP_REFRESH:-0}" = 1 ]; then
    echo "== Building vector tiles for $VECTOR_BOUNDS with planetiler (downloads the sources if missing)"
    refresh=false
    if [ "${MAP_REFRESH:-0}" = 1 ]; then refresh=true; fi
    rm -f "$BUILD/tiles/openmaptiles-new.mbtiles"
    # transportation_z13_paths: unnamed tracks and paths are in the vector tiles from zoom 13
    # (dashboard zoom 14) instead of 14 (dashboard zoom 15). Temporary files (about 3 GB)
    # stay inside the container, which is faster than a shared Mac folder.
    docker run --rm --name montero-map-planetiler --memory="$DOCKER_MEMORY" \
        -e JAVA_TOOL_OPTIONS="-Xmx$JAVA_HEAP" \
        -v "$BUILD/sources:/sources" -v "$BUILD/tiles:/out" \
        "$PLANETILER_IMAGE" \
        --area=philippines --download --refresh_sources="$refresh" --download_dir=/sources \
        --bounds="$VECTOR_BOUNDS" \
        --tmpdir=/tmp/planetiler --output=/out/openmaptiles-new.mbtiles --force \
        --maxzoom=14 --languages=en --exclude_layers=housenumber \
        --transportation_z13_paths=true \
        --storage=mmap --nodemap_type=sparsearray --nodemap_storage=mmap
    mv "$BUILD/tiles/openmaptiles-new.mbtiles" "$MBTILES"
    printf '%s\n' "$VECTOR_BOUNDS" > "$MBTILES.bounds"
fi

echo "== Starting the tile server on 127.0.0.1:$PORT"
docker rm -f "$SERVER_NAME" >/dev/null 2>&1 || true
docker run -d --rm --name "$SERVER_NAME" --memory="$DOCKER_MEMORY" \
    -p "127.0.0.1:$PORT:8080" \
    -v "$MAP_DIR/tileserver/config.json:/data/config.json:ro" \
    -v "$MAP_DIR/style:/data/styles:ro" \
    -v "$BUILD/tiles:/data/tiles:ro" \
    "$TILESERVER_IMAGE" --config /data/config.json --port 8080 >/dev/null
trap 'docker stop "$SERVER_NAME" >/dev/null 2>&1 || true' EXIT INT TERM

tries=0
until curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; do
    tries=$((tries + 1))
    if [ "$tries" -gt 120 ]; then
        echo "error: the tile server did not start; its log:" >&2
        docker logs "$SERVER_NAME" 2>&1 | tail -40 >&2
        exit 1
    fi
    sleep 1
done

echo "== Rendering and packing"
python3 "$MAP_DIR/build_map.py" \
    --url "http://127.0.0.1:$PORT/styles/montero-dark/256/{z}/{x}/{y}.png" \
    --cache-key-file "$MAP_DIR/style/montero-dark.json" \
    --cache-key-file "$MAP_DIR/tileserver/config.json" \
    --cache-key-file "$MBTILES" \
    --data-bounds "$VECTOR_BOUNDS" \
    "$@"
