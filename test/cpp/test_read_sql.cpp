#include "catch.hpp"
#include "oraduck/errors.hpp"
#include "oraduck/read_sql.hpp"

using namespace oraduck;

namespace {

OracleColumn Col(const std::string &name, const std::string &type, int p = -1, int s = -1) {
	OracleColumn c;
	c.name = name;
	c.data_type = type;
	c.precision = p;
	c.scale = s;
	return c;
}

SqlFragment Fragment(const std::string &sql, int64_t value) {
	SqlFragment f;
	f.sql = sql;
	f.binds.push_back(BindNumber(value));
	return f;
}

} // namespace

TEST_CASE("Option parsing", "[read_sql]") {
	CHECK(ParseSplitStrategy("AUTO") == SplitStrategy::kAuto);
	CHECK(ParseSplitStrategy("Extents") == SplitStrategy::kExtents);
	CHECK(ParseSplitStrategy("hash") == SplitStrategy::kHash);
	CHECK(ParseSplitStrategy("none") == SplitStrategy::kNone);
	CHECK_THROWS_WITH(ParseSplitStrategy("x"), Catch::Contains("auto, extents, hash, none"));
	CHECK(ParseSnapshotMode("SCN") == SnapshotMode::kScn);
	CHECK(ParseSnapshotMode("None") == SnapshotMode::kNone);
	CHECK(ParseSnapshotMode("auto") == SnapshotMode::kAuto);
	CHECK_THROWS_WITH(ParseSnapshotMode("x"), Catch::Contains("auto, scn, none"));
}

TEST_CASE("QuoteIdentifier", "[read_sql]") {
	CHECK(QuoteIdentifier("Order") == "\"Order\"");
}

TEST_CASE("BuildScanSql", "[read_sql]") {
	const auto id = PlanReadColumn(Col("ID", "NUMBER", 38, 0), UnconstrainedNumber(), "AL32UTF8");
	const auto order = PlanReadColumn(Col("Order", "VARCHAR2"), UnconstrainedNumber(), "AL32UTF8");
	ScanSqlInput input;
	input.owner = "BENCH";
	input.object = "T";
	input.columns = {&id, &order};
	input.filters = {Fragment("(\"ID\" = :b1)", 5)};
	input.snapshot_scn = "123";
	Slice hash;
	hash.kind = SliceKind::kHash;
	hash.hash_buckets = 4;
	hash.hash_bucket = 2;

	const auto sql = BuildScanSql(input, hash);
	CHECK(sql.sql == "SELECT /* oraduck_scan */ \"ID\", \"Order\" FROM \"BENCH\".\"T\" AS OF SCN TO_NUMBER(:b1) "
	                 "WHERE (\"ID\" = :b2) AND ORA_HASH(ROWID, :b3) = :b4");
	REQUIRE(sql.binds.size() == 4);
	CHECK(sql.binds[0].dty == SQLT_CHR);
	CHECK(sql.binds[0].display == "'123'");
	CHECK(sql.binds[1].display == "5");
	CHECK(sql.binds[2].display == "3"); // buckets - 1
	CHECK(sql.binds[3].display == "2");

	SECTION("no column, no filter, no slice") {
		ScanSqlInput bare;
		bare.owner = "O";
		bare.object = "V";
		const auto s = BuildScanSql(bare, Slice());
		CHECK(s.sql == "SELECT /* oraduck_scan */ 1 FROM \"O\".\"V\"");
		CHECK(s.binds.empty());
	}
	SECTION("ROWID range") {
		input.snapshot_scn.clear();
		Slice range;
		range.kind = SliceKind::kRowidRange;
		range.rowid_lo = "AAAAAAAAAAAAAAAAAA";
		range.rowid_hi = "AAAAABAAAAAAAAAAAA";
		const auto s = BuildScanSql(input, range);
		CHECK(s.sql == "SELECT /* oraduck_scan */ \"ID\", \"Order\" FROM \"BENCH\".\"T\" WHERE (\"ID\" = :b1) AND "
		               "ROWID >= CHARTOROWID(:b2) AND ROWID < CHARTOROWID(:b3)");
		REQUIRE(s.binds.size() == 3);
		CHECK(s.binds[2].display == "'AAAAABAAAAAAAAAAAA'");
		range.hi_inclusive = true;
		CHECK(BuildScanSql(input, range).sql.find("WHERE (\"ID\" = :b1) AND ROWID BETWEEN CHARTOROWID(:b2) AND "
		                                          "CHARTOROWID(:b3)") != std::string::npos);
	}
	SECTION("slice only, filters renumbered") {
		input.snapshot_scn.clear();
		input.filters = {Fragment("(\"ID\" = :b1)", 1), Fragment("(\"ID\" <> :b1 AND \"ID\" < :b2)", 2)};
		input.filters[1].binds.push_back(BindNumber(3));
		const auto s = BuildScanSql(input, Slice());
		CHECK(s.sql.find("WHERE (\"ID\" = :b1) AND (\"ID\" <> :b2 AND \"ID\" < :b3)") != std::string::npos);
		CHECK(s.binds.size() == 3);
	}
}
