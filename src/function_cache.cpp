#include "function_cache.h"

FunctionCache::FunctionCache(const std::string& filename) {
	db.init(filename, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX);
	int sqlite3_busy_timeout(sqlite3*, int ms);

	std::string journalMode;
	int busyTimeout;
	db << "PRAGMA busy_timeout = 60000;" >> busyTimeout;
	db << "PRAGMA synchronous = OFF;";
	db << "PRAGMA journal_mode = WAL;" >> journalMode;

	if (journalMode != "wal") {
		throw std::runtime_error("fatal: could not set WAL mode on function cache DB");
	}

	db << "CREATE TABLE IF NOT EXISTS cache(k1 INTEGER NOT NULL, k2 INTEGER NOT NULL, k3 INTEGER NOT NULL, func INTEGER NOT NULL, result BLOB, UNIQUE (k1, k2, k3, func));";

	/*
	std::string newFile = "file:";
	newFile += filename;
	newFile += "?immutable=1";
	db.init(filename, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_URI);
	*/

}

FunctionCache::~FunctionCache() {
}

std::pair<bool, int64_t> FunctionCache::getCachedInt64(
	const uint64_t k1,
	const uint64_t k2,
	const uint64_t k3,
	const Function func
) {
	bool exists = false;
	int64_t rv = 0;

	db << "SELECT result FROM cache WHERE k1 = ? AND k2 = ? AND k3 = ? AND func = ?" << static_cast<sqlite_int64>(k1) << static_cast<sqlite_int64>(k2) << static_cast<sqlite_int64>(k3) << static_cast<char>(func) >> [&](sqlite_int64 result) {
		exists = true;
		rv = result;
	};

	return std::pair<bool, int64_t>(exists, rv);
}

void FunctionCache::addCachedInt64(
	const uint64_t k1, 
	const uint64_t k2,
	const uint64_t k3,
	const Function func,
	int64_t value
) {
	db << "INSERT INTO cache(k1, k2, k3, func, result) VALUES(?, ?, ?, ?, ?)" << static_cast<sqlite_int64>(k1) << static_cast<sqlite_int64>(k2) << static_cast<sqlite_int64>(k3) << static_cast<char>(func) << (sqlite_int64)value;
}
