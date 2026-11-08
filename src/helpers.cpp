#include <string>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>

#include <sys/stat.h>
#include "external/libdeflate/libdeflate.h"
#include <zstd.h>
#include "helpers.h"

#ifdef _MSC_VER
#define stat64 __stat64
#endif

// stat64 is a glibc extension; elsewhere (macOS, musl, the BSDs) stat is already 64-bit
#if !defined(_MSC_VER) && !defined(__GLIBC__)
#define stat64 stat
#endif

#define MOD_GZIP_ZLIB_WINDOWSIZE 15
#define MOD_GZIP_ZLIB_CFACTOR 9
#define MOD_GZIP_ZLIB_BSIZE 8096

using namespace std;

class Compressor {
public:
	int level;
	libdeflate_compressor* compressor;

	Compressor(int level): level(level), compressor(NULL) {
		setLevel(level);
	}

	void setLevel(int level) {
		libdeflate_free_compressor(compressor);
		this->level = level;
		compressor = libdeflate_alloc_compressor(level);

		if (!compressor)
			throw std::runtime_error("libdeflate_alloc_compressor failed (level=" + std::to_string(level) + ")");
	}

	Compressor & operator=(const Compressor&) = delete;
	Compressor(const Compressor&) = delete;

	~Compressor() {
		libdeflate_free_compressor(compressor);
	}
};

class Decompressor {
public:
	libdeflate_decompressor* decompressor;

	Decompressor(): decompressor(NULL) {
		decompressor = libdeflate_alloc_decompressor();

		if (!decompressor)
			throw std::runtime_error("libdeflate_alloc_decompressor failed");
	}

	Decompressor & operator=(const Decompressor&) = delete;
	Decompressor(const Decompressor&) = delete;

	~Decompressor() {
		libdeflate_free_decompressor(decompressor);
	}
};


thread_local Compressor compressor(6);
thread_local Decompressor decompressor;

// zstd contexts are expensive to create, so keep one of each per thread.
struct ZstdContexts {
	ZSTD_CCtx* cctx = nullptr;
	ZSTD_DCtx* dctx = nullptr;
	ZstdContexts() = default;
	ZstdContexts(const ZstdContexts&) = delete;
	ZstdContexts& operator=(const ZstdContexts&) = delete;
	~ZstdContexts() {
		ZSTD_freeCCtx(cctx);
		ZSTD_freeDCtx(dctx);
	}
	ZSTD_CCtx* compressor() {
		if (!cctx && !(cctx = ZSTD_createCCtx()))
			throw std::runtime_error("ZSTD_createCCtx failed");
		return cctx;
	}
	ZSTD_DCtx* decompressor() {
		if (!dctx && !(dctx = ZSTD_createDCtx()))
			throw std::runtime_error("ZSTD_createDCtx failed");
		return dctx;
	}
};
thread_local ZstdContexts zstdContexts;

// Bounding box string parsing

double bboxElementFromStr(const std::string& number) {
	try {
		return boost::lexical_cast<double>(number);
	} catch (boost::bad_lexical_cast&) {
		std::cerr << "Failed to parse coordinate " << number << std::endl;
		exit(1);
	}
}

// Split bounding box provided as a comma-separated list of coordinates.
std::vector<std::string> parseBox(const std::string& bbox) {
	std::vector<std::string> bboxParts;
	if (!bbox.empty()) {
		boost::split(bboxParts, bbox, boost::is_any_of(","));
		if (bboxParts.size() != 4) {
			std::cerr << "Bounding box must contain 4 elements: minlon,minlat,maxlon,maxlat" << std::endl;
			exit(1);
		}
	}
	return bboxParts;
}

// Compress a STL string using zlib with given compression level, and return the binary data
std::string compress_string(const std::string& str,
                            int compressionlevel,
                            bool asGzip) {
	if (compressionlevel == Z_DEFAULT_COMPRESSION)
		compressionlevel = 6;

	if (compressionlevel != compressor.level)
		compressor.setLevel(compressionlevel);

	std::string rv;
	if (asGzip) {
		size_t maxSize = libdeflate_gzip_compress_bound(compressor.compressor, str.size());
		rv.resize(maxSize);

		size_t compressedSize = libdeflate_gzip_compress(compressor.compressor, str.data(), str.size(), &rv[0], maxSize);
		if (compressedSize == 0)
			throw std::runtime_error("libdeflate_gzip_compress failed");
		rv.resize(compressedSize);
	} else {
		size_t maxSize = libdeflate_zlib_compress_bound(compressor.compressor, str.size());
		rv.resize(maxSize);

		size_t compressedSize = libdeflate_zlib_compress(compressor.compressor, str.data(), str.size(), &rv[0], maxSize);
		if (compressedSize == 0)
			throw std::runtime_error("libdeflate_zlib_compress failed");
		rv.resize(compressedSize);
	}

	return rv;
}

// Decompress an STL string using zlib and return the original data.
// The output buffer is passed in; callers are meant to re-use the buffer such
// that eventually no allocations are needed when decompressing.
void decompress_string(std::string& output, const char* input, uint32_t inputSize, bool asGzip) {
	size_t uncompressedSize;

	if (output.size() < inputSize)
		output.resize(inputSize);

	while (true) {
		libdeflate_result rv = LIBDEFLATE_BAD_DATA;

		if (asGzip) {
			rv = libdeflate_gzip_decompress(
				decompressor.decompressor,
				input,
				inputSize,
				&output[0],
				output.size(),
				&uncompressedSize
			);
		} else {
			rv = libdeflate_zlib_decompress(
				decompressor.decompressor,
				input,
				inputSize,
				&output[0],
				output.size(),
				&uncompressedSize
			);
		}

		if (rv == LIBDEFLATE_SUCCESS) {
			output.resize(uncompressedSize);
			return;
		}

		if (rv == LIBDEFLATE_INSUFFICIENT_SPACE) {
			output.resize((output.size() + 128) * 2);
		} else
			throw std::runtime_error("libdeflate_gzip_decompress failed");
	}
}


std::string compress_zstd(const std::string& str, int compressionlevel) {
	if (compressionlevel == 0)
		compressionlevel = ZSTD_CLEVEL_DEFAULT;

	std::string rv;
	rv.resize(ZSTD_compressBound(str.size()));
	size_t compressedSize = ZSTD_compressCCtx(zstdContexts.compressor(), &rv[0], rv.size(), str.data(), str.size(), compressionlevel);
	if (ZSTD_isError(compressedSize))
		throw std::runtime_error(std::string("ZSTD_compress failed: ") + ZSTD_getErrorName(compressedSize));
	rv.resize(compressedSize);
	return rv;
}

void decompress_zstd(std::string& output, const char* input, size_t inputSize, size_t expectedSize) {
	size_t size = expectedSize;
	if (size == 0) {
		unsigned long long frameSize = ZSTD_getFrameContentSize(input, inputSize);
		if (frameSize == ZSTD_CONTENTSIZE_ERROR)
			throw std::runtime_error("ZSTD_decompress failed: not zstd data");
		if (frameSize != ZSTD_CONTENTSIZE_UNKNOWN)
			size = frameSize;
	}

	if (size != 0) {
		output.resize(size);
		size_t rv = ZSTD_decompressDCtx(zstdContexts.decompressor(), &output[0], output.size(), input, inputSize);
		if (ZSTD_isError(rv))
			throw std::runtime_error(std::string("ZSTD_decompress failed: ") + ZSTD_getErrorName(rv));
		output.resize(rv);
		return;
	}

	// The frame doesn't record its size (e.g. it was written by a streaming
	// compressor), so decompress it as a stream.
	ZSTD_DCtx* dctx = zstdContexts.decompressor();
	ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
	output.clear();
	std::string chunk(ZSTD_DStreamOutSize(), '\0');
	ZSTD_inBuffer in = { input, inputSize, 0 };
	while (true) {
		ZSTD_outBuffer out = { &chunk[0], chunk.size(), 0 };
		size_t rv = ZSTD_decompressStream(dctx, &out, &in);
		if (ZSTD_isError(rv))
			throw std::runtime_error(std::string("ZSTD_decompress failed: ") + ZSTD_getErrorName(rv));
		output.append(chunk.data(), out.pos);
		// 0 means a frame just ended; input may hold further frames.
		if (rv == 0 && in.pos == in.size)
			return;
		// zstd wants more input than there is
		if (in.pos == in.size && out.pos < out.size)
			throw std::runtime_error("ZSTD_decompress failed: truncated input");
	}
}

std::string compress_tile(const std::string& str, TileCompression compression) {
	switch (compression) {
		case TileCompression::None:    return str;
		case TileCompression::Gzip:    return compress_string(str, Z_DEFAULT_COMPRESSION, true);
		case TileCompression::Deflate: return compress_string(str, Z_DEFAULT_COMPRESSION, false);
		case TileCompression::Zstd:    return compress_zstd(str);
	}
	throw std::runtime_error("compress_tile: unknown compression");
}

void decompress_tile(std::string& output, const char* input, size_t inputSize, TileCompression compression) {
	switch (compression) {
		case TileCompression::None:    output.assign(input, inputSize); return;
		case TileCompression::Gzip:    decompress_string(output, input, inputSize, true); return;
		case TileCompression::Deflate: decompress_string(output, input, inputSize, false); return;
		case TileCompression::Zstd:    decompress_zstd(output, input, inputSize); return;
	}
	throw std::runtime_error("decompress_tile: unknown compression");
}

// Parse a Boost error
std::string boost_validity_error(unsigned failure) {
	switch (failure) {
		case 10: return "too few points";
		case 11: return "wrong topological dimension";
		case 12: return "spikes (nodes go back on themselves)";
		case 13: return "consecutive duplicate points";
		case 20: return "not been closed";
		case 21: return "self-intersections";
		case 22: return "the wrong orientation";
		case 30: return "interior rings outside";
		case 31: return "nested interior rings";
		case 32: return "disconnected interior (contains polygons whose interiors are not disjoint)";
		case 40: return "intersecting interiors";
		default: return "something mysterious wrong with it, Boost validity_failure_type " + to_string(failure);
	}
}

uint64_t getFileSize(std::string filename) {
	struct stat64 statBuf;
	int rc = stat64(filename.c_str(), &statBuf);

	if (rc == 0) return statBuf.st_size;

	throw std::runtime_error("unable to stat " + filename);
}

FilePtr openFile(const std::string &filename, const char *mode) {
	FilePtr fp(fopen(filename.c_str(), mode));
	if (fp == nullptr)
		throw std::runtime_error("unable to open " + filename);
	return fp;
}

// Given a file, attempt to divide it into N chunks, with each chunk separated
// by a newline.
//
// Useful for dividing a JSON lines file into blocks suitable for parallel processing.
std::vector<OffsetAndLength> getNewlineChunks(const std::string &filename, uint64_t chunks) {
	std::vector<OffsetAndLength> rv;

	const uint64_t size = getFileSize(filename);
	const uint64_t chunkSize = std::max<uint64_t>(size / chunks, 1ul);
	FilePtr fp = openFile(filename, "r");

	// Our approach is naive: skip chunkSize bytes, scan for a newline, repeat.
	//
	// Per UTF-8's ascii transparency property, a newline is guaranteed not to form
	// part of any multi-byte character, so the byte '\n' reliably indicates a safe
	// place to start a new chunk.
	uint64_t offset = 0;
	uint64_t length = 0;
	char buffer[8192];
	while (offset < size) {
		// The last chunk will not be a full `chunkSize`.
		length = std::min(chunkSize, size - offset);

		if (fseek(fp.get(), offset + length, SEEK_SET) != 0) throw std::runtime_error("unable to seek to " + std::to_string(offset) + " in " + filename);

		bool foundNewline = false;

		while(!foundNewline) {
			size_t read = fread(buffer, 1, sizeof(buffer), fp.get());
			if (read == 0) break;
			for (int i = 0; i < read; i++) {
				if (buffer[i] == '\n') {
					length += i;
					foundNewline = true;
					break;
				}
			}

			if (!foundNewline) length += read;
    }

		rv.push_back({offset, length});
		offset += length;
	}

	return rv;
}
