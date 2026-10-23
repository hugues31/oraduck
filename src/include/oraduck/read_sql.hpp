#pragma once

#include "oraduck/read_types.hpp"
#include "oraduck/sql_binds.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oraduck {

enum class SplitStrategy { kAuto, kExtents, kHash, kNone };
enum class SnapshotMode { kAuto, kScn, kNone };

// Case-insensitive; throw OraduckError listing the accepted values.
SplitStrategy ParseSplitStrategy(const std::string &text);
SnapshotMode ParseSnapshotMode(const std::string &text);

enum class SliceKind { kNone, kRowidRange, kHash };

// ROWID range [lo, hi), or [lo, hi] when hi_inclusive (DBMS_PARALLEL_EXECUTE chunks), or hash bucket.
struct Slice {
	SliceKind kind = SliceKind::kNone;
	std::string rowid_lo, rowid_hi;
	bool hi_inclusive = false;
	uint32_t hash_buckets = 0, hash_bucket = 0;
};

struct ScanSqlInput {
	std::string owner, object;
	std::vector<const ReadColumnPlan *> columns;
	std::vector<SqlFragment> filters; // placeholders numbered from :b1 in each fragment
	std::string snapshot_scn;         // empty: no AS OF SCN
};

// "NAME" (dictionary names are used as they are).
std::string QuoteIdentifier(const std::string &name);

// Binds in textual order: snapshot, filters, slice.
SqlStatement BuildScanSql(const ScanSqlInput &input, const Slice &slice);

} // namespace oraduck
