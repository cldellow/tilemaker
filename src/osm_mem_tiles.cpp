#include "osm_mem_tiles.h"
#include "osm_store.h"
#include "node_store.h"
#include "way_store.h"
#include "relation_store.h"
using namespace std;

template<typename T>
void correctGeometry(T& geom) {
	geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
	geom::is_valid(geom, failure);
	if (failure==boost::geometry::failure_spikes)
		geom::remove_spikes(geom);
	if (failure)
		make_valid(geom);
}

OsmMemTiles::OsmMemTiles(
	size_t threadNum,
	uint baseZoom,
	bool includeID,
	const OSMStore& osmStore
)
	: TileDataSource(threadNum, baseZoom, includeID),
	osmStore(osmStore),
	cacheMutex(threadNum * 4),
	cachedPolygons(threadNum * 4),
	polygonCacheSize(threadNum * 4),
	cachedLinestrings(threadNum * 4),
	linestringCacheSize(threadNum * 4)
{ }

LatpLon OsmMemTiles::buildNodeGeometry(const OutputGeometryType geomType, const NodeID objectID, const TileBbox &bbox) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildNodeGeometry(geomType, objectID, bbox);
	}

	if (IS_NODE(objectID)) {
		const LatpLon& node = osmStore.nodes.at(OSM_ID(objectID));
		return node;
	}

	throw std::runtime_error("OsmMemTiles: buildNodeGeometry: unexpected objectID: " + std::to_string(objectID));
}

std::shared_ptr<Linestring> OsmMemTiles::buildLinestring(const NodeID objectID) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildLinestring(objectID);
	}

	if (IS_WAY(objectID)) {
		{
			const size_t shard = objectID % cacheMutex.size();
			std::lock_guard<std::mutex> lock(cacheMutex[shard]);
			const auto& cache = cachedLinestrings[shard];
			const auto& rv = cache.find(objectID);
			if (rv != cache.end())
				return rv->second;
		}

		std::shared_ptr<Linestring> ls = std::make_shared<Linestring>();

		std::vector<LatpLon> nodes = osmStore.ways.at(OSM_ID(objectID));
		for (const LatpLon& node : nodes) {
			boost::geometry::range::push_back(*ls, boost::geometry::make<Point>(node.lon/10000000.0, node.latp/10000000.0));
		}

		correctGeometry(*ls);

		{
			const size_t shard = objectID % cacheMutex.size();
			std::lock_guard<std::mutex> lock(cacheMutex[shard]);
			linestringCacheSize[shard]++;
			if (linestringCacheSize[shard] == 5000) {
				cachedLinestrings[shard].clear();
				linestringCacheSize[shard] = 0;
			}
			cachedLinestrings[shard][objectID] = ls;
		}

		return ls;
	}

	throw std::runtime_error("OsmMemTiles: buildLinestring: unexpected objectID: " + std::to_string(objectID));
}

MultiLinestring OsmMemTiles::buildMultiLinestring(const NodeID objectID) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildMultiLinestring(objectID);
	}

	if (IS_RELATION(objectID)) {
		WayVec outers, inners;
		const auto& relation = osmStore.relations.at(OSM_ID(objectID));
		for (const auto& way : relation.first)
			outers.push_back(way);

		MultiLinestring mls = osmStore.wayListMultiLinestring(outers.begin(), outers.end());
		const multi_linestring_t& mls2 = retrieve_multi_linestring(objectID);
		boost::geometry::assign(mls, mls2);

		correctGeometry(mls);
		return mls;
	}

	throw std::runtime_error("OsmMemTiles: buildMultiLinestring: unexpected objectID: " + std::to_string(objectID));
}


std::shared_ptr<MultiPolygon> OsmMemTiles::buildMultiPolygon(const NodeID objectID) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildMultiPolygon(objectID);
	}

	if (IS_WAY(objectID)) {
		/*
		{
			const size_t shard = objectID % cacheMutex.size();
			std::lock_guard<std::mutex> lock(cacheMutex[shard]);
			const auto& cache = cachedPolygons[shard];
			const auto& rv = cache.find(objectID);
			if (rv != cache.end())
				return rv->second;
		}
		*/

		Linestring ls;

		std::vector<LatpLon> nodes = osmStore.ways.at(OSM_ID(objectID));
		for (const LatpLon& node : nodes) {
			boost::geometry::range::push_back(ls, boost::geometry::make<Point>(node.lon/10000000.0, node.latp/10000000.0));
		}
		std::shared_ptr<MultiPolygon> mp = std::make_shared<MultiPolygon>();
		Polygon p;
		geom::assign_points(p, ls);
		mp->push_back(p);

		correctGeometry(*mp);

		/*
		{
			const size_t shard = objectID % cacheMutex.size();
			std::lock_guard<std::mutex> lock(cacheMutex[shard]);
			polygonCacheSize[shard]++;
			if (polygonCacheSize[shard] == 5000) {
				cachedPolygons[shard].clear();
				polygonCacheSize[shard] = 0;
			}
			cachedPolygons[shard][objectID] = mp;
		}
		*/

		return mp;
	}

	if (IS_RELATION(objectID)) {
		{
			const size_t shard = objectID % cacheMutex.size();
			std::lock_guard<std::mutex> lock(cacheMutex[shard]);
			const auto& cache = cachedPolygons[shard];
			const auto& rv = cache.find(objectID);
			if (rv != cache.end())
				return rv->second;
		}

		if (false) {
			std::lock_guard<std::mutex> lock(mutex);
			freqs[objectID] = freqs[objectID] + 1;
		}




		std::shared_ptr<MultiPolygon> mp = std::make_shared<MultiPolygon>();
		WayVec outers, inners;
		const auto& relation = osmStore.relations.at(OSM_ID(objectID));
		for (const auto& way : relation.first)
			outers.push_back(way);
		for (const auto& way : relation.second)
			inners.push_back(way);

		//MultiPolygon mp = osmStore.wayListMultiPolygon(outers.begin(), outers.end(), inners.begin(), inners.end());
		osmStore.wayListMultiPolygon(*mp, outers.begin(), outers.end(), inners.begin(), inners.end());

		if (relationsThatNeedCorrection.find(OSM_ID(objectID)) != relationsThatNeedCorrection.end()) {
			correctGeometry(*mp);
		}

		{
			const size_t shard = objectID % cacheMutex.size();
			std::lock_guard<std::mutex> lock(cacheMutex[shard]);
			polygonCacheSize[shard]++;
			if (polygonCacheSize[shard] == 5000) {
				polygonCacheSize[shard] = 0;
				cachedPolygons[shard].clear();
			}
			cachedPolygons[shard][objectID] = mp;
		}

		return mp;
	}

	throw std::runtime_error("OsmMemTiles: buildMultiPolygon: unexpected objectID: " + std::to_string(objectID));
}

void OsmMemTiles::relationNeedsCorrection(RelationID id) {
	std::lock_guard<std::mutex> lock(mutex);
	relationsThatNeedCorrection.insert(id);
}

void OsmMemTiles::Clear() {
	for (auto& entry : objects)
		entry.clear();
	for (auto& entry : objectsWithIds)
		entry.clear();
}
