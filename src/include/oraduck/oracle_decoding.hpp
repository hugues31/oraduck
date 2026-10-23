#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace oraduck {

// Signed 128-bit integer, two's complement; same layout as duckdb::hugeint_t (lower then upper half).
struct OracleInt128 {
	uint64_t lo;
	int64_t hi;
};

enum class NumberDecodeStatus { kOk, kOverflow, kInexact, kInfinity };

// Decodes an internal NUMBER (SQLT_NUM) into value * 10^scale.
// kInexact: digits remain beyond scale and round is false (out is left unspecified).
// With round, the value is rounded half away from zero. kOverflow: out of the integer range.
// kInfinity: Oracle +/-infinity. Throws OraduckError on malformed bytes.
NumberDecodeStatus DecodeOracleNumberScaled64(const uint8_t *bytes, size_t len, uint8_t scale, bool round,
                                              int64_t &out);
NumberDecodeStatus DecodeOracleNumberScaled128(const uint8_t *bytes, size_t len, uint8_t scale, bool round,
                                               OracleInt128 &out);

// Canonical decimal text: no exponent, no trailing fractional zeros, "0" before the point;
// "Infinity" / "-Infinity" for Oracle's infinities. Throws OraduckError on malformed bytes.
std::string DecodeOracleNumberText(const uint8_t *bytes, size_t len);

// Proleptic Gregorian calendar, astronomical year numbering: days since 1970-01-01.
int64_t DaysFromCivil(int64_t year, uint32_t month, uint32_t day);

// Oracle internal DATE (7 bytes) -> microseconds since 1970-01-01. Fields are taken as is (no Julian
// conversion); an Oracle BC year Y (negative) becomes astronomical year Y + 1. False on invalid bytes.
bool DecodeOracleDate(const uint8_t *bytes7, int64_t &micros);

// Oracle internal TIMESTAMP (len 7 or 11), nanoseconds truncated to microseconds. False on invalid bytes.
bool DecodeOracleTimestamp(const uint8_t *bytes, size_t len, int64_t &micros);

} // namespace oraduck
