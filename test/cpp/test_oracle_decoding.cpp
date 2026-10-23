#include "catch.hpp"
#include "oraduck/oracle_decoding.hpp"
#include "oraduck/oracle_encoding.hpp"

#include <string>
#include <vector>

using namespace oraduck;

namespace {

// Decimal digits (no sign, no point) -> unsigned 128-bit magnitude.
OracleUint128 ParseMagnitude(const std::string &digits) {
	OracleUint128 value {0, 0};
	for (char c : digits) {
		// value = value * 10 + digit, over 32-bit limbs
		uint32_t limbs[4] = {static_cast<uint32_t>(value.hi >> 32), static_cast<uint32_t>(value.hi),
		                     static_cast<uint32_t>(value.lo >> 32), static_cast<uint32_t>(value.lo)};
		uint64_t carry = static_cast<uint64_t>(c - '0');
		for (int i = 3; i >= 0; i--) {
			const uint64_t current = static_cast<uint64_t>(limbs[i]) * 10 + carry;
			limbs[i] = static_cast<uint32_t>(current);
			carry = current >> 32;
		}
		value.hi = (static_cast<uint64_t>(limbs[0]) << 32) | limbs[1];
		value.lo = (static_cast<uint64_t>(limbs[2]) << 32) | limbs[3];
	}
	return value;
}

OracleInt128 Signed(OracleUint128 m, bool negative) {
	OracleInt128 r {m.lo, static_cast<int64_t>(m.hi)};
	if (negative) { // two's complement negation
		r.lo = ~r.lo + 1;
		r.hi = static_cast<int64_t>(~static_cast<uint64_t>(r.hi) + (r.lo == 0 ? 1 : 0));
	}
	return r;
}

} // namespace

TEST_CASE("NUMBER reference bytes", "[decoding]") {
	const uint8_t zero[] = {0x80}, one[] = {0xC1, 0x02}, minus_one[] = {0x3E, 0x64, 0x66};
	const uint8_t v12345[] = {0xC2, 0x02, 0x18, 0x2E};       // 123.45
	const uint8_t m12345[] = {0x3D, 0x64, 0x4E, 0x38, 0x66}; // -123.45
	const uint8_t half[] = {0xC0, 0x33};                     // 0.5
	int64_t v = 0;
	CHECK(DecodeOracleNumberScaled64(zero, 1, 0, false, v) == NumberDecodeStatus::kOk);
	CHECK(v == 0);
	CHECK(DecodeOracleNumberScaled64(one, 2, 0, false, v) == NumberDecodeStatus::kOk);
	CHECK(v == 1);
	CHECK(DecodeOracleNumberScaled64(minus_one, 3, 0, false, v) == NumberDecodeStatus::kOk);
	CHECK(v == -1);
	CHECK(DecodeOracleNumberScaled64(v12345, 4, 2, false, v) == NumberDecodeStatus::kOk);
	CHECK(v == 12345);
	CHECK(DecodeOracleNumberScaled64(m12345, 5, 2, false, v) == NumberDecodeStatus::kOk);
	CHECK(v == -12345);
	CHECK(DecodeOracleNumberScaled64(v12345, 4, 1, false, v) == NumberDecodeStatus::kInexact);
	CHECK(DecodeOracleNumberScaled64(v12345, 4, 1, true, v) == NumberDecodeStatus::kOk);
	CHECK(v == 1235);
	CHECK(DecodeOracleNumberScaled64(m12345, 5, 1, true, v) == NumberDecodeStatus::kOk);
	CHECK(v == -1235);
	CHECK(DecodeOracleNumberText(half, 2) == "0.5");
	CHECK(DecodeOracleNumberText(m12345, 5) == "-123.45");
	CHECK(DecodeOracleNumberText(zero, 1) == "0");
}

TEST_CASE("NUMBER special values and rounding edges", "[decoding]") {
	const uint8_t pinf[] = {0xFF, 0x65}, ninf[] = {0x00};
	int64_t v = 0;
	OracleInt128 w {0, 0};
	CHECK(DecodeOracleNumberScaled64(pinf, 2, 0, false, v) == NumberDecodeStatus::kInfinity);
	CHECK(DecodeOracleNumberScaled64(ninf, 1, 0, false, v) == NumberDecodeStatus::kInfinity);
	CHECK(DecodeOracleNumberScaled128(pinf, 2, 0, true, w) == NumberDecodeStatus::kInfinity);
	CHECK(DecodeOracleNumberText(pinf, 2) == "Infinity");
	CHECK(DecodeOracleNumberText(ninf, 1) == "-Infinity");
	// 0.04 at scale 1: rounds to 0, inexact without rounding
	uint8_t buf[kOracleNumberMaxBytes];
	size_t n = EncodeOracleNumber(static_cast<uint64_t>(4), false, 2, buf);
	CHECK(DecodeOracleNumberScaled64(buf, n, 1, true, v) == NumberDecodeStatus::kOk);
	CHECK(v == 0);
	CHECK(DecodeOracleNumberScaled64(buf, n, 1, false, v) == NumberDecodeStatus::kInexact);
	// 0.05 at scale 1 rounds away from zero
	n = EncodeOracleNumber(static_cast<uint64_t>(5), true, 2, buf);
	CHECK(DecodeOracleNumberScaled64(buf, n, 1, true, v) == NumberDecodeStatus::kOk);
	CHECK(v == -1);
	// Malformed input throws
	CHECK_THROWS(DecodeOracleNumberText(buf, 0));
}

TEST_CASE("NUMBER round trip with the encoder", "[decoding]") {
	// Each literal is encoded with its own scale, then decoded at that scale. Scaled64 must return
	// kOverflow above int64 and the value otherwise, Scaled128 the exact value, and the text the literal
	// (canonical: no trailing fractional zeros).
	const struct {
		const char *literal;
		const char *canonical; // nullptr: same as literal
		const char *int64_value;
	} cases[] = {
	    {"0", nullptr, "0"},
	    {"1", nullptr, "1"},
	    {"-1", nullptr, "-1"},
	    {"9223372036854775807", nullptr, "9223372036854775807"},
	    {"-9223372036854775807", nullptr, "-9223372036854775807"},
	    {"-9223372036854775808", nullptr, "-9223372036854775808"},
	    {"9223372036854775808", nullptr, nullptr},
	    {"-9223372036854775809", nullptr, nullptr},
	    {"99999999999999999999999999999999999999", nullptr, nullptr},
	    {"-99999999999999999999999999999999999999", nullptr, nullptr},
	    {"12345678901234567890123456789012345678", nullptr, nullptr},
	    {"123.45", nullptr, "12345"},
	    {"-0.05", nullptr, "-5"},
	    {"1234567.123456789", nullptr, "1234567123456789"},
	    {"0.0000000001", nullptr, "1"},
	    {"-0.0000000001", nullptr, "-1"},
	    {"1234567890123456789012345678.1234567890", "1234567890123456789012345678.123456789", nullptr},
	    {"1234567.1234567890", "1234567.123456789", "12345671234567890"},
	};
	for (const auto &c : cases) {
		const std::string literal = c.literal;
		const bool negative = literal[0] == '-';
		std::string digits;
		uint8_t scale = 0;
		bool after_point = false;
		for (char ch : literal) {
			if (ch == '-') {
				continue;
			}
			if (ch == '.') {
				after_point = true;
				continue;
			}
			digits += ch;
			scale = static_cast<uint8_t>(scale + (after_point ? 1 : 0));
		}
		INFO(literal);
		uint8_t buf[kOracleNumberMaxBytes];
		const size_t n = EncodeOracleNumber(ParseMagnitude(digits), negative, scale, buf);
		REQUIRE(n > 0);

		int64_t v = 0;
		const auto status = DecodeOracleNumberScaled64(buf, n, scale, false, v);
		if (c.int64_value == nullptr) {
			CHECK(status == NumberDecodeStatus::kOverflow);
		} else {
			CHECK(status == NumberDecodeStatus::kOk);
			CHECK(v == std::stoll(c.int64_value));
		}

		OracleInt128 w {1, 1};
		CHECK(DecodeOracleNumberScaled128(buf, n, scale, false, w) == NumberDecodeStatus::kOk);
		const OracleInt128 expected = Signed(ParseMagnitude(digits), negative);
		CHECK(w.lo == expected.lo);
		CHECK(w.hi == expected.hi);

		CHECK(DecodeOracleNumberText(buf, n) == std::string(c.canonical ? c.canonical : c.literal));
	}
}

TEST_CASE("NUMBER at the 128-bit limits", "[decoding]") {
	// 2^127 = 170141183460469231731687303715884105728: overflows as positive, is the minimum as negative
	const std::string two127 = "170141183460469231731687303715884105728";
	uint8_t buf[kOracleNumberMaxBytes];
	OracleInt128 w {0, 0};
	size_t n = EncodeOracleNumber(ParseMagnitude(two127), false, 0, buf);
	REQUIRE(n > 0);
	CHECK(DecodeOracleNumberScaled128(buf, n, 0, false, w) == NumberDecodeStatus::kOverflow);
	CHECK(DecodeOracleNumberText(buf, n) == two127);
	n = EncodeOracleNumber(ParseMagnitude(two127), true, 0, buf);
	REQUIRE(n > 0);
	CHECK(DecodeOracleNumberScaled128(buf, n, 0, false, w) == NumberDecodeStatus::kOk);
	CHECK(w.lo == 0);
	CHECK(w.hi == INT64_MIN);
	// 2 * 10^38 > 2^127
	n = EncodeOracleNumber(ParseMagnitude("200000000000000000000000000000000000000"), true, 0, buf);
	REQUIRE(n > 0);
	CHECK(DecodeOracleNumberScaled128(buf, n, 0, false, w) == NumberDecodeStatus::kOverflow);
	// A huge exponent (1e100) overflows without looping
	const uint8_t huge[] = {0xC1 + 50, 0x02};
	CHECK(DecodeOracleNumberScaled128(huge, 2, 0, false, w) == NumberDecodeStatus::kOverflow);
	int64_t v = 0;
	CHECK(DecodeOracleNumberScaled64(huge, 2, 0, false, v) == NumberDecodeStatus::kOverflow);
}

TEST_CASE("DATE and TIMESTAMP", "[decoding]") {
	const uint8_t d[] = {120, 124, 5, 7, 11, 21, 31}; // 2024-05-07 10:20:30
	int64_t micros = 0;
	CHECK(DecodeOracleDate(d, micros));
	CHECK(micros == 1715077230000000LL);
	const uint8_t bc[] = {100, 99, 1, 1, 1, 1, 1}; // Oracle -0001-01-01 = astronomical 0000-01-01
	CHECK(DecodeOracleDate(bc, micros));
	CHECK(micros == -719528LL * 86400 * 1000000);
	const uint8_t bad_month[] = {120, 124, 13, 7, 11, 21, 31};
	const uint8_t bad_day[] = {120, 124, 2, 30, 11, 21, 31};
	const uint8_t bad_hour[] = {120, 124, 5, 7, 26, 21, 31};
	CHECK_FALSE(DecodeOracleDate(bad_month, micros));
	CHECK_FALSE(DecodeOracleDate(bad_day, micros));
	CHECK_FALSE(DecodeOracleDate(bad_hour, micros));
	CHECK(DaysFromCivil(1970, 1, 1) == 0);
	CHECK(DaysFromCivil(2024, 5, 7) == 19850);

	// Round trip through EncodeOracleTimestamp: (days, micros of day) -> bytes -> micros
	const struct {
		int64_t year;
		uint32_t month, day;
		int64_t micros_of_day;
		size_t len;
	} cases[] = {
	    {1, 1, 1, 0, 7},
	    {1969, 12, 31, 86399999999LL, 11},
	    {9999, 12, 31, 86399999999LL, 11},
	    {2024, 5, 7, 37230000000LL, 7},
	    {2024, 5, 7, 37230123456LL, 11},
	};
	for (const auto &c : cases) {
		const int64_t days = DaysFromCivil(c.year, c.month, c.day);
		uint8_t buf[kOracleTimestampBytes];
		const size_t len = EncodeOracleTimestamp(days, c.micros_of_day, buf);
		REQUIRE(len == c.len);
		CHECK(DecodeOracleTimestamp(buf, len, micros));
		CHECK(micros == days * kMicrosPerDay + c.micros_of_day);
		// The 7-byte prefix is a DATE: seconds only
		CHECK(DecodeOracleDate(buf, micros));
		CHECK(micros == days * kMicrosPerDay + c.micros_of_day / kMicrosPerSecond * kMicrosPerSecond);
	}
	// 11 bytes with 123456789 ns: truncated to 123456 microseconds
	uint8_t ts[kOracleTimestampBytes] = {120, 124, 5, 7, 11, 21, 31, 0, 0, 0, 0};
	const uint32_t nanos = 123456789;
	ts[7] = static_cast<uint8_t>(nanos >> 24);
	ts[8] = static_cast<uint8_t>(nanos >> 16);
	ts[9] = static_cast<uint8_t>(nanos >> 8);
	ts[10] = static_cast<uint8_t>(nanos);
	CHECK(DecodeOracleTimestamp(ts, 11, micros));
	CHECK(micros == 1715077230123456LL);
	CHECK(DecodeOracleTimestamp(ts, 7, micros));
	CHECK(micros == 1715077230000000LL);
	CHECK_FALSE(DecodeOracleTimestamp(ts, 8, micros));
}
