#include "catch.hpp"
#include "oraduck/rowid_ranges.hpp"

using namespace oraduck;

TEST_CASE("EncodeRowid", "[rowid]") {
	CHECK(EncodeRowid(0, 0, 0, 0) == "AAAAAAAAAAAAAAAAAA");
	CHECK(EncodeRowid(1, 1, 1, 1) == "AAAAABAABAAAAABAAB");
	CHECK(EncodeRowid(0xFFFFFFFFu, 1023, 0x3FFFFF, 65535) == "D/////AP/AAP///P//");
	CHECK(EncodeRowid(7, 1024, (5u << 22) | 9u, 0) == "AAAAAHAAFAAAAAJAAA"); // bigfile: block 5*2^22 + 9
}

TEST_CASE("TargetBlocks", "[rowid]") {
	CHECK(TargetBlocks(100000, 8) == 3125);
	CHECK(TargetBlocks(100, 8) == 128);
	CHECK(TargetBlocks(1000000, 0) == 250000);
}

TEST_CASE("BuildRowidSlices covers each object once", "[rowid]") {
	CHECK(BuildRowidSlices({}, 128).empty());

	// object 7: files 4 and 5, 60+60+40 blocks; object 9: one extent. Target 100.
	// Extents given out of order: they are sorted by (file, block).
	const auto slices = BuildRowidSlices({{7, 5, 8, 40}, {9, 4, 0, 30}, {7, 4, 16, 60}, {7, 4, 64, 60}}, 100);
	REQUIRE(slices.size() == 3);
	// object 7: 60 + 60 >= 100 reached before (5,8), which therefore opens the second unit.
	CHECK(slices[0].rowid_lo == EncodeRowid(7, 0, 0, 0));
	CHECK(slices[0].rowid_hi == EncodeRowid(7, 5, 8, 0));
	CHECK(slices[1].rowid_lo == EncodeRowid(7, 5, 8, 0));
	CHECK(slices[1].rowid_hi == EncodeRowid(8, 0, 0, 0));
	CHECK(slices[2].rowid_lo == EncodeRowid(9, 0, 0, 0));
	CHECK(slices[2].rowid_hi == EncodeRowid(10, 0, 0, 0));
	for (const auto &s : slices) {
		CHECK(s.kind == SliceKind::kRowidRange);
		CHECK_FALSE(s.hi_inclusive);
	}
}

TEST_CASE("BuildRowidSlices chains the units of one object and orders largest first", "[rowid]") {
	// 10 extents of 50 blocks, target 100: a unit closes once 100 blocks are accumulated -> 5 units of 100
	std::vector<SegmentExtent> extents;
	for (uint32_t i = 0; i < 10; i++) {
		extents.push_back({7, 4, i * 50, 50});
	}
	extents.push_back({3, 4, 0, 500});
	const auto slices = BuildRowidSlices(extents, 100);
	REQUIRE(slices.size() == 6);
	CHECK(slices[0].rowid_lo == EncodeRowid(3, 0, 0, 0)); // 500 blocks first
	CHECK(slices[0].rowid_hi == EncodeRowid(4, 0, 0, 0));
	for (size_t i = 1; i + 1 < slices.size(); i++) {
		// units of object 7 are in order here (equal size, stable)
		CHECK(slices[i].rowid_hi == slices[i + 1].rowid_lo);
	}
	CHECK(slices[1].rowid_lo == EncodeRowid(7, 0, 0, 0));
	CHECK(slices[5].rowid_hi == EncodeRowid(8, 0, 0, 0));
}

TEST_CASE("BuildRowidSlices closes the last object number inclusively", "[rowid]") {
	const auto slices = BuildRowidSlices({{0xFFFFFFFFu, 4, 0, 10}}, 128);
	REQUIRE(slices.size() == 1);
	CHECK(slices[0].hi_inclusive);
	CHECK(slices[0].rowid_hi == EncodeRowid(0xFFFFFFFFu, 1023, 0x3FFFFF, 65535));
}
