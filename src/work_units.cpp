#include "oraduck/work_units.hpp"

#include "oraduck/errors.hpp"
#include "oraduck/rowid_ranges.hpp"
#include "oraduck/sql_binds.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oraduck {

namespace {

// TIMESTAMP_TO_SCN(SYSTIMESTAMP) is not a source: it lags a few seconds, so a snapshot would miss recent commits
const char *const kScnSources[][2] = {
    {"DBMS_FLASHBACK", "SELECT TO_CHAR(DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER) FROM dual"},
    {"V$DATABASE", "SELECT TO_CHAR(current_scn) FROM v$database"}};
const char *const kScnGrants = "needs EXECUTE ON DBMS_FLASHBACK or SELECT ON V_$DATABASE";

constexpr int32_t kTableNotFound = 942;
constexpr int32_t kInsufficientPrivileges = 1031;
constexpr int32_t kDuplicateTaskName = 29497;
constexpr uint64_t kBlocksWithoutStatistics = 1024;

// Oracle's message ("ORA-NNNNN: text") on one line, without the OCI call that failed and the PL/SQL call stack
// (ORA-06512 lines)
std::string OracleMessage(const OracleError &e) {
	std::string text = e.what();
	const auto ora = text.find("ORA-");
	if (ora != std::string::npos) {
		text = text.substr(ora);
	}
	std::string message;
	size_t start = 0;
	while (start < text.size()) {
		auto end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		const std::string line = text.substr(start, end - start);
		if (!line.empty() && line.compare(0, 9, "ORA-06512") != 0) {
			message += (message.empty() ? "" : " ") + line;
		}
		start = end + 1;
	}
	return message;
}

// "ORA-NNNNN", or the message when there is no ORA code
std::string OracleCode(const OracleError &e) {
	if (e.ora_code == 0) {
		return OracleMessage(e);
	}
	char code[16];
	std::snprintf(code, sizeof(code), "ORA-%05d", static_cast<int>(e.ora_code));
	return code;
}

void AddNote(WorkPlan &plan, const std::string &note) {
	plan.notes += (plan.notes.empty() ? "" : "; ") + note;
}

std::string DisplayName(const OracleTableDescription &source) {
	return source.owner + "." + source.name;
}

std::string ObjectSql(const OracleTableDescription &source) {
	return QuoteIdentifier(source.owner) + "." + QuoteIdentifier(source.name);
}

std::string ReadScn(OciSession &session, std::string &failures) {
	for (const auto &source : kScnSources) {
		try {
			const auto rows = session.QueryStrings(source[1]);
			if (!rows.empty() && !rows[0].empty() && !rows[0][0].empty()) {
				return rows[0][0];
			}
			failures += (failures.empty() ? "" : ", ") + std::string(source[0]) + ": no value";
		} catch (const OracleError &e) {
			failures += (failures.empty() ? "" : ", ") + std::string(source[0]) + ": " + OracleCode(e);
		}
	}
	return "";
}

// The snapshot notes come first
void AddSnapshotNote(WorkPlan &plan, const std::string &note) {
	plan.notes = note + (plan.notes.empty() ? "" : "; " + plan.notes);
}

// Sets plan.scn to scn (read before the units, "" when it could not be, failures saying why) once Oracle accepts
// AS OF SCN on the source; otherwise notes per-session reads under 'auto' and throws under 'scn'
void PlanSnapshot(OciSession &session, const OracleTableDescription &source, SnapshotMode mode, const std::string &scn,
                  const std::string &failures, WorkPlan &plan) {
	if (scn.empty()) {
		if (mode == SnapshotMode::kScn) {
			throw OraduckError("snapshot 'scn' cannot read the current SCN: " + std::string(kScnGrants) + " (" +
			                   failures + ")");
		}
		AddSnapshotNote(plan, "snapshot: per session (no current SCN: " + std::string(kScnGrants) + ")");
		return;
	}
	// Flashback query on another user's table needs the FLASHBACK privilege; a table changed by DDL in the last few
	// seconds raises ORA-01466, even at the current SCN
	try {
		session.QueryStrings("SELECT 1 FROM " + ObjectSql(source) + " AS OF SCN TO_NUMBER(:b1) WHERE ROWNUM < 1",
		                     {scn});
	} catch (const OracleError &e) {
		if (mode == SnapshotMode::kScn) {
			throw OracleError("snapshot 'scn' cannot read " + DisplayName(source) + " AS OF SCN " + scn + ": " +
			                      OracleMessage(e),
			                  e.ora_code);
		}
		AddSnapshotNote(plan, "snapshot: per session (AS OF SCN: " + OracleMessage(e) + ")");
		return;
	}
	plan.scn = scn;
}

void PlanSingle(WorkPlan &plan) {
	plan.strategy = SplitStrategy::kNone;
	plan.slices.assign(1, Slice());
}

// Why the table cannot be read by ROWID ranges, "" for a heap table. ALL_ALL_TABLES, unlike ALL_TABLES, lists object
// tables; a table it does not show is not known to be a heap table.
std::string NonHeapReason(OciSession &session, const OracleTableDescription &source) {
	const auto rows = session.QueryStrings(
	    "SELECT iot_type, cluster_name, external, hybrid FROM all_all_tables WHERE owner = :1 AND table_name = :2",
	    {source.owner, source.name});
	if (rows.empty()) {
		return "not in ALL_ALL_TABLES";
	}
	const auto &row = rows[0];
	if (!row[0].empty()) {
		return "an index-organized table";
	}
	if (!row[1].empty()) {
		return "a clustered table";
	}
	if (row[2] == "YES") {
		return "an external table";
	}
	if (row[3] == "YES") {
		return "a hybrid partitioned table";
	}
	return "";
}

// ROWID ranges from DBA_EXTENTS, sized from the largest data object; false when an extent's segment has no data
// object in ALL_OBJECTS (partitions changed between the two queries). DBA_EXTENTS and ALL_OBJECTS are joined here:
// joined in Oracle they took 3.5 s for a table of 300 partitions, against 40 ms for the two queries.
bool SlicesFromDbaExtents(OciSession &session, const OracleTableDescription &source, uint32_t sessions,
                          std::vector<Slice> &slices) {
	const auto extent_rows = session.QueryStrings(
	    "SELECT segment_type, partition_name, TO_CHAR(relative_fno), TO_CHAR(block_id), TO_CHAR(blocks) "
	    "FROM dba_extents WHERE owner = :1 AND segment_name = :2 "
	    "AND segment_type IN ('TABLE', 'TABLE PARTITION', 'TABLE SUBPARTITION')",
	    {source.owner, source.name});
	if (extent_rows.empty()) {
		slices.clear();
		return true;
	}
	// Data object of each segment, by (segment type, partition name); NULL names read as ""
	std::map<std::pair<std::string, std::string>, uint32_t> objects;
	for (const auto &row : session.QueryStrings(
	         "SELECT object_type, subobject_name, TO_CHAR(data_object_id) FROM all_objects WHERE owner = :1 "
	         "AND object_name = :2 AND object_type IN ('TABLE', 'TABLE PARTITION', 'TABLE SUBPARTITION') "
	         "AND data_object_id IS NOT NULL",
	         {source.owner, source.name})) {
		objects[{row[0], row[1]}] = static_cast<uint32_t>(std::stoul(row[2]));
	}
	std::vector<SegmentExtent> extents;
	extents.reserve(extent_rows.size());
	std::map<uint32_t, uint64_t> object_blocks;
	uint64_t largest = 0;
	for (const auto &row : extent_rows) {
		const auto object = objects.find({row[0], row[1]});
		if (object == objects.end()) {
			return false;
		}
		SegmentExtent extent;
		extent.data_object_id = object->second;
		extent.relative_fno = static_cast<uint32_t>(std::stoul(row[2]));
		extent.block_id = static_cast<uint32_t>(std::stoul(row[3]));
		extent.blocks = static_cast<uint32_t>(std::stoul(row[4]));
		extents.push_back(extent);
		largest = std::max(largest, object_blocks[extent.data_object_id] += extent.blocks);
	}
	slices = BuildRowidSlices(std::move(extents), TargetBlocks(largest, sessions));
	return true;
}

// Largest BLOCKS statistic of one segment: subpartitions, else partitions, else the table
uint64_t LargestSegmentStatistic(OciSession &session, const OracleTableDescription &source) {
	const auto rows = session.QueryStrings(
	    "SELECT TO_CHAR(COALESCE("
	    "(SELECT MAX(blocks) FROM all_tab_subpartitions WHERE table_owner = :1 AND table_name = :2), "
	    "(SELECT MAX(blocks) FROM all_tab_partitions WHERE table_owner = :3 AND table_name = :4), "
	    "(SELECT blocks FROM all_tables WHERE owner = :5 AND table_name = :6), " +
	        std::to_string(kBlocksWithoutStatistics) + ")) FROM dual",
	    {source.owner, source.name, source.owner, source.name, source.owner, source.name});
	return std::stoull(rows.at(0).at(0));
}

// ORADUCK_ and 16 random hexadecimal characters
std::string TaskName() {
	uint64_t seed = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
	try {
		std::random_device device;
		seed ^= (static_cast<uint64_t>(device()) << 32) ^ device();
	} catch (const std::exception &) {
		// No entropy source: the clock alone
	}
	std::mt19937_64 generator(seed);
	char name[32];
	std::snprintf(name, sizeof(name), "ORADUCK_%016llX", static_cast<unsigned long long>(generator()));
	return name;
}

void DropTask(OciSession &session, const std::string &task) {
	session.Execute("BEGIN DBMS_PARALLEL_EXECUTE.DROP_TASK(" + SqlStringLiteral(task) + "); END;");
}

// On a failure path: the task may not exist (CREATE_TASK failed) or the session may be unusable
void DropTaskQuietly(OciSession &session, const std::string &task) {
	try {
		DropTask(session, task);
	} catch (const std::exception &) {
	}
}

// ROWID ranges from DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID (inclusive bounds). The task is dropped on every
// path once created; when that drop fails after the chunks were read, the chunks are kept and drop_failure says so.
std::vector<Slice> SlicesFromParallelExecute(OciSession &session, const OracleTableDescription &source,
                                             uint32_t sessions, std::string &drop_failure) {
	const uint64_t chunk_blocks = TargetBlocks(LargestSegmentStatistic(session, source), sessions);
	const std::string task = TaskName();
	std::vector<std::vector<std::string>> rows;
	try {
		session.Execute("BEGIN DBMS_PARALLEL_EXECUTE.CREATE_TASK(" + SqlStringLiteral(task) +
		                "); DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID(" + SqlStringLiteral(task) + ", " +
		                SqlStringLiteral(source.owner) + ", " + SqlStringLiteral(source.name) + ", FALSE, " +
		                std::to_string(chunk_blocks) + "); END;");
		rows = session.QueryStrings("SELECT ROWIDTOCHAR(start_rowid), ROWIDTOCHAR(end_rowid) FROM "
		                            "user_parallel_execute_chunks WHERE task_name = :1 ORDER BY chunk_id",
		                            {task});
	} catch (const OracleError &e) {
		// Another session's task of the same name is not ours to drop
		if (e.ora_code != kDuplicateTaskName) {
			DropTaskQuietly(session, task);
		}
		throw;
	} catch (...) {
		DropTaskQuietly(session, task);
		throw;
	}
	try {
		DropTask(session, task);
	} catch (const OracleError &e) {
		drop_failure = "task " + task + " not dropped: " + OracleMessage(e);
	}
	std::vector<Slice> slices;
	slices.reserve(rows.size());
	for (const auto &row : rows) {
		Slice slice;
		slice.kind = SliceKind::kRowidRange;
		slice.rowid_lo = row[0];
		slice.rowid_hi = row[1];
		slice.hi_inclusive = true;
		slices.push_back(std::move(slice));
	}
	return slices;
}

// Fills plan with ROWID range slices; false, with the error and the note explaining why, when no extent source
// works
bool PlanExtents(OciSession &session, const OracleTableDescription &source, uint32_t sessions, WorkPlan &plan,
                 std::string &error, std::string &note) {
	const std::string non_heap = NonHeapReason(session, source);
	if (!non_heap.empty()) {
		error = "split 'extents' needs a heap table: " + DisplayName(source) + " is " + non_heap;
		note = "extents: " + DisplayName(source) + " is " + non_heap;
		return false;
	}
	std::string dba_message, dba_code;
	try {
		if (SlicesFromDbaExtents(session, source, sessions, plan.slices)) {
			plan.strategy = SplitStrategy::kExtents;
			AddNote(plan, "extents from DBA_EXTENTS");
			return true;
		}
		dba_message = dba_code = "segments without a data object in ALL_OBJECTS";
	} catch (const OracleError &e) {
		if (e.ora_code != kTableNotFound && e.ora_code != kInsufficientPrivileges) {
			throw;
		}
		dba_message = OracleMessage(e);
		dba_code = OracleCode(e);
	}
	try {
		std::string drop_failure;
		plan.slices = SlicesFromParallelExecute(session, source, sessions, drop_failure);
		plan.strategy = SplitStrategy::kExtents;
		AddNote(plan, "extents from DBMS_PARALLEL_EXECUTE (DBA_EXTENTS: " + dba_code + ")");
		if (!drop_failure.empty()) {
			AddNote(plan, "DBMS_PARALLEL_EXECUTE " + drop_failure);
		}
		return true;
	} catch (const OracleError &e) {
		error = "split 'extents' needs DBA_EXTENTS or DBMS_PARALLEL_EXECUTE: DBA_EXTENTS: " + dba_message +
		        "; DBMS_PARALLEL_EXECUTE: " + OracleMessage(e);
		note = "extents: DBA_EXTENTS " + dba_code + ", DBMS_PARALLEL_EXECUTE " + OracleCode(e);
	}
	return false;
}

// Slices for reading source with up to `sessions` sessions (more than one): extents, hash or one session, with the
// fallbacks of 'auto'
void PlanUnits(OciSession &session, const OracleTableDescription &source, SplitStrategy split, uint32_t sessions,
               WorkPlan &plan) {
	const bool view = source.kind == SourceKind::kView;
	if (split == SplitStrategy::kExtents && view) {
		throw OraduckError("split 'extents' needs a table: " + DisplayName(source) + " is a view");
	}
	if (split == SplitStrategy::kExtents || (split == SplitStrategy::kAuto && !view)) {
		std::string error, note;
		if (PlanExtents(session, source, sessions, plan, error, note)) {
			return;
		}
		if (split == SplitStrategy::kExtents) {
			throw OraduckError(error);
		}
		AddNote(plan, note + ", using hash");
	}

	// Hash of the ROWID: a join or aggregate view has none (ORA-01445)
	try {
		session.QueryStrings("SELECT 1 FROM " + ObjectSql(source) + " WHERE ORA_HASH(ROWID, 1) = 0 AND ROWNUM < 1");
	} catch (const OracleError &e) {
		if (split == SplitStrategy::kHash) {
			throw OracleError("split 'hash' needs ORA_HASH(ROWID) on " + DisplayName(source) + ": " + OracleMessage(e),
			                  e.ora_code);
		}
		AddNote(plan, "hash: " + OracleCode(e) + ", using one session");
		PlanSingle(plan);
		return;
	}
	plan.strategy = SplitStrategy::kHash;
	plan.slices.clear();
	for (uint32_t bucket = 0; bucket < sessions; bucket++) {
		Slice slice;
		slice.kind = SliceKind::kHash;
		slice.hash_buckets = sessions;
		slice.hash_bucket = bucket;
		plan.slices.push_back(slice);
	}
}

} // namespace

std::string ReadCurrentScn(OciSession &session) {
	std::string failures;
	return ReadScn(session, failures);
}

WorkPlan PlanWork(OciSession &session, const OracleTableDescription &source, SplitStrategy split, SnapshotMode snapshot,
                  uint32_t sessions) {
	WorkPlan plan;
	// One statement is read-consistent by itself: a single unit needs no snapshot
	if (sessions <= 1 || split == SplitStrategy::kNone) {
		PlanSingle(plan);
		return plan;
	}
	// The SCN is read before the units: a segment created after it (a new partition) holds no row of the snapshot,
	// whereas ranges built from an extent map older than the SCN would miss such a segment's rows
	std::string scn, failures;
	if (snapshot != SnapshotMode::kNone) {
		scn = ReadScn(session, failures);
	}
	PlanUnits(session, source, split, sessions, plan);
	if (snapshot != SnapshotMode::kNone && plan.slices.size() > 1) {
		PlanSnapshot(session, source, snapshot, scn, failures, plan);
	}
	return plan;
}

} // namespace oraduck
