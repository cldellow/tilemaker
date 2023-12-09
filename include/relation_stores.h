#ifndef _RELATION_STORES_H
#define _RELATION_STORES_H

#include "relation_store.h"
#include <mutex>
#include <memory>

class BinarySearchRelationStore: public RelationStore {

public:	

	void reopen() override;
	void insert(std::vector<element_t> &relations) override;
	void clear() override;
	std::size_t size() const override;
	void finalize(size_t threadNum) override;

private: 	
	mutable std::mutex mutex;
	// TODO: why is this a unique_ptr?
	std::unique_ptr<map_t> mOutInLists;
};

#endif
