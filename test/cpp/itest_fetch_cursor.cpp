#include "catch.hpp"
#include "oraduck/describe_source.hpp"
#include "oraduck/fetch_cursor.hpp"
#include "oraduck/oracle_decoding.hpp"
#include "oraduck/oracle_encoding.hpp"
#include "oraduck/read_sql.hpp"
#include "itest_support.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace oraduck;

namespace {

// Drops the listed tables and views when the test ends, even when it fails
struct DropGuard {
	OciSession &session;
	std::vector<std::string> tables;
	std::vector<std::string> views = {};

	void DropAll() {
		for (const auto &view : views) {
			session.Execute("BEGIN EXECUTE IMMEDIATE 'DROP VIEW " + view +
			                "'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;");
		}
		for (const auto &table : tables) {
			DropTableIfExists(session, table);
		}
	}
	~DropGuard() {
		try {
			DropAll();
		} catch (...) {
		}
	}
};

// Every column of a table or view planned for reading, and its scan statement
class ReadPlan {
public:
	ReadPlan(OciSession &session, const std::string &name, const std::string &unconstrained = "DOUBLE") {
		source = DescribeSource(session, ParseTargetName(name));
		const auto number = ParseUnconstrainedNumber(unconstrained);
		for (const auto &column : source.columns) {
			plans.push_back(PlanReadColumn(column, number, source.db_charset));
		}
		for (const auto &plan : plans) {
			columns.push_back(&plan);
		}
		statement = Statement(columns, Slice());
	}
	ReadPlan(const ReadPlan &) = delete;
	ReadPlan &operator=(const ReadPlan &) = delete;

	SqlStatement Statement(const std::vector<const ReadColumnPlan *> &projected, const Slice &slice) const {
		ScanSqlInput input;
		input.owner = source.owner;
		input.object = source.name;
		input.columns = projected;
		return BuildScanSql(input, slice);
	}

	OracleTableDescription source;
	std::vector<ReadColumnPlan> plans;
	std::vector<const ReadColumnPlan *> columns;
	SqlStatement statement;
};

struct Cell {
	bool null = true;
	std::string bytes;
};

using Rows = std::vector<std::vector<Cell>>;

// Copies every value of every batch; batches receives the row count of each batch
Rows ReadAll(FetchCursor &cursor, size_t columns, std::vector<uint32_t> *batches = nullptr) {
	Rows rows;
	for (uint32_t n; (n = cursor.Fetch()) > 0;) {
		if (batches) {
			batches->push_back(n);
		}
		for (uint32_t r = 0; r < n; r++) {
			std::vector<Cell> row(columns);
			for (size_t c = 0; c < columns; c++) {
				uint32_t length = 0;
				const uint8_t *value = cursor.Value(c, r, length);
				if (value) {
					row[c].null = false;
					row[c].bytes.assign(reinterpret_cast<const char *>(value), length);
				}
			}
			rows.push_back(std::move(row));
		}
	}
	return rows;
}

int64_t Int64(const std::string &bytes) {
	REQUIRE(bytes.size() == sizeof(int64_t));
	int64_t value;
	std::memcpy(&value, bytes.data(), sizeof(value));
	return value;
}

std::string Hex(const std::string &bytes) {
	static const char digits[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : bytes) {
		out.push_back(digits[c >> 4]);
		out.push_back(digits[c & 15]);
	}
	return out;
}

// "Typ=2 Len=3: 194,2,24" -> the listed bytes
std::string DumpBytes(const std::string &dump) {
	const size_t colon = dump.find(": ");
	REQUIRE(colon != std::string::npos);
	std::string out;
	const char *p = dump.c_str() + colon + 2;
	while (*p) {
		char *end = nullptr;
		out.push_back(static_cast<char>(std::strtol(p, &end, 10)));
		p = *end == ',' ? end + 1 : end;
	}
	return out;
}

std::string MicrosText(int64_t micros) {
	int64_t days = 0, micros_of_day = 0;
	SplitMicros(micros, days, micros_of_day);
	char text[kDateTimeTextBytes];
	REQUIRE(FormatDateTimeText(days, micros_of_day, text));
	return std::string(text, kDateTimeTextBytes);
}

std::string Int128Text(OracleInt128 value) {
	const bool negative = value.hi < 0;
	uint64_t hi = static_cast<uint64_t>(value.hi), lo = value.lo;
	if (negative) {
		lo = ~lo + 1;
		hi = ~hi + (lo == 0 ? 1 : 0);
	}
	std::string digits;
	while (hi != 0 || lo != 0) {
		// divide the 128-bit magnitude by 10 through 32-bit limbs
		uint32_t limbs[4] = {static_cast<uint32_t>(hi >> 32), static_cast<uint32_t>(hi),
		                     static_cast<uint32_t>(lo >> 32), static_cast<uint32_t>(lo)};
		uint64_t remainder = 0;
		for (auto &limb : limbs) {
			const uint64_t current = (remainder << 32) | limb;
			limb = static_cast<uint32_t>(current / 10);
			remainder = current % 10;
		}
		hi = (static_cast<uint64_t>(limbs[0]) << 32) | limbs[1];
		lo = (static_cast<uint64_t>(limbs[2]) << 32) | limbs[3];
		digits.insert(digits.begin(), static_cast<char>('0' + remainder));
	}
	if (digits.empty()) {
		digits = "0";
	}
	return negative ? "-" + digits : digits;
}

// Oracle's TO_CHAR(n, 'TM9') in the decoder's canonical form ("0" before the point)
std::string CanonicalNumber(std::string text) {
	if (text.compare(0, 2, "-.") == 0) {
		text.insert(1, "0");
	} else if (!text.empty() && text[0] == '.') {
		text.insert(0, "0");
	}
	return text;
}

// Oracle expression giving the text a value's decoded form must match (NULL for NULL)
std::string ExpectedExpr(const ReadColumnPlan &plan) {
	const std::string q = QuoteIdentifier(plan.name);
	std::string text;
	switch (plan.decoder) {
	case FetchDecoder::kNativeInt64:
	case FetchDecoder::kNativeDouble:
	case FetchDecoder::kNativeFloat:
		text = "TO_CHAR(" + q + ")";
		break;
	case FetchDecoder::kNumberDecimal64:
	case FetchDecoder::kNumberDecimal128:
		text = "TO_CHAR(" + q + " * POWER(10, " + std::to_string(plan.scale) + "))";
		break;
	case FetchDecoder::kNumberText:
		text = "TO_CHAR(" + q + ", 'TM9')";
		break;
	case FetchDecoder::kDate:
		text = "TO_CHAR(" + q + ", 'YYYY-MM-DD HH24:MI:SS') || '.000000'";
		break;
	case FetchDecoder::kTimestamp:
		text = "SUBSTR(TO_CHAR(" + plan.select_expr + ", 'YYYY-MM-DD HH24:MI:SS.FF9'), 1, 26)";
		break;
	case FetchDecoder::kString:
		text = plan.lob ? "''" : plan.national ? "TO_CHAR(" + q + ")" : plan.select_expr;
		break;
	case FetchDecoder::kBinary:
		text = plan.lob ? "''" : "RAWTOHEX(" + q + ")";
		break;
	default:
		FAIL("unexpected decoder for " + plan.name);
	}
	return text;
}

// Checks a fetched value against Oracle's NULL marker ('N' or 'V') and the text of ExpectedExpr
void CheckValue(const ReadColumnPlan &plan, const Cell &cell, const std::string &marker, const std::string &text) {
	INFO("column " << plan.name << ", expected " << marker << " " << text);
	REQUIRE(cell.null == (marker == "N"));
	if (cell.null) {
		return;
	}
	const auto *bytes = reinterpret_cast<const uint8_t *>(cell.bytes.data());
	const size_t length = cell.bytes.size();
	switch (plan.decoder) {
	case FetchDecoder::kNativeInt64:
		CHECK(std::to_string(Int64(cell.bytes)) == text);
		break;
	case FetchDecoder::kNumberDecimal64: {
		int64_t value = 0;
		REQUIRE(DecodeOracleNumberScaled64(bytes, length, plan.scale, plan.round, value) == NumberDecodeStatus::kOk);
		CHECK(std::to_string(value) == text);
		break;
	}
	case FetchDecoder::kNumberDecimal128: {
		OracleInt128 value {};
		REQUIRE(DecodeOracleNumberScaled128(bytes, length, plan.scale, plan.round, value) == NumberDecodeStatus::kOk);
		CHECK(Int128Text(value) == text);
		break;
	}
	case FetchDecoder::kNumberText:
		CHECK(DecodeOracleNumberText(bytes, length) == CanonicalNumber(text));
		break;
	case FetchDecoder::kNativeDouble: {
		REQUIRE(length == sizeof(double));
		double value;
		std::memcpy(&value, bytes, sizeof(value));
		CHECK(value == std::strtod(text.c_str(), nullptr));
		break;
	}
	case FetchDecoder::kNativeFloat: {
		REQUIRE(length == sizeof(float));
		float value;
		std::memcpy(&value, bytes, sizeof(value));
		CHECK(value == std::strtof(text.c_str(), nullptr));
		break;
	}
	case FetchDecoder::kDate: {
		REQUIRE(length == kOracleDateBytes);
		int64_t micros = 0;
		REQUIRE(DecodeOracleDate(bytes, micros));
		CHECK(MicrosText(micros) == text);
		break;
	}
	case FetchDecoder::kTimestamp: {
		REQUIRE((length == 7 || length == 11));
		int64_t micros = 0;
		REQUIRE(DecodeOracleTimestamp(bytes, length, micros));
		CHECK(MicrosText(micros) == text);
		break;
	}
	case FetchDecoder::kString:
		if (!plan.lob) {
			CHECK(cell.bytes == text);
		}
		break;
	case FetchDecoder::kBinary:
		if (!plan.lob) {
			CHECK(Hex(cell.bytes) == text);
		}
		break;
	default:
		FAIL("unexpected decoder");
	}
}

std::map<std::string, int64_t> SessionStats(OciSession &session) {
	std::map<std::string, int64_t> stats;
	for (const auto &row : session.QueryStrings(
	         "SELECT n.name, s.value FROM v$mystat s JOIN v$statname n ON n.statistic# = s.statistic# "
	         "WHERE n.name IN ('SQL*Net roundtrips to/from client', 'user calls', 'opened cursors current')")) {
		stats[row[0]] = std::stoll(row[1]);
	}
	return stats;
}

// A CLOB value built on the server from repeated pieces: head || TO_CHAR(k) (when numbered) || tail, k = 1..count
struct Piece {
	std::string head;
	int count;
	bool numbered = false;
	std::string tail = "";
};

// Builds the value in the PL/SQL variable c, runs insert (a statement using c) and returns the value
std::string BuildClob(OciSession &session, const std::vector<Piece> &pieces, const std::string &insert) {
	std::string sql =
	    "DECLARE c CLOB; buf VARCHAR2(32767); "
	    "PROCEDURE add(s VARCHAR2) IS BEGIN "
	    "IF LENGTHB(buf) + LENGTHB(s) > 32000 THEN DBMS_LOB.APPEND(c, TO_CLOB(buf)); buf := NULL; END IF; "
	    "buf := buf || s; END; "
	    "BEGIN DBMS_LOB.CREATETEMPORARY(c, TRUE); ";
	std::string expected;
	for (const auto &piece : pieces) {
		sql += "FOR k IN 1 .. " + std::to_string(piece.count) + " LOOP add(" + SqlStringLiteral(piece.head) +
		       (piece.numbered ? " || TO_CHAR(k)" : "") + " || " + SqlStringLiteral(piece.tail) + "); END LOOP; ";
		for (int k = 1; k <= piece.count; k++) {
			expected += piece.head + (piece.numbered ? std::to_string(k) : "") + piece.tail;
		}
	}
	sql += "IF buf IS NOT NULL THEN DBMS_LOB.APPEND(c, TO_CLOB(buf)); END IF; " + insert +
	       "; DBMS_LOB.FREETEMPORARY(c); END;";
	session.Execute(sql);
	return expected;
}

// Inserts (id, value) into table (ID, C) and returns the value the server built
std::string InsertClob(OciSession &session, const std::string &table, int id, const std::vector<Piece> &pieces) {
	return BuildClob(session, pieces, "INSERT INTO " + table + " (ID, C) VALUES (" + std::to_string(id) + ", c)");
}

std::string RowGenerator(int64_t rows) {
	return "(SELECT LEVEL r FROM dual CONNECT BY LEVEL <= " + std::to_string(rows) + ")";
}

} // namespace

TEST_CASE("Fetch cursor: every read type, NULLs and multi-byte text", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_TYPES"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_TYPES (ID NUMBER(10), N4 NUMBER(4), N18 NUMBER(18), N38 NUMBER(38,0), "
	                "N259 NUMBER(25,9), N102 NUMBER(10,2), NNEG NUMBER(5,-2), NU NUMBER, FL FLOAT(20), "
	                "BF BINARY_FLOAT, BD BINARY_DOUBLE, D DATE, TS TIMESTAMP(6), TS9 TIMESTAMP(9), "
	                "TZ TIMESTAMP(6) WITH TIME ZONE, LTZ TIMESTAMP(6) WITH LOCAL TIME ZONE, V VARCHAR2(20 CHAR), "
	                "VB VARCHAR2(4000 BYTE), NV NVARCHAR2(10), C CHAR(3), NC NCHAR(3), CL CLOB, NCL NCLOB, "
	                "RW RAW(16), BL BLOB, RID ROWID, URID UROWID)");
	const std::string columns = "(ID, N4, N18, N38, N259, N102, NNEG, NU, FL, BF, BD, D, TS, TS9, TZ, LTZ, V, VB, NV, "
	                            "C, NC, CL, NCL, RW, BL)";
	session.Execute("INSERT INTO ITEST_FC_TYPES " + columns +
	                " VALUES (1, -1234, 999999999999999999, 99999999999999999999999999999999999999, "
	                "-1234567890123456.123456789, 12345678.91, 9876500, 123.456, 0.1, 1.5f, 0.1d, "
	                "TO_DATE('2024-02-29 13:45:59', 'YYYY-MM-DD HH24:MI:SS'), TIMESTAMP '2024-02-29 13:45:59.123456', "
	                "TIMESTAMP '1999-12-31 23:59:59.987654321', TIMESTAMP '2024-06-30 23:30:00.5 +02:00', "
	                "TIMESTAMP '2024-01-01 00:15:00 -05:00', 'é漢字😀x', RPAD('a', 4000, 'b'), N'é漢字😀', 'A', N'é', "
	                "TO_CLOB('é漢字😀 clob'), TO_NCLOB('é漢字😀 nclob'), HEXTORAW('00FF7F80'), "
	                "TO_BLOB(HEXTORAW('DEADBEEF00')))");
	session.Execute("INSERT INTO ITEST_FC_TYPES (ID) VALUES (2)");
	session.Execute("INSERT INTO ITEST_FC_TYPES " + columns +
	                " VALUES (3, 9999, -999999999999999999, -99999999999999999999999999999999999999, 0.000000001, "
	                "-0.01, 0, -98765.4321, -2.5, BINARY_FLOAT_INFINITY, -BINARY_DOUBLE_INFINITY, "
	                "TO_DATE('0001-01-01 00:00:00', 'YYYY-MM-DD HH24:MI:SS'), TIMESTAMP '9999-12-31 23:59:59.999999', "
	                "TIMESTAMP '0001-01-01 00:00:00', TIMESTAMP '2000-01-01 00:00:00 -08:00', "
	                "TIMESTAMP '1970-01-01 00:00:00 +00:00', 'x', 'é', N'a', 'ABC', N'漢字a', EMPTY_CLOB(), "
	                "EMPTY_CLOB(), HEXTORAW('00'), EMPTY_BLOB())");
	session.Execute("UPDATE ITEST_FC_TYPES SET RID = ROWID, URID = ROWID WHERE ID <> 2");

	for (const std::string unconstrained : {"DOUBLE", "VARCHAR", "DECIMAL(38,10)"}) {
		INFO("unconstrained NUMBER as " << unconstrained);
		ReadPlan plan(session, "ITEST_FC_TYPES", unconstrained);
		REQUIRE(plan.plans.size() == 27);
		for (const auto &column : plan.plans) {
			REQUIRE(column.decoder != FetchDecoder::kUnsupported);
		}
		FetchCursor cursor(session, plan.statement, plan.columns, FetchOptions());
		const Rows rows = ReadAll(cursor, plan.columns.size());
		REQUIRE(rows.size() == 3);
		std::map<int64_t, const std::vector<Cell> *> by_id;
		for (const auto &row : rows) {
			by_id[Int64(row[0].bytes)] = &row;
		}
		REQUIRE(by_id.size() == 3);

		// Decoded values against Oracle's TO_CHAR, raw NUMBER and DATE bytes against DUMP
		std::string sql = "SELECT ";
		for (size_t c = 0; c < plan.plans.size(); c++) {
			const auto &column = plan.plans[c];
			const std::string q = QuoteIdentifier(column.name);
			sql += (c ? ", " : "") + std::string("CASE WHEN ") + q + " IS NULL THEN 'N' ELSE 'V' END, " +
			       ExpectedExpr(column);
			if (column.dty == SQLT_NUM || column.dty == SQLT_DAT) {
				sql += ", DUMP(" + QuoteIdentifier(column.name) + ")";
			}
		}
		const auto expected = session.QueryStrings(sql + " FROM ITEST_FC_TYPES ORDER BY ID");
		REQUIRE(expected.size() == 3);
		for (int64_t id = 1; id <= 3; id++) {
			INFO("row " << id);
			const auto &row = *by_id.at(id);
			const auto &oracle = expected[id - 1];
			size_t e = 0;
			for (size_t c = 0; c < plan.plans.size(); c++) {
				const auto &column = plan.plans[c];
				CheckValue(column, row[c], oracle[e], oracle[e + 1]);
				e += 2;
				if (column.dty == SQLT_NUM || column.dty == SQLT_DAT) {
					if (!row[c].null) {
						INFO("DUMP of " << column.name);
						CHECK(row[c].bytes == DumpBytes(oracle[e]));
					}
					e++;
				}
			}
		}

		auto value = [&](int64_t id, const std::string &name) -> const Cell & {
			for (size_t c = 0; c < plan.plans.size(); c++) {
				if (plan.plans[c].name == name) {
					return (*by_id.at(id))[c];
				}
			}
			FAIL("no column " + name);
			return (*by_id.at(id))[0];
		};
		// The row of NULLs: every column but ID
		for (size_t c = 1; c < plan.plans.size(); c++) {
			INFO("column " << plan.plans[c].name);
			CHECK((*by_id.at(2))[c].null);
		}
		// Text, LOB and binary values as inserted
		CHECK(value(1, "V").bytes == "é漢字😀x");
		CHECK(value(1, "VB").bytes == "a" + std::string(3999, 'b'));
		CHECK(value(1, "NV").bytes == "é漢字😀");
		CHECK(value(1, "C").bytes == "A  ");
		CHECK(value(1, "NC").bytes == "é  ");
		CHECK(value(1, "CL").bytes == "é漢字😀 clob");
		CHECK(value(1, "NCL").bytes == "é漢字😀 nclob");
		CHECK(Hex(value(1, "RW").bytes) == "00FF7F80");
		CHECK(Hex(value(1, "BL").bytes) == "DEADBEEF00");
		CHECK(value(3, "V").bytes == "x");
		CHECK(value(3, "VB").bytes == "é");
		CHECK(value(3, "NV").bytes == "a");
		CHECK(value(3, "C").bytes == "ABC");
		CHECK(value(3, "NC").bytes == "漢字a");
		CHECK_FALSE(value(3, "CL").null);
		CHECK(value(3, "CL").bytes.empty());
		CHECK_FALSE(value(3, "NCL").null);
		CHECK(value(3, "NCL").bytes.empty());
		CHECK(Hex(value(3, "RW").bytes) == "00");
		CHECK_FALSE(value(3, "BL").null);
		CHECK(value(3, "BL").bytes.empty());
		// Exact values of the instants and numbers
		int64_t micros = 0;
		REQUIRE(DecodeOracleTimestamp(reinterpret_cast<const uint8_t *>(value(1, "TZ").bytes.data()),
		                              value(1, "TZ").bytes.size(), micros));
		CHECK(MicrosText(micros) == "2024-06-30 21:30:00.500000");
		REQUIRE(DecodeOracleTimestamp(reinterpret_cast<const uint8_t *>(value(1, "LTZ").bytes.data()),
		                              value(1, "LTZ").bytes.size(), micros));
		CHECK(MicrosText(micros) == "2024-01-01 05:15:00.000000");
		REQUIRE(DecodeOracleTimestamp(reinterpret_cast<const uint8_t *>(value(1, "TS9").bytes.data()),
		                              value(1, "TS9").bytes.size(), micros));
		CHECK(MicrosText(micros) == "1999-12-31 23:59:59.987654");
		CHECK(value(3, "TS9").bytes.size() == 7);
		CHECK(DecodeOracleNumberText(reinterpret_cast<const uint8_t *>(value(1, "N38").bytes.data()),
		                             value(1, "N38").bytes.size()) == std::string(38, '9'));
		CHECK(DecodeOracleNumberText(reinterpret_cast<const uint8_t *>(value(1, "N259").bytes.data()),
		                             value(1, "N259").bytes.size()) == "-1234567890123456.123456789");
	}
}

TEST_CASE("Fetch cursor: CLOB values read byte for byte", "[oracle][fetch_cursor][lob]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_CLOB"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_CLOB (ID NUMBER(10), C CLOB)");
	session.Execute("INSERT INTO ITEST_FC_CLOB VALUES (1, NULL)");
	session.Execute("INSERT INTO ITEST_FC_CLOB VALUES (2, EMPTY_CLOB())");
	std::map<int64_t, std::string> expected;
	expected[2] = "";
	expected[3] = InsertClob(session, "ITEST_FC_CLOB", 3, {{"aé漢😀", 1}});
	expected[4] = InsertClob(session, "ITEST_FC_CLOB", 4, {{"é漢😀;", 500}});
	// 2 MB: multi-byte characters straddle every piece, chunk and prefetch boundary
	expected[5] = InsertClob(session, "ITEST_FC_CLOB", 5, {{"a", 130000, true, "é漢😀;"}});
	// a surrogate pair split by the end of the 32,768-character LOB prefetch
	expected[6] = InsertClob(session, "ITEST_FC_CLOB", 6, {{"x", 32767}, {"😀", 1000}});
	// exactly 32,768 characters of 3 bytes
	expected[7] = InsertClob(session, "ITEST_FC_CLOB", 7, {{"漢", 32768}});
	REQUIRE(expected[3].size() == 10);
	REQUIRE(expected[4].size() == 5000);
	REQUIRE(expected[5].size() > 2000000);

	ReadPlan plan(session, "ITEST_FC_CLOB");
	FetchOptions small;
	small.max_rows = 3;
	small.lob_prefetch_chars = 4097;
	FetchOptions none;
	none.lob_prefetch_chars = 0;
	// several OCILobRead2 calls per value, the call boundaries inside surrogate pairs
	FetchOptions chunked;
	chunked.lob_read_chunk_for_tests = 1001;
	for (const auto &options : {FetchOptions(), small, none, chunked}) {
		INFO("max_rows " << options.max_rows << ", LOB prefetch " << options.lob_prefetch_chars << ", read chunk "
		                 << options.lob_read_chunk_for_tests);
		FetchCursor cursor(session, plan.statement, plan.columns, options);
		std::map<int64_t, Cell> read;
		for (uint32_t n; (n = cursor.Fetch()) > 0;) {
			// every value of the batch stays valid until the next fetch
			std::vector<std::pair<const uint8_t *, uint32_t>> values(n);
			for (uint32_t r = 0; r < n; r++) {
				values[r].first = cursor.Value(1, r, values[r].second);
			}
			for (uint32_t r = 0; r < n; r++) {
				uint32_t length = 0;
				Cell &cell = read[Int64(std::string(reinterpret_cast<const char *>(cursor.Value(0, r, length)), 8))];
				cell.null = values[r].first == nullptr;
				if (!cell.null) {
					cell.bytes.assign(reinterpret_cast<const char *>(values[r].first), values[r].second);
				}
			}
		}
		REQUIRE(read.size() == 7);
		CHECK(read[1].null);
		for (int64_t id = 2; id <= 7; id++) {
			INFO("row " << id);
			REQUIRE_FALSE(read[id].null);
			CHECK(read[id].bytes.size() == expected[id].size());
			CHECK(read[id].bytes == expected[id]);
		}
	}
}

TEST_CASE("Fetch cursor: 2 MB CLOB of 4-byte characters", "[oracle][fetch_cursor][lob]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_EMOJI"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_EMOJI (ID NUMBER(10), C CLOB)");
	// 524,288 characters of 4 bytes (1,048,576 UTF-16 code units), aligned and shifted by one character
	const std::string eight = "😀😀😀😀😀😀😀😀";
	std::map<int64_t, std::string> expected;
	expected[1] = InsertClob(session, "ITEST_FC_EMOJI", 1, {{eight, 65536}});
	expected[2] = InsertClob(session, "ITEST_FC_EMOJI", 2, {{"x", 1}, {eight, 65536}});
	REQUIRE(expected[1].size() == 2097152);

	ReadPlan plan(session, "ITEST_FC_EMOJI");
	std::vector<FetchOptions> variants(4);
	variants[1].lob_prefetch_chars = 32767;
	variants[2].lob_prefetch_chars = 0;
	variants[3].lob_read_chunk_for_tests = 1001; // several OCILobRead2 calls, boundaries inside surrogate pairs
	for (const auto &options : variants) {
		INFO("LOB prefetch " << options.lob_prefetch_chars << ", read chunk " << options.lob_read_chunk_for_tests);
		FetchCursor cursor(session, plan.statement, plan.columns, options);
		const Rows rows = ReadAll(cursor, 2);
		REQUIRE(rows.size() == 2);
		for (const auto &row : rows) {
			const int64_t id = Int64(row[0].bytes);
			INFO("row " << id);
			REQUIRE_FALSE(row[1].null);
			CHECK(row[1].bytes.size() == expected[id].size());
			CHECK(row[1].bytes == expected[id]);
		}
	}
}

TEST_CASE("Fetch cursor: BLOB values read byte for byte", "[oracle][fetch_cursor][lob]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_BLOB"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_BLOB (ID NUMBER(10), B BLOB)");
	session.Execute("INSERT INTO ITEST_FC_BLOB VALUES (1, NULL)");
	session.Execute("INSERT INTO ITEST_FC_BLOB VALUES (2, EMPTY_BLOB())");
	session.Execute("INSERT INTO ITEST_FC_BLOB VALUES (3, HEXTORAW('00FF10'))");
	// 70,002 bytes: beyond the 32,768-byte LOB prefetch
	session.Execute("DECLARE b BLOB; BEGIN DBMS_LOB.CREATETEMPORARY(b, TRUE); FOR i IN 1 .. 3 LOOP "
	                "DBMS_LOB.APPEND(b, TO_BLOB(UTL_RAW.COPIES(HEXTORAW('00FF10'), 7778))); END LOOP; "
	                "INSERT INTO ITEST_FC_BLOB VALUES (4, b); DBMS_LOB.FREETEMPORARY(b); END;");
	std::string pattern;
	for (int i = 0; i < 3 * 7778; i++) {
		pattern += std::string("\x00\xFF\x10", 3);
	}
	const std::map<int64_t, std::string> expected = {{2, ""}, {3, std::string("\x00\xFF\x10", 3)}, {4, pattern}};

	ReadPlan plan(session, "ITEST_FC_BLOB");
	FetchOptions none;
	none.lob_prefetch_chars = 0;
	FetchOptions chunked;
	chunked.lob_read_chunk_for_tests = 1001; // bytes per OCILobRead2 call
	for (const auto &options : {FetchOptions(), none, chunked}) {
		INFO("LOB prefetch " << options.lob_prefetch_chars << ", read chunk " << options.lob_read_chunk_for_tests);
		FetchCursor cursor(session, plan.statement, plan.columns, options);
		const Rows rows = ReadAll(cursor, 2);
		REQUIRE(rows.size() == 4);
		for (const auto &row : rows) {
			const int64_t id = Int64(row[0].bytes);
			INFO("row " << id);
			if (id == 1) {
				CHECK(row[1].null);
				continue;
			}
			REQUIRE_FALSE(row[1].null);
			CHECK(row[1].bytes == expected.at(id));
		}
	}
}

TEST_CASE("Fetch cursor: large LOB values bound the batches by the budget", "[oracle][fetch_cursor][lob]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_BIG"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_BIG (ID NUMBER(10), C CLOB)");
	// 64 rows: a 5-digit ID then the same 1.1 MB of multi-byte text
	const std::string base = BuildClob(session, {{"a", 70000, true, "é漢😀;"}},
	                                   "INSERT INTO ITEST_FC_BIG (ID, C) SELECT LEVEL, "
	                                   "TO_CLOB(TO_CHAR(LEVEL, 'FM00000')) || c FROM dual CONNECT BY LEVEL <= 64");
	REQUIRE(base.size() > 1000000);
	auto expected = [&base](int64_t id) {
		const std::string digits = std::to_string(id);
		return std::string(5 - digits.size(), '0') + digits + base;
	};

	ReadPlan plan(session, "ITEST_FC_BIG");
	FetchOptions options;
	options.buffer_budget_bytes = 4 * 1024 * 1024;
	options.max_rows = 16; // the first batch, before any LOB size is known
	FetchCursor cursor(session, plan.statement, plan.columns, options);
	REQUIRE(cursor.rows_per_fetch() == 16);
	std::vector<uint32_t> batches;
	std::vector<uint64_t> batch_bytes;
	int64_t rows = 0, wrong = 0;
	for (uint32_t n; (n = cursor.Fetch()) > 0;) {
		uint64_t bytes = 0;
		for (uint32_t r = 0; r < n; r++) {
			uint32_t length = 0;
			const int64_t id = Int64(std::string(reinterpret_cast<const char *>(cursor.Value(0, r, length)), 8));
			const uint8_t *value = cursor.Value(1, r, length);
			if (!value || std::string(reinterpret_cast<const char *>(value), length) != expected(id)) {
				wrong++;
			}
			bytes += length;
		}
		batches.push_back(n);
		batch_bytes.push_back(bytes);
		rows += n;
	}
	CHECK(rows == 64);
	CHECK(wrong == 0);
	REQUIRE(batches.size() > 1);
	CHECK(batches[0] == 16);
	for (size_t b = 1; b < batches.size(); b++) {
		INFO("batch " << b << ": " << batches[b] << " rows, " << batch_bytes[b] << " LOB bytes");
		CHECK(batches[b] >= 1);
		CHECK(batch_bytes[b] + batches[b] * (16 + 12288) <= options.buffer_budget_bytes);
	}
}

TEST_CASE("Fetch cursor: 550-column table with a 16 MiB budget", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_WIDE"}};
	guard.DropAll();
	constexpr int64_t kRows = 5000;
	std::string ddl = "CREATE TABLE ITEST_FC_WIDE (ID NUMBER(10)";
	std::string select = "SELECT r";
	std::string lengths, nulls;
	for (int k = 1; k <= 200; k++) {
		const std::string name = "V" + std::to_string(k);
		ddl += ", " + name + " VARCHAR2(4000)";
		select += ", CASE WHEN MOD(r + " + std::to_string(k) + ", 10) = 0 THEN NULL ELSE RPAD('v', 5 + MOD(r * 7 + " +
		          std::to_string(k) + ", 26), 'abcé') END";
		lengths += (k > 1 ? " + " : "") + std::string("NVL(LENGTHB(") + name + "), 0)";
		nulls += (k > 1 ? " + " : "") + std::string("NVL2(") + name + ", 0, 1)";
	}
	for (int k = 1; k <= 100; k++) {
		ddl += ", N" + std::to_string(k) + " NUMBER(38,0)";
		select += ", r * 100000000000000000000 + " + std::to_string(k);
	}
	for (int k = 1; k <= 80; k++) {
		ddl += ", M" + std::to_string(k) + " NUMBER(25,9)";
		select += ", ROUND(r / 7, 9) + " + std::to_string(k);
	}
	for (int k = 1; k <= 60; k++) {
		ddl += ", D" + std::to_string(k) + " DATE";
		select += ", DATE '2000-01-01' + r + " + std::to_string(k) + " / 24";
	}
	for (int k = 1; k <= 50; k++) {
		ddl += ", C" + std::to_string(k) + " CHAR(1)";
		select += ", CHR(65 + MOD(r + " + std::to_string(k) + ", 26))";
	}
	for (int k = 1; k <= 30; k++) {
		ddl += ", T" + std::to_string(k) + " TIMESTAMP(6)";
		select +=
		    ", TIMESTAMP '2000-01-01 00:00:00' + NUMTODSINTERVAL(r + " + std::to_string(k) + " / 1000000, 'SECOND')";
	}
	for (int k = 1; k <= 29; k++) {
		ddl += ", F" + std::to_string(k) + " NUMBER";
		select += ", r * 1.5 + " + std::to_string(k);
	}
	session.Execute(ddl + ")");
	session.Execute("INSERT /*+ APPEND */ INTO ITEST_FC_WIDE " + select + " FROM " + RowGenerator(kRows));

	ReadPlan plan(session, "ITEST_FC_WIDE");
	REQUIRE(plan.columns.size() == 550);
	FetchOptions options;
	options.buffer_budget_bytes = 16 * 1024 * 1024;
	const auto start = std::chrono::steady_clock::now();
	FetchCursor cursor(session, plan.statement, plan.columns, options);
	CHECK(cursor.rows_per_fetch() >= 16);
	int64_t rows = 0, id_sum = 0, text_bytes = 0, text_nulls = 0, decode_errors = 0;
	OracleInt128 n1_sum {0, 0};
	for (uint32_t n; (n = cursor.Fetch()) > 0;) {
		REQUIRE(n <= cursor.rows_per_fetch());
		for (uint32_t r = 0; r < n; r++) {
			uint32_t length = 0;
			const uint8_t *id = cursor.Value(0, r, length);
			REQUIRE(id);
			int64_t value;
			std::memcpy(&value, id, sizeof(value));
			id_sum += value;
			for (size_t c = 1; c <= 200; c++) {
				if (cursor.Value(c, r, length)) {
					text_bytes += length;
				} else {
					text_nulls++;
				}
			}
			const uint8_t *n1 = cursor.Value(201, r, length);
			OracleInt128 decoded {};
			if (!n1 || DecodeOracleNumberScaled128(n1, length, 0, false, decoded) != NumberDecodeStatus::kOk) {
				decode_errors++;
			} else {
				const uint64_t lo = n1_sum.lo + decoded.lo;
				n1_sum.hi += decoded.hi + (lo < n1_sum.lo ? 1 : 0);
				n1_sum.lo = lo;
			}
			for (size_t c = 202; c < 550; c++) {
				if (!cursor.Value(c, r, length)) {
					decode_errors++;
				}
			}
		}
		rows += n;
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::fprintf(stderr, "550-column read: %lld rows in %.3f s (%.0f rows/s), %u rows per fetch\n",
	             static_cast<long long>(rows), seconds, static_cast<double>(rows) / seconds, cursor.rows_per_fetch());
	CHECK(rows == kRows);
	CHECK(id_sum == kRows * (kRows + 1) / 2);
	CHECK(decode_errors == 0);
	const auto oracle = session.QueryStrings("SELECT TO_CHAR(SUM(" + lengths + ")), TO_CHAR(SUM(" + nulls +
	                                         ")), TO_CHAR(SUM(N1)) FROM ITEST_FC_WIDE");
	CHECK(std::to_string(text_bytes) == oracle[0][0]);
	CHECK(std::to_string(text_nulls) == oracle[0][1]);
	CHECK(Int128Text(n1_sum) == oracle[0][2]);
}

TEST_CASE("Fetch cursor: no column counts rows", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_COUNT"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_COUNT (ID NUMBER(10))");
	session.Execute("INSERT /*+ APPEND */ INTO ITEST_FC_COUNT SELECT r FROM " + RowGenerator(1000));
	ReadPlan plan(session, "ITEST_FC_COUNT");
	const SqlStatement statement = plan.Statement({}, Slice());
	REQUIRE(statement.sql.find("SELECT /* oraduck_scan */ 1 FROM") == 0);

	FetchOptions narrow;
	narrow.max_rows = 300;
	for (const auto &options : {FetchOptions(), narrow}) {
		FetchCursor cursor(session, statement, {}, options);
		CHECK(cursor.rows_per_fetch() == options.max_rows);
		std::vector<uint32_t> batches;
		uint32_t total = 0;
		for (uint32_t n; (n = cursor.Fetch()) > 0;) {
			batches.push_back(n);
			total += n;
		}
		CHECK(total == 1000);
		CHECK(batches.size() == (options.max_rows == 300 ? 4u : 1u));
		CHECK(cursor.Fetch() == 0);
	}
}

TEST_CASE("Fetch cursor: binds of every type and hash slices", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_BINDS"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_BINDS (ID NUMBER(10), V VARCHAR2(10), D DATE, X BINARY_DOUBLE, "
	                "N NUMBER(25,9))");
	session.Execute("INSERT /*+ APPEND */ INTO ITEST_FC_BINDS SELECT r, 'v' || MOD(r, 10), DATE '2020-01-01' + r, "
	                "r / 4, r / 8 FROM " +
	                RowGenerator(1000));
	ReadPlan plan(session, "ITEST_FC_BINDS");
	const std::vector<const ReadColumnPlan *> id_only = {&plan.plans[0]};

	SqlStatement statement;
	statement.sql = "SELECT ID FROM ITEST_FC_BINDS WHERE ID >= :b1 AND V <> :b2 AND D >= :b3 AND X < :b4 AND N > :b5";
	statement.binds = {BindNumber(100), BindText("v3"), BindDate(DaysFromCivil(2020, 3, 1) * kMicrosPerDay),
	                   BindDouble(200.0), BindDecimal({155, 0}, 1)};
	FetchCursor cursor(session, statement, id_only, FetchOptions());
	int64_t count = 0, sum = 0;
	for (const auto &row : ReadAll(cursor, 1)) {
		count++;
		sum += Int64(row[0].bytes);
	}
	const auto oracle = session.QueryStrings("SELECT COUNT(*), SUM(ID) FROM ITEST_FC_BINDS WHERE ID >= 100 AND V <> "
	                                         "'v3' AND D >= DATE '2020-03-01' AND X < 200 AND N > 15.5");
	CHECK(count > 0);
	CHECK(std::to_string(count) == oracle[0][0]);
	CHECK(std::to_string(sum) == oracle[0][1]);

	int64_t slice_rows = 0, slice_sum = 0;
	for (uint32_t bucket = 0; bucket < 4; bucket++) {
		Slice slice;
		slice.kind = SliceKind::kHash;
		slice.hash_buckets = 4;
		slice.hash_bucket = bucket;
		FetchCursor slice_cursor(session, plan.Statement(id_only, slice), id_only, FetchOptions());
		for (const auto &row : ReadAll(slice_cursor, 1)) {
			slice_rows++;
			slice_sum += Int64(row[0].bytes);
		}
	}
	CHECK(slice_rows == 1000);
	CHECK(slice_sum == 500500);
}

TEST_CASE("Fetch cursor: run again with new binds, parsed once", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_REEXEC"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_REEXEC (ID NUMBER(10), C CLOB)");
	session.Execute("INSERT /*+ APPEND */ INTO ITEST_FC_REEXEC SELECT r, TO_CLOB('c' || r) FROM " +
	                RowGenerator(1000));
	ReadPlan plan(session, "ITEST_FC_REEXEC");
	// A tag of this run in the text: V$SQL then shows this test's cursor only
	const std::string tag =
	    "itest " + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
	auto bucket = [&](uint32_t k) {
		Slice slice;
		slice.kind = SliceKind::kHash;
		slice.hash_buckets = 4;
		slice.hash_bucket = k;
		SqlStatement statement = plan.Statement(plan.columns, slice);
		const std::string head = "SELECT /* oraduck_scan */";
		REQUIRE(statement.sql.compare(0, head.size(), head) == 0);
		statement.sql.replace(0, head.size(), "SELECT /* oraduck_scan " + tag + " */");
		return statement;
	};
	FetchOptions options;
	options.max_rows = 100;
	FetchCursor cursor(session, bucket(0), plan.columns, options);
	CHECK(cursor.sql() == bucket(0).sql);
	int64_t rows = 0, sum = 0, wrong = 0;
	auto read = [&]() {
		for (const auto &row : ReadAll(cursor, 2)) {
			const int64_t id = Int64(row[0].bytes);
			rows++;
			sum += id;
			wrong += row[1].bytes != "c" + std::to_string(id);
		}
	};
	read();
	for (uint32_t k = 1; k < 4; k++) {
		cursor.Execute(bucket(k));
		read();
	}
	CHECK(rows == 1000);
	CHECK(sum == 500500);
	CHECK(wrong == 0);
	// Run again before the end of the rows: the rest of the previous execution is discarded
	cursor.Execute(bucket(0));
	CHECK(cursor.Fetch() > 0);
	cursor.Execute(bucket(1));
	int64_t bucket_rows = 0;
	for (uint32_t n; (n = cursor.Fetch()) > 0;) {
		bucket_rows += n;
	}
	CHECK(std::to_string(bucket_rows) == Scalar(session, "SELECT COUNT(*) FROM ITEST_FC_REEXEC WHERE ORA_HASH(ROWID, 3) = 1"));
	// Another text is refused: the cursor runs its own statement only
	CHECK_THROWS_AS(cursor.Execute(plan.statement), OraduckError);

	const auto cursors = session.QueryStrings(
	    "SELECT TO_CHAR(COUNT(*)), TO_CHAR(SUM(executions)), TO_CHAR(SUM(parse_calls)) FROM v$sql "
	    "WHERE sql_text LIKE :1",
	    {"SELECT /* oraduck_scan " + tag + " */%"});
	CHECK(cursors[0][0] == "1"); // child cursors
	CHECK(cursors[0][1] == "6"); // executions
	CHECK(cursors[0][2] == "1"); // parse calls
}

TEST_CASE("Fetch cursor: varchar binds keep their trailing blanks", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_BLANKS"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_BLANKS (ID NUMBER(10), V VARCHAR2(10), C CHAR(3), NV NVARCHAR2(10))");
	session.Execute("INSERT INTO ITEST_FC_BLANKS VALUES (1, 'abc ', 'A', 'n ')");
	session.Execute("INSERT INTO ITEST_FC_BLANKS VALUES (2, 'abc', 'AB', 'n')");
	ReadPlan plan(session, "ITEST_FC_BLANKS");
	const std::vector<const ReadColumnPlan *> id_only = {&plan.plans[0]};
	auto ids = [&](const std::string &where, const std::string &text) {
		SqlStatement statement;
		statement.sql = "SELECT ID FROM ITEST_FC_BLANKS WHERE " + where + " = :b1 ORDER BY ID";
		statement.binds = {BindVarchar(text)};
		FetchCursor cursor(session, statement, id_only, FetchOptions());
		std::vector<int64_t> result;
		for (const auto &row : ReadAll(cursor, 1)) {
			result.push_back(Int64(row[0].bytes));
		}
		return result;
	};
	// Non-padded comparison of the exact value (SQLT_CHR would strip the blanks: 'abc ' would find row 2)
	CHECK(ids("V", "abc ") == std::vector<int64_t> {1});
	CHECK(ids("V", "abc") == std::vector<int64_t> {2});
	CHECK(ids("C", "A  ") == std::vector<int64_t> {1});
	CHECK(ids("C", "A").empty());
	CHECK(ids("NV", "n ") == std::vector<int64_t> {1});
}

TEST_CASE("Fetch cursor: round trips with 5 small CLOB columns", "[oracle][fetch_cursor][lob]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_LOBS"}};
	guard.DropAll();
	constexpr int64_t kRows = 100000;
	std::string select = "SELECT r";
	std::string chars, nulls;
	for (int k = 1; k <= 5; k++) {
		const std::string name = "C" + std::to_string(k);
		select += ", CASE WHEN MOD(r + " + std::to_string(k) +
		          ", 50) = 0 THEN NULL ELSE TO_CLOB(RPAD('x', 1 + MOD(r * 7919 + " + std::to_string(k) +
		          ", 200), 'abcé')) END";
		chars += (k > 1 ? " + " : "") + std::string("NVL(DBMS_LOB.GETLENGTH(") + name + "), 0)";
		nulls += (k > 1 ? " + " : "") + std::string("NVL2(") + name + ", 0, 1)";
	}
	session.Execute("CREATE TABLE ITEST_FC_LOBS (ID NUMBER(10), C1 CLOB, C2 CLOB, C3 CLOB, C4 CLOB, C5 CLOB)");
	session.Execute("INSERT /*+ APPEND */ INTO ITEST_FC_LOBS " + select + " FROM " + RowGenerator(kRows));
	ReadPlan plan(session, "ITEST_FC_LOBS");

	const auto before = SessionStats(session);
	const auto start = std::chrono::steady_clock::now();
	int64_t rows = 0, characters = 0, null_values = 0;
	{
		FetchCursor cursor(session, plan.statement, plan.columns, FetchOptions());
		// ID (8 + 8 bytes) and 12 KiB per LOB column
		CHECK(cursor.rows_per_fetch() == 67108864 / (16 + 5 * 12288));
		for (uint32_t n; (n = cursor.Fetch()) > 0;) {
			for (uint32_t r = 0; r < n; r++) {
				for (size_t c = 1; c <= 5; c++) {
					uint32_t length = 0;
					const uint8_t *value = cursor.Value(c, r, length);
					if (!value) {
						null_values++;
						continue;
					}
					for (uint32_t i = 0; i < length; i++) {
						characters += (value[i] & 0xC0) != 0x80; // one UTF-8 lead byte per (BMP) character
					}
				}
			}
			rows += n;
		}
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const auto after = SessionStats(session);
	const int64_t round_trips =
	    after.at("SQL*Net roundtrips to/from client") - before.at("SQL*Net roundtrips to/from client");
	const int64_t user_calls = after.at("user calls") - before.at("user calls");
	std::fprintf(stderr, "5 CLOB columns: %lld rows in %.3f s (%.0f rows/s), %lld round trips, %lld user calls\n",
	             static_cast<long long>(rows), seconds, static_cast<double>(rows) / seconds,
	             static_cast<long long>(round_trips), static_cast<long long>(user_calls));
	CHECK(rows == kRows);
	CHECK(round_trips <= 1000);
	CHECK(user_calls <= 1000);
	const auto oracle =
	    session.QueryStrings("SELECT TO_CHAR(SUM(" + chars + ")), TO_CHAR(SUM(" + nulls + ")) FROM ITEST_FC_LOBS");
	CHECK(std::to_string(characters) == oracle[0][0]);
	CHECK(std::to_string(null_values) == oracle[0][1]);
}

TEST_CASE("Fetch cursor: ORA error during the fetch", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_ERR"}, {"ITEST_FC_ERR_V"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_ERR (ID NUMBER(10))");
	session.Execute("INSERT /*+ APPEND */ INTO ITEST_FC_ERR SELECT r FROM " + RowGenerator(100000));
	session.Execute("CREATE VIEW ITEST_FC_ERR_V AS SELECT id, 1 / (id - 50000) AS x FROM ITEST_FC_ERR");
	ReadPlan plan(session, "ITEST_FC_ERR_V");
	FetchCursor cursor(session, plan.statement, plan.columns, FetchOptions());
	int64_t rows = 0;
	int32_t code = 0;
	try {
		for (uint32_t n; (n = cursor.Fetch()) > 0;) {
			rows += n;
		}
	} catch (const OracleError &e) {
		code = e.ora_code;
	}
	CHECK(code == 1476);
	CHECK(rows > 0);
	CHECK(rows < 100000);
}

TEST_CASE("Fetch cursor: truncated value", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_TRUNC"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_TRUNC (V VARCHAR2(10))");
	session.Execute("INSERT INTO ITEST_FC_TRUNC VALUES ('abcdefghij')");
	ReadPlan plan(session, "ITEST_FC_TRUNC");
	plan.plans[0].define_bytes = 4; // too small: the value must not come back cut
	FetchCursor cursor(session, plan.statement, plan.columns, FetchOptions());
	int32_t code = 0;
	try {
		cursor.Fetch();
	} catch (const OracleError &e) {
		code = e.ora_code;
	}
	CHECK(code == 1406);
}

TEST_CASE("Fetch cursor: statements released on every path", "[oracle][fetch_cursor]") {
	OciSession session(TestCredentials());
	DropGuard guard {session, {"ITEST_FC_RELEASE"}};
	guard.DropAll();
	session.Execute("CREATE TABLE ITEST_FC_RELEASE (ID NUMBER(10), C CLOB)");
	session.Execute("INSERT /*+ APPEND */ INTO ITEST_FC_RELEASE SELECT r, TO_CLOB('c' || r) FROM " + RowGenerator(100));
	ReadPlan plan(session, "ITEST_FC_RELEASE");
	FetchOptions options;
	options.max_rows = 10;
	SqlStatement missing = plan.statement;
	missing.sql += " WHERE ID = :b1"; // no bind: ORA-01008 at execute, after the defines
	const auto before = SessionStats(session);
	for (int i = 0; i < 30; i++) {
		FetchCursor cursor(session, plan.statement, plan.columns, options);
		CHECK(cursor.Fetch() == 10); // left open after one batch
		CHECK_THROWS_AS(FetchCursor(session, missing, plan.columns, options), OracleError);
	}
	const auto after = SessionStats(session);
	CHECK(after.at("opened cursors current") - before.at("opened cursors current") < 5);
}
