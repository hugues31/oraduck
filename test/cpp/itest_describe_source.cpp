#include "catch.hpp"
#include "oraduck/describe_source.hpp"
#include "itest_support.hpp"

using namespace oraduck;

namespace {

void DropIfExists(OciSession &session, const std::string &kind, const std::string &name) {
	session.Execute("BEGIN EXECUTE IMMEDIATE 'DROP " + kind + " " + name +
	                "'; EXCEPTION WHEN OTHERS THEN NULL; END;");
}

void CleanUp(OciSession &session) {
	DropIfExists(session, "PUBLIC SYNONYM", "ITEST_SRC_PUB");
	DropIfExists(session, "SYNONYM", "ITEST_SRC_SYN");
	DropIfExists(session, "SYNONYM", "ITEST_SRC_LOOP_A");
	DropIfExists(session, "SYNONYM", "ITEST_SRC_LOOP_B");
	DropIfExists(session, "VIEW", "ITEST_SRC_V");
	DropTableIfExists(session, "ITEST_SRC_P");
	DropTableIfExists(session, "ITEST_SRC");
}

} // namespace

TEST_CASE("Source description: column types", "[oracle][describe_source]") {
	OciSession session(TestCredentials());
	CleanUp(session);
	session.Execute("CREATE TABLE ITEST_SRC (N NUMBER, N0 NUMBER(*,0), N259 NUMBER(25,9), V VARCHAR2(10 CHAR), "
	                "NV NVARCHAR2(5), C CHAR(3), D DATE, TS TIMESTAMP(6), TZ TIMESTAMP WITH TIME ZONE, "
	                "BF BINARY_FLOAT, BD BINARY_DOUBLE, RW RAW(8), "
	                "ID NUMBER(10) NOT NULL)");
	const auto d = DescribeSource(session, ParseTargetName("itest_src"));
	CHECK(d.owner == "BENCH");
	CHECK(d.name == "ITEST_SRC");
	CHECK(d.kind == SourceKind::kTable);
	CHECK_FALSE(d.partitioned);
	CHECK(d.owned_by_session_user);
	CHECK(d.db_charset == "AL32UTF8");
	CHECK(d.nchar_charset == "AL16UTF16");
	REQUIRE(d.columns.size() == 13);
	CHECK(d.columns[0].data_type == "NUMBER");
	CHECK(d.columns[0].precision == -1);
	CHECK(d.columns[0].scale == -1);
	CHECK(d.columns[1].precision == -1);
	CHECK(d.columns[1].scale == 0);
	CHECK(d.columns[2].precision == 25);
	CHECK(d.columns[2].scale == 9);
	CHECK(d.columns[3].char_length == 10);
	CHECK(d.columns[3].data_length == 40);
	CHECK_FALSE(d.columns[3].national);
	CHECK(d.columns[4].data_type == "NVARCHAR2");
	CHECK(d.columns[4].national);
	CHECK(d.columns[4].char_length == 5);
	CHECK(d.columns[5].data_type == "CHAR");
	CHECK(d.columns[7].data_type == "TIMESTAMP(6)");
	CHECK(d.columns[8].data_type == "TIMESTAMP(6) WITH TIME ZONE");
	CHECK(d.columns[9].data_type == "BINARY_FLOAT");
	CHECK(d.columns[11].data_type == "RAW");
	// COLLATE is refused with MAX_STRING_SIZE=STANDARD (ORA-43929, so BINARY_CI cannot be tested); 19c reports
	// USING_NLS_COMP for character columns and NULL otherwise
	CHECK(d.columns[3].collation == "USING_NLS_COMP");
	CHECK(d.columns[0].collation.empty());
	CHECK_FALSE(d.columns[12].nullable);
	CleanUp(session);
}

TEST_CASE("Source description: partitioned table", "[oracle][describe_source]") {
	OciSession session(TestCredentials());
	CleanUp(session);
	session.Execute("CREATE TABLE ITEST_SRC_P (ID NUMBER, K VARCHAR2(5)) PARTITION BY LIST (K) "
	                "(PARTITION P1 VALUES ('A'), PARTITION P2 VALUES ('B'))");
	session.Execute("INSERT INTO ITEST_SRC_P VALUES (1, 'A')");
	session.Execute("INSERT INTO ITEST_SRC_P VALUES (2, 'B')");
	session.Execute("INSERT INTO ITEST_SRC_P VALUES (3, 'B')");
	session.Execute("BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, 'ITEST_SRC_P'); END;");
	const auto d = DescribeSource(session, ParseTargetName("ITEST_SRC_P"));
	CHECK(d.partitioned);
	CHECK(d.num_rows == 3);
	CleanUp(session);
}

TEST_CASE("Source description: view and synonyms", "[oracle][describe_source]") {
	OciSession session(TestCredentials());
	CleanUp(session);
	session.Execute("CREATE TABLE ITEST_SRC (ID NUMBER, V VARCHAR2(5))");
	session.Execute("CREATE VIEW ITEST_SRC_V AS SELECT ID FROM ITEST_SRC");
	session.Execute("CREATE SYNONYM ITEST_SRC_SYN FOR ITEST_SRC");
	session.Execute("CREATE PUBLIC SYNONYM ITEST_SRC_PUB FOR BENCH.ITEST_SRC");

	const auto view = DescribeSource(session, ParseTargetName("ITEST_SRC_V"));
	CHECK(view.kind == SourceKind::kView);
	CHECK(view.columns.size() == 1);
	CHECK(view.num_rows == -1);
	CHECK_FALSE(view.partitioned);

	const auto priv = DescribeSource(session, ParseTargetName("ITEST_SRC_SYN"));
	CHECK(priv.kind == SourceKind::kTable);
	CHECK(priv.owner == "BENCH");
	CHECK(priv.name == "ITEST_SRC");
	CHECK(priv.columns.size() == 2);

	const auto pub = DescribeSource(session, ParseTargetName("ITEST_SRC_PUB"));
	CHECK(pub.name == "ITEST_SRC");
	CHECK(pub.columns.size() == 2);
	CleanUp(session);
}

TEST_CASE("Source description: errors", "[oracle][describe_source]") {
	OciSession session(TestCredentials());
	CleanUp(session);
	session.Execute("CREATE SYNONYM ITEST_SRC_LOOP_A FOR ITEST_SRC_LOOP_B");
	session.Execute("CREATE SYNONYM ITEST_SRC_LOOP_B FOR ITEST_SRC_LOOP_A");
	CHECK_THROWS_WITH(DescribeSource(session, ParseTargetName("ITEST_SRC_LOOP_A")), Catch::Contains("synonym"));
	CHECK_THROWS_WITH(DescribeSource(session, ParseTargetName("N_EXISTE_PAS")),
	                  "Oracle table or view not found or not accessible: BENCH.N_EXISTE_PAS");
	CleanUp(session);
}

TEST_CASE("Read session settings", "[oracle][describe_source]") {
	OciSession session(TestCredentials());
	session.Execute("ALTER SESSION SET NLS_CALENDAR = 'Thai Buddha'");
	PrepareReadSession(session);
	CHECK(Scalar(session, "SELECT value FROM nls_session_parameters WHERE parameter = 'NLS_COMP'") == "BINARY");
	CHECK(Scalar(session, "SELECT value FROM nls_session_parameters WHERE parameter = 'NLS_SORT'") == "BINARY");
	CHECK(Scalar(session, "SELECT value FROM nls_session_parameters WHERE parameter = 'NLS_CALENDAR'") ==
	      "GREGORIAN");
	// The text of a TIMESTAMP filter constant reads as a Gregorian date
	CHECK(Scalar(session, "SELECT TO_CHAR(TO_TIMESTAMP('2024-05-07 10:20:30.123456', 'YYYY-MM-DD HH24:MI:SS.FF6') "
	                      "- TIMESTAMP '2024-05-07 00:00:00') FROM dual") == "+000000000 10:20:30.123456000");
}
