#include <iostream>
#include <stdexcept>
#include <zstd.h>
#include "external/minunit.h"
#include "helpers.h"

MU_TEST(test_open_file) {
	FilePtr fp = openFile("test/test.jsonl", "r");
	mu_check(fp != nullptr);
	mu_check(fgetc(fp.get()) == '{');

	bool caughtException = false;
	try {
		openFile("test/this-file-does-not-exist", "r");
	} catch (const std::runtime_error&) {
		caughtException = true;
	}
	mu_check(caughtException);
}

MU_TEST(test_get_chunks) {
	{
		auto rv = getNewlineChunks("test/test.jsonl", 1);
		mu_check(rv.size() == 1);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 24);
	}

	{
		auto rv = getNewlineChunks("test/test.jsonl", 2);
		mu_check(rv.size() == 2);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 12);
		mu_check(rv[1].offset == 12);
		mu_check(rv[1].length == 12);
	}

	// Dividing into 3 chunks gives a lop-sided result; one of the chunks
	// consists only of whitespace. This is OK.
	{
		auto rv = getNewlineChunks("test/test.jsonl", 3);
		mu_check(rv.size() == 3);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 12);
		mu_check(rv[1].offset == 12);
		mu_check(rv[1].length == 11);
		mu_check(rv[2].offset == 23);
		mu_check(rv[2].length == 1);
	}

	// Dividing into many more chunks than is possible devolves into
	// one chunk per newline.
	{
		auto rv = getNewlineChunks("test/test.jsonl", 128);
		mu_check(rv.size() == 4);
		mu_check(rv[0].offset == 0);
		mu_check(rv[0].length == 2);
		mu_check(rv[1].offset == 2);
		mu_check(rv[1].length == 10);
		mu_check(rv[2].offset == 12);
		mu_check(rv[2].length == 11);
		mu_check(rv[3].offset == 23);
		mu_check(rv[3].length == 1);
	}
}

MU_TEST(test_compression_gzip) {
	std::string input = "a random string to be compressed";

	for (int level = 1; level < 9; level++) {
		std::string gzipped = compress_string(input, level, true);
		std::string unzipped;
		decompress_string(unzipped, gzipped.data(), gzipped.size(), true);
		mu_check(unzipped == input);
	}

	std::string gzipped = compress_string(input, -1, true);
	std::string unzipped;
	decompress_string(unzipped, gzipped.data(), gzipped.size(), true);
	mu_check(unzipped == input);

}

MU_TEST(test_compression_zlib) {
	std::string input = "a random string to be compressed";

	for (int level = 1; level < 9; level++) {
		std::string gzipped = compress_string(input, level, false);
		std::string unzipped;
		decompress_string(unzipped, gzipped.data(), gzipped.size(), false);
		mu_check(unzipped == input);
	}

	std::string gzipped = compress_string(input, -1, false);
	std::string unzipped;
	decompress_string(unzipped, gzipped.data(), gzipped.size(), false);
	mu_check(unzipped == input);
}

MU_TEST(test_compression_zstd) {
	std::string input = "a random string to be compressed";

	for (int level = 0; level <= 19; level++) {
		std::string compressed = compress_zstd(input, level);
		std::string output;
		decompress_zstd(output, compressed.data(), compressed.size());
		mu_check(output == input);
	}

	// larger than the output buffer's initial size, and reusing the output
	std::string big;
	for (int i = 0; i < 200000; i++)
		big += std::to_string(i);
	std::string compressed = compress_zstd(big);
	mu_check(compressed.size() < big.size());
	std::string output = "leftover data";
	decompress_zstd(output, compressed.data(), compressed.size());
	mu_check(output == big);

	// with the size known up front, e.g. from a PBF blob's raw_size
	decompress_zstd(output, compressed.data(), compressed.size(), big.size());
	mu_check(output == big);

	// empty input
	compressed = compress_zstd("");
	decompress_zstd(output, compressed.data(), compressed.size());
	mu_check(output.empty());
}

MU_TEST(test_compression_zstd_unknown_size) {
	// A streaming compressor doesn't record the frame's content size.
	std::string input;
	for (int i = 0; i < 100000; i++)
		input += std::to_string(i * 7);

	ZSTD_CCtx* cctx = ZSTD_createCCtx();
	std::string compressed(ZSTD_compressBound(input.size()), '\0');
	ZSTD_inBuffer in = { input.data(), input.size(), 0 };
	ZSTD_outBuffer out = { &compressed[0], compressed.size(), 0 };
	// Given all the input in one ZSTD_e_end call, zstd would record the size,
	// so feed it first and end the frame separately.
	ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue);
	ZSTD_inBuffer empty = { nullptr, 0, 0 };
	size_t rv = ZSTD_compressStream2(cctx, &out, &empty, ZSTD_e_end);
	ZSTD_freeCCtx(cctx);
	mu_check(rv == 0);
	compressed.resize(out.pos);
	mu_check(ZSTD_getFrameContentSize(compressed.data(), compressed.size()) == ZSTD_CONTENTSIZE_UNKNOWN);

	std::string output;
	decompress_zstd(output, compressed.data(), compressed.size());
	mu_check(output == input);

	// a truncated frame is an error, not a short result
	bool caughtException = false;
	try {
		decompress_zstd(output, compressed.data(), compressed.size() / 2);
	} catch (std::runtime_error&) {
		caughtException = true;
	}
	mu_check(caughtException);
}

MU_TEST(test_compression_zstd_invalid) {
	std::string garbage = "not zstd data at all";
	bool caughtException = false;
	try {
		std::string output;
		decompress_zstd(output, garbage.data(), garbage.size());
	} catch (std::runtime_error&) {
		caughtException = true;
	}
	mu_check(caughtException);
}

MU_TEST(test_compress_tile) {
	std::string input = "a random string to be compressed, a random string to be compressed";

	for (TileCompression c : { TileCompression::None, TileCompression::Gzip, TileCompression::Deflate, TileCompression::Zstd }) {
		std::string compressed = compress_tile(input, c);
		std::string output;
		decompress_tile(output, compressed.data(), compressed.size(), c);
		mu_check(output == input);
	}

	mu_check(compress_tile(input, TileCompression::None) == input);
	std::string gzipped = compress_tile(input, TileCompression::Gzip);
	mu_check((unsigned char)gzipped[0] == 0x1f && (unsigned char)gzipped[1] == 0x8b);
	std::string zstd = compress_tile(input, TileCompression::Zstd);
	mu_check((unsigned char)zstd[0] == 0x28 && (unsigned char)zstd[1] == 0xb5 && (unsigned char)zstd[2] == 0x2f && (unsigned char)zstd[3] == 0xfd);
}

MU_TEST_SUITE(test_suite_helpers) {
	MU_RUN_TEST(test_open_file);
	MU_RUN_TEST(test_get_chunks);
	MU_RUN_TEST(test_compression_gzip);
	MU_RUN_TEST(test_compression_zlib);
	MU_RUN_TEST(test_compression_zstd);
	MU_RUN_TEST(test_compression_zstd_unknown_size);
	MU_RUN_TEST(test_compression_zstd_invalid);
	MU_RUN_TEST(test_compress_tile);
}

int main() {
	MU_RUN_SUITE(test_suite_helpers);
	MU_REPORT();
	return MU_EXIT_CODE;
}
