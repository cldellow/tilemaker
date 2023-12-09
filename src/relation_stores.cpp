#include "relation_stores.h"

void BinarySearchRelationStore::reopen() {
	std::lock_guard<std::mutex> lock(mutex);
	mOutInLists = std::make_unique<map_t>();
}

void BinarySearchRelationStore::insert(std::vector<element_t> &relations) {
	std::lock_guard<std::mutex> lock(mutex);
	auto i = mOutInLists->size();
	mOutInLists->resize(i + relations.size());
	std::copy(std::make_move_iterator(relations.begin()), std::make_move_iterator(relations.end()), mOutInLists->begin() + i); 
}

void BinarySearchRelationStore::clear() {
	std::lock_guard<std::mutex> lock(mutex);
	mOutInLists->clear();
}

std::size_t BinarySearchRelationStore::size() const {
	std::lock_guard<std::mutex> lock(mutex);
	return mOutInLists->size(); 
}

void BinarySearchRelationStore::finalize(size_t threadNum) {
	// TODO: sort map_t
}
