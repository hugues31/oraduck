#include "oraduck/rowid_ranges.hpp"

#include <algorithm>
#include <map>

namespace oraduck {

namespace {

void AppendBase64(std::string &out, uint64_t value, int digits) {
	static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (int i = digits - 1; i >= 0; i--) {
		out.push_back(kAlphabet[(value >> (6 * i)) & 63]);
	}
}

struct Unit {
	Slice slice;
	uint64_t blocks = 0;
};

} // namespace

std::string EncodeRowid(uint32_t data_object_id, uint32_t relative_fno, uint32_t block, uint16_t row) {
	uint64_t file = relative_fno;
	uint64_t block_field = block;
	if (relative_fno == 1024) {
		file = block >> 22;
		block_field = block & 0x3FFFFF;
	}
	std::string out;
	AppendBase64(out, data_object_id, 6);
	AppendBase64(out, file, 3);
	AppendBase64(out, block_field, 6);
	AppendBase64(out, row, 3);
	return out;
}

uint64_t TargetBlocks(uint64_t largest_segment_blocks, uint32_t sessions) {
	const uint64_t divisor = 4ULL * std::max<uint32_t>(sessions, 1);
	return std::max<uint64_t>((largest_segment_blocks + divisor - 1) / divisor, kMinUnitBlocks);
}

std::vector<Slice> BuildRowidSlices(std::vector<SegmentExtent> extents, uint64_t target_blocks) {
	target_blocks = std::max<uint64_t>(target_blocks, 1);
	std::map<uint32_t, std::vector<SegmentExtent>> by_object;
	for (const auto &e : extents) {
		by_object[e.data_object_id].push_back(e);
	}
	std::vector<Unit> units;
	for (auto &entry : by_object) {
		const uint32_t object = entry.first;
		auto &list = entry.second;
		std::sort(list.begin(), list.end(), [](const SegmentExtent &a, const SegmentExtent &b) {
			return a.relative_fno != b.relative_fno ? a.relative_fno < b.relative_fno : a.block_id < b.block_id;
		});
		Unit current;
		current.slice.kind = SliceKind::kRowidRange;
		current.slice.rowid_lo = EncodeRowid(object, 0, 0, 0);
		for (const auto &e : list) {
			// Fill units of target_blocks, cutting the extent at a block boundary when it does not fit
			uint64_t offset = 0;
			while (offset < e.blocks) {
				if (current.blocks >= target_blocks) {
					const std::string boundary =
					    EncodeRowid(object, e.relative_fno, static_cast<uint32_t>(e.block_id + offset), 0);
					current.slice.rowid_hi = boundary;
					units.push_back(std::move(current));
					current = Unit();
					current.slice.kind = SliceKind::kRowidRange;
					current.slice.rowid_lo = boundary;
				}
				const uint64_t take = std::min<uint64_t>(e.blocks - offset, target_blocks - current.blocks);
				current.blocks += take;
				offset += take;
			}
		}
		if (object == 0xFFFFFFFFu) {
			// No next object number: close the last range on the highest possible ROWID
			current.slice.rowid_hi = EncodeRowid(object, 1023, 0x3FFFFF, 65535);
			current.slice.hi_inclusive = true;
		} else {
			current.slice.rowid_hi = EncodeRowid(object + 1, 0, 0, 0);
		}
		units.push_back(current);
	}
	std::stable_sort(units.begin(), units.end(), [](const Unit &a, const Unit &b) { return a.blocks > b.blocks; });
	std::vector<Slice> slices;
	slices.reserve(units.size());
	for (auto &u : units) {
		slices.push_back(std::move(u.slice));
	}
	return slices;
}

} // namespace oraduck
