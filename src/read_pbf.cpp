#include <iostream>
#include "read_pbf.h"
#include "pbf_blocks.h"
#include "pbf_reader.h"

#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <unordered_set>

#include "node_store.h"
#include "way_store.h"
#include "osm_lua_processing.h"
#include "mmap_allocator.h"

using namespace std;

const std::string OptionSortTypeThenID = "Sort.Type_then_ID";
const std::string OptionLocationsOnWays = "LocationsOnWays";
std::atomic<uint64_t> blocksProcessed(0), blocksToProcess(0);
std::mutex pbfm;

PbfProcessor::PbfProcessor(OSMStore &osmStore)
	: osmStore(osmStore)
{ }

bool PbfProcessor::ReadNodes(OsmLuaProcessing& output, PbfReader::PrimitiveGroup& pg, const PbfReader::PrimitiveBlock& pb, const unordered_set<int>& nodeKeyPositions)
{
	// ----	Read nodes
	std::vector<NodeStore::element_t> nodes;		

	bool hadNodes = false;
	for (auto& node : pg.nodes()) {
		hadNodes = true;

		NodeID nodeId = node.id;
		LatpLon latplon = { int(lat2latp(double(node.lat)/10000000.0)*10000000.0), node.lon };

		bool significant = false;
		for (int i = node.tagStart; i < node.tagEnd; i += 2) {
			auto keyIndex = pg.translateNodeKeyValue(i);

			if (nodeKeyPositions.find(keyIndex) != nodeKeyPositions.end()) {
				significant = true;
			}
		}

		nodes.push_back(std::make_pair(static_cast<NodeID>(nodeId), latplon));

		if (significant) {
			// For tagged nodes, call Lua, then save the OutputObject
			boost::container::flat_map<protozero::data_view, protozero::data_view, DataViewLessThan> tags;
			tags.reserve((node.tagEnd - node.tagStart) / 2);

			for (int n = node.tagStart; n < node.tagEnd; n += 2) {
				auto keyIndex = pg.translateNodeKeyValue(n);
				auto valueIndex = pg.translateNodeKeyValue(n + 1);

				protozero::data_view key{pb.stringTable[keyIndex].data(), pb.stringTable[keyIndex].size()};
				protozero::data_view value{pb.stringTable[valueIndex].data(), pb.stringTable[valueIndex].size()};
				tags[key] = value;
			}
			output.setNode(static_cast<NodeID>(nodeId), latplon, tags);
		} 
	}

	if (nodes.size() > 0) {
		osmStore.nodes.insert(nodes);
	}

	return hadNodes;
}

bool PbfProcessor::ReadWays(
	OsmLuaProcessing &output,
	PbfReader::PrimitiveGroup& pg,
	const PbfReader::PrimitiveBlock& pb,
	bool locationsOnWays,
	uint shard,
	uint effectiveShards
) {
	// ----	Read ways
	{
		// TODO: make this less ugly
		auto b = pg.ways().begin();
		auto e = pg.ways().end();
		if (!(b != e)) return false;
	}

	const bool wayStoreRequiresNodes = osmStore.ways.requiresNodes();

	std::vector<WayStore::ll_element_t> llWays;
	std::vector<std::pair<WayID, std::vector<NodeID>>> nodeWays;
	LatpLonVec llVec;
	std::vector<NodeID> nodeVec;

	for (PbfReader::Way pbfWay : pg.ways()) {
		llVec.clear();
		nodeVec.clear();

		WayID wayId = static_cast<WayID>(pbfWay.id);
		if (wayId >= pow(2,42)) throw std::runtime_error("Way ID negative or too large: "+std::to_string(wayId));

		// Assemble nodelist
		if (locationsOnWays) {
			llVec.reserve(pbfWay.lats.size());
			for (int k=0; k<pbfWay.lats.size(); k++) {
				int lat = pbfWay.lats[k];
				int lon = pbfWay.lons[k];
				LatpLon ll = { int(lat2latp(double(lat)/10000000.0)*10000000.0), lon };
				llVec.push_back(ll);
			}
		} else {
			llVec.reserve(pbfWay.refs.size());
			nodeVec.reserve(pbfWay.refs.size());

			bool skipToNext = false;

			for (int k=0; k<pbfWay.refs.size(); k++) {
				NodeID nodeId = pbfWay.refs[k];

				if (k == 0 && effectiveShards > 1 && !osmStore.nodes.contains(shard, nodeId)) {
					skipToNext = true;
					break;
				}

				try {
					llVec.push_back(osmStore.nodes.at(static_cast<NodeID>(nodeId)));
					nodeVec.push_back(nodeId);
				} catch (std::out_of_range &err) {
					if (osmStore.integrity_enforced()) throw err;
				}
			}

			if (skipToNext)
				continue;
		}
		if (llVec.empty()) continue;

		try {
			tag_map_t tags;
			readTags(pbfWay, pb, tags);
			bool emitted = output.setWay(static_cast<WayID>(pbfWay.id), llVec, tags);

			// If we need it for later, store the way's coordinates in the global way store
			if (emitted || osmStore.way_is_used(wayId)) {
				if (wayStoreRequiresNodes)
					nodeWays.push_back(std::make_pair(wayId, nodeVec));
				else
					llWays.push_back(std::make_pair(wayId, WayStore::latplon_vector_t(llVec.begin(), llVec.end())));
			}

		} catch (std::out_of_range &err) {
			// Way is missing a node?
			cerr << endl << err.what() << endl;
		}

	}

	if (wayStoreRequiresNodes) {
		osmStore.ways.shard(shard).insertNodes(nodeWays);
	} else {
		osmStore.ways.shard(shard).insertLatpLons(llWays);
	}

	return true;
}

bool PbfProcessor::ScanRelations(OsmLuaProcessing& output, PbfReader::PrimitiveGroup& pg, const PbfReader::PrimitiveBlock& pb) {
	// Scan relations to see which ways we need to save
	{
		// TODO: make this less ugly
		auto b = pg.relations().begin();
		auto e = pg.relations().end();
		if (!(b != e)) return false;
	}

	int typeKey = findStringPosition(pb, "type");
	int mpKey   = findStringPosition(pb, "multipolygon");

	for (PbfReader::Relation pbfRelation : pg.relations()) {
		bool isMultiPolygon = relationIsType(pbfRelation, typeKey, mpKey);
		bool isAccepted = false;
		WayID relid = static_cast<WayID>(pbfRelation.id);
		if (!isMultiPolygon) {
			if (output.canReadRelations()) {
				tag_map_t tags;
				readTags(pbfRelation, pb, tags);
				isAccepted = output.scanRelation(relid, tags);
			}
			if (!isAccepted) continue;
		}
		for (int n=0; n < pbfRelation.memids.size(); n++) {
			uint64_t lastID = pbfRelation.memids[n];
			if (pbfRelation.types[n] != PbfReader::Relation::MemberType::WAY) { continue; }
			if (lastID >= pow(2,42)) throw std::runtime_error("Way ID in relation "+std::to_string(relid)+" negative or too large: "+std::to_string(lastID));
			osmStore.mark_way_used(static_cast<WayID>(lastID));
			if (isAccepted) { osmStore.relation_contains_way(relid, lastID); }
		}
	}
	return true;
}

bool PbfProcessor::ReadRelations(
	OsmLuaProcessing& output,
	PbfReader::PrimitiveGroup& pg,
	const PbfReader::PrimitiveBlock& pb,
	const BlockMetadata& blockMetadata,
	uint shard,
	uint effectiveShards
) {
	// ----	Read relations
	{
		// TODO: make this less ugly
		auto b = pg.relations().begin();
		auto e = pg.relations().end();
		if (!(b != e)) return false;
	}

	std::vector<RelationStore::element_t> relations;

	int typeKey = findStringPosition(pb, "type");
	int mpKey   = findStringPosition(pb, "multipolygon");
	int boundaryKey = findStringPosition(pb, "boundary");
	int innerKey= findStringPosition(pb, "inner");
	int outerKey= findStringPosition(pb, "outer");
	if (typeKey >-1 && mpKey>-1) {
		int j = -1;
		for (PbfReader::Relation pbfRelation : pg.relations()) {
			j++;
			if (j % blockMetadata.chunks != blockMetadata.chunk)
				continue;

			bool isMultiPolygon = relationIsType(pbfRelation, typeKey, mpKey);
			bool isBoundary = relationIsType(pbfRelation, typeKey, boundaryKey);
			if (!isMultiPolygon && !isBoundary && !output.canWriteRelations()) continue;

			// Read relation members
			WayVec outerWayVec, innerWayVec;
			bool isInnerOuter = isBoundary || isMultiPolygon;
			bool skipToNext = false;
			bool firstWay = true;
			for (int n = 0; n < pbfRelation.memids.size(); n++) {
				uint64_t lastID = pbfRelation.memids[n];
				if (pbfRelation.types[n] != PbfReader::Relation::MemberType::WAY) { continue; }
				int32_t role = pbfRelation.roles_sid[n];
				if (role==innerKey || role==outerKey) isInnerOuter=true;
				WayID wayId = static_cast<WayID>(lastID);

				if (firstWay && effectiveShards > 1 && !osmStore.ways.contains(shard, wayId)) {
					skipToNext = true;
					break;
				}
				if (firstWay)
					firstWay = false;
				(role == innerKey ? innerWayVec : outerWayVec).push_back(wayId);
			}

			if (skipToNext)
				continue;

			try {
				tag_map_t tags;
				readTags(pbfRelation, pb, tags);
				timespec start, end;
				clock_gettime(CLOCK_MONOTONIC, &start);

				output.setRelation(pbfRelation.id, outerWayVec, innerWayVec, tags, isMultiPolygon, isInnerOuter);
				if (pbfRelation.id == 1414848 || pbfRelation.id == 4039486 || pbfRelation.id == 1205151) {
					clock_gettime(CLOCK_MONOTONIC, &end);
					uint64_t tileNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;

					std::lock_guard<std::mutex> lock(pbfm);
					std::cout << std::endl << "rel: " << pbfRelation.id << ": duration= " << (uint32_t)(tileNs / 1e6) << " ms, members= " << (outerWayVec.size() + innerWayVec.size()) << " outers= " << outerWayVec.size() << " inners= " << innerWayVec.size() << " isMultiPolygon=" << isMultiPolygon << " isInnerOuter=" << isInnerOuter << std::endl;
				}

			} catch (std::out_of_range &err) {
				// Relation is missing a member?
				cerr << endl << err.what() << endl;
			}
		}
	}

	osmStore.relations_insert_front(relations);
	return true;
}

// Returns true when block was completely handled, thus could be omited by another phases.
bool PbfProcessor::ReadBlock(
	std::istream& infile,
	OsmLuaProcessing& output,
	const BlockMetadata& blockMetadata,
	const unordered_set<string>& nodeKeys,
	bool locationsOnWays,
	ReadPhase phase,
	uint shard,
	uint effectiveShards
) 
{
	infile.seekg(blockMetadata.offset);

	protozero::data_view blob = PbfReader::readBlob(blockMetadata.length, infile);
	PbfReader::PrimitiveBlock& pb = PbfReader::readPrimitiveBlock(blob);
	if (infile.eof()) {
		return true;
	}

	// Keep count of groups read during this phase.
	std::size_t read_groups = 0;

	// Read the string table, and pre-calculate the positions of valid node keys
	unordered_set<int> nodeKeyPositions;
	for (auto it : nodeKeys) {
		//nodeKeyPositions.insert(findStringPosition(pb, it));
		auto rv = findStringPosition(pb, it);
		nodeKeyPositions.insert(rv);
	}

	int primitiveGroupSize = 0;
	for (auto& pg : pb.groups()) {
		primitiveGroupSize++;
	
		auto output_progress = [&]()
		{
			if (ioMutex.try_lock()) {
				std::ostringstream str;
				str << "\r";
				void_mmap_allocator::reportStoreSize(str);
				if (effectiveShards > 1)
					str << std::to_string(shard + 1) << "/" << std::to_string(effectiveShards) << " ";

				// TODO: revive showing the # of ways/relations?
				str << "Block " << blocksProcessed.load() << "/" << blocksToProcess.load() << " ";
				std::cout << str.str();
				std::cout.flush();
				ioMutex.unlock();
			}
		};

		if(phase == ReadPhase::Nodes) {
			bool done = ReadNodes(output, pg, pb, nodeKeyPositions);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}

		if(phase == ReadPhase::RelationScan) {
			osmStore.ensureUsedWaysInited();
			bool done = ScanRelations(output, pg, pb);
			if(done) { 
				if (ioMutex.try_lock()) {
					std::cout << "\r(Scanning for ways used in relations: " << (100*blocksProcessed.load()/blocksToProcess.load()) << "%)           ";
					std::cout.flush();
					ioMutex.unlock();
				}
				continue;
			}
		}
	
		if(phase == ReadPhase::Ways) {
			bool done = ReadWays(output, pg, pb, locationsOnWays, shard, effectiveShards);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}

		if(phase == ReadPhase::Relations) {
			bool done = ReadRelations(output, pg, pb, blockMetadata, shard, effectiveShards);
			if(done) { 
				output_progress();
				++read_groups;
				continue;
			}
		}
	}

	// Possible cases of a block contents:
	// - single group
	// - multiple groups of the same type
	// - multiple groups of the different type
	// 
	// In later case block would not be handled during this phase, and should be
	// read again in remaining phases. Thus we return false to indicate that the
	// block was not handled completelly.
	if(read_groups != primitiveGroupSize) {
		return false;
	}

	// We can only delete blocks if we're confident we've processed everything,
	// which is not possible in the case of subdivided blocks.
	return (shard + 1 == effectiveShards) && blockMetadata.chunks == 1;
}

bool blockHasPrimitiveGroupSatisfying(
	std::istream& infile,
	const BlockMetadata block,
	std::function<bool(const PbfReader::PrimitiveGroup&)> test
) {
	// We may have previously read to EOF, so clear the internal error state
	infile.clear();
	infile.seekg(block.offset);
	protozero::data_view blob = PbfReader::readBlob(block.length, infile);
	PbfReader::PrimitiveBlock pb = PbfReader::readPrimitiveBlock(blob);

	if (infile.eof()) {
		throw std::runtime_error("blockHasPrimitiveGroupSatisfying got unexpected eof");
	}

	for (auto& pg : pb.groups()) {
		if (test(pg))
			return false;
	}
	
	return true;
}

int PbfProcessor::ReadPbfFile(
	uint shards,
	bool hasSortTypeThenID,
	unordered_set<string> const& nodeKeys,
	unsigned int threadNum,
	const pbfreader_generate_stream& generate_stream,
	const pbfreader_generate_output& generate_output,
	const NodeStore& nodeStore,
	const WayStore& wayStore
)
{
	auto infile = generate_stream();

	// ----	Read PBF
	osmStore.clear();

	PbfReader::HeaderBlock block = PbfReader::readHeaderFromFile(*infile);
	bool locationsOnWays = block.optionalFeatures.find(OptionLocationsOnWays) != block.optionalFeatures.end();
	if (locationsOnWays) {
		std::cout << ".osm.pbf file has locations on ways" << std::endl;
	}

	std::map<std::size_t, BlockMetadata> blocks;

	// Track the filesize - note that we can't rely on tellg(), as
	// its meant to be an opaque token useful only for seeking.
	size_t filesize = 0;
	while (true) {
		PbfReader::BlobHeader bh = PbfReader::readBlobHeader(*infile);
		filesize += bh.datasize;
		if (infile->eof()) {
			break;
		}

		blocks[blocks.size()] = { (long int)infile->tellg(), bh.datasize, true, true, true, 0, 1 };
		infile->seekg(bh.datasize, std::ios_base::cur);
	}

	if (hasSortTypeThenID) {
		// The PBF's blocks are sorted by type, then ID. We can do a binary search
		// to learn where the blocks transition between object types, which
		// enables a more efficient partitioning of work for reading.
		std::vector<size_t> indexes;
		for (int i = 0; i < blocks.size(); i++)
			indexes.push_back(i);

		const auto& waysStart = std::lower_bound(
			indexes.begin(),
			indexes.end(),
			0,
			[&blocks, &infile](const auto &i, const auto &ignored) {
				return blockHasPrimitiveGroupSatisfying(
					*infile,
					blocks[i],
					[](const PbfReader::PrimitiveGroup& pg) {
						for(auto w : pg.ways()) return true;
						for(auto r : pg.relations()) return true;
						return false;
					}
				);
			}
		);

		const auto& relationsStart = std::lower_bound(
			indexes.begin(),
			indexes.end(),
			0,
			[&blocks, &infile](const auto &i, const auto &ignored) {
				return blockHasPrimitiveGroupSatisfying(
					*infile,
					blocks[i],
					[](const PbfReader::PrimitiveGroup& pg) {
						for (auto r : pg.relations()) return true;
						return false;
					}
				);
			}
		);

		for (auto it = indexes.begin(); it != indexes.end(); it++) {
			blocks[*it].hasNodes = it <= waysStart;
			blocks[*it].hasWays = it >= waysStart && it <= relationsStart;
			blocks[*it].hasRelations = it >= relationsStart;
		}
	}


	// PBFs generated by Osmium have 8,000 entities per block,
	// and each block is about 64KB.
	//
	// PBFs generated by osmconvert (e.g., BBBike PBFs) have as
	// many entities as fit in 31MB. Each block is about 16MB.
	//
	// Osmium PBFs seem to be processed about 3x faster than osmconvert
	// PBFs, so try to hint to the user when they could speed up their
	// pipeline.
	if (filesize / blocks.size() > 1000000) {
		std::cout << "warning: PBF has very large blocks, which may slow processing" << std::endl;
		std::cout << "         to fix: osmium cat -f pbf your-file.osm.pbf -o optimized.osm.pbf" << std::endl;
	}


	std::vector<ReadPhase> all_phases = { ReadPhase::Nodes, ReadPhase::RelationScan, ReadPhase::Ways, ReadPhase::Relations };
	for(auto phase: all_phases) {
		uint effectiveShards = 1;

		// On memory-constrained machines, we might read ways/relations
		// multiple times in order to keep the working set of nodes limited.
		if (phase == ReadPhase::Ways || phase == ReadPhase::Relations)
			effectiveShards = shards;

		for (int shard = 0; shard < effectiveShards; shard++) {
			// If we're in ReadPhase::Ways, only do a pass if there is at least one
			// entry in the pass's shard.
			if (phase == ReadPhase::Ways && nodeStore.shard(shard).size() == 0)
				continue;

			// Ditto, but for relations
			if (phase == ReadPhase::Relations && wayStore.shard(shard).size() == 0)
				continue;

#ifdef CLOCK_MONOTONIC
			timespec start, end;
			clock_gettime(CLOCK_MONOTONIC, &start);
#endif

			// Launch the pool with threadNum threads
			boost::asio::thread_pool pool(threadNum);
			std::mutex block_mutex;

			// If we're in ReadPhase::Relations and there aren't many blocks left
			// to read, increase parallelism by letting each thread only process
			// a portion of the block.
			if (phase == ReadPhase::Relations && blocks.size() < threadNum * 2) {
				std::cout << "only " << blocks.size() << " relation blocks; subdividing for better parallelism" << std::endl;
				std::map<std::size_t, BlockMetadata> moreBlocks;
				for (const auto& block : blocks) {
					BlockMetadata newBlock = block.second;
					newBlock.chunks = threadNum;
					for (size_t i = 0; i < threadNum; i++) {
						newBlock.chunk = i;
						moreBlocks[moreBlocks.size()] = newBlock;
					}
				}
				blocks = moreBlocks;
			}

			std::deque<std::vector<IndexedBlockMetadata>> blockRanges;
			std::map<std::size_t, BlockMetadata> filteredBlocks;
			for (const auto& entry : blocks) {
				if ((phase == ReadPhase::Nodes && entry.second.hasNodes) ||
						(phase == ReadPhase::RelationScan && entry.second.hasRelations) ||
						(phase == ReadPhase::Ways && entry.second.hasWays) ||
						(phase == ReadPhase::Relations && entry.second.hasRelations))
					filteredBlocks[entry.first] = entry.second;
			}

			blocksToProcess = filteredBlocks.size();
			blocksProcessed = 0;

			// Relations have very non-uniform processing times, so prefer
			// to process them as granularly as possible.
			size_t batchSize = 1;

			// When creating NodeStore/WayStore, we try to give each worker
			// large batches of contiguous blocks, so that they might benefit from
			// long runs of sorted indexes, and locality of nearby IDs.
			if (phase == ReadPhase::Nodes || phase == ReadPhase::Ways)
				batchSize = (filteredBlocks.size() / (threadNum * 8)) + 1;

			size_t consumed = 0;
			auto it = filteredBlocks.begin();
			while(it != filteredBlocks.end()) {
				std::vector<IndexedBlockMetadata> blockRange;
				blockRange.reserve(batchSize);
				size_t max = consumed + batchSize;
				for (; consumed < max && it != filteredBlocks.end(); consumed++) {
					IndexedBlockMetadata ibm;
					memcpy(&ibm, &it->second, sizeof(BlockMetadata));
					ibm.index = it->first;
					blockRange.push_back(ibm);
					it++;
				}
				blockRanges.push_back(blockRange);
			}

			{
				for(const std::vector<IndexedBlockMetadata>& blockRange: blockRanges) {
					boost::asio::post(pool, [=, &blockRange, &blocks, &block_mutex, &nodeKeys]() {
						if (phase == ReadPhase::Nodes)
							osmStore.nodes.batchStart();
						if (phase == ReadPhase::Ways)
							osmStore.ways.batchStart();

						for (const IndexedBlockMetadata& indexedBlockMetadata: blockRange) {
							auto infile = generate_stream();
							auto output = generate_output();

							if(ReadBlock(*infile, *output, indexedBlockMetadata, nodeKeys, locationsOnWays, phase, shard, effectiveShards)) {
								const std::lock_guard<std::mutex> lock(block_mutex);
								blocks.erase(indexedBlockMetadata.index);	
							}
							blocksProcessed++;
						}
					});
				}
			}
		
			pool.join();

#ifdef CLOCK_MONOTONIC
			clock_gettime(CLOCK_MONOTONIC, &end);
			uint64_t elapsedNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
			std::cout << "(" << std::to_string((uint32_t)(elapsedNs / 1e6)) << " ms)" << std::endl;
#endif
		}

		if(phase == ReadPhase::Nodes) {
			osmStore.nodes.finalize(threadNum);
		}
		if(phase == ReadPhase::Ways) {
			osmStore.ways.finalize(threadNum);
		}
	}
	return 0;
}

// Find a string in the dictionary
int PbfProcessor::findStringPosition(const PbfReader::PrimitiveBlock& pb, const std::string& str) {
	for (int i = 0; i < pb.stringTable.size(); i++) {
		if(str.size() == pb.stringTable[i].size() && memcmp(str.data(), pb.stringTable[i].data(), str.size()) == 0)
			return i;
	}
	return -1;
}


// *************************************************

int ReadPbfBoundingBox(const std::string &inputFile, double &minLon, double &maxLon, 
	double &minLat, double &maxLat, bool &hasClippingBox)
{
	fstream infile(inputFile, ios::in | ios::binary);
	if (!infile) { cerr << "Couldn't open .pbf file " << inputFile << endl; return -1; }
	auto header = PbfReader::readHeaderFromFile(infile);
	if (header.hasBbox) {
		hasClippingBox = true;
		minLon = header.bbox.minLon;
		maxLon = header.bbox.maxLon;
		minLat = header.bbox.minLat;
		maxLat = header.bbox.maxLat;
	}
	infile.close();
	return 0;
}

bool PbfHasOptionalFeature(const std::string& inputFile, const std::string& feature) {
	std::ifstream infile(inputFile, std::ifstream::in);
	auto header = PbfReader::readHeaderFromFile(infile);
	infile.close();
	return header.optionalFeatures.find(feature) != header.optionalFeatures.end();
}
