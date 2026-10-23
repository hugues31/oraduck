#include "catch.hpp"
#include "oraduck/filter_sql.hpp"
#include "oraduck/oracle_decoding.hpp"
#include "oraduck/oracle_encoding.hpp"

#include <cmath>
#include <limits>

using namespace oraduck;

namespace {

using Kind = FilterConstant::Kind;

OracleColumn Col(const std::string &name, const std::string &type, int p = -1, int s = -1, bool national = false,
                 const std::string &collation = "") {
	OracleColumn c;
	c.collation = collation;
	c.name = name;
	c.data_type = type;
	c.precision = p;
	c.scale = s;
	c.national = national;
	return c;
}

// 0 PART_DATE DATE, 1 SUB_KEY VARCHAR2, 2 ID NUMBER(38,0), 3 AMOUNT NUMBER, 4 FLAG CHAR, 5 NOTE CLOB,
// 6 NAME NVARCHAR2, 7 TS TIMESTAMP(6), 8 TSZ TIMESTAMP(6) WITH TIME ZONE, 9 BD BINARY_DOUBLE,
// 10 PRICE NUMBER(20,3), 11 TS9 TIMESTAMP(9), 12 NCH NCHAR, 13 TSL TIMESTAMP(6) WITH LOCAL TIME ZONE, 14 RAWC RAW
const std::vector<OracleColumn> &Columns() {
	static const std::vector<OracleColumn> columns = {
	    Col("PART_DATE", "DATE"),
	    Col("SUB_KEY", "VARCHAR2"),
	    Col("ID", "NUMBER", 38, 0),
	    Col("AMOUNT", "NUMBER"),
	    Col("FLAG", "CHAR"),
	    Col("NOTE", "CLOB"),
	    Col("NAME", "NVARCHAR2", -1, -1, true),
	    Col("TS", "TIMESTAMP(6)"),
	    Col("TSZ", "TIMESTAMP(6) WITH TIME ZONE"),
	    Col("BD", "BINARY_DOUBLE"),
	    Col("PRICE", "NUMBER", 20, 3),
	    Col("TS9", "TIMESTAMP(9)"),
	    Col("NCH", "NCHAR", -1, -1, true),
	    Col("TSL", "TIMESTAMP(6) WITH LOCAL TIME ZONE"),
	    Col("RAWC", "RAW"),
	    Col("BF", "BINARY_FLOAT"),
	    Col("STAR0", "NUMBER", -1, 0),
	    Col("NEGS", "NUMBER", 5, -2),
	    Col("HUGES", "NUMBER", 10, 40),
	    Col("CI", "VARCHAR2", -1, -1, false, "BINARY_CI"),
	    Col("NLSC", "VARCHAR2", -1, -1, false, "USING_NLS_COMP"),
	    Col("BIN", "VARCHAR2", -1, -1, false, "BINARY"),
	};
	return columns;
}

int64_t Days(int y, unsigned m, unsigned d) {
	return DaysFromCivil(y, m, d);
}

FilterConstant TimestampMicros(int64_t micros) {
	FilterConstant k;
	k.kind = Kind::kTimestamp;
	k.i64 = micros;
	return k;
}

FilterConstant Timestamp(int y, unsigned m, unsigned d) {
	return TimestampMicros(Days(y, m, d) * kMicrosPerDay);
}

FilterConstant Date(int y, unsigned m, unsigned d) {
	FilterConstant k;
	k.kind = Kind::kDate;
	k.i64 = Days(y, m, d);
	return k;
}

FilterConstant Text(const std::string &s) {
	FilterConstant k;
	k.kind = Kind::kString;
	k.text = s;
	return k;
}

FilterConstant Integer(int64_t v) {
	FilterConstant k;
	k.kind = Kind::kInteger;
	k.i64 = v;
	return k;
}

FilterConstant Double(double v) {
	FilterConstant k;
	k.kind = Kind::kDouble;
	k.f64 = v;
	return k;
}

FilterConstant Decimal(int64_t unscaled, uint8_t scale) {
	FilterConstant k;
	k.kind = Kind::kDecimal;
	k.i128 = {static_cast<uint64_t>(unscaled), unscaled < 0 ? -1 : 0};
	k.scale = scale;
	return k;
}

FilterNode Leaf(FilterOp op, size_t column, std::vector<FilterConstant> constants = {}) {
	FilterNode n;
	n.op = op;
	n.column = column;
	n.constants = std::move(constants);
	return n;
}

FilterNode Group(FilterOp op, std::vector<FilterNode> children) {
	FilterNode n;
	n.op = op;
	n.children = std::move(children);
	return n;
}

std::optional<SqlFragment> Translate(const FilterNode &n, const char *charset = "AL32UTF8") {
	return TranslateFilter(n, Columns(), charset);
}

} // namespace

TEST_CASE("DATE equality binds a DATE", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kEq, 0, {Timestamp(2024, 5, 7)}));
	REQUIRE(f);
	CHECK(f->sql == "(\"PART_DATE\" = :b1)");
	REQUIRE(f->binds.size() == 1);
	CHECK(f->binds[0].dty == SQLT_DAT);
	CHECK(f->binds[0].bytes.size() == 7);
	CHECK(f->display == "(\"PART_DATE\" = DATE '2024-05-07')");
	// a DATE constant works too; a time of day is displayed in full
	CHECK(Translate(Leaf(FilterOp::kGe, 0, {Date(2024, 5, 7)}))->sql == "(\"PART_DATE\" >= :b1)");
	f = Translate(Leaf(FilterOp::kLt, 0, {TimestampMicros(Days(2024, 5, 7) * kMicrosPerDay + 3661 * kMicrosPerSecond)}));
	REQUIRE(f);
	CHECK(f->display == "(\"PART_DATE\" < TO_DATE('2024-05-07 01:01:01', 'YYYY-MM-DD HH24:MI:SS'))");
}

TEST_CASE("DATE constant with a fractional second is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 0, {TimestampMicros(1715077230000001)})));
}

TEST_CASE("DATE out of the Oracle range is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 0, {Timestamp(10000, 1, 1)})));
}

TEST_CASE("kDateEq becomes a sargable range", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kDateEq, 0, {Date(2024, 5, 7)}));
	REQUIRE(f);
	CHECK(f->sql == "(\"PART_DATE\" >= :b1 AND \"PART_DATE\" < :b2 + 1)");
	REQUIRE(f->binds.size() == 2);
	CHECK(f->binds[0].dty == SQLT_DAT);
	CHECK(f->binds[0].bytes == f->binds[1].bytes);
	CHECK(f->display == "(\"PART_DATE\" >= DATE '2024-05-07' AND \"PART_DATE\" < DATE '2024-05-07' + 1)");
	// only on DATE columns, with a date constant
	CHECK_FALSE(Translate(Leaf(FilterOp::kDateEq, 7, {Date(2024, 5, 7)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kDateEq, 0, {Timestamp(2024, 5, 7)})));
}

TEST_CASE("TIMESTAMP up to 6 digits binds a text through TO_TIMESTAMP", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kEq, 7, {TimestampMicros(Days(2024, 5, 7) * kMicrosPerDay + 123456)}));
	REQUIRE(f);
	CHECK(f->sql == "(\"TS\" = TO_TIMESTAMP(:b1, 'YYYY-MM-DD HH24:MI:SS.FF6'))");
	REQUIRE(f->binds.size() == 1);
	CHECK(f->binds[0].dty == SQLT_CHR);
	CHECK(std::string(f->binds[0].bytes.begin(), f->binds[0].bytes.end()) == "2024-05-07 00:00:00.123456");
	CHECK(f->display == "(\"TS\" = TIMESTAMP '2024-05-07 00:00:00.123456')");
}

TEST_CASE("TIMESTAMP above 6 digits is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 11, {Timestamp(2024, 5, 7)})));
}

TEST_CASE("TIMESTAMP WITH (LOCAL) TIME ZONE is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 8, {Timestamp(2024, 5, 7)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 13, {Timestamp(2024, 5, 7)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kIsNull, 8)));
}

TEST_CASE("VARCHAR2 IN list and ranges", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kIn, 1, {Text("A"), Text("B")}));
	REQUIRE(f);
	CHECK(f->sql == "(\"SUB_KEY\" IN (:b1, :b2))");
	CHECK(f->display == "(\"SUB_KEY\" IN ('A', 'B'))");
	CHECK(f->binds[0].dty == SQLT_CHR);
	CHECK(Translate(Leaf(FilterOp::kGt, 1, {Text("A")})));
	CHECK(Translate(Leaf(FilterOp::kEq, 1, {Text("it's")}))->display == "(\"SUB_KEY\" = 'it''s')");
}

TEST_CASE("String ranges need a UTF-8 database and a non-national column", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kGt, 1, {Text("A")}), "WE8MSWIN1252"));
	CHECK_FALSE(Translate(Leaf(FilterOp::kGt, 6, {Text("a")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kLe, 12, {Text("a")})));
	// equality stays pushable on a national column and on a single-byte database (ASCII only)
	CHECK(Translate(Leaf(FilterOp::kEq, 6, {Text("a")})));
	CHECK(Translate(Leaf(FilterOp::kEq, 1, {Text("a")}), "WE8MSWIN1252"));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 1, {Text("\xC3\xA9")}), "WE8MSWIN1252"));
	CHECK(Translate(Leaf(FilterOp::kEq, 1, {Text("\xC3\xA9")})));
}

TEST_CASE("The empty string is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kNe, 1, {Text("")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 1, {Text("")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kIn, 1, {Text("A"), Text("")})));
}

TEST_CASE("CHAR comparisons use a character bind", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kEq, 4, {Text("Y")}));
	REQUIRE(f);
	CHECK(f->binds[0].dty == SQLT_CHR);
}

TEST_CASE("NUMBER with precision binds a NUMBER", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kEq, 2, {Integer(20240507)}));
	REQUIRE(f);
	CHECK(f->sql == "(\"ID\" = :b1)");
	CHECK(f->binds[0].dty == SQLT_NUM);
	CHECK(f->display == "(\"ID\" = 20240507)");
	f = Translate(Leaf(FilterOp::kGe, 10, {Decimal(-12345, 3)}));
	REQUIRE(f);
	CHECK(f->binds[0].dty == SQLT_NUM);
	CHECK(f->display == "(\"PRICE\" >= -12.345)");
	f = Translate(Leaf(FilterOp::kEq, 2, {Integer(std::numeric_limits<int64_t>::min())}));
	REQUIRE(f);
	CHECK(f->display == "(\"ID\" = -9223372036854775808)");
}

TEST_CASE("NUMBER without precision is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 3, {Integer(1)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 3, {Double(1.5)})));
}

TEST_CASE("BINARY_DOUBLE binds a double, never NaN or infinity", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kLt, 9, {Double(1.5)}));
	REQUIRE(f);
	CHECK(f->binds[0].dty == SQLT_BDOUBLE);
	CHECK(f->binds[0].bytes.size() == 8);
	CHECK_FALSE(Translate(Leaf(FilterOp::kLt, 9, {Double(std::nan(""))})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kLt, 9, {Double(INFINITY)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kLt, 9, {Integer(1)})));
}

TEST_CASE("A constant of another kind than the column is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 1, {Integer(1)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 2, {Text("1")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 0, {Text("2024-05-07")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 2, {Double(1)})));
}

TEST_CASE("IN with a constant that is not pushable is not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kIn, 2, {Integer(1), Text("x")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kIn, 2, {})));
}

TEST_CASE("LOB columns only accept IS NULL and IS NOT NULL", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 5, {Text("x")})));
	auto f = Translate(Leaf(FilterOp::kIsNull, 5));
	REQUIRE(f);
	CHECK(f->sql == "(\"NOTE\" IS NULL)");
	CHECK(f->binds.empty());
	CHECK(Translate(Leaf(FilterOp::kIsNotNull, 5))->sql == "(\"NOTE\" IS NOT NULL)");
}

TEST_CASE("Unsupported column types and bad columns are not pushed", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 14, {Text("x")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kIsNull, 14)));
	CHECK_FALSE(Translate(Leaf(FilterOp::kIsNull, 99)));
}

TEST_CASE("AND and OR", "[filter_sql]") {
	auto f = Translate(Group(FilterOp::kAnd, {Leaf(FilterOp::kEq, 2, {Integer(1)}), Leaf(FilterOp::kEq, 1, {Text("A")})}));
	REQUIRE(f);
	CHECK(f->sql == "((\"ID\" = :b1) AND (\"SUB_KEY\" = :b2))");
	CHECK(f->binds.size() == 2);
	CHECK(f->display == "((\"ID\" = 1) AND (\"SUB_KEY\" = 'A'))");
	f = Translate(Group(FilterOp::kOr, {Leaf(FilterOp::kEq, 2, {Integer(1)}), Leaf(FilterOp::kIsNull, 1)}));
	REQUIRE(f);
	CHECK(f->sql == "((\"ID\" = :b1) OR (\"SUB_KEY\" IS NULL))");
	// one child not pushable: the whole node is not
	CHECK_FALSE(Translate(Group(FilterOp::kOr, {Leaf(FilterOp::kEq, 2, {Integer(1)}), Leaf(FilterOp::kEq, 3, {Integer(1)})})));
	CHECK_FALSE(Translate(Group(FilterOp::kAnd, {Leaf(FilterOp::kEq, 2, {Integer(1)}), Leaf(FilterOp::kEq, 3, {Integer(1)})})));
	CHECK_FALSE(Translate(Group(FilterOp::kAnd, {})));
}

TEST_CASE("Partition key filters are pushed", "[filter_sql]") {
	CHECK(Translate(Leaf(FilterOp::kEq, 0, {Timestamp(2024, 5, 7)})));
	CHECK(Translate(Leaf(FilterOp::kIn, 1, {Text("A"), Text("B")})));
	CHECK(Translate(Leaf(FilterOp::kEq, 2, {Integer(7)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 1, {Integer(1)})));
}

TEST_CASE("String ranges are pushed on AL32UTF8 only, not on UTF8 (CESU-8)", "[filter_sql]") {
	CHECK(Translate(Leaf(FilterOp::kGt, 1, {Text("A")}), "AL32UTF8"));
	CHECK_FALSE(Translate(Leaf(FilterOp::kGt, 1, {Text("A")}), "UTF8"));
	CHECK(Translate(Leaf(FilterOp::kEq, 1, {Text("\xF0\x9F\x98\x80")}), "UTF8"));
	CHECK(Translate(Leaf(FilterOp::kIn, 1, {Text("A")}), "UTF8"));
}

TEST_CASE("IN lists above 1000 constants are not pushed", "[filter_sql]") {
	std::vector<FilterConstant> constants;
	for (int i = 0; i < 1000; i++) {
		constants.push_back(Integer(i));
	}
	auto f = Translate(Leaf(FilterOp::kIn, 2, constants));
	REQUIRE(f);
	CHECK(f->binds.size() == 1000);
	constants.push_back(Integer(1000));
	CHECK_FALSE(Translate(Leaf(FilterOp::kIn, 2, constants)));
}

TEST_CASE("NUMBER pushability follows the read mapping", "[filter_sql]") {
	CHECK(Translate(Leaf(FilterOp::kEq, 16, {Integer(1)})));  // NUMBER(*,0)
	CHECK(Translate(Leaf(FilterOp::kEq, 17, {Integer(100)}))); // NUMBER(5,-2)
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 18, {Integer(1)}))); // NUMBER(10,40) is read as DOUBLE
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 3, {Integer(1)})));  // unconstrained
	CHECK(Translate(Leaf(FilterOp::kIsNull, 18)));
}

TEST_CASE("Column collation other than binary blocks string comparisons", "[filter_sql]") {
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 19, {Text("a")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kIn, 19, {Text("a")})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kGt, 19, {Text("a")})));
	CHECK(Translate(Leaf(FilterOp::kIsNull, 19)));
	CHECK(Translate(Leaf(FilterOp::kEq, 20, {Text("a")})));
	CHECK(Translate(Leaf(FilterOp::kGt, 20, {Text("a")})));
	CHECK(Translate(Leaf(FilterOp::kEq, 21, {Text("a")})));
}

TEST_CASE("kDateEq is not pushed at 9999-12-31", "[filter_sql]") {
	CHECK(Translate(Leaf(FilterOp::kDateEq, 0, {Date(9999, 12, 30)})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kDateEq, 0, {Date(9999, 12, 31)})));
}

TEST_CASE("Text constants above 4000 bytes are not pushed", "[filter_sql]") {
	CHECK(Translate(Leaf(FilterOp::kEq, 1, {Text(std::string(4000, 'a'))})));
	CHECK_FALSE(Translate(Leaf(FilterOp::kEq, 1, {Text(std::string(4001, 'a'))})));
}

TEST_CASE("BINARY_FLOAT and NCHAR columns", "[filter_sql]") {
	auto f = Translate(Leaf(FilterOp::kGe, 15, {Double(0.5)}));
	REQUIRE(f);
	CHECK(f->binds[0].dty == SQLT_BDOUBLE);
	CHECK_FALSE(Translate(Leaf(FilterOp::kGe, 15, {Double(INFINITY)})));
	f = Translate(Leaf(FilterOp::kEq, 12, {Text("x")}));
	REQUIRE(f);
	CHECK(f->sql == "(\"NCH\" = :b1)");
}
