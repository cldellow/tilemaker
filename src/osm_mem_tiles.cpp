#include "osm_mem_tiles.h"
#include "osm_store.h"
#include "node_store.h"
#include "way_store.h"
#include "relation_store.h"
using namespace std;

OsmMemTiles::OsmMemTiles(
	size_t threadNum,
	uint baseZoom,
	bool includeID,
	const OSMStore& osmStore
)
	: TileDataSource(threadNum, baseZoom, includeID),
	osmStore(osmStore)
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

Linestring OsmMemTiles::buildLinestring(const NodeID objectID) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildLinestring(objectID);
	}

	if (IS_WAY(objectID)) {
		Linestring ls;

		std::vector<LatpLon> nodes = osmStore.ways.at(OSM_ID(objectID));
		for (const LatpLon& node : nodes) {
			boost::geometry::range::push_back(ls, boost::geometry::make<Point>(node.lon/10000000.0, node.latp/10000000.0));
		}
		// TODO: CorrectGeometry: extract into shared library?
		geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
		geom::is_valid(ls, failure);
		if (failure==boost::geometry::failure_spikes)
			geom::remove_spikes(ls);
		if (failure)
			make_valid(ls);


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

		// TODO: CorrectGeometry: extract into shared library?
		geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
		geom::is_valid(mls, failure);
		if (failure==boost::geometry::failure_spikes)
			geom::remove_spikes(mls);
		if (failure)
			make_valid(mls);

		return mls;
	}

	throw std::runtime_error("OsmMemTiles: buildMultiLinestring: unexpected objectID: " + std::to_string(objectID));
}


MultiPolygon OsmMemTiles::buildMultiPolygon(const NodeID objectID) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildMultiPolygon(objectID);
	}

	if (IS_WAY(objectID)) {
		Linestring ls;

		std::vector<LatpLon> nodes = osmStore.ways.at(OSM_ID(objectID));
		for (const LatpLon& node : nodes) {
			boost::geometry::range::push_back(ls, boost::geometry::make<Point>(node.lon/10000000.0, node.latp/10000000.0));
		}
		MultiPolygon mp;
		Polygon p;
		geom::assign_points(p, ls);
		mp.push_back(p);

		// TODO: CorrectGeometry: extract into shared library?
		geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
		geom::is_valid(mp, failure);
		if (failure==boost::geometry::failure_spikes)
			geom::remove_spikes(mp);
		if (failure)
			make_valid(mp);

		return mp;
	}

	if (IS_RELATION(objectID)) {
		WayVec outers, inners;
		const auto& relation = osmStore.relations.at(OSM_ID(objectID));
		for (const auto& way : relation.first)
			outers.push_back(way);
		for (const auto& way : relation.second)
			inners.push_back(way);

		MultiPolygon mp = osmStore.wayListMultiPolygon(outers.begin(), outers.end(), inners.begin(), inners.end());

		// TODO: CorrectGeometry: extract into shared library?
		geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
		geom::is_valid(mp, failure);
		if (failure==boost::geometry::failure_spikes)
			geom::remove_spikes(mp);
		if (failure)
			make_valid(mp);

		return mp;
	}

	throw std::runtime_error("OsmMemTiles: buildMultiPolygon: unexpected objectID: " + std::to_string(objectID));
}

void OsmMemTiles::Clear() {
	for (auto& entry : objects)
		entry.clear();
	for (auto& entry : objectsWithIds)
		entry.clear();
}
