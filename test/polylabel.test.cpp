#include <iostream>
#include "geom.h"
#include "external/minunit.h"
#include "geojson.h"
#include "./simplify.test.h"
#include "polylabel.h"

void save(std::string filename, const MultiPolygon& mp) {
	GeoJSON json;
	json.addGeometry(mp);
	json.finalise();
	json.toFile(filename);
}

MU_TEST(test_simplify) {
	//MultiPolygon mp0 = mthope();
	MultiPolygon mp0 = gsmnp();
	for (const auto& p : mp0) {
		std::cout << "mp0: poly.outer().size() = " << p.outer().size() << std::endl;
	}
	save("poly-s0.txt", mp0);

	timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	auto pt = mapbox::polylabel(mp0.at(0));
	std::cout << " point is at " << pt.get<0>() << ", " << pt.get<1>() << std::endl;

	clock_gettime(CLOCK_MONOTONIC, &end);
	uint64_t elapsedNs = 1e9 * (end.tv_sec - start.tv_sec) + end.tv_nsec - start.tv_nsec;
	std::cout << "took " << std::to_string((uint32_t)(elapsedNs / 1e6)) << " ms" << std::endl;


}

MU_TEST_SUITE(test_suite_simplify) {
	MU_RUN_TEST(test_simplify);
}

int main() {
	MU_RUN_SUITE(test_suite_simplify);
	MU_REPORT();
	return MU_EXIT_CODE;
}

