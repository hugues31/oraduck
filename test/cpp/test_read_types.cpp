#include "catch.hpp"
#include "oraduck/errors.hpp"
#include "oraduck/read_types.hpp"

using namespace oraduck;

namespace {

OracleColumn Col(const std::string &type, int p = -1, int s = -1, uint32_t len = 0) {
	OracleColumn c;
	c.name = "C";
	c.data_type = type;
	c.precision = p;
	c.scale = s;
	c.data_length = len;
	return c;
}

ReadColumnPlan Plan(const OracleColumn &c, UnconstrainedNumber u = UnconstrainedNumber(),
                    const std::string &cs = "AL32UTF8") {
	return PlanReadColumn(c, u, cs);
}

} // namespace

TEST_CASE("PlanReadColumn NUMBER", "[read_types]") {
	SECTION("p 1..4 s 0 is SMALLINT") {
		auto p = Plan(Col("NUMBER", 4, 0));
		CHECK(p.duck_type.id == DuckTypeId::kSmallint);
		CHECK(p.decoder == FetchDecoder::kNativeInt64);
		CHECK(p.dty == SQLT_INT);
		CHECK(p.define_bytes == 8);
		CHECK(p.select_expr == "\"C\"");
	}
	SECTION("p 5..9 s 0 is INTEGER") {
		auto p = Plan(Col("NUMBER", 9, 0));
		CHECK(p.duck_type.id == DuckTypeId::kInteger);
		CHECK(p.decoder == FetchDecoder::kNativeInt64);
	}
	SECTION("p 10..18 s 0 is BIGINT") {
		auto p = Plan(Col("NUMBER", 18, 0));
		CHECK(p.duck_type.id == DuckTypeId::kBigint);
		CHECK(p.dty == SQLT_INT);
	}
	SECTION("p <= 18 s > 0 is DECIMAL64") {
		auto p = Plan(Col("NUMBER", 10, 2));
		CHECK(p.duck_type.id == DuckTypeId::kDecimal);
		CHECK(p.duck_type.width == 10);
		CHECK(p.duck_type.scale == 2);
		CHECK(p.decoder == FetchDecoder::kNumberDecimal64);
		CHECK(p.dty == SQLT_NUM);
		CHECK(p.define_bytes == 22);
		CHECK(p.scale == 2);
		CHECK_FALSE(p.round);
	}
	SECTION("p 19..38 is DECIMAL128") {
		auto p = Plan(Col("NUMBER", 38, 10));
		CHECK(p.duck_type.width == 38);
		CHECK(p.duck_type.scale == 10);
		CHECK(p.decoder == FetchDecoder::kNumberDecimal128);
		p = Plan(Col("NUMBER", 20, 0));
		CHECK(p.duck_type.id == DuckTypeId::kDecimal);
		CHECK(p.decoder == FetchDecoder::kNumberDecimal128);
	}
	SECTION("negative scale") {
		auto p = Plan(Col("NUMBER", 5, -2));
		CHECK(p.duck_type.id == DuckTypeId::kDecimal);
		CHECK(p.duck_type.width == 7);
		CHECK(p.duck_type.scale == 0);
		CHECK(p.decoder == FetchDecoder::kNumberDecimal64);
		p = Plan(Col("NUMBER", 38, -2));
		CHECK(p.duck_type.id == DuckTypeId::kDouble);
		CHECK(p.decoder == FetchDecoder::kNativeDouble);
	}
	SECTION("s > p is DECIMAL(s,s)") {
		auto p = Plan(Col("NUMBER", 3, 5));
		CHECK(p.duck_type.width == 5);
		CHECK(p.duck_type.scale == 5);
		CHECK(p.decoder == FetchDecoder::kNumberDecimal64);
	}
	SECTION("NUMBER(*,0) is DECIMAL(38,0)") {
		auto p = Plan(Col("NUMBER", -1, 0));
		CHECK(p.duck_type.id == DuckTypeId::kDecimal);
		CHECK(p.duck_type.width == 38);
		CHECK(p.duck_type.scale == 0);
		CHECK(p.decoder == FetchDecoder::kNumberDecimal128);
	}
	SECTION("unconstrained NUMBER and FLOAT follow the setting") {
		auto p = Plan(Col("NUMBER"));
		CHECK(p.duck_type.id == DuckTypeId::kDouble);
		CHECK(p.decoder == FetchDecoder::kNativeDouble);
		CHECK(p.dty == SQLT_BDOUBLE);
		CHECK(p.define_bytes == 8);
		UnconstrainedNumber v;
		v.kind = UnconstrainedNumber::Kind::kVarchar;
		p = Plan(Col("FLOAT", 126), v);
		CHECK(p.duck_type.id == DuckTypeId::kVarchar);
		CHECK(p.decoder == FetchDecoder::kNumberText);
		CHECK(p.dty == SQLT_NUM);
		CHECK(p.define_bytes == 22);
		UnconstrainedNumber d;
		d.kind = UnconstrainedNumber::Kind::kDecimal;
		d.width = 20;
		d.scale = 4;
		p = Plan(Col("NUMBER"), d);
		CHECK(p.duck_type.id == DuckTypeId::kDecimal);
		CHECK(p.duck_type.width == 20);
		CHECK(p.duck_type.scale == 4);
		CHECK(p.decoder == FetchDecoder::kNumberDecimal128);
		CHECK(p.round);
		CHECK(p.scale == 4);
	}
}

TEST_CASE("PlanReadColumn other types", "[read_types]") {
	SECTION("BINARY_FLOAT and BINARY_DOUBLE") {
		auto p = Plan(Col("BINARY_FLOAT"));
		CHECK(p.duck_type.id == DuckTypeId::kFloat);
		CHECK(p.decoder == FetchDecoder::kNativeFloat);
		CHECK(p.dty == SQLT_BFLOAT);
		CHECK(p.define_bytes == 4);
		p = Plan(Col("BINARY_DOUBLE"));
		CHECK(p.duck_type.id == DuckTypeId::kDouble);
		CHECK(p.dty == SQLT_BDOUBLE);
		CHECK(p.define_bytes == 8);
	}
	SECTION("DATE") {
		auto p = Plan(Col("DATE"));
		CHECK(p.duck_type.id == DuckTypeId::kTimestamp);
		CHECK(p.decoder == FetchDecoder::kDate);
		CHECK(p.dty == SQLT_DAT);
		CHECK(p.define_bytes == 7);
	}
	SECTION("TIMESTAMP") {
		auto p = Plan(Col("TIMESTAMP(6)"));
		CHECK(p.duck_type.id == DuckTypeId::kTimestamp);
		CHECK(p.decoder == FetchDecoder::kTimestamp);
		CHECK(p.dty == kInternalTimestamp);
		CHECK(p.define_bytes == 11);
		CHECK(p.select_expr == "\"C\"");
	}
	SECTION("TIMESTAMP WITH TIME ZONE") {
		auto p = Plan(Col("TIMESTAMP(6) WITH TIME ZONE"));
		CHECK(p.duck_type.id == DuckTypeId::kTimestampTz);
		CHECK(p.decoder == FetchDecoder::kTimestamp);
		CHECK(p.dty == kInternalTimestamp);
		CHECK(p.select_expr == "SYS_EXTRACT_UTC(\"C\")");
	}
	SECTION("TIMESTAMP WITH LOCAL TIME ZONE") {
		auto p = Plan(Col("TIMESTAMP(9) WITH LOCAL TIME ZONE"));
		CHECK(p.duck_type.id == DuckTypeId::kTimestampTz);
		CHECK(p.select_expr == "SYS_EXTRACT_UTC(CAST(\"C\" AS TIMESTAMP WITH TIME ZONE))");
	}
	SECTION("VARCHAR2 and CHAR") {
		auto p = Plan(Col("VARCHAR2", -1, -1, 100));
		CHECK(p.duck_type.id == DuckTypeId::kVarchar);
		CHECK(p.decoder == FetchDecoder::kString);
		CHECK(p.dty == SQLT_CHR);
		CHECK(p.define_bytes == 100);
		CHECK_FALSE(p.national);
		p = Plan(Col("CHAR", -1, -1, 10), UnconstrainedNumber(), "WE8MSWIN1252");
		CHECK(p.define_bytes == 30);
		p = Plan(Col("VARCHAR2", -1, -1, 10), UnconstrainedNumber(), "UTF8");
		CHECK(p.define_bytes == 10);
	}
	SECTION("NVARCHAR2 and NCHAR") {
		auto p = Plan(Col("NVARCHAR2", -1, -1, 20));
		CHECK(p.duck_type.id == DuckTypeId::kVarchar);
		CHECK(p.decoder == FetchDecoder::kString);
		CHECK(p.dty == SQLT_CHR);
		CHECK(p.national);
		CHECK(p.define_bytes == 30);
		p = Plan(Col("NCHAR", -1, -1, 3));
		CHECK(p.define_bytes == 5);
	}
	SECTION("CLOB and NCLOB") {
		auto p = Plan(Col("CLOB", -1, -1, 4000));
		CHECK(p.duck_type.id == DuckTypeId::kVarchar);
		CHECK(p.decoder == FetchDecoder::kString);
		CHECK(p.dty == SQLT_CLOB);
		CHECK(p.lob);
		CHECK(p.define_bytes == 8);
		CHECK_FALSE(p.national);
		p = Plan(Col("NCLOB"));
		CHECK(p.lob);
		CHECK(p.national);
	}
	SECTION("RAW") {
		auto p = Plan(Col("RAW", -1, -1, 16));
		CHECK(p.duck_type.id == DuckTypeId::kBlob);
		CHECK(p.decoder == FetchDecoder::kBinary);
		CHECK(p.dty == SQLT_BIN);
		CHECK(p.define_bytes == 16);
		CHECK_FALSE(p.lob);
	}
	SECTION("BLOB") {
		auto p = Plan(Col("BLOB"));
		CHECK(p.duck_type.id == DuckTypeId::kBlob);
		CHECK(p.dty == SQLT_BLOB);
		CHECK(p.lob);
		CHECK(p.define_bytes == 8);
	}
	SECTION("ROWID and UROWID") {
		auto p = Plan(Col("ROWID", -1, -1, 10));
		CHECK(p.duck_type.id == DuckTypeId::kVarchar);
		CHECK(p.decoder == FetchDecoder::kString);
		CHECK(p.dty == SQLT_CHR);
		CHECK(p.define_bytes == 4000);
		CHECK(p.select_expr == "ROWIDTOCHAR(\"C\")");
		p = Plan(Col("UROWID", -1, -1, 4000));
		CHECK(p.select_expr == "CAST(\"C\" AS VARCHAR2(4000))");
	}
	SECTION("unsupported types") {
		for (const char *t : {"LONG", "LONG RAW", "XMLTYPE", "JSON", "VECTOR", "BFILE", "INTERVAL DAY(2) TO SECOND(6)",
		                      "INTERVAL YEAR(2) TO MONTH", "SDO_GEOMETRY"}) {
			auto p = Plan(Col(t));
			CHECK(p.decoder == FetchDecoder::kUnsupported);
			CHECK(p.duck_type.id == DuckTypeId::kVarchar);
			CHECK(p.unsupported ==
			      std::string("column \"C\" has Oracle type ") + t + ", which oraduck_scan does not read");
		}
	}
	SECTION("mixed-case names stay quoted") {
		auto c = Col("DATE");
		c.name = "Mixed";
		CHECK(Plan(c).select_expr == "\"Mixed\"");
	}
}

TEST_CASE("IsUtf8Charset", "[read_types]") {
	CHECK(IsUtf8Charset("AL32UTF8"));
	CHECK(IsUtf8Charset("UTF8"));
	CHECK_FALSE(IsUtf8Charset("WE8ISO8859P1"));
}

TEST_CASE("ParseUnconstrainedNumber", "[read_types]") {
	CHECK(ParseUnconstrainedNumber("double").kind == UnconstrainedNumber::Kind::kDouble);
	CHECK(ParseUnconstrainedNumber("VARCHAR").kind == UnconstrainedNumber::Kind::kVarchar);
	auto d = ParseUnconstrainedNumber(" decimal( 20 , 4 ) ");
	CHECK(d.kind == UnconstrainedNumber::Kind::kDecimal);
	CHECK(d.width == 20);
	CHECK(d.scale == 4);
	CHECK_THROWS_AS(ParseUnconstrainedNumber("DECIMAL(39,0)"), OraduckError);
	CHECK_THROWS_AS(ParseUnconstrainedNumber("DECIMAL(5,6)"), OraduckError);
	CHECK_THROWS_AS(ParseUnconstrainedNumber("DECIMAL(0,0)"), OraduckError);
	CHECK_THROWS_AS(ParseUnconstrainedNumber("BIGINT"), OraduckError);
}
