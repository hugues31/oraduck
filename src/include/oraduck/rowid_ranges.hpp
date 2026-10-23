#pragma once

#include "oraduck/read_sql.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oraduck {

// Extended ROWID OOOOOOFFFBBBBBBRRR in Oracle's base 64 (A-Z a-z 0-9 + /). For a bigfile tablespace
// (relative_fno == 1024) the file field holds block >> 22 and the block field block & 0x3FFFFF.
std::string EncodeRowid(uint32_t data_object_id, uint32_t relative_fno, uint32_t block, uint16_t row);

struct SegmentExtent {
	uint32_t data_object_id;
	uint32_t relative_fno;
	uint32_t block_id;
	uint32_t blocks;
};

// max(ceil(largest_segment_blocks / (4 * sessions)), 128): sized from the largest segment so that a
// partition kept by the filter is still cut into enough units.
uint64_t TargetBlocks(uint64_t largest_segment_blocks, uint32_t sessions);

// ROWID range slices covering every row of every object exactly once, largest first. Each object is cut into
// units of target_blocks blocks (its last unit takes the rest), extents being cut at block boundaries: an 8 MB
// partition extent is not one unit.
std::vector<Slice> BuildRowidSlices(std::vector<SegmentExtent> extents, uint64_t target_blocks);

} // namespace oraduck
