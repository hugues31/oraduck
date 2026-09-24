#include "catch.hpp"
#include "oraduck/describe_target.hpp"
#include "itest_support.hpp"

using namespace oraduck;

TEST_CASE("Connection and queries", "[oracle]") {
	OciSession session(TestCredentials());
	CHECK(Scalar(session, "SELECT 1 + 1 FROM dual") == "2");
	CHECK(session.QueryStrings("SELECT :1 || '-' || :2 FROM dual", {"a", "b"}).at(0).at(0) == "a-b");
	CHECK(Scalar(session, "SELECT NULL FROM dual").empty());
}

TEST_CASE("Wrong password", "[oracle]") {
	auto credentials = TestCredentials();
	credentials.password = "wrong";
	try {
		OciSession session(credentials);
		FAIL("unexpected successful connection");
	} catch (const OracleError &e) {
		CHECK(e.ora_code == 1017);
	}
}

TEST_CASE("Table description", "[oracle]") {
	OciSession session(TestCredentials());
	DropTableIfExists(session, "ITEST_DESC");
	session.Execute("CREATE TABLE ITEST_DESC (ID NUMBER(10) NOT NULL, MONTANT NUMBER(15,2), LIBELLE VARCHAR2(30 CHAR), "
	                "D DATE, TS TIMESTAMP(6), BD BINARY_DOUBLE, TZ TIMESTAMP WITH TIME ZONE)");

	const auto d = DescribeTarget(session, ParseTargetName("itest_desc"));
	CHECK(d.owner == "BENCH");
	CHECK(d.table == "ITEST_DESC");
	REQUIRE(d.columns.size() == 7);
	CHECK(d.columns[0].name == "ID");
	CHECK(d.columns[0].family == OracleTypeFamily::kNumber);
	CHECK(d.columns[0].precision == 10);
	CHECK(d.columns[0].scale == 0);
	CHECK_FALSE(d.columns[0].nullable);
	CHECK(d.columns[1].precision == 15);
	CHECK(d.columns[1].scale == 2);
	CHECK(d.columns[2].family == OracleTypeFamily::kVarchar);
	CHECK(d.columns[2].data_length == 120);
	CHECK(d.columns[3].family == OracleTypeFamily::kDate);
	CHECK(d.columns[4].family == OracleTypeFamily::kTimestamp);
	CHECK(d.columns[5].family == OracleTypeFamily::kBinaryDouble);
	CHECK(d.columns[6].family == OracleTypeFamily::kUnsupported);
	CHECK(d.index_count == 0);

	session.Execute("CREATE INDEX ITEST_DESC_IX ON ITEST_DESC (ID)");
	CHECK(DescribeTarget(session, ParseTargetName("BENCH.ITEST_DESC")).index_count == 1);
	DropTableIfExists(session, "ITEST_DESC");
}

TEST_CASE("Missing table", "[oracle]") {
	OciSession session(TestCredentials());
	CHECK_THROWS_WITH(DescribeTarget(session, ParseTargetName("N_EXISTE_PAS")), Catch::Contains("not found"));
}
