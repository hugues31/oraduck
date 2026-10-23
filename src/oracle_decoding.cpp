#include "oraduck/oracle_decoding.hpp"

#include "oraduck/errors.hpp"
#include "oraduck/oracle_encoding.hpp"

namespace oraduck {

namespace {

// A NUMBER as decimal digits: value = 0.DIGITS * 10^point (point = digits before the decimal point,
// negative or zero for leading fractional zeros). digits has no leading or trailing zero; empty = zero.
struct DecodedNumber {
	bool negative = false;
	bool infinity = false;
	std::string digits;
	int point = 0;
};

DecodedNumber ParseNumber(const uint8_t *bytes, size_t len) {
	DecodedNumber r;
	if (len == 0 || len > kOracleNumberMaxBytes) {
		throw OraduckError("malformed Oracle NUMBER (length " + std::to_string(len) + ")");
	}
	if (len == 1 && bytes[0] == 0x80) {
		return r;
	}
	if (len == 1 && bytes[0] == 0x00) {
		r.negative = r.infinity = true;
		return r;
	}
	if (len == 2 && bytes[0] == 0xFF && bytes[1] == 0x65) {
		r.infinity = true;
		return r;
	}
	r.negative = bytes[0] < 0x80;
	const int exponent = r.negative ? 0x3E - bytes[0] : bytes[0] - 0xC1;
	size_t count = len - 1;
	if (r.negative && count > 0 && bytes[len - 1] == 102) {
		count--; // terminator of a negative mantissa shorter than 20 digits
	}
	if (count == 0 || count > 20) {
		throw OraduckError("malformed Oracle NUMBER (mantissa)");
	}
	std::string digits;
	for (size_t i = 0; i < count; i++) {
		const int d = r.negative ? 101 - bytes[1 + i] : bytes[1 + i] - 1;
		if (d < 0 || d > 99) {
			throw OraduckError("malformed Oracle NUMBER (digit)");
		}
		digits += static_cast<char>('0' + d / 10);
		digits += static_cast<char>('0' + d % 10);
	}
	int point = 2 * (exponent + 1);
	size_t first = 0;
	while (first < digits.size() && digits[first] == '0') {
		first++;
		point--;
	}
	size_t last = digits.size();
	while (last > first && digits[last - 1] == '0') {
		last--;
	}
	r.digits = digits.substr(first, last - first);
	r.point = point;
	if (r.digits.empty()) {
		r.negative = false;
	}
	return r;
}

// Unsigned 128-bit accumulator (portable: MSVC has no __int128).
struct Acc {
	uint64_t hi = 0;
	uint64_t lo = 0;

	// this = this * 10 + digit; false on overflow of 128 bits
	bool MulAdd10(uint32_t digit) {
		const uint64_t low = (lo & 0xFFFFFFFFULL) * 10 + digit;
		const uint64_t high = (lo >> 32) * 10 + (low >> 32);
		const uint64_t carry = high >> 32;
		if (hi > (UINT64_MAX - carry) / 10) {
			return false;
		}
		hi = hi * 10 + carry;
		lo = (high << 32) | (low & 0xFFFFFFFFULL);
		return true;
	}
	bool Increment() {
		if (++lo == 0) {
			if (++hi == 0) {
				return false;
			}
		}
		return true;
	}
};

// value * 10^scale as an unsigned magnitude with the given bit capacity (64 or 128).
NumberDecodeStatus ScaledMagnitude(const DecodedNumber &n, uint8_t scale, bool round, Acc &acc) {
	if (n.infinity) {
		return NumberDecodeStatus::kInfinity;
	}
	if (n.digits.empty()) {
		return NumberDecodeStatus::kOk;
	}
	const int keep = n.point + scale; // digits kept, the rest is the fractional remainder
	if (keep > 40) {
		return NumberDecodeStatus::kOverflow;
	}
	const int size = static_cast<int>(n.digits.size());
	for (int i = 0; i < keep; i++) {
		if (!acc.MulAdd10(i < size ? static_cast<uint32_t>(n.digits[i] - '0') : 0)) {
			return NumberDecodeStatus::kOverflow;
		}
	}
	if (keep >= size) {
		return NumberDecodeStatus::kOk;
	}
	// Digits remain (the last digit is non zero, so the remainder is non zero)
	if (!round) {
		return NumberDecodeStatus::kInexact;
	}
	const bool up = keep >= 0 ? n.digits[keep] >= '5' : false; // keep < 0: value below 0.1 unit
	if (up && !acc.Increment()) {
		return NumberDecodeStatus::kOverflow;
	}
	return NumberDecodeStatus::kOk;
}

} // namespace

NumberDecodeStatus DecodeOracleNumberScaled64(const uint8_t *bytes, size_t len, uint8_t scale, bool round,
                                              int64_t &out) {
	const DecodedNumber n = ParseNumber(bytes, len);
	Acc acc;
	const auto status = ScaledMagnitude(n, scale, round, acc);
	if (status != NumberDecodeStatus::kOk) {
		return status;
	}
	const uint64_t limit = static_cast<uint64_t>(INT64_MAX) + (n.negative ? 1 : 0);
	if (acc.hi != 0 || acc.lo > limit) {
		return NumberDecodeStatus::kOverflow;
	}
	out = n.negative ? static_cast<int64_t>(~acc.lo + 1) : static_cast<int64_t>(acc.lo);
	return NumberDecodeStatus::kOk;
}

NumberDecodeStatus DecodeOracleNumberScaled128(const uint8_t *bytes, size_t len, uint8_t scale, bool round,
                                               OracleInt128 &out) {
	const DecodedNumber n = ParseNumber(bytes, len);
	Acc acc;
	const auto status = ScaledMagnitude(n, scale, round, acc);
	if (status != NumberDecodeStatus::kOk) {
		return status;
	}
	const uint64_t top = static_cast<uint64_t>(1) << 63; // 2^127 is the magnitude of the minimum
	if (acc.hi > top || (acc.hi == top && (!n.negative || acc.lo != 0))) {
		return NumberDecodeStatus::kOverflow;
	}
	uint64_t lo = acc.lo, hi = acc.hi;
	if (n.negative) { // two's complement negation
		lo = ~lo + 1;
		hi = ~hi + (lo == 0 ? 1 : 0);
	}
	out.lo = lo;
	out.hi = static_cast<int64_t>(hi);
	return NumberDecodeStatus::kOk;
}

std::string DecodeOracleNumberText(const uint8_t *bytes, size_t len) {
	const DecodedNumber n = ParseNumber(bytes, len);
	if (n.infinity) {
		return n.negative ? "-Infinity" : "Infinity";
	}
	if (n.digits.empty()) {
		return "0";
	}
	std::string text = n.negative ? "-" : "";
	const int size = static_cast<int>(n.digits.size());
	if (n.point <= 0) {
		text += "0.";
		text.append(static_cast<size_t>(-n.point), '0');
		text += n.digits;
	} else if (n.point >= size) {
		text += n.digits;
		text.append(static_cast<size_t>(n.point - size), '0');
	} else {
		text += n.digits.substr(0, static_cast<size_t>(n.point));
		text += '.';
		text += n.digits.substr(static_cast<size_t>(n.point));
	}
	return text;
}

// Howard Hinnant's "days_from_civil" algorithm
int64_t DaysFromCivil(int64_t year, uint32_t month, uint32_t day) {
	year -= month <= 2 ? 1 : 0;
	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const uint32_t yoe = static_cast<uint32_t>(year - era * 400);
	const uint32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool DecodeOracleDate(const uint8_t *bytes7, int64_t &micros) {
	const int64_t oracle_year = (static_cast<int64_t>(bytes7[0]) - 100) * 100 + (static_cast<int64_t>(bytes7[1]) - 100);
	if (oracle_year == 0 || oracle_year < -4712 || oracle_year > 9999) {
		return false;
	}
	const int64_t year = oracle_year < 0 ? oracle_year + 1 : oracle_year;
	const uint32_t month = bytes7[2], day = bytes7[3];
	const int64_t hour = static_cast<int64_t>(bytes7[4]) - 1, minute = static_cast<int64_t>(bytes7[5]) - 1,
	              second = static_cast<int64_t>(bytes7[6]) - 1;
	if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
	    second < 0 || second > 59) {
		return false;
	}
	const int64_t days = DaysFromCivil(year, month, day);
	int64_t check_year = 0;
	uint32_t check_month = 0, check_day = 0;
	CivilFromDays(days, check_year, check_month, check_day);
	if (check_year != year || check_month != month || check_day != day) {
		return false; // e.g. February 30
	}
	micros = days * kMicrosPerDay + ((hour * 60 + minute) * 60 + second) * kMicrosPerSecond;
	return true;
}

bool DecodeOracleTimestamp(const uint8_t *bytes, size_t len, int64_t &micros) {
	if (len != kOracleDateBytes && len != kOracleTimestampBytes) {
		return false;
	}
	int64_t value = 0;
	if (!DecodeOracleDate(bytes, value)) {
		return false;
	}
	if (len == kOracleTimestampBytes) {
		const uint32_t nanos = (static_cast<uint32_t>(bytes[7]) << 24) | (static_cast<uint32_t>(bytes[8]) << 16) |
		                       (static_cast<uint32_t>(bytes[9]) << 8) | bytes[10];
		if (nanos >= 1000000000u) {
			return false;
		}
		value += nanos / 1000;
	}
	micros = value;
	return true;
}

} // namespace oraduck
