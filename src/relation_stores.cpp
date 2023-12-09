#include "relation_stores.h"

void BinarySearchRelationStore::reopen() {
	std::lock_guard<std::mutex> lock(mutex);
	relations = std::make_unique<map_t>();
}

void BinarySearchRelationStore::insert(std::vector<element_t> &newRelations) {
	std::lock_guard<std::mutex> lock(mutex);
	auto i = relations->size();
	relations->resize(i + newRelations.size());
	std::copy(std::make_move_iterator(newRelations.begin()), std::make_move_iterator(newRelations.end()), relations->begin() + i); 
}

void BinarySearchRelationStore::clear() {
	std::lock_guard<std::mutex> lock(mutex);
	relations->clear();
}

std::size_t BinarySearchRelationStore::size() const {
	std::lock_guard<std::mutex> lock(mutex);
	return relations->size(); 
}

void BinarySearchRelationStore::finalize(size_t threadNum) {
	// TODO: sort map_t
}
