#include <iostream>
#include <fstream>
#include <sstream>
#include "external/minunit.h"
#include <protozero/pbf_writer.hpp>
#include "helpers.h"
#include "pbf_reader.h"

// Reads monaco.pbf (or a re-encoding of it) and checks its contents.
void checkMonaco(std::istream& monaco) {
	PbfReader::PbfReader reader;
	PbfReader::BlobHeader bh = reader.readBlobHeader(monaco);
	protozero::data_view blob = reader.readBlob(bh.datasize, monaco);
	PbfReader::HeaderBlock header = reader.readHeaderBlock(blob);

	mu_check(header.hasBbox);
	mu_check(header.optionalFeatures.size() == 1);
	mu_check(header.optionalFeatures.find("Sort.Type_then_ID") != header.optionalFeatures.end());

	mu_check(header.bbox.minLon == 7.409205);
	mu_check(header.bbox.maxLon == 7.448637);
	mu_check(header.bbox.minLat == 43.723350);
	mu_check(header.bbox.maxLat == 43.751690);


	bool foundNode = false, foundWay = false, foundRelation = false;
	int blocks = 0, groups = 0, strings = 0, nodes = 0, ways = 0, relations = 0;
	while (!monaco.eof()) {
		bh = reader.readBlobHeader(monaco);
		if (bh.type == "eof")
			break;


		blocks++;
		blob = reader.readBlob(bh.datasize, monaco);

		PbfReader::PrimitiveBlock pb = reader.readPrimitiveBlock(blob);

		for (const auto str : pb.stringTable) {
			if (strings == 200) {
				std::string s(str.data(), str.size());
				mu_check(s == "description:FR");
			}
			strings++;
		}

		for (const auto& group : pb.groups()) {
			groups++;
			for (const auto& node : group.nodes()) {
				nodes++;

				if (node.id == 21911886) {
					foundNode = true;

					bool foundHighwayCrossing = false;

					for (int i = node.tagStart; i < node.tagEnd; i += 2) {
						const auto keyIndex = group.translateNodeKeyValue(i);
						const auto valueIndex = group.translateNodeKeyValue(i + 1);
						std::string key(pb.stringTable[keyIndex].data(), pb.stringTable[keyIndex].size());
						std::string value(pb.stringTable[valueIndex].data(), pb.stringTable[valueIndex].size());

						if (key == "highway" && value == "crossing")
							foundHighwayCrossing = true;
					}
					mu_check(foundHighwayCrossing);
				}
			}

			for (const auto& way : group.ways()) {
				ways++;

				if (way.id == 4224978) {
					foundWay = true;

					bool foundSportSoccer = false;
					for (int i = 0; i < way.keys.size(); i++) {
						std::string key(pb.stringTable[way.keys[i]].data(), pb.stringTable[way.keys[i]].size());
						std::string value(pb.stringTable[way.vals[i]].data(), pb.stringTable[way.vals[i]].size());

						if (key == "sport" && value == "soccer")
							foundSportSoccer = true;
					}
					mu_check(foundSportSoccer);

					mu_check(way.refs.size() == 5);
					mu_check(way.refs[0] == 25178088);
					mu_check(way.refs[2] == 25178045);
					mu_check(way.refs[4] == 25178088);
				}
			}

			for (const auto& relation : group.relations()) {
				relations++;

				if (relation.id == 1124039) {
					foundRelation = true;
					mu_check(relation.memids.size() == 17);
					mu_check(relation.types.size() == 17);
					mu_check(relation.roles_sid.size() == 17);
					mu_check(relation.types[0] == PbfReader::Relation::MemberType::NODE);
					mu_check(relation.types[2] == PbfReader::Relation::MemberType::WAY);
					mu_check(relation.types[16] == PbfReader::Relation::MemberType::RELATION);
				}
			}
		}
	}

	//std::cout << blocks << " blocks, " << groups << " groups, " << nodes << " nodes, " << ways << " ways, " << relations << " relations" << std::endl;

	mu_check(foundNode);
	mu_check(foundWay);
	mu_check(foundRelation);

	mu_check(blocks == 6);
	mu_check(groups == 6);
	mu_check(strings == 8236);
	mu_check(nodes == 30477);
	mu_check(ways == 4825);
	mu_check(relations == 285);
}

MU_TEST(test_pbf_reader) {
	std::ifstream monaco("test/monaco.pbf", std::ifstream::in);
	checkMonaco(monaco);
}

void writeBlobHeaderAndBlob(std::ostream& out, const std::string& type, const std::string& blob) {
	std::string header;
	protozero::pbf_writer headerWriter{header};
	headerWriter.add_string(1, type); // type
	headerWriter.add_int32(3, blob.size()); // datasize

	unsigned int size = header.size();
	endian_swap(size);
	out.write((const char*)&size, sizeof(size));
	out << header << blob;
}

MU_TEST(test_pbf_reader_zstd) {
	// Re-encode monaco.pbf with zstd-compressed blobs, then read it back.
	std::ifstream monaco("test/monaco.pbf", std::ifstream::in);
	std::stringstream zstdPbf;
	PbfReader::PbfReader reader;
	std::string raw;
	int blobs = 0;
	while (true) {
		PbfReader::BlobHeader bh = reader.readBlobHeader(monaco);
		if (bh.type == "eof")
			break;
		protozero::data_view data = reader.readBlob(bh.datasize, monaco, raw, true);
		std::string uncompressed(data.data(), data.size());

		std::string blob;
		protozero::pbf_writer blobWriter{blob};
		// Leave out raw_size from one blob, which is allowed for zstd since the
		// frame records its size.
		if (blobs != 1)
			blobWriter.add_int32(2, uncompressed.size()); // raw_size
		blobWriter.add_bytes(7, compress_zstd(uncompressed)); // zstd_data
		writeBlobHeaderAndBlob(zstdPbf, bh.type, blob);
		blobs++;
	}
	mu_check(blobs == 7);

	zstdPbf.seekg(0);
	checkMonaco(zstdPbf);
}

MU_TEST_SUITE(test_suite_pbf_reader) {
	MU_RUN_TEST(test_pbf_reader);
	MU_RUN_TEST(test_pbf_reader_zstd);
}

int main() {
	MU_RUN_SUITE(test_suite_pbf_reader);
	MU_REPORT();
	return MU_EXIT_CODE;
}
