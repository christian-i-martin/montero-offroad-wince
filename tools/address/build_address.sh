#!/bin/sh
# Builds the dashboard's offline address file from OpenStreetMap data, all on this Mac.
#
#   tools/address/build_address.sh [build_address.py options]
#
# 1. Builds a small Docker image with osmium-tool (a Debian package, versions pinned below),
#    unless it is already there. This is the only step that uses the network.
# 2. osmium (in Docker, without network) picks the administrative boundaries, the highways and
#    the place nodes out of the Geofabrik Philippines extract and writes them as GeoJSON
#    sequences (one feature per line) into dist/map-build/address/: areas.geojsonseq (osmium
#    joins each boundary relation's ways into a multipolygon), roads.geojsonseq and
#    places.geojsonseq. Skipped when these are newer than the extract and this script.
# 3. tools/address/build_address.py turns them into the address file, by default
#    dist/map/Map/luzon.addr (options such as --output, --bounds, --cell-deg and
#    --place-radius-m pass through; --check adds the coverage report of check_address.py).
#
# Environment:
#   ADDR_PBF=dist/map-build/sources/philippines.osm.pbf
#                          the OpenStreetMap extract (tools/map/build_map.sh downloads it)
#   ADDR_REBUILD=1         redo the osmium step
#   ADDR_DOCKER_MEMORY=3g  memory limit of the container (protects other containers)
#
# The extract is untrusted data: Docker reads it from a read-only mount, and the host Python
# only reads osmium's GeoJSON output.
set -eu

# Debian 13 "trixie" slim, multi-platform index (linux/amd64, linux/arm64; runs natively here).
BASE_IMAGE="debian@sha256:a72b4433de1cf7d6bae0c4cee74d0547f58601a643b3375f070c51600d63b6b2"
OSMIUM_VERSION="1.18.0-1"   # Debian package osmium-tool (osmium 1.18.0, libosmium 2.22.0)
IMAGE="montero-address-osmium:$OSMIUM_VERSION"

ADDR_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$ADDR_DIR/../.." && pwd)
WORK="$ROOT/dist/map-build/address"
PBF="${ADDR_PBF:-$ROOT/dist/map-build/sources/philippines.osm.pbf}"
DOCKER_MEMORY="${ADDR_DOCKER_MEMORY:-3g}"

for arg in "$@"; do
    if [ "$arg" = "--help" ] || [ "$arg" = "-h" ]; then
        exec python3 "$ADDR_DIR/build_address.py" --help
    fi
done

if [ ! -f "$PBF" ]; then
    echo "error: $PBF is missing. Run tools/map/build_map.sh once (it downloads the Geofabrik" >&2
    echo "Philippines extract), or set ADDR_PBF to an extract in its own folder under dist/map-build/." >&2
    exit 1
fi
PBF=$(cd "$(dirname "$PBF")" && pwd)/$(basename "$PBF")
mkdir -p "$WORK"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "== Building the Docker image $IMAGE"
    printf '%s\n' \
        "FROM $BASE_IMAGE" \
        "RUN apt-get update \\" \
        " && apt-get install -y --no-install-recommends osmium-tool=$OSMIUM_VERSION \\" \
        " && rm -rf /var/lib/apt/lists/*" \
        | docker build --tag "$IMAGE" -
fi

stale=0
for f in areas roads places; do
    if [ ! -f "$WORK/$f.geojsonseq" ] || [ "$PBF" -nt "$WORK/$f.geojsonseq" ] \
            || [ "$0" -nt "$WORK/$f.geojsonseq" ]; then
        stale=1
    fi
done
if [ "$stale" = 1 ] || [ "${ADDR_REBUILD:-0}" = 1 ]; then
    echo "== Extracting boundaries, highways and places with osmium"
    # Export settings: keep only the tags the build reads; type and id tell relations from ways.
    cat > "$WORK/areas-config.json" <<'EOF'
{"attributes": {"type": true, "id": true}, "linear_tags": false, "area_tags": true,
 "include_tags": ["name", "name:en", "boundary", "admin_level"]}
EOF
    cat > "$WORK/roads-config.json" <<'EOF'
{"attributes": {"type": true, "id": true}, "linear_tags": true, "area_tags": false,
 "include_tags": ["name", "name:en", "highway"]}
EOF
    cat > "$WORK/places-config.json" <<'EOF'
{"attributes": {"type": true, "id": true}, "linear_tags": false, "area_tags": false,
 "include_tags": ["name", "name:en", "place", "admin_level"]}
EOF
    # The node location index (sparse_mem_array) needs about 300 MB for the highways.
    docker run --rm --network none --memory="$DOCKER_MEMORY" --user "$(id -u):$(id -g)" \
        -v "$PBF:/in/extract.osm.pbf:ro" -v "$WORK:/work" \
        "$IMAGE" sh -eu -c '
            osmium tags-filter --overwrite --no-progress /in/extract.osm.pbf \
                r/boundary=administrative w/boundary=administrative -o /tmp/areas.osm.pbf
            osmium tags-filter --overwrite --no-progress /in/extract.osm.pbf w/highway -o /tmp/roads.osm.pbf
            osmium tags-filter --overwrite --no-progress /in/extract.osm.pbf n/place -o /tmp/places.osm.pbf
            for f in areas:polygon roads:linestring places:point; do
                name=${f%%:*}
                osmium export --overwrite --no-progress -c /work/$name-config.json -i sparse_mem_array \
                    --geometry-types=${f#*:} -f geojsonseq -x print_record_separator=false \
                    /tmp/$name.osm.pbf -o /work/$name.geojsonseq.part
                mv /work/$name.geojsonseq.part /work/$name.geojsonseq
            done
        '
fi

echo "== Writing the address file"
python3 "$ADDR_DIR/build_address.py" \
    --areas "$WORK/areas.geojsonseq" --roads "$WORK/roads.geojsonseq" --places "$WORK/places.geojsonseq" \
    --stats "$WORK/stats.json" "$@"
