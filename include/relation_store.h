#ifndef _RELATION_STORE_H
#define _RELATION_STORE_H

#include "mmap_allocator.h"
#include "coordinates.h"
#include <vector>

class RelationStore {
public:	
	using wayid_vector_t = std::vector<WayID, mmap_allocator<WayID>>;
	// 0 is outeres, 1 is inners
	// TODO: might be nicer to make this a template struct w/named inners/outers?
	using relation_entry_t = std::pair<wayid_vector_t, wayid_vector_t>;

	using element_t = std::pair<RelationID, relation_entry_t>;
	using map_t = std::deque<element_t, mmap_allocator<element_t>>;

	virtual void reopen() = 0;
	virtual void insert(std::vector<element_t>& relations) = 0;
	virtual void clear() = 0;
	virtual std::size_t size() const = 0;
	virtual void finalize(size_t threadNum) = 0;
};

#endif
