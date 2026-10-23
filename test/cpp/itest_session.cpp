#include "catch.hpp"
#include "oraduck/describe_target.hpp"
#include "oraduck/rowid_ranges.hpp"
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

TEST_CASE("The fetch functions are loaded", "[session]") {
	CHECK(Oci().OCIDefineByPos2 != nullptr);
	CHECK(Oci().OCIDefineDynamic != nullptr);
	CHECK(Oci().OCIBreak != nullptr);
	CHECK(Oci().OCIReset != nullptr);
}

TEST_CASE("The test database has the read-path privileges", "[session]") {
	OciSession session(TestCredentials());
	CHECK(Scalar(session, "SELECT COUNT(*) FROM v$mystat WHERE ROWNUM = 1") == "1");
	CHECK(Scalar(session, "SELECT COUNT(*) FROM user_ts_quotas WHERE tablespace_name = 'BENCH_SMALL'") == "1");
	OciSession reader(ReaderCredentials());
	CHECK_THROWS_WITH(reader.QueryStrings("SELECT COUNT(*) FROM dba_extents WHERE ROWNUM = 1"),
	                  Catch::Contains("ORA-00942"));
}

TEST_CASE("EncodeRowid matches Oracle", "[oracle][rowid]") {
	OciSession session(TestCredentials());
	const uint32_t objects[] = {1, 73512, 4294967295u};
	const uint32_t files[] = {1, 4, 1023};
	const uint32_t blocks[] = {0, 1, 12345, 0x3FFFFF};
	const uint16_t rows[] = {0, 7, 4095};
	for (auto o : objects) {
		for (auto f : files) {
			for (auto b : blocks) {
				for (auto r : rows) {
					const auto expected = Scalar(session, "SELECT ROWIDTOCHAR(DBMS_ROWID.ROWID_CREATE(1, " + std::to_string(o) +
					                                          ", " + std::to_string(f) + ", " + std::to_string(b) +
					                                          ", " + std::to_string(r) + ")) FROM dual");
					CHECK(EncodeRowid(o, f, b, r) == expected);
				}
			}
		}
	}

	// Bigfile tablespace: one file, the block number spans 32 bits
	DropTableIfExists(session, "ITEST_ROWID");
	session.Execute("CREATE TABLE ITEST_ROWID (ID NUMBER(5)) TABLESPACE BENCH");
	session.Execute("INSERT INTO ITEST_ROWID VALUES (1)");
	const auto row = session.QueryStrings("SELECT ROWIDTOCHAR(ROWID), DBMS_ROWID.ROWID_OBJECT(ROWID), "
	                                      "DBMS_ROWID.ROWID_BLOCK_NUMBER(ROWID, 'BIGFILE'), "
	                                      "DBMS_ROWID.ROWID_ROW_NUMBER(ROWID) FROM ITEST_ROWID")
	                 .at(0);
	CHECK(EncodeRowid(static_cast<uint32_t>(std::stoul(row[1])), 1024, static_cast<uint32_t>(std::stoul(row[2])),
	                  static_cast<uint16_t>(std::stoul(row[3]))) == row[0]);
	DropTableIfExists(session, "ITEST_ROWID");
}
