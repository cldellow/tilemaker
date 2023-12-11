#ifndef _LAZY_WAY_NODES_H
#define _LAZY_WAY_NODES_H

#include <vector>
#include "coordinates.h"
#include "osmformat.pb.h"

class OSMStore;

class LazyWayNodes {
public:
	LazyWayNodes(NodeID originalOsmID, bool locationsOnWays, Way &pbfWay, OSMStore &osmStore);
	const LatpLonVec& getLlVec();
	const std::vector<NodeID>& getNodeVec();
	bool isClosed();

	bool initedNodes;
	bool initedLatLons;

private:
	void ensurePopulated(bool needLatLons);

	NodeID originalOsmID;
	bool locationsOnWays;
	LatpLonVec llVec;
	std::vector<NodeID> nodeVec;
	Way &pbfWay;
	OSMStore &osmStore;
};

#endif
