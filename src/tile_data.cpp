#include <algorithm>
#include <iostream>
#include "tile_data.h"
#include "coordinates_geom.h"
#include <ciso646>

#define USE_RELATION_STORE (3ull << 34)
#define IS_RELATION(x) (((x) >> 34) == (USE_RELATION_STORE >> 34))

using namespace std;
extern bool verbose;

TileDataSource::TileDataSource(size_t threadNum, unsigned int baseZoom, bool includeID)
	:
	includeID(includeID),
	z6OffsetDivisor(baseZoom >= CLUSTER_ZOOM ? (1 << (baseZoom - CLUSTER_ZOOM)) : 1),
	objectsMutex(threadNum * 4),
	objects(CLUSTER_ZOOM_AREA),
	objectsWithIds(CLUSTER_ZOOM_AREA),
	baseZoom(baseZoom),
	clipCacheMutex(threadNum * 4),
	clipCacheSize(threadNum * 4)
{
	clipCache.reserve(threadNum * 4);
	for (int i = 0; i < threadNum * 4; i++)
		clipCache.push_back(
			boost::compute::detail::lru_cache<std::tuple<uint16_t, TileCoordinates, NodeID>, std::shared_ptr<MultiPolygon>>(5000)
		);
}

void TileDataSource::finalize(size_t threadNum) {
	finalizeObjects<OutputObjectXY>(threadNum, baseZoom, objects.begin(), objects.end());
	finalizeObjects<OutputObjectXYID>(threadNum, baseZoom, objectsWithIds.begin(), objectsWithIds.end());
}

void TileDataSource::addObjectToSmallIndex(const TileCoordinates& index, const OutputObject& oo, uint64_t id) {
	// Pick the z6 index
	const size_t z6x = index.x / z6OffsetDivisor;
	const size_t z6y = index.y / z6OffsetDivisor;

	if (z6x >= 64 || z6y >= 64) {
		if (verbose) std::cerr << "ignoring OutputObject with invalid z" << baseZoom << " coordinates " << index.x << ", " << index.y << " (id: " << id << ")" << std::endl;
		return;
	}

	const size_t z6index = z6x * CLUSTER_ZOOM_WIDTH + z6y;

	std::lock_guard<std::mutex> lock(objectsMutex[z6index % objectsMutex.size()]);

	if (id == 0 || !includeID)
		objects[z6index].push_back({
			oo,
			(Z6Offset)(index.x - (z6x * z6OffsetDivisor)),
			(Z6Offset)(index.y - (z6y * z6OffsetDivisor))
		});
	else
		objectsWithIds[z6index].push_back({
			oo,
			(Z6Offset)(index.x - (z6x * z6OffsetDivisor)),
			(Z6Offset)(index.y - (z6y * z6OffsetDivisor)),
			id
		});
}

void TileDataSource::collectTilesWithObjectsAtZoom(uint zoom, TileCoordinatesSet& output) {
	// Scan through all shards. Convert to base zoom, then convert to the requested zoom.
	collectTilesWithObjectsAtZoomTemplate<OutputObjectXY>(baseZoom, objects.begin(), objects.size(), zoom, output);
	collectTilesWithObjectsAtZoomTemplate<OutputObjectXYID>(baseZoom, objectsWithIds.begin(), objectsWithIds.size(), zoom, output);
}

void addCoveredTilesToOutput(const uint baseZoom, const uint zoom, const Box& box, TileCoordinatesSet& output) {
	int scale = pow(2, baseZoom-zoom);
	TileCoordinate minx = box.min_corner().x() / scale;
	TileCoordinate maxx = box.max_corner().x() / scale;
	TileCoordinate miny = box.min_corner().y() / scale;
	TileCoordinate maxy = box.max_corner().y() / scale;
	for (int x=minx; x<=maxx; x++) {
		for (int y=miny; y<=maxy; y++) {
			TileCoordinates newIndex(x, y);
			output.insert(newIndex);
		}
	}
}

// Find the tiles used by the "large objects" from the rtree index
void TileDataSource::collectTilesWithLargeObjectsAtZoom(uint zoom, TileCoordinatesSet &output) {
	for(auto const &result: boxRtree)
		addCoveredTilesToOutput(baseZoom, zoom, result.first, output);

	for(auto const &result: boxRtreeWithIds)
		addCoveredTilesToOutput(baseZoom, zoom, result.first, output);
}

// Copy objects from the tile at dstIndex (in the dataset srcTiles) into output
void TileDataSource::collectObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output
) {
	size_t iStart = 0;
	size_t iEnd = objects.size();

	// TODO: we could also narrow the search space for z1..z5, too.
	//       They're less important, as they have fewer tiles.
	if (zoom >= CLUSTER_ZOOM) {
		// Compute the x, y at the base zoom level
		TileCoordinate z6x = dstIndex.x / (1 << (zoom - CLUSTER_ZOOM));
		TileCoordinate z6y = dstIndex.y / (1 << (zoom - CLUSTER_ZOOM));

		if (z6x >= 64 || z6y >= 64) {
			if (verbose) std::cerr << "collectObjectsForTile: invalid tile z" << zoom << "/" << dstIndex.x << "/" << dstIndex.y << std::endl;
			return;
		}
		iStart = z6x * CLUSTER_ZOOM_WIDTH + z6y;
		iEnd = iStart + 1;
	}

	collectObjectsForTileTemplate<OutputObjectXY>(baseZoom, objects.begin(), iStart, iEnd, zoom, dstIndex, output);
	collectObjectsForTileTemplate<OutputObjectXYID>(baseZoom, objectsWithIds.begin(), iStart, iEnd, zoom, dstIndex, output);
}

// Copy objects from the large index into output
void TileDataSource::collectLargeObjectsForTile(
	uint zoom,
	TileCoordinates dstIndex,
	std::vector<OutputObjectID>& output
) {
	int scale = pow(2, baseZoom - zoom);
	TileCoordinates srcIndex1( dstIndex.x   *scale  ,  dstIndex.y   *scale  );
	TileCoordinates srcIndex2((dstIndex.x+1)*scale-1, (dstIndex.y+1)*scale-1);
	Box box = Box(geom::make<Point>(srcIndex1.x, srcIndex1.y),
	              geom::make<Point>(srcIndex2.x, srcIndex2.y));
	for(auto const& result: boxRtree | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box))) {
		if (result.second.minZoom <= zoom)
			output.push_back({result.second, 0});
	}

	for(auto const& result: boxRtreeWithIds | boost::geometry::index::adaptors::queried(boost::geometry::index::intersects(box))) {
		if (result.second.oo.minZoom <= zoom)
			output.push_back({result.second.oo, result.second.id});
	}
}

/*
Geometry TileDataSource::buildWayGeometryInternal(const OutputGeometryType geomType, const NodeID objectID) const {
}
*/

Point TileDataSource::buildPoint(const NodeID objectID) const {
	return retrieve_point(objectID);
}

std::shared_ptr<Linestring> TileDataSource::buildLinestring(const NodeID objectID) const {
	std::shared_ptr<Linestring> ls = std::make_shared<Linestring>();
	const linestring_t& ls2 = retrieve_linestring(objectID);
	boost::geometry::assign(*ls, ls2);
	return ls;
}

std::shared_ptr<MultiLinestring> TileDataSource::buildMultiLinestring(const NodeID objectID) const {
	std::shared_ptr<MultiLinestring> mls = std::make_shared<MultiLinestring>();
	const multi_linestring_t& mls2 = retrieve_multi_linestring(objectID);
	boost::geometry::assign(*mls, mls2);
	return mls;
}

std::shared_ptr<MultiPolygon> TileDataSource::buildMultiPolygon(const NodeID objectID) const {
	std::shared_ptr<MultiPolygon> rv = std::make_shared<MultiPolygon>();;
	const auto &input = retrieve_multi_polygon(objectID);
	boost::geometry::assign(*rv, input);
	return rv;
}

// Build node and way geometries
Geometry TileDataSource::buildWayGeometry(OutputGeometryType const geomType, 
                                          NodeID const objectID, const TileBbox &bbox) {
	switch(geomType) {
		case POINT_: {
			Point p = buildPoint(objectID);
			if (geom::within(p, bbox.clippingBox)) {
				return p;
			} 
			return MultiLinestring();
		}

		case LINESTRING_: {
			MultiLinestring out;
			std::shared_ptr<Linestring> lsPtr = buildLinestring(objectID);

			const Linestring& ls = *lsPtr;

			if(ls.empty())
				return out;

			Linestring current_ls;
			geom::append(current_ls, ls[0]);

			for(size_t i = 1; i < ls.size(); ++i) {
				if(!geom::intersects(Linestring({ ls[i-1], ls[i] }), bbox.clippingBox)) {
					if(current_ls.size() > 1)
						out.push_back(std::move(current_ls));
					current_ls.clear();
				}
				geom::append(current_ls, ls[i]);
			}

			if(current_ls.size() > 1)
				out.push_back(std::move(current_ls));

			MultiLinestring result;
			geom::intersection(out, bbox.getExtendBox(), result);
			return result;
		}

		case MULTILINESTRING_: {
			std::shared_ptr<MultiLinestring> mls = buildMultiLinestring(objectID);
			// investigate whether filtering the constituent linestrings improves performance
			MultiLinestring result;
			geom::intersection(*mls, bbox.getExtendBox(), result);
			return result;
		}

		case POLYGON_: {
			// Look for a previously clipped version at z-1, z-2, ...
			std::shared_ptr<MultiPolygon> cachedClip;

			if (IS_RELATION(objectID))
			{
				size_t zoom = bbox.zoom;
				size_t x = bbox.index.x;
				size_t y = bbox.index.y;
				std::lock_guard<std::mutex> lock(clipCacheMutex[objectID % clipCacheMutex.size()]);
				while (zoom > 0) {
					zoom--;
					x /= 2;
					y /= 2;
					auto& cache = clipCache[objectID % clipCache.size()];
					//const auto& rv = cache.find(std::make_tuple(zoom, TileCoordinates(x, y), objectID));
					const auto& rv = cache.get(std::make_tuple(zoom, TileCoordinates(x, y), objectID));
					if (!!rv) {
						cachedClip = rv.get();
						break;
					}
				}
			}

			std::shared_ptr<MultiPolygon> uncached;

			if (cachedClip == nullptr) {
				// The cached multipolygon uses a non-standard allocator, so copy it
				uncached = buildMultiPolygon(objectID);
			}

			const auto &input = cachedClip == nullptr ? *uncached : *cachedClip;

			Box box = bbox.clippingBox;
			
			if (bbox.endZoom) {
				for(auto const &p: input) {
					for(auto const &inner: p.inners()) {
						for(std::size_t i = 0; i < inner.size() - 1; ++i) 
						{
							Point p1 = inner[i];
							Point p2 = inner[i + 1];

							if(geom::within(p1, bbox.clippingBox) != geom::within(p2, bbox.clippingBox)) {
								box.min_corner() = Point(	
									std::min(box.min_corner().x(), std::min(p1.x(), p2.x())), 
									std::min(box.min_corner().y(), std::min(p1.y(), p2.y())));
								box.max_corner() = Point(	
									std::max(box.max_corner().x(), std::max(p1.x(), p2.x())), 
									std::max(box.max_corner().y(), std::max(p1.y(), p2.y())));
							}
						}
					}

					for(std::size_t i = 0; i < p.outer().size() - 1; ++i) {
						Point p1 = p.outer()[i];
						Point p2 = p.outer()[i + 1];

						if(geom::within(p1, bbox.clippingBox) != geom::within(p2, bbox.clippingBox)) {
							box.min_corner() = Point(	
								std::min(box.min_corner().x(), std::min(p1.x(), p2.x())), 
								std::min(box.min_corner().y(), std::min(p1.y(), p2.y())));
							box.max_corner() = Point(	
								std::max(box.max_corner().x(), std::max(p1.x(), p2.x())), 
								std::max(box.max_corner().y(), std::max(p1.y(), p2.y())));
						}
					}
				}

				Box extBox = bbox.getExtendBox();
				box.min_corner() = Point(	
					std::max(box.min_corner().x(), extBox.min_corner().x()), 
					std::max(box.min_corner().y(), extBox.min_corner().y()));
				box.max_corner() = Point(	
					std::min(box.max_corner().x(), extBox.max_corner().x()), 
					std::min(box.max_corner().y(), extBox.max_corner().y()));
			}

			MultiPolygon mp;
			geom::assign(mp, input);
			fast_clip(mp, box);
			geom::correct(mp);
			geom::validity_failure_type failure = geom::validity_failure_type::no_failure;
			if (!geom::is_valid(mp,failure)) { 
				if (failure==geom::failure_spikes) {
					geom::remove_spikes(mp);
				} else if (failure==geom::failure_self_intersections || failure==geom::failure_intersecting_interiors) {
					// retry with Boost intersection if fast_clip has caused self-intersections
					MultiPolygon output;
					geom::intersection(input, box, output);
					geom::correct(output);
					if (IS_RELATION(objectID))
						cacheClippedGeometry(bbox, objectID, output);
					return output;
				} else {
					// occasionally also wrong_topological_dimension, disconnected_interior
				}
			}

			if (IS_RELATION(objectID))
				cacheClippedGeometry(bbox, objectID, mp);
			return mp;
		}

		default:
			throw std::runtime_error("Invalid output geometry");
	}
}

void TileDataSource::cacheClippedGeometry(const TileBbox& box, const NodeID objectID, const MultiPolygon& mp) {
	// The point of caching is to reuse the clip, so caching at the terminal zoom is
	// pointless.
	if (box.zoom == baseZoom)
		return;

	std::shared_ptr<MultiPolygon> copy = std::make_shared<MultiPolygon>();
	boost::geometry::assign(*copy, mp);

	size_t index = objectID % clipCacheMutex.size();
	std::lock_guard<std::mutex> lock(clipCacheMutex[index]);
	auto& cache = clipCache[index];
	// In a perfect world, this would be an LRU cache and we'd evict old entries
	// that are unlikely to be used again.
	//
	// But for now, just reset the cache every so often to prevent it growing
	// without bound.
	/*
	clipCacheSize[index]++;
	if (clipCacheSize[index] > 5000) {
		clipCacheSize[index] = 0;
		cache.clear();
	}

	cache[std::make_tuple(box.zoom, box.index, objectID)] = copy;
	*/
	cache.insert(std::make_tuple(box.zoom, box.index, objectID), copy);
}

LatpLon TileDataSource::buildNodeGeometry(OutputGeometryType const geomType, 
                                          NodeID const objectID, const TileBbox &bbox) const {
	switch(geomType) {
		case POINT_: {
			auto p = retrieve_point(objectID);
			LatpLon out;
			out.latp = p.y();
			out.lon  = p.x();
			return out;
		}

		default:
			break;
	}

	throw std::runtime_error("Geometry type is not point");			
}


// Report number of stored geometries
void TileDataSource::reportSize() const {
	std::cout << "Generated points: " << (point_store->size()-1) << ", lines: " << (linestring_store->size() + multi_linestring_store->size() - 2) << ", polygons: " << (multi_polygon_store->size()-1) << std::endl;
}

TileCoordinatesSet getTilesAtZoom(
	const std::vector<class TileDataSource *>& sources,
	unsigned int zoom
) {
	TileCoordinatesSet tileCoordinates;

	// Create list of tiles
	tileCoordinates.clear();
	for(size_t i=0; i<sources.size(); i++) {
		sources[i]->collectTilesWithObjectsAtZoom(zoom, tileCoordinates);
		sources[i]->collectTilesWithLargeObjectsAtZoom(zoom, tileCoordinates);
	}

	return tileCoordinates;
}

std::vector<OutputObjectID> TileDataSource::getObjectsForTile(
	const std::vector<bool>& sortOrders, 
	unsigned int zoom,
	TileCoordinates coordinates
) {
	std::vector<OutputObjectID> data;
	collectObjectsForTile(zoom, coordinates, data);
	collectLargeObjectsForTile(zoom, coordinates, data);

	// Lexicographic comparison, with the order of: layer, geomType, attributes, and objectID.
	// Note that attributes is preferred to objectID.
	// It is to arrange objects with the identical attributes continuously.
	// Such objects will be merged into one object, to reduce the size of output.
	boost::sort::pdqsort(data.begin(), data.end(), [&sortOrders](const OutputObjectID& x, const OutputObjectID& y) -> bool {
		if (x.oo.layer < y.oo.layer) return true;
		if (x.oo.layer > y.oo.layer) return false;
		if (x.oo.z_order < y.oo.z_order) return  sortOrders[x.oo.layer];
		if (x.oo.z_order > y.oo.z_order) return !sortOrders[x.oo.layer];
		if (x.oo.geomType < y.oo.geomType) return true;
		if (x.oo.geomType > y.oo.geomType) return false;
		if (x.oo.attributes < y.oo.attributes) return true;
		if (x.oo.attributes > y.oo.attributes) return false;
		if (x.oo.objectID < y.oo.objectID) return true;
		return false;
	});
	data.erase(unique(data.begin(), data.end()), data.end());
	return data;
}

// ------------------------------------
// Add geometries to tile/large indices

void TileDataSource::addGeometryToIndex(
	const Linestring& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	unordered_set<TileCoordinates> tileSet;
	try {
		insertIntermediateTiles(geom, baseZoom, tileSet);

		bool polygonExists = false;
		TileCoordinate minTileX = TILE_COORDINATE_MAX, maxTileX = 0, minTileY = TILE_COORDINATE_MAX, maxTileY = 0;
		for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
			TileCoordinates index = *it;
			minTileX = std::min(index.x, minTileX);
			minTileY = std::min(index.y, minTileY);
			maxTileX = std::max(index.x, maxTileX);
			maxTileY = std::max(index.y, maxTileY);
			for (const auto& output : outputs) {
				if (output.geomType == POLYGON_) {
					polygonExists = true;
					continue;
				}
				addObjectToSmallIndex(index, output, id); // not a polygon
			}
		}

		// for polygon, fill inner tiles
		if (polygonExists) {
			bool tilesetFilled = false;
			uint size = (maxTileX - minTileX + 1) * (maxTileY - minTileY + 1);
			for (const auto& output : outputs) {
				if (output.geomType != POLYGON_) continue;
				if (size>= 16) {
					// Larger objects - add to rtree
					Box box = Box(geom::make<Point>(minTileX, minTileY),
					              geom::make<Point>(maxTileX, maxTileY));
					addObjectToLargeIndex(box, output, id);
				} else {
					// Smaller objects - add to each individual tile index
					if (!tilesetFilled) { fillCoveredTiles(tileSet); tilesetFilled = true; }
					for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
						TileCoordinates index = *it;
						addObjectToSmallIndex(index, output, id);
					}
				}
			}
		}
	} catch(std::out_of_range &err) {
		cerr << "Error calculating intermediate tiles: " << err.what() << endl;
	}
}

void TileDataSource::addGeometryToIndex(
	const MultiLinestring& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	for (Linestring ls : geom) {
		unordered_set<TileCoordinates> tileSet;
		insertIntermediateTiles(ls, baseZoom, tileSet);
		for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
			TileCoordinates index = *it;
			for (const auto& output : outputs) {
				addObjectToSmallIndex(index, output, id);
			}
		}
	}
}

void TileDataSource::addGeometryToIndex(
	const MultiPolygon& geom,
	const std::vector<OutputObject>& outputs,
	const uint64_t id
) {
	unordered_set<TileCoordinates> tileSet;
	bool singleOuter = geom.size()==1;
	for (Polygon poly : geom) {
		unordered_set<TileCoordinates> tileSetTmp;
		insertIntermediateTiles(poly.outer(), baseZoom, tileSetTmp);
		fillCoveredTiles(tileSetTmp);
		if (singleOuter) {
			tileSet = std::move(tileSetTmp);
		} else {
			tileSet.insert(tileSetTmp.begin(), tileSetTmp.end());
		}
	}
	
	TileCoordinate minTileX = TILE_COORDINATE_MAX, maxTileX = 0, minTileY = TILE_COORDINATE_MAX, maxTileY = 0;
	for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
		TileCoordinates index = *it;
		minTileX = std::min(index.x, minTileX);
		minTileY = std::min(index.y, minTileY);
		maxTileX = std::max(index.x, maxTileX);
		maxTileY = std::max(index.y, maxTileY);
	}
	for (const auto& output : outputs) {
		if (tileSet.size()>=16) {
			// Larger objects - add to rtree
			// note that the bbox is currently the envelope of the entire multipolygon,
			// which is suboptimal in shapes like (_) ...... (_) where the outers are significantly disjoint
			Box box = Box(geom::make<Point>(minTileX, minTileY),
			              geom::make<Point>(maxTileX, maxTileY));
			addObjectToLargeIndex(box, output, id);
		} else {
			// Smaller objects - add to each individual tile index
			for (auto it = tileSet.begin(); it != tileSet.end(); ++it) {
				TileCoordinates index = *it;
				addObjectToSmallIndex(index, output, id);
			}
		}
	}
}
