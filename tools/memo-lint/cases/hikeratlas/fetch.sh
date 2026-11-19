#!/bin/bash
# Fetch the inputs of the hikeratlas test cases into data/: an area's .osm.pbf and the
# GeoJSON lines that some slices index (parks, city parks...), from hikeratlas' public mirror.
# Usage: ./fetch.sh [area]   (default nova-scotia; geojsonl is only published for some areas)
set -euo pipefail
area=${1:-nova-scotia}
cd "$(dirname "$0")"; mkdir -p data
base=https://public.hikeratlas.com
[ -f data/$area.osm.pbf ] || curl -fsS -o data/$area.osm.pbf $base/pbfs/raw/$area.osm.pbf
for x in parks city-parks campgrounds hiking-routes trail-areas parks-near urban-lots lake-centerlines; do
  if curl -fsS -o data/$x.geojsonl.zst $base/geojsonl/$area/$x.geojsonl.zst 2>/dev/null; then
    zstd -dqf --rm data/$x.geojsonl.zst
  else
    # not published for this area (made by bin/*.py in hikeratlas/basemap): an empty layer
    rm -f data/$x.geojsonl.zst; : > data/$x.geojsonl
    echo "note: no $x.geojsonl for $area, using an empty one" >&2
  fi
done
ls -la data
