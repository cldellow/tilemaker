#!/bin/bash
# Run one hikeratlas slice with a tilemaker binary.
# Usage: ./run.sh TILEMAKER SLICE PBF [OUT.mbtiles] [extra tilemaker args...]
# Prints the wall time and tilemaker's reading phases (nodes, ways, relations) in ms.
set -uo pipefail
tm=$(readlink -f "$1"); slice=$2; pbf=$(readlink -f "$3"); out=$(readlink -f "${4:-/tmp/hikeratlas-$slice.mbtiles}"); shift 4 2>/dev/null || shift $#
cd "$(dirname "$0")/slices"
rm -f "$out"; log=$(mktemp)
t0=$(date +%s.%N)
"$tm" --input "$pbf" --output "$out" --config "$slice.json" --process "$slice.lua" "$@" > "$log" 2>&1; rc=$?
t=$(echo "$(date +%s.%N) - $t0" | bc)
phases=$(tr '\r' '\n' < "$log" | grep -oE '[0-9]+ ms\)$' | sed -n 2,4p | tr -d ' ms)' | tr '\n' ' ')
memo=$(grep -h "memo:" "$log" | sed -E 's/ memo: / /; s/ replayed, / /; s/ recorded, / /; s/ through Lua, / /; s/ nodes//' | tr '\n' ';')
echo "$slice rc=$rc wall=$t phases_ms=[ $phases] memo=[$memo]"
[ $rc -eq 0 ] || tail -5 "$log" >&2
rm -f "$log"
