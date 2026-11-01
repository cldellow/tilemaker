/*! \file */ 
#ifndef _HELPERS_H
#define _HELPERS_H

#include <cstdio>
#include <cstdint>
#include <memory>
#include <sstream>
#include <vector>

#define Z_DEFAULT_COMPRESSION -1

// How vector tiles are compressed (the "compress" setting)
enum class TileCompression { None, Gzip, Deflate, Zstd };

#ifdef _MSVC_LANG
#define ISATTY true
#else
#define ISATTY isatty(1)
#endif

// General helper routines

inline void endian_swap(unsigned int& x) {
	x = (x>>24) | 
        ((x<<8) & 0x00FF0000) |
        ((x>>8) & 0x0000FF00) |
        (x<<24);
}

inline bool ends_with(std::string const & value, std::string const & ending) {
	if (ending.size() > value.size()) return false;
	return std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

inline std::vector<std::string> split_string(std::string &inputStr, char sep) {
	std::stringstream ss(inputStr);
	std::string item;
	std::vector<std::string> res;
	while (std::getline(ss, item, sep)) { res.push_back(item); }
	return res;
}

struct OffsetAndLength {
	uint64_t offset;
	uint64_t length;
};

struct FileCloser {
	void operator()(FILE* fp) const {
		if (fp != nullptr)
			fclose(fp);
	}
};

using FilePtr = std::unique_ptr<FILE, FileCloser>;

FilePtr openFile(const std::string &filename, const char *mode);
uint64_t getFileSize(std::string filename);
std::vector<OffsetAndLength> getNewlineChunks(const std::string &filename, uint64_t chunks);

void decompress_string(std::string& output, const char* input, uint32_t inputSize, bool asGzip = false);
double bboxElementFromStr(const std::string& number);

std::vector<std::string> parseBox(const std::string& bbox);

std::string compress_string(const std::string& str,
                            int compressionlevel = Z_DEFAULT_COMPRESSION,
                            bool asGzip = false);

// Zstandard. A level of 0 means zstd's default (currently 3).
std::string compress_zstd(const std::string& str, int compressionlevel = 0);
// expectedSize, if known (e.g. a PBF blob's raw_size), avoids reading it from
// the frame header and growing the buffer.
void decompress_zstd(std::string& output, const char* input, size_t inputSize, size_t expectedSize = 0);

// Compress or decompress a tile with the configured method. TileCompression::None
// passes the data through.
std::string compress_tile(const std::string& str, TileCompression compression);
void decompress_tile(std::string& output, const char* input, size_t inputSize, TileCompression compression);

std::string boost_validity_error(unsigned failure);

#endif //_HELPERS_H
