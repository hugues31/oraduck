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
	// object 7: the first unit takes 60 blocks of (4,16) and 40 of (4,64), so the second starts at block 104
	CHECK(slices[0].rowid_lo == EncodeRowid(7, 0, 0, 0));
	CHECK(slices[0].rowid_hi == EncodeRowid(7, 4, 104, 0));
	CHECK(slices[1].rowid_lo == EncodeRowid(7, 4, 104, 0));
	CHECK(slices[1].rowid_hi == EncodeRowid(8, 0, 0, 0));
	CHECK(slices[2].rowid_lo == EncodeRowid(9, 0, 0, 0));
	CHECK(slices[2].rowid_hi == EncodeRowid(10, 0, 0, 0));
	for (const auto &s : slices) {
		CHECK(s.kind == SliceKind::kRowidRange);
		CHECK_FALSE(s.hi_inclusive);
	}
}

TEST_CASE("BuildRowidSlices chains the units of one object and orders largest first", "[rowid]") {
	// object 7: 10 extents of 50 blocks; object 3: one extent of 500 blocks; object 5: 30 blocks. Target 100:
	// 5 units of 100 blocks each for objects 3 and 7, then object 5.
	std::vector<SegmentExtent> extents;
	for (uint32_t i = 0; i < 10; i++) {
		extents.push_back({7, 4, i * 50, 50});
	}
	extents.push_back({5, 4, 1000, 30});
	extents.push_back({3, 4, 0, 500});
	const auto slices = BuildRowidSlices(extents, 100);
	REQUIRE(slices.size() == 11);
	CHECK(slices[0].rowid_lo == EncodeRowid(3, 0, 0, 0));
	CHECK(slices[0].rowid_hi == EncodeRowid(3, 4, 100, 0)); // the 500-block extent is cut every 100 blocks
	CHECK(slices[3].rowid_hi == EncodeRowid(3, 4, 400, 0));
	CHECK(slices[4].rowid_hi == EncodeRowid(4, 0, 0, 0));
	CHECK(slices[5].rowid_lo == EncodeRowid(7, 0, 0, 0));
	CHECK(slices[5].rowid_hi == EncodeRowid(7, 4, 100, 0));
	CHECK(slices[9].rowid_hi == EncodeRowid(8, 0, 0, 0));
	CHECK(slices[10].rowid_lo == EncodeRowid(5, 0, 0, 0)); // smallest last
	CHECK(slices[10].rowid_hi == EncodeRowid(6, 0, 0, 0));
	for (size_t i = 0; i + 1 < 10; i++) {
		if (i != 4) {
			// units of one object are in order here (equal size, stable)
			CHECK(slices[i].rowid_hi == slices[i + 1].rowid_lo);
		}
	}
}

TEST_CASE("BuildRowidSlices cuts large extents at block boundaries", "[rowid]") {
	// A partition of four 8 MB extents (1,024 blocks) in a bigfile tablespace, the second extent crossing a
	// 2^22-block boundary: TargetBlocks(4096, 4) = 256 gives 4 units per session, 4 per extent
	const uint32_t base = (5u << 22) - 1536;
	std::vector<SegmentExtent> extents;
	for (uint32_t i = 0; i < 4; i++) {
		extents.push_back({42, 1024, base + i * 1024, 1024});
	}
	const auto slices = BuildRowidSlices(extents, TargetBlocks(4096, 4));
	REQUIRE(slices.size() == 16);
	CHECK(slices[0].rowid_lo == EncodeRowid(42, 0, 0, 0));
	for (size_t i = 0; i + 1 < slices.size(); i++) {
		CHECK(slices[i].rowid_hi == EncodeRowid(42, 1024, base + static_cast<uint32_t>(i + 1) * 256, 0));
		CHECK(slices[i].rowid_hi == slices[i + 1].rowid_lo);
	}
	CHECK(slices[15].rowid_hi == EncodeRowid(43, 0, 0, 0));

	// An extent that fills a unit exactly does not open an empty one
	const auto exact = BuildRowidSlices({{9, 4, 0, 128}, {9, 4, 128, 128}}, 128);
	REQUIRE(exact.size() == 2);
	CHECK(exact[0].rowid_hi == EncodeRowid(9, 4, 128, 0));
	CHECK(exact[1].rowid_hi == EncodeRowid(10, 0, 0, 0));
}

TEST_CASE("BuildRowidSlices closes the last object number inclusively", "[rowid]") {
	const auto slices = BuildRowidSlices({{0xFFFFFFFFu, 4, 0, 10}}, 128);
	REQUIRE(slices.size() == 1);
	CHECK(slices[0].hi_inclusive);
	CHECK(slices[0].rowid_hi == EncodeRowid(0xFFFFFFFFu, 1023, 0x3FFFFF, 65535));
}
