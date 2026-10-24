#include "catch.hpp"
#include "oraduck/describe_source.hpp"
#include "oraduck/fetch_cursor.hpp"
#include "oraduck/read_sql.hpp"
#include "oraduck/read_types.hpp"
#include "oraduck/rowid_ranges.hpp"
#include "oraduck/work_units.hpp"
#include "itest_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace oraduck;

namespace {

constexpr int64_t kRows = 300000;
constexpr uint32_t kSessions = 4;

// Drops the listed views, tables and types when the test ends, even when it fails
struct DropGuard {
	OciSession &session;
	std::vector<std::string> tables;
	std::vector<std::string> views = {};
	std::vector<std::string> types = {};

	void DropAll() {
		for (const auto &view : views) {
			session.Execute("BEGIN EXECUTE IMMEDIATE 'DROP VIEW " + view +
			                "'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;");
		}
		for (const auto &table : tables) {
			DropTableIfExists(session, table);
		}
		for (const auto &type : types) {
			session.Execute("BEGIN EXECUTE IMMEDIATE 'DROP TYPE " + type +
			                " FORCE'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -4043 THEN RAISE; END IF; END;");
		}
	}
	~DropGuard() {
		try {
			DropAll();
		} catch (...) {
		}
	}
};

const char *StrategyName(SplitStrategy strategy) {
	switch (strategy) {
	case SplitStrategy::kAuto:
		return "auto";
	case SplitStrategy::kExtents:
		return "extents";
	case SplitStrategy::kHash:
		return "hash";
	case SplitStrategy::kNone:
		return "none";
	}
	return "?";
}

// PlanWork, timed on stderr
WorkPlan TimedPlan(OciSession &session, const OracleTableDescription &source, SplitStrategy split,
                   SnapshotMode snapshot, uint32_t sessions, const std::string &label) {
	const auto start = std::chrono::steady_clock::now();
	WorkPlan plan = PlanWork(session, source, split, snapshot, sessions);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::fprintf(stderr, "plan %s.%s split=%s (%s): %.1f ms, %s, %zu slices%s%s\n", source.owner.c_str(),
	             source.name.c_str(), StrategyName(split), label.c_str(), seconds * 1e3, StrategyName(plan.strategy),
	             plan.slices.size(), plan.notes.empty() ? "" : "; notes: ", plan.notes.c_str());
	return plan;
}

// LIST (PART_DATE) / LIST (SUB_KEY) table of 300,000 rows with a unique ID: half of them in subpartition P1_S_A
// (the largest), the rest over three dates and three keys; partition P4 stays empty (no segments).
void CreatePartitionedTable(OciSession &session, const std::string &table, const std::string &tablespace) {
	session.Execute("CREATE TABLE " + table +
	                " (ID NUMBER(10) NOT NULL, PART_DATE DATE NOT NULL, SUB_KEY VARCHAR2(8) NOT NULL, "
	                "PAD VARCHAR2(120)) TABLESPACE " +
	                tablespace +
	                " PARTITION BY LIST (PART_DATE) SUBPARTITION BY LIST (SUB_KEY) SUBPARTITION TEMPLATE "
	                "(SUBPARTITION S_A VALUES ('A'), SUBPARTITION S_B VALUES ('B'), SUBPARTITION S_OTHER VALUES "
	                "(DEFAULT)) (PARTITION P1 VALUES (DATE '2024-01-01'), PARTITION P2 VALUES (DATE '2024-01-02'), "
	                "PARTITION P3 VALUES (DATE '2024-01-03'), PARTITION P4 VALUES (DATE '2024-01-04'))");
	session.Execute("INSERT /*+ APPEND */ INTO " + table +
	                " SELECT r, CASE WHEN r <= 150000 THEN DATE '2024-01-01' ELSE DATE '2024-01-01' + MOD(r, 3) END, "
	                "CASE WHEN r <= 150000 OR MOD(r, 5) = 0 THEN 'A' WHEN MOD(r, 5) = 1 THEN 'B' ELSE 'C' || "
	                "MOD(r, 7) END, RPAD('x', 120, 'y') FROM (SELECT (a.n - 1) * 1000 + b.n AS r FROM (SELECT LEVEL "
	                "AS n FROM dual CONNECT BY LEVEL <= 300) a CROSS JOIN (SELECT LEVEL AS n FROM dual CONNECT BY "
	                "LEVEL <= 1000) b)");
	session.Execute("BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, '" + table + "'); END;");
	session.Execute("GRANT SELECT ON " + table + " TO " + ReaderCredentials().user);
}

struct ReadResult {
	int64_t rows = 0;
	int64_t distinct = 0;
	int64_t sum = 0;
	int64_t nulls = 0;
};

// Reads column ID (NUMBER(10), IDs 1..max_id) of every slice of the plan, one slice after the other
ReadResult ReadIds(OciSession &session, const OracleTableDescription &source, const WorkPlan &plan, int64_t max_id) {
	const OracleColumn *id = nullptr;
	for (const auto &column : source.columns) {
		if (column.name == "ID") {
			id = &column;
		}
	}
	REQUIRE(id);
	const ReadColumnPlan column = PlanReadColumn(*id, UnconstrainedNumber(), source.db_charset);
	REQUIRE(column.decoder == FetchDecoder::kNativeInt64);
	ScanSqlInput input;
	input.owner = source.owner;
	input.object = source.name;
	input.columns = {&column};
	input.snapshot_scn = plan.scn;

	std::vector<bool> seen(static_cast<size_t>(max_id) + 1);
	ReadResult result;
	for (const auto &slice : plan.slices) {
		FetchCursor cursor(session, BuildScanSql(input, slice), input.columns, FetchOptions());
		while (const uint32_t rows = cursor.Fetch()) {
			for (uint32_t r = 0; r < rows; r++) {
				uint32_t length = 0;
				const uint8_t *value = cursor.Value(0, r, length);
				if (!value || length != sizeof(int64_t)) {
					result.nulls++;
					continue;
				}
				int64_t v = 0;
				std::memcpy(&v, value, sizeof(v));
				result.rows++;
				result.sum += v;
				if (v >= 1 && v <= max_id && !seen[static_cast<size_t>(v)]) {
					seen[static_cast<size_t>(v)] = true;
					result.distinct++;
				}
			}
		}
	}
	return result;
}

// Every row of the table read exactly once: row count, distinct IDs and SUM(ID) as Oracle computes it
void CheckEveryRowOnce(OciSession &session, const OracleTableDescription &source, const WorkPlan &plan, int64_t rows,
                       const std::string &oracle_sum) {
	const auto result = ReadIds(session, source, plan, rows);
	CHECK(result.nulls == 0);
	CHECK(result.rows == rows);
	CHECK(result.distinct == rows);
	CHECK(std::to_string(result.sum) == oracle_sum);
}

// Data object number of an extended ROWID: its first six base-64 characters
uint32_t RowidObject(const std::string &rowid) {
	static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	REQUIRE(rowid.size() == 18);
	uint64_t value = 0;
	for (size_t i = 0; i < 6; i++) {
		const auto digit = kAlphabet.find(rowid[i]);
		REQUIRE(digit != std::string::npos);
		value = value * 64 + digit;
	}
	return static_cast<uint32_t>(value);
}

size_t UnitsOfObject(const WorkPlan &plan, uint32_t data_object_id) {
	size_t units = 0;
	for (const auto &slice : plan.slices) {
		if (slice.kind == SliceKind::kRowidRange && RowidObject(slice.rowid_lo) == data_object_id) {
			units++;
		}
	}
	return units;
}

size_t LeftoverTasks(OciSession &session) {
	return std::stoul(
	    Scalar(session, "SELECT COUNT(*) FROM user_parallel_execute_tasks WHERE task_name LIKE 'ORADUCK%'"));
}

// AS OF SCN on a table changed by DDL a few seconds ago raises ORA-01466, even at the current SCN: wait for it
void WaitForFlashback(OciSession &session, const std::string &table) {
	for (int attempt = 0; attempt < 60; attempt++) {
		try {
			session.QueryStrings("SELECT 1 FROM " + table +
			                     " AS OF SCN DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER WHERE ROWNUM < 1");
			return;
		} catch (const OracleError &e) {
			if (e.ora_code != 1466) {
				throw;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	FAIL("flashback query on " << table << " still raises ORA-01466 after 15 s");
}

// Message of the OracleError that fn throws, "" when it throws none
template <class F>
std::string OracleErrorText(F fn) {
	try {
		fn();
	} catch (const OracleError &e) {
		return e.what();
	}
	return "";
}

bool AllDigits(const std::string &text) {
	if (text.empty()) {
		return false;
	}
	for (char c : text) {
		if (c < '0' || c > '9') {
			return false;
		}
	}
	return true;
}

// All the checks on the partitioned table, created in the given tablespace
void CheckPartitionedTable(const std::string &table, const std::string &tablespace, bool bigfile) {
	OciSession session(TestCredentials());
	OciSession reader(ReaderCredentials());
	DropGuard guard {session, {table}};
	guard.DropAll();
	CreatePartitionedTable(session, table, tablespace);
	const std::string owner = Scalar(session, "SELECT USER FROM dual");
	const std::string sum = Scalar(session, "SELECT TO_CHAR(SUM(ID)) FROM " + table);

	// The DBA_EXTENTS path sees bigfile block addresses (relative_fno 1024) in the BENCH tablespace only
	const auto files = session.QueryStrings(
	    "SELECT DISTINCT TO_CHAR(relative_fno) FROM dba_extents WHERE owner = USER AND segment_name = :1", {table});
	REQUIRE(files.size() == 1);
	CHECK((files[0][0] == "1024") == bigfile);

	// Largest subpartition: P1_S_A, several extents
	const auto largest = session.QueryStrings(
	    "SELECT o.subobject_name, TO_CHAR(o.data_object_id), TO_CHAR(s.blocks) FROM user_segments s JOIN "
	    "user_objects o ON o.object_name = s.segment_name AND o.subobject_name = s.partition_name WHERE "
	    "s.segment_name = :1 ORDER BY s.blocks DESC, o.subobject_name",
	    {table});
	REQUIRE(!largest.empty());
	REQUIRE(largest[0][0] == "P1_S_A");
	const auto largest_object = static_cast<uint32_t>(std::stoul(largest[0][1]));
	const uint64_t largest_blocks = std::stoull(largest[0][2]);
	REQUIRE(largest_blocks > 2 * kMinUnitBlocks);
	const uint64_t target = TargetBlocks(largest_blocks, kSessions);

	const auto source = DescribeSource(session, ParseTargetName(table));

	// Each strategy reads every row once
	{
		const auto extents =
		    TimedPlan(session, source, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions, "bench");
		CHECK(extents.strategy == SplitStrategy::kExtents);
		CHECK(extents.scn.empty());
		REQUIRE(extents.slices.size() > kSessions);
		CHECK(extents.slices[0].kind == SliceKind::kRowidRange);
		CheckEveryRowOnce(session, source, extents, kRows, sum);

		const auto hash = TimedPlan(session, source, SplitStrategy::kHash, SnapshotMode::kNone, kSessions, "bench");
		CHECK(hash.strategy == SplitStrategy::kHash);
		REQUIRE(hash.slices.size() == kSessions);
		for (uint32_t k = 0; k < kSessions; k++) {
			CHECK(hash.slices[k].kind == SliceKind::kHash);
			CHECK(hash.slices[k].hash_buckets == kSessions);
			CHECK(hash.slices[k].hash_bucket == k);
		}
		CheckEveryRowOnce(session, source, hash, kRows, sum);

		const auto none = TimedPlan(session, source, SplitStrategy::kNone, SnapshotMode::kNone, kSessions, "bench");
		CHECK(none.strategy == SplitStrategy::kNone);
		REQUIRE(none.slices.size() == 1);
		CHECK(none.slices[0].kind == SliceKind::kNone);
		CheckEveryRowOnce(session, source, none, kRows, sum);
	}

	// Auto: extents from DBA_EXTENTS for the owner, from DBMS_PARALLEL_EXECUTE for a SELECT-only user
	{
		const auto plan = TimedPlan(session, source, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "bench");
		CHECK(plan.strategy == SplitStrategy::kExtents);
		CHECK_THAT(plan.notes, Catch::Contains("DBA_EXTENTS"));
		CHECK_THAT(plan.notes, !Catch::Contains("DBMS_PARALLEL_EXECUTE"));
		for (const auto &slice : plan.slices) {
			CHECK(slice.kind == SliceKind::kRowidRange);
			CHECK_FALSE(slice.hi_inclusive);
		}
		// Units sized from the largest segment (at least one 8 MB extent each): the largest subpartition is cut
		CHECK(UnitsOfObject(plan, largest_object) == (largest_blocks + target - 1) / target);
		CheckEveryRowOnce(session, source, plan, kRows, sum);

		const auto reader_source = DescribeSource(reader, ParseTargetName(owner + "." + table));
		const auto chunks =
		    TimedPlan(reader, reader_source, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "reader");
		CHECK(chunks.strategy == SplitStrategy::kExtents);
		CHECK_THAT(chunks.notes, Catch::Contains("DBMS_PARALLEL_EXECUTE"));
		REQUIRE(!chunks.slices.empty());
		for (const auto &slice : chunks.slices) {
			CHECK(slice.kind == SliceKind::kRowidRange);
			CHECK(slice.hi_inclusive);
		}
		CHECK(UnitsOfObject(chunks, largest_object) >= 2);
		CheckEveryRowOnce(reader, reader_source, chunks, kRows, sum);
		CHECK(LeftoverTasks(reader) == 0);

		// Forced extents with the reader: DBMS_PARALLEL_EXECUTE too
		const auto forced =
		    TimedPlan(reader, reader_source, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions, "reader");
		CHECK(forced.strategy == SplitStrategy::kExtents);
		CHECK_THAT(forced.notes, Catch::Contains("DBMS_PARALLEL_EXECUTE"));
		CheckEveryRowOnce(reader, reader_source, forced, kRows, sum);
		CHECK(LeftoverTasks(reader) == 0);
	}

	// Snapshot
	{
		// The current SCN comes from DBMS_FLASHBACK (granted to the test user), never from TIMESTAMP_TO_SCN, which lags
		const std::string scn = ReadCurrentScn(session);
		REQUIRE(AllDigits(scn));
		CHECK(std::stoll(scn) <=
		      std::stoll(Scalar(session, "SELECT TO_CHAR(DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER) FROM dual")));
		CHECK(ReadCurrentScn(reader).empty());
		WaitForFlashback(session, table);

		const auto plan = TimedPlan(session, source, SplitStrategy::kAuto, SnapshotMode::kAuto, kSessions, "bench");
		CHECK(AllDigits(plan.scn));
		CHECK_THAT(plan.notes, !Catch::Contains("snapshot"));
		CheckEveryRowOnce(session, source, plan, kRows, sum);
		const auto forced = TimedPlan(session, source, SplitStrategy::kAuto, SnapshotMode::kScn, kSessions, "bench");
		CHECK(AllDigits(forced.scn));

		// The reader can read neither DBMS_FLASHBACK nor V$DATABASE: per-session reads under auto, an error under scn
		const auto reader_source = DescribeSource(reader, ParseTargetName(owner + "." + table));
		const auto fallback =
		    TimedPlan(reader, reader_source, SplitStrategy::kAuto, SnapshotMode::kAuto, kSessions, "reader");
		CHECK(fallback.scn.empty());
		CHECK_THAT(fallback.notes, Catch::Contains("snapshot: per session (no current SCN: needs EXECUTE ON "
		                                           "DBMS_FLASHBACK or SELECT ON V_$DATABASE)"));
		CheckEveryRowOnce(reader, reader_source, fallback, kRows, sum);
		CHECK_THROWS_WITH(PlanWork(reader, reader_source, SplitStrategy::kAuto, SnapshotMode::kScn, kSessions),
		                  Catch::StartsWith("snapshot 'scn'") && Catch::Contains("EXECUTE ON DBMS_FLASHBACK"));
		CHECK(LeftoverTasks(reader) == 0);
	}
}

} // namespace

TEST_CASE("Work planning: partitioned table in a bigfile tablespace", "[oracle][work_units]") {
	CheckPartitionedTable("ITEST_WU_BIG", "BENCH", true);
}

TEST_CASE("Work planning: partitioned table in a smallfile tablespace", "[oracle][work_units]") {
	CheckPartitionedTable("ITEST_WU_SMALL", "BENCH_SMALL", false);
}

TEST_CASE("Work planning: views", "[oracle][work_units]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_WU_BASE", "ITEST_WU_BASE2"}, {"ITEST_WU_V", "ITEST_WU_JV"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_WU_BASE (ID NUMBER(10), K NUMBER(4))");
	session.Execute("CREATE TABLE ITEST_WU_BASE2 (ID NUMBER(10), V VARCHAR2(10))");
	session.Execute("INSERT INTO ITEST_WU_BASE SELECT LEVEL, MOD(LEVEL, 10) FROM dual CONNECT BY LEVEL <= 1000");
	session.Execute("INSERT INTO ITEST_WU_BASE2 SELECT LEVEL, 'v' || LEVEL FROM dual CONNECT BY LEVEL <= 1000");
	session.Execute("CREATE VIEW ITEST_WU_V AS SELECT ID, K FROM ITEST_WU_BASE WHERE K < 5");
	session.Execute("CREATE VIEW ITEST_WU_JV AS SELECT a.ID, b.V FROM ITEST_WU_BASE a JOIN ITEST_WU_BASE2 b ON b.ID = "
	                "a.ID");

	// A simple view: hash under auto; extents cannot run on a view
	const auto view = DescribeSource(session, ParseTargetName("ITEST_WU_V"));
	const auto plan = TimedPlan(session, view, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "bench");
	CHECK(plan.strategy == SplitStrategy::kHash);
	CHECK(plan.slices.size() == kSessions);
	const auto read = ReadIds(session, view, plan, 1000);
	CHECK(read.rows == 500);
	CHECK(read.distinct == 500);
	CHECK_THROWS_WITH(PlanWork(session, view, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions),
	                  Catch::StartsWith("split 'extents'") && Catch::Contains("view"));

	// A join view has no ROWID (ORA-01445): one session under auto, an error under hash
	const auto join = DescribeSource(session, ParseTargetName("ITEST_WU_JV"));
	const auto single = TimedPlan(session, join, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "bench");
	CHECK(single.strategy == SplitStrategy::kNone);
	REQUIRE(single.slices.size() == 1);
	CHECK(single.slices[0].kind == SliceKind::kNone);
	CHECK_THAT(single.notes, Catch::Contains("ORA-01445"));
	CHECK_THAT(OracleErrorText([&] { PlanWork(session, join, SplitStrategy::kHash, SnapshotMode::kNone, kSessions); }),
	           Catch::StartsWith("split 'hash'") && Catch::Contains("ORA-01445"));
}

TEST_CASE("Work planning: empty table without segment", "[oracle][work_units]") {
	OciSession session(TestCredentials());
	OciSession reader(ReaderCredentials());
	DropGuard guard {session, {"ITEST_WU_EMPTY"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_WU_EMPTY (ID NUMBER(10)) SEGMENT CREATION DEFERRED");
	session.Execute("GRANT SELECT ON ITEST_WU_EMPTY TO " + ReaderCredentials().user);
	REQUIRE(Scalar(session, "SELECT segment_created FROM user_tables WHERE table_name = 'ITEST_WU_EMPTY'") == "NO");
	const std::string owner = Scalar(session, "SELECT USER FROM dual");

	const auto source = DescribeSource(session, ParseTargetName("ITEST_WU_EMPTY"));
	const auto plan = TimedPlan(session, source, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions, "bench");
	CHECK(plan.strategy == SplitStrategy::kExtents);
	CHECK(plan.slices.empty());

	const auto reader_source = DescribeSource(reader, ParseTargetName(owner + ".ITEST_WU_EMPTY"));
	const auto chunks =
	    TimedPlan(reader, reader_source, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions, "reader");
	CHECK(chunks.strategy == SplitStrategy::kExtents);
	CHECK(chunks.slices.empty());
	CHECK(LeftoverTasks(reader) == 0);
}

TEST_CASE("Work planning: index-organized table", "[oracle][work_units]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_WU_IOT"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_WU_IOT (ID NUMBER(10) PRIMARY KEY, V VARCHAR2(10)) ORGANIZATION INDEX");
	session.Execute("INSERT INTO ITEST_WU_IOT SELECT LEVEL, 'v' || LEVEL FROM dual CONNECT BY LEVEL <= 1000");

	// No ROWID ranges on an index-organized table (its rows have no physical address): hash under auto
	const auto source = DescribeSource(session, ParseTargetName("ITEST_WU_IOT"));
	const auto plan = TimedPlan(session, source, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "bench");
	CHECK(plan.strategy == SplitStrategy::kHash);
	CHECK_THAT(plan.notes, Catch::Contains("index-organized"));
	CheckEveryRowOnce(session, source, plan, 1000, "500500");
	CHECK_THROWS_WITH(PlanWork(session, source, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions),
	                  Catch::StartsWith("split 'extents'") && Catch::Contains("index-organized"));
}

TEST_CASE("Work planning: object tables", "[oracle][work_units]") {
	OciSession session(TestCredentials());
	OciSession reader(ReaderCredentials());
	DropGuard guard {session, {"ITEST_WU_OBJ", "ITEST_WU_OBJIOT"}, {}, {"ITEST_WU_TYPE"}};
	guard.DropAll();
	session.Execute("CREATE TYPE ITEST_WU_TYPE AS OBJECT (ID NUMBER(10), V VARCHAR2(10))");
	session.Execute("CREATE TABLE ITEST_WU_OBJ OF ITEST_WU_TYPE");
	session.Execute("CREATE TABLE ITEST_WU_OBJIOT OF ITEST_WU_TYPE (PRIMARY KEY (ID)) ORGANIZATION INDEX");
	for (const std::string table : {"ITEST_WU_OBJ", "ITEST_WU_OBJIOT"}) {
		session.Execute("INSERT INTO " + table +
		                " SELECT ITEST_WU_TYPE(LEVEL, 'v' || LEVEL) FROM dual CONNECT BY LEVEL <= 1000");
		session.Execute("GRANT SELECT ON " + table + " TO " + ReaderCredentials().user);
	}
	const std::string owner = Scalar(session, "SELECT USER FROM dual");
	// ALL_TABLES does not list object tables (ALL_ALL_TABLES does)
	REQUIRE(Scalar(session, "SELECT COUNT(*) FROM all_tables WHERE owner = USER AND table_name LIKE 'ITEST_WU_OBJ%'") ==
	        "0");

	// A heap object table: extents from DBA_EXTENTS
	const auto heap = DescribeSource(session, ParseTargetName("ITEST_WU_OBJ"));
	const auto extents = TimedPlan(session, heap, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "bench");
	CHECK(extents.strategy == SplitStrategy::kExtents);
	CHECK_THAT(extents.notes, Catch::Contains("DBA_EXTENTS"));
	CheckEveryRowOnce(session, heap, extents, 1000, "500500");

	// An object IOT has no TABLE segment: hash
	const auto iot = DescribeSource(session, ParseTargetName("ITEST_WU_OBJIOT"));
	const auto hash = TimedPlan(session, iot, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "bench");
	CHECK(hash.strategy == SplitStrategy::kHash);
	CHECK_THAT(hash.notes, Catch::Contains("index-organized"));
	CheckEveryRowOnce(session, iot, hash, 1000, "500500");
	CHECK_THROWS_WITH(PlanWork(session, iot, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions),
	                  Catch::StartsWith("split 'extents'") && Catch::Contains("index-organized"));

	// The reader: no DBA_EXTENTS (ORA-00942), and DBMS_PARALLEL_EXECUTE refuses object tables (ORA-29491) once its
	// task is created
	const auto reader_heap = DescribeSource(reader, ParseTargetName(owner + ".ITEST_WU_OBJ"));
	CHECK_THROWS_WITH(PlanWork(reader, reader_heap, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions),
	                  Catch::StartsWith("split 'extents' needs DBA_EXTENTS or DBMS_PARALLEL_EXECUTE: ") &&
	                      Catch::Contains("ORA-00942") && Catch::Contains("ORA-29491"));
	// The task created before the failure is dropped
	CHECK(LeftoverTasks(reader) == 0);
	const auto fallback =
	    TimedPlan(reader, reader_heap, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "reader");
	CHECK(fallback.strategy == SplitStrategy::kHash);
	CHECK_THAT(fallback.notes, Catch::Contains("ORA-29491") && Catch::Contains("using hash"));
	CheckEveryRowOnce(reader, reader_heap, fallback, 1000, "500500");
	CHECK(LeftoverTasks(reader) == 0);
}

TEST_CASE("Work planning: table not listed in ALL_ALL_TABLES", "[oracle][work_units]") {
	OciSession session(TestCredentials());
	OciSession reader(ReaderCredentials());
	DropGuard guard {session, {"ITEST_WU_PRIVATE"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_WU_PRIVATE (ID NUMBER(10))");
	session.Execute("INSERT INTO ITEST_WU_PRIVATE VALUES (1)");

	// The reader has no privilege on the table: nothing proves it is a heap table, so no extents
	OracleTableDescription source;
	source.owner = Scalar(session, "SELECT USER FROM dual");
	source.name = "ITEST_WU_PRIVATE";
	source.kind = SourceKind::kTable;
	CHECK_THROWS_WITH(PlanWork(reader, source, SplitStrategy::kExtents, SnapshotMode::kNone, kSessions),
	                  Catch::StartsWith("split 'extents' needs a heap table: ") && Catch::Contains("ALL_ALL_TABLES"));
	const auto plan = TimedPlan(reader, source, SplitStrategy::kAuto, SnapshotMode::kNone, kSessions, "reader");
	CHECK(plan.strategy == SplitStrategy::kNone);
	CHECK(plan.slices.size() == 1);
	CHECK_THAT(plan.notes, Catch::Contains("ALL_ALL_TABLES") && Catch::Contains("hash: ORA-00942"));
	CHECK(LeftoverTasks(reader) == 0);
}

TEST_CASE("Work planning: snapshot refused by Oracle", "[oracle][work_units]") {
	OciSession session(TestCredentials());
	// Flashback query on another user's table needs the FLASHBACK privilege, which the test user lacks on SYS.DUAL
	const auto source = DescribeSource(session, ParseTargetName("SYS.DUAL"));
	const auto plan = TimedPlan(session, source, SplitStrategy::kHash, SnapshotMode::kAuto, kSessions, "bench");
	REQUIRE(plan.slices.size() == kSessions);
	CHECK(plan.scn.empty());
	CHECK_THAT(plan.notes, Catch::StartsWith("snapshot: per session (AS OF SCN: ORA-01031"));
	CHECK_THAT(OracleErrorText([&] { PlanWork(session, source, SplitStrategy::kHash, SnapshotMode::kScn, kSessions); }),
	           Catch::StartsWith("snapshot 'scn' cannot read SYS.DUAL AS OF SCN ") && Catch::Contains("ORA-01031"));

	// One statement is read-consistent: no snapshot, so nothing to refuse (V_$SESSION has no ROWID: one session)
	OracleTableDescription view;
	view.owner = "SYS";
	view.name = "V_$SESSION";
	view.kind = SourceKind::kView;
	for (auto split : {SplitStrategy::kNone, SplitStrategy::kAuto}) {
		const auto single = TimedPlan(session, view, split, SnapshotMode::kScn, kSessions, "bench");
		CHECK(single.slices.size() == 1);
		CHECK(single.scn.empty());
		CHECK_THAT(single.notes, !Catch::Contains("snapshot"));
	}
}

TEST_CASE("Work planning: one session", "[oracle][work_units]") {
	OciSession session(TestCredentials());
	OciSession reader(ReaderCredentials());
	DropGuard guard {session, {"ITEST_WU_ONE"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_WU_ONE (ID NUMBER(10))");
	session.Execute("INSERT INTO ITEST_WU_ONE SELECT LEVEL FROM dual CONNECT BY LEVEL <= 10");
	session.Execute("GRANT SELECT ON ITEST_WU_ONE TO " + ReaderCredentials().user);
	const auto source = DescribeSource(session, ParseTargetName("ITEST_WU_ONE"));

	// No query at all, not even for the snapshot (one statement is read-consistent): the session's execute count
	// moves only with the two readings of it
	const std::string executes = "SELECT TO_CHAR(m.value) FROM v$mystat m JOIN v$statname n ON n.statistic# = "
	                             "m.statistic# WHERE n.name = 'execute count'";
	const auto before = std::stoll(Scalar(session, executes));
	const auto between = std::stoll(Scalar(session, executes));
	const auto plan = PlanWork(session, source, SplitStrategy::kAuto, SnapshotMode::kScn, 1);
	const auto after = std::stoll(Scalar(session, executes));
	CHECK(after - between == between - before);
	CHECK(plan.strategy == SplitStrategy::kNone);
	REQUIRE(plan.slices.size() == 1);
	CHECK(plan.slices[0].kind == SliceKind::kNone);
	CHECK(plan.scn.empty());
	CHECK(plan.notes.empty());

	const auto forced = PlanWork(session, source, SplitStrategy::kExtents, SnapshotMode::kNone, 1);
	CHECK(forced.strategy == SplitStrategy::kNone);
	CHECK(forced.slices.size() == 1);
	CheckEveryRowOnce(session, source, forced, 10, "55");

	// A table of one extent is one unit with several sessions too: no snapshot either
	const auto extent = TimedPlan(session, source, SplitStrategy::kAuto, SnapshotMode::kScn, kSessions, "bench");
	CHECK(extent.strategy == SplitStrategy::kExtents);
	REQUIRE(extent.slices.size() == 1);
	CHECK(extent.scn.empty());
	CHECK_THAT(extent.notes, !Catch::Contains("snapshot"));

	// The reader, who cannot read the current SCN: 'scn' is no error for one statement
	const auto reader_source =
	    DescribeSource(reader, ParseTargetName(Scalar(session, "SELECT USER FROM dual") + ".ITEST_WU_ONE"));
	for (uint32_t sessions : {1u, kSessions}) {
		const auto one = TimedPlan(reader, reader_source, SplitStrategy::kAuto, SnapshotMode::kScn, sessions, "reader");
		CHECK(one.slices.size() == 1);
		CHECK(one.scn.empty());
		CHECK_THAT(one.notes, !Catch::Contains("snapshot"));
	}
	CHECK(LeftoverTasks(reader) == 0);
}
