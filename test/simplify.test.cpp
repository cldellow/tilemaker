#include <iostream>
#include "geom.h"
#include "external/minunit.h"
#include "geojson.h"
#include "./simplify.test.h"

/*
void a(Polygon& p, double lon, double lat) {
	boost::geometry::range::push_back(p.outer(), boost::geometry::make<Point>(lon, lat));
}
*/

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
	MultiPolygon mp1 = simplify(mp0, 0.0003);
	for (const auto& p : mp1) {
		std::cout << "mp1: poly.outer().size() = " << p.outer().size() << std::endl;
	}
	//save("poly-s1.txt", mp1);

	if (false) {
		MultiPolygon mp2a = simplify(mp1, 0.0003 * 2);
		for (const auto& p : mp2a) {
			std::cout << "mp2a: poly.outer().size() = " << p.outer().size() << std::endl;
		}
		save("poly-s2a.txt", mp2a);

		MultiPolygon mp3a = simplify(mp1, 0.0003 * 4);
		for (const auto& p : mp3a) {
			std::cout << "mp3a: poly.outer().size() = " << p.outer().size() << std::endl;
		}
		save("poly-s3a.txt", mp3a);

		MultiPolygon mp4a = simplify(mp1, 0.0003 * 8);
		for (const auto& p : mp4a) {
			std::cout << "mp4a: poly.outer().size() = " << p.outer().size() << std::endl;
		}
		save("poly-s4a.txt", mp4a);

	} else {
		MultiPolygon mp2b = simplify(mp0, 0.0003 * 2);
		for (const auto& p : mp2b) {
			std::cout << "mp2b: poly.outer().size() = " << p.outer().size() << std::endl;
		}
		save("poly-s2b.txt", mp2b);

		MultiPolygon mp3b = simplify(mp0, 0.0003 * 4);
		for (const auto& p : mp3b) {
			std::cout << "mp3b: poly.outer().size() = " << p.outer().size() << std::endl;
		}
		save("poly-s3b.txt", mp3b);

		MultiPolygon mp4b = simplify(mp0, 0.0003 * 8);
		for (const auto& p : mp4b) {
			std::cout << "mp4b: poly.outer().size() = " << p.outer().size() << std::endl;
		}
		save("poly-s4b.txt", mp4b);

	}


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

