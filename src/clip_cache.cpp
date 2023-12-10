#include "clip_cache.h"
#include "coordinates_geom.h"

ClipCache::ClipCache(size_t threadNum, unsigned int baseZoom):
	baseZoom(baseZoom),
	clipCacheMutex(threadNum * 4) {
	clipCache.reserve(threadNum * 4);
	for (int i = 0; i < threadNum * 4; i++)
		clipCache.push_back(
			boost::compute::detail::lru_cache<std::tuple<uint16_t, TileCoordinates, NodeID>, std::shared_ptr<MultiPolygon>>(5000)
		);
}

const std::shared_ptr<MultiPolygon> ClipCache::get(uint zoom, TileCoordinate x, TileCoordinate y, NodeID objectID) {
	// Look for a previously clipped version at z-1, z-2, ...

	std::lock_guard<std::mutex> lock(clipCacheMutex[objectID % clipCacheMutex.size()]);
	while (zoom > 0) {
		zoom--;
		x /= 2;
		y /= 2;
		auto& cache = clipCache[objectID % clipCache.size()];
		const auto& rv = cache.get(std::make_tuple(zoom, TileCoordinates(x, y), objectID));
		if (!!rv) {
			return rv.get();
		}
	}

	return nullptr;
}

void ClipCache::add(const TileBbox& box, const NodeID objectID, const MultiPolygon& mp) {
	// The point of caching is to reuse the clip, so caching at the terminal zoom is
	// pointless.
	if (box.zoom == baseZoom)
		return;

	std::shared_ptr<MultiPolygon> copy = std::make_shared<MultiPolygon>();
	boost::geometry::assign(*copy, mp);

	size_t index = objectID % clipCacheMutex.size();
	std::lock_guard<std::mutex> lock(clipCacheMutex[index]);
	auto& cache = clipCache[index];

	cache.insert(std::make_tuple(box.zoom, box.index, objectID), copy);
}
