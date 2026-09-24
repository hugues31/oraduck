#pragma once

#include <cstddef>
#include <cstdint>

namespace oraduck {

// Unsigned 128-bit magnitude of a DECIMAL with more than 18 digits (portable: MSVC has no __int128).
struct OracleUint128 {
	uint64_t hi;
	uint64_t lo;
};

constexpr size_t kOracleNumberMaxBytes = 22;
constexpr size_t kOracleDateBytes = 7;
constexpr size_t kOracleTimestampBytes = 11;
constexpr size_t kDateTimeTextBytes = 26; // "YYYY-MM-DD HH:MM:SS.ffffff"
constexpr int64_t kMicrosPerSecond = 1000000;
constexpr int64_t kMicrosPerDay = 86400LL * kMicrosPerSecond;

// Encodes (negative ? -1 : 1) * magnitude * 10^-scale in the internal NUMBER format (SQLT_NUM).
// out must hold kOracleNumberMaxBytes bytes. Returns the number of bytes written,
// or 0 when the value exceeds 20 base-100 digits (not representable).
size_t EncodeOracleNumber(uint64_t magnitude, bool negative, uint8_t scale, uint8_t *out);
size_t EncodeOracleNumber(OracleUint128 magnitude, bool negative, uint8_t scale, uint8_t *out);

// Microseconds since 1970-01-01 -> days and microseconds of the day (floor division).
void SplitMicros(int64_t micros, int64_t &days, int64_t &micros_of_day);

// Proleptic Gregorian calendar date from days since 1970-01-01.
void CivilFromDays(int64_t days, int64_t &year, uint32_t &month, uint32_t &day);

// Oracle internal DATE (SQLT_DAT, 7 bytes), seconds truncated.
// Returns false when the year is outside 1..9999.
bool EncodeOracleDate(int64_t days, int64_t micros_of_day, uint8_t *out);

// Oracle internal TIMESTAMP (DATE then big-endian nanoseconds) in its canonical form:
// 7 bytes without fractional seconds, 11 otherwise. Returns the length, 0 when the year is outside 1..9999.
size_t EncodeOracleTimestamp(int64_t days, int64_t micros_of_day, uint8_t *out);

// Text "YYYY-MM-DD HH:MM:SS.ffffff" (26 characters, not terminated);
// the first 19 characters form "YYYY-MM-DD HH:MM:SS".
bool FormatDateTimeText(int64_t days, int64_t micros_of_day, char *out);

} // namespace oraduck
