#include "catch.hpp"
#include "oraduck/oracle_encoding.hpp"

#include <string>
#include <vector>

using namespace oraduck;

namespace {

std::vector<int> Num(uint64_t magnitude, bool negative, uint8_t scale) {
	uint8_t buffer[kOracleNumberMaxBytes];
	const size_t n = EncodeOracleNumber(magnitude, negative, scale, buffer);
	return std::vector<int>(buffer, buffer + n);
}

std::vector<int> Dat(int64_t days, int64_t micros_of_day) {
	uint8_t buffer[kOracleDateBytes];
	REQUIRE(EncodeOracleDate(days, micros_of_day, buffer));
	return std::vector<int>(buffer, buffer + kOracleDateBytes);
}

std::string Text(int64_t days, int64_t micros_of_day) {
	char buffer[kDateTimeTextBytes];
	REQUIRE(FormatDateTimeText(days, micros_of_day, buffer));
	return std::string(buffer, kDateTimeTextBytes);
}

// Days since 1970-01-01 of a few reference dates
constexpr int64_t k20240229 = 19782;
constexpr int64_t k00010101 = -719162;
constexpr int64_t k99991231 = 2932896;

} // namespace

TEST_CASE("NUMBER: integers", "[encoding]") {
	CHECK(Num(0, false, 0) == std::vector<int>{128});
	CHECK(Num(1, false, 0) == std::vector<int>{193, 2});
	CHECK(Num(100, false, 0) == std::vector<int>{194, 2});
	CHECK(Num(123, false, 0) == std::vector<int>{194, 2, 24});
	CHECK(Num(1000000, false, 0) == std::vector<int>{196, 2});
	CHECK(Num(1, true, 0) == std::vector<int>{62, 100, 102});
	CHECK(Num(123, true, 0) == std::vector<int>{61, 100, 78, 102});
	CHECK(Num(9223372036854775807ULL, false, 0) == std::vector<int>{202, 10, 23, 34, 73, 4, 69, 55, 78, 59, 8});
	CHECK(Num(9223372036854775808ULL, true, 0) ==
	      std::vector<int>{53, 92, 79, 68, 29, 98, 33, 47, 24, 43, 93, 102});
	CHECK(Num(18446744073709551615ULL, false, 0) == std::vector<int>{202, 19, 45, 68, 45, 8, 38, 10, 56, 17, 16});
}

TEST_CASE("NUMBER: decimals", "[encoding]") {
	CHECK(Num(5, false, 1) == std::vector<int>{192, 51});           // 0.5
	CHECK(Num(1234, false, 2) == std::vector<int>{193, 13, 35});    // 12.34
	CHECK(Num(12345, false, 2) == std::vector<int>{194, 2, 24, 46}); // 123.45
	CHECK(Num(1, false, 2) == std::vector<int>{192, 2});            // 0.01
	CHECK(Num(1, false, 3) == std::vector<int>{191, 11});           // 0.001
	CHECK(Num(110, false, 2) == std::vector<int>{193, 2, 11});      // 1.10
	CHECK(Num(5, true, 1) == std::vector<int>{63, 51, 102});        // -0.5
	CHECK(Num(0, false, 2) == std::vector<int>{128});               // 0.00
}

TEST_CASE("NUMBER: 128 bits", "[encoding]") {
	const OracleUint128 nines {0x4B3B4CA85A86C47AULL, 0x098A223FFFFFFFFFULL}; // 10^38 - 1: 38 nines
	uint8_t buffer[kOracleNumberMaxBytes];

	size_t n = EncodeOracleNumber(nines, false, 0, buffer);
	REQUIRE(n == 20);
	CHECK(buffer[0] == 211);
	for (size_t i = 1; i < n; i++) {
		CHECK(buffer[i] == 100);
	}

	// -9999999999999999999999999999.9999999999 (DECIMAL(38,10))
	n = EncodeOracleNumber(nines, true, 10, buffer);
	REQUIRE(n == 21);
	CHECK(buffer[0] == 49);
	for (size_t i = 1; i < 20; i++) {
		CHECK(buffer[i] == 2);
	}
	CHECK(buffer[20] == 102);

	n = EncodeOracleNumber(OracleUint128 {0, 123}, false, 0, buffer);
	CHECK(std::vector<int>(buffer, buffer + n) == std::vector<int>{194, 2, 24});

	// 2^64 = 18 44 67 44 07 37 09 55 16 16 (base 100): carries across the two 64-bit halves
	n = EncodeOracleNumber(OracleUint128 {1, 0}, false, 0, buffer);
	CHECK(std::vector<int>(buffer, buffer + n) == std::vector<int>{202, 19, 45, 68, 45, 8, 38, 10, 56, 17, 17});
}

TEST_CASE("SplitMicros and CivilFromDays", "[encoding]") {
	int64_t days = 0, micros = 0;
	SplitMicros(-1, days, micros);
	CHECK(days == -1);
	CHECK(micros == kMicrosPerDay - 1);
	SplitMicros(kMicrosPerDay, days, micros);
	CHECK(days == 1);
	CHECK(micros == 0);

	int64_t year = 0;
	uint32_t month = 0, day = 0;
	CivilFromDays(k20240229, year, month, day);
	CHECK(year == 2024);
	CHECK(month == 2);
	CHECK(day == 29);
	CivilFromDays(k00010101, year, month, day);
	CHECK(year == 1);
	CHECK(month == 1);
	CHECK(day == 1);
}

TEST_CASE("Internal DATE (7 bytes)", "[encoding]") {
	CHECK(Dat(0, 0) == std::vector<int>{119, 170, 1, 1, 1, 1, 1});   // 1970-01-01
	CHECK(Dat(-1, 0) == std::vector<int>{119, 169, 12, 31, 1, 1, 1}); // 1969-12-31
	// 2024-02-29 13:45:07.999999: seconds truncated
	CHECK(Dat(k20240229, 49507999999LL) == std::vector<int>{120, 124, 2, 29, 14, 46, 8});
	CHECK(Dat(k00010101, 0) == std::vector<int>{100, 101, 1, 1, 1, 1, 1});
	CHECK(Dat(k99991231, 0) == std::vector<int>{199, 199, 12, 31, 1, 1, 1});

	uint8_t buffer[kOracleDateBytes];
	CHECK_FALSE(EncodeOracleDate(k00010101 - 1, 0, buffer)); // year 0
	CHECK_FALSE(EncodeOracleDate(k99991231 + 1, 0, buffer)); // year 10000
	CHECK_FALSE(EncodeOracleDate(2147483647LL, 0, buffer));  // DuckDB 'infinity' date
}

TEST_CASE("Internal TIMESTAMP (11 bytes)", "[encoding]") {
	uint8_t buffer[kOracleTimestampBytes];
	REQUIRE(EncodeOracleTimestamp(k20240229, 49507123456LL, buffer) == kOracleTimestampBytes);
	CHECK(std::vector<int>(buffer, buffer + kOracleTimestampBytes) ==
	      std::vector<int>{120, 124, 2, 29, 14, 46, 8, 7, 91, 202, 0});
}

TEST_CASE("Internal TIMESTAMP without fraction: canonical 7-byte form", "[encoding]") {
	// Oracle stores a TIMESTAMP without fraction in 7 bytes; 11 bytes with zero nanoseconds
	// do not compare equal to the canonical form in SQL.
	uint8_t buffer[kOracleTimestampBytes];
	REQUIRE(EncodeOracleTimestamp(k20240229, 49507000000LL, buffer) == kOracleDateBytes);
	CHECK(std::vector<int>(buffer, buffer + kOracleDateBytes) == std::vector<int>{120, 124, 2, 29, 14, 46, 8});
	CHECK(EncodeOracleTimestamp(k99991231 + 1, 0, buffer) == 0);
}

TEST_CASE("Date-time text", "[encoding]") {
	CHECK(Text(k20240229, 49507123456LL) == "2024-02-29 13:45:07.123456");
	CHECK(Text(-1, kMicrosPerDay - 1) == "1969-12-31 23:59:59.999999");
	CHECK(Text(k00010101, 0) == "0001-01-01 00:00:00.000000");
	char buffer[kDateTimeTextBytes];
	CHECK_FALSE(FormatDateTimeText(k99991231 + 1, 0, buffer));
}
