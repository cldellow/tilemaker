/*! \file */ 
#ifndef _FUNCTION_CACHE_H
#define _FUNCTION_CACHE_H

#include <string>
#include "sqlite_modern_cpp.h"

class FunctionCache {
public:
	enum class Function: char {
		IsValid = 0
	};

	enum class CachedBoolean: char {
		NotPresent = -1,
		False = 0,
		True = 1
	};

	FunctionCache(const std::string& fileName);
	~FunctionCache();

	std::pair<bool, int64_t> getCachedInt64(
		const uint64_t k1, 
		const uint64_t k2,
		const uint64_t k3,
		const Function func
	);

	void addCachedInt64(
		const uint64_t k1, 
		const uint64_t k2,
		const uint64_t k3,
		const Function func,
		int64_t value
	);

private:
	sqlite::database db;
};
#endif
