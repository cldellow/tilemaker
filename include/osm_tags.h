#ifndef _OSM_TAGS_H
#define _OSM_TAGS_H

#include <string>
#include <vector>

#include "kaguya.hpp"
#include "osmformat.pb.h"

// An alternative to std::string -- don't copy the buffer,
// do remember the size.
struct CharStarWithSize {
	const char* ptr;
	const size_t length;
};

// An alternative to map<std::string, std::string>.
//
// Goals:
// - Be re-usable when reading many OSM primitives from a PBF to avoid
//   many small allocations.
// - Don't require Lua to allocate std::strings when calling into C++
// - Defer reading values unless required
// - Exploit the reality that the majority of OSM objects have very few tags,
//   and most tags won't be queried - so the cost of initializing the map
//   is relatively high compared to the cost of querying it.
//
//   https://gist.github.com/cldellow/8155317fb7f6dcf909385a2c51090993 shows
//   a frequency histogram for the GSMNP -- majority of objects have fewer than 4 tags.

class OsmTagMap {
	using tag_map_t = boost::container::flat_map<std::string, std::string>;

	public:
		OsmTagMap(const StringTable& stringTable) :
			stringTable(stringTable) {
		}

		void clear() {
			keys.clear();
			valueIndexes.clear();
		}

		void add(const std::string& key, int32_t valueIndex) {
			keys.push_back(key);
			valueIndexes.push_back(valueIndex);
		}

		int32_t size() const { return keys.size(); }

		int32_t getValueIndex(const CharStarWithSize& key) const {
			auto size = keys.size();
			for (auto i = 0; i < size; i++) {
				if (keys[i].size() == key.length && keys[i] == key.ptr)
					return valueIndexes[i];
			}

			return -1;
		}

		tag_map_t as_boost_map() const {
			tag_map_t rv;
			for (auto i = 0; i < keys.size(); i++) {
				rv[keys[i]] = stringTable.s(valueIndexes[i]);
			}

			return rv;
		};

		const std::string& getValueForIndex(int32_t valueIndex) const {
			return stringTable.s(valueIndex);
		}
		

	private:
		std::vector<std::string> keys;
		std::vector<int32_t> valueIndexes;

		StringTable stringTable;
};

// Teach Kaguya how to serialize Lua strings to C++ as CharStarWithSize,
// so that we can avoid allocations (std::string) and avoid strlen (char *)
template<>  struct kaguya::lua_type_traits<CharStarWithSize> {
	typedef CharStarWithSize get_type;
	typedef const CharStarWithSize& push_type;

	static bool strictCheckType(lua_State* l, int index)
	{
		return lua_type(l, index) == LUA_TSTRING;
	}
	static bool checkType(lua_State* l, int index)
	{
		return lua_isstring(l, index) != 0;
	}
	static get_type get(lua_State* l, int index)
	{
		size_t size = 0;
		const char* buffer = lua_tolstring(l, index, &size);
		return { buffer, size };
	}
	static int push(lua_State* l, push_type s)
	{
		throw kaguya::KaguyaException("we do not send CharStarWithSize to Lua");
	}
};
#endif // _OSM_TAGS_H
