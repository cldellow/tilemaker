#include "lazy_way_nodes.h"

#include "osm_store.h"
#include "node_store.h"

LazyWayNodes::LazyWayNodes(NodeID originalOsmID, bool locationsOnWays, Way &pbfWay, OSMStore &osmStore):
	originalOsmID(originalOsmID),
	initedNodes(false),
	initedLatLons(false),
	locationsOnWays(locationsOnWays),
	pbfWay(pbfWay),
	osmStore(osmStore)
{
}

void LazyWayNodes::ensurePopulated(bool needLatLons) {
	if (!needLatLons && initedNodes)
		return;

	if (initedLatLons)
		return;

	if (locationsOnWays) {
		initedNodes = true;
		initedLatLons = true;
		int lat=0, lon=0;
		llVec.reserve(pbfWay.lats_size());
		for (int k=0; k<pbfWay.lats_size(); k++) {
			lat += pbfWay.lats(k);
			lon += pbfWay.lons(k);
			LatpLon ll = { int(lat2latp(double(lat)/10000000.0)*10000000.0), lon };
			llVec.push_back(ll);
		}
	} else {
		if (!initedNodes) {
			initedNodes = true;
			int64_t nodeId = 0;
			nodeVec.reserve(pbfWay.refs_size());
			for (int k=0; k<pbfWay.refs_size(); k++) {
				nodeId += pbfWay.refs(k);
					nodeVec.push_back(nodeId);
			}
		}

		if (needLatLons) {
			initedLatLons = true;
			llVec.reserve(nodeVec.size());

			for (auto nodeId : nodeVec) {
				try {
					llVec.push_back(osmStore.nodes.at(nodeId));
				} catch (std::out_of_range &err) {
					if (osmStore.integrity_enforced()) throw err;
				}
			}
		}
	}
}

const LatpLonVec& LazyWayNodes::getLlVec() {
	ensurePopulated(true);
	return llVec;
}

const std::vector<NodeID>& LazyWayNodes::getNodeVec() {
	ensurePopulated(false);
	return nodeVec;
}

bool LazyWayNodes::isClosed() {
	try {
		if (locationsOnWays)
			return getLlVec().front() == getLlVec().back();
		else
			return getNodeVec().front() == getNodeVec().back();
	} catch (std::out_of_range &err) {
		std::stringstream ss;
		ss << "Way " << originalOsmID << " is missing a node";
		throw std::out_of_range(ss.str());
	}
}
