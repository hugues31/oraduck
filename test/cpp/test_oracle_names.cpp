#include "catch.hpp"
#include "oraduck/errors.hpp"
#include "oraduck/oracle_names.hpp"

using namespace oraduck;

TEST_CASE("ParseTargetName", "[names]") {
	auto t = ParseTargetName("MON_SCHEMA.MA_TABLE");
	CHECK(t.owner == "MON_SCHEMA");
	CHECK(t.table == "MA_TABLE");

	t = ParseTargetName("ma_table");
	CHECK(t.owner.empty());
	CHECK(t.table == "MA_TABLE");

	t = ParseTargetName("bench.t$1#");
	CHECK(t.owner == "BENCH");
	CHECK(t.table == "T$1#");

	t = ParseTargetName("\"MonSchema\".\"Ma.Table\"");
	CHECK(t.owner == "MonSchema");
	CHECK(t.table == "Ma.Table");

	t = ParseTargetName("  BENCH.T  ");
	CHECK(t.owner == "BENCH");
	CHECK(t.table == "T");

	CHECK_THROWS_AS(ParseTargetName(""), OraduckError);
	CHECK_THROWS_AS(ParseTargetName("a.b.c"), OraduckError);
	CHECK_THROWS_AS(ParseTargetName("1abc"), OraduckError);
	CHECK_THROWS_AS(ParseTargetName("\"\""), OraduckError);
	CHECK_THROWS_AS(ParseTargetName("\"abc"), OraduckError);
	CHECK_THROWS_AS(ParseTargetName("a b"), OraduckError);
	CHECK_THROWS_AS(ParseTargetName("schema."), OraduckError);
}

TEST_CASE("ClassifyOracleType", "[names]") {
	CHECK(ClassifyOracleType("NUMBER") == OracleTypeFamily::kNumber);
	CHECK(ClassifyOracleType("FLOAT") == OracleTypeFamily::kNumber);
	CHECK(ClassifyOracleType("BINARY_FLOAT") == OracleTypeFamily::kBinaryFloat);
	CHECK(ClassifyOracleType("BINARY_DOUBLE") == OracleTypeFamily::kBinaryDouble);
	CHECK(ClassifyOracleType("VARCHAR2") == OracleTypeFamily::kVarchar);
	CHECK(ClassifyOracleType("CHAR") == OracleTypeFamily::kVarchar);
	CHECK(ClassifyOracleType("DATE") == OracleTypeFamily::kDate);
	CHECK(ClassifyOracleType("TIMESTAMP(6)") == OracleTypeFamily::kTimestamp);
	CHECK(ClassifyOracleType("TIMESTAMP(6) WITH TIME ZONE") == OracleTypeFamily::kUnsupported);
	CHECK(ClassifyOracleType("TIMESTAMP(6) WITH LOCAL TIME ZONE") == OracleTypeFamily::kUnsupported);
	CHECK(ClassifyOracleType("NVARCHAR2") == OracleTypeFamily::kUnsupported);
	CHECK(ClassifyOracleType("CLOB") == OracleTypeFamily::kUnsupported);
}
