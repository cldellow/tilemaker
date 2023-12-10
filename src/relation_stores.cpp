#include <boost/sort/sort.hpp>
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

const std::pair<RelationStore::wayid_vector_t, RelationStore::wayid_vector_t>& BinarySearchRelationStore::at(RelationID id) const {
	auto it = std::lower_bound(relations->begin(), relations->end(), id, [](auto const &e, auto id) { 
		return e.first < id; 
	});

	if(it == relations->end() || it->first != id)
		throw std::out_of_range("Could not find relation with id " + std::to_string(id));

	return it->second;
}

void BinarySearchRelationStore::finalize(size_t threadNum) {
	std::lock_guard<std::mutex> lock(mutex);
	boost::sort::block_indirect_sort(
		relations->begin(), relations->end(), 
		[](auto const &a, auto const &b) { return a.first < b.first; }, 
		threadNum);
}
