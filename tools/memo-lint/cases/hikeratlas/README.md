# hikeratlas test cases

Layer profiles ("slices") from [hikeratlas/basemap](https://github.com/hikeratlas/basemap)
(Apache-2.0, commit d48c5b1), changed so they run in stock tilemaker:

- `"compress": "zstd"` → `"gzip"` (zstd tiles are a fork feature).
- Layers dropped: `park_outline` (`outline_of`, a fork feature) and `ocean` (needs the
  coastline shapefile).
- `ways.lua`: stand-ins for the fork's `LengthInside`/`LayerInside`/`LayerOutside`, defined only
  when tilemaker doesn't provide them. A path is never split at a park's edge.
- `qrank.lua`: a deterministic pure-Lua stand-in for the SQLite QRank lookup.
- `keydb.lua`: a per-thread table instead of files in /tmp. It still carries state from one object
  to the next, so `places.lua` stays unsafe to memoize (memo-lint flags it).
- geojsonl sources are read from `data/` (`./fetch.sh`). Those not published for an area are
  empty files.

Not included: `hikingroutes`, `routes`, `trailareas` (they only read geojsonl, no OSM Lua).

```bash
./fetch.sh nova-scotia
./run.sh ../../../../build/tilemaker ways data/nova-scotia.osm.pbf /tmp/ways.mbtiles
```
