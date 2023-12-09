#include "osm_mem_tiles.h"
#include "node_store.h"
#include "way_store.h"
using namespace std;

OsmMemTiles::OsmMemTiles(
	size_t threadNum,
	uint baseZoom,
	bool includeID,
	const NodeStore& nodeStore,
	const WayStore& wayStore
)
	: TileDataSource(threadNum, baseZoom, includeID),
	nodeStore(nodeStore),
	wayStore(wayStore)
{ }

LatpLon OsmMemTiles::buildNodeGeometry(const OutputGeometryType geomType, const NodeID objectID, const TileBbox &bbox) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildNodeGeometry(geomType, objectID, bbox);
	}

	if (IS_NODE(objectID)) {
		const LatpLon& node = nodeStore.at(OSM_ID(objectID));
		return node;
	}

	throw std::runtime_error("buildNodeGeometry: unexpected objectID: " + std::to_string(objectID));
}

Linestring OsmMemTiles::buildLinestring(const NodeID objectID) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildLinestring(objectID);
	}

	if (IS_WAY(objectID)) {
		Linestring ls;

		std::vector<LatpLon> nodes = wayStore.at(OSM_ID(objectID));
		for (const LatpLon& node : nodes) {
			boost::geometry::range::push_back(ls, boost::geometry::make<Point>(node.lon/10000000.0, node.latp/10000000.0));
		}
		// TODO: CorrectGeometry?
		return ls;
	}

	throw std::runtime_error("buildLinestring: unexpected objectID: " + std::to_string(objectID));
}

MultiPolygon OsmMemTiles::buildMultiPolygon(const NodeID objectID) const {
	if (objectID < OSM_THRESHOLD) {
		return TileDataSource::buildMultiPolygon(objectID);
	}

	if (IS_WAY(objectID)) {
		Linestring ls;

		std::vector<LatpLon> nodes = wayStore.at(OSM_ID(objectID));
		for (const LatpLon& node : nodes) {
			boost::geometry::range::push_back(ls, boost::geometry::make<Point>(node.lon/10000000.0, node.latp/10000000.0));
		}
		MultiPolygon mp;
		Polygon p;
		geom::assign_points(p, ls);
		mp.push_back(p);

		// TODO: CorrectGeometry?
		return mp;
	}

	throw std::runtime_error("buildMultiPolygon: unexpected objectID: " + std::to_string(objectID));
}

void OsmMemTiles::Clear() {
	for (auto& entry : objects)
		entry.clear();
	for (auto& entry : objectsWithIds)
		entry.clear();
}
