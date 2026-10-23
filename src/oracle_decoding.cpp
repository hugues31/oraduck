#include "oraduck/oracle_decoding.hpp"

#include "oraduck/errors.hpp"
#include "oraduck/oracle_encoding.hpp"

namespace oraduck {

namespace {

constexpr uint64_t kPow10[19] = {1ULL,
                                 10ULL,
                                 100ULL,
                                 1000ULL,
                                 10000ULL,
                                 100000ULL,
                                 1000000ULL,
                                 10000000ULL,
                                 100000000ULL,
                                 1000000000ULL,
                                 10000000000ULL,
                                 100000000000ULL,
                                 1000000000000ULL,
                                 10000000000000ULL,
                                 100000000000000ULL,
                                 1000000000000000ULL,
                                 10000000000000000ULL,
                                 100000000000000000ULL,
                                 1000000000000000000ULL};
// A NUMBER parsed in place, without allocation. The mantissa is kept as base-100 digits ("pairs"); the
// decimal digit stream is the pairs read as two digits each. value = 0.DIGITS * 10^point, where DIGITS
// are stream digits [lead, lead + size): no leading and no trailing zero digit. zero: size == 0.
struct ParsedNumber {
	bool negative = false;
	bool infinity = false;
	bool zero = false;
	int point = 0;
	int lead = 0;
	int size = 0;
	uint8_t pairs[20];

	// i-th digit of the stream
	uint32_t StreamDigit(int i) const {
		const uint32_t pair = pairs[i >> 1];
		return (i & 1) ? pair % 10 : pair / 10;
	}
	// Value of the stream digits [pos, pos + digits), digits <= 18. Whole pairs are combined in
	// independent groups so that the multiplications do not form one long dependency chain.
	uint64_t ChunkValue(int pos, int digits) const {
		const int end = pos + digits;
		uint64_t value = 0;
		if (pos & 1) {
			value = pairs[pos >> 1] % 10;
			pos++;
		}
		const uint8_t *p = pairs + (pos >> 1);
		int count = (end - pos) >> 1;
		pos += 2 * count;
		uint64_t whole = 0;
		const int whole_pairs = count;
		while (count >= 4) {
			const uint64_t group = (static_cast<uint64_t>(p[0]) * 100 + p[1]) * 10000 + (p[2] * 100 + p[3]);
			whole = whole * 100000000ULL + group;
			p += 4;
			count -= 4;
		}
		for (; count > 0; count--) {
			whole = whole * 100 + *p++;
		}
		value = value * kPow10[2 * whole_pairs] + whole;
		if (pos < end) {
			value = value * 10 + pairs[pos >> 1] / 10;
		}
		return value;
	}
	// i-th significant digit
	uint32_t Digit(int i) const {
		return StreamDigit(lead + i);
	}
};

void ParseNumber(const uint8_t *bytes, size_t len, ParsedNumber &r) {
	if (len == 0 || len > kOracleNumberMaxBytes) {
		throw OraduckError("malformed Oracle NUMBER (length " + std::to_string(len) + ")");
	}
	if (bytes[0] == 0x80) {
		if (len != 1) {
			throw OraduckError("malformed Oracle NUMBER (zero with digits)");
		}
		r.zero = true;
		return;
	}
	if (len == 1 && bytes[0] == 0x00) {
		r.negative = r.infinity = true;
		return;
	}
	if (len == 2 && bytes[0] == 0xFF && bytes[1] == 0x65) {
		r.infinity = true;
		return;
	}
	r.negative = bytes[0] < 0x80;
	const int exponent = r.negative ? 0x3E - bytes[0] : bytes[0] - 0xC1;
	int count = static_cast<int>(len) - 1;
	if (r.negative && count > 0 && bytes[len - 1] == 102) {
		count--; // terminator of a negative mantissa shorter than 20 digits
	}
	if (count == 0 || count > 20) {
		throw OraduckError("malformed Oracle NUMBER (mantissa)");
	}
	// Digit: b - 1 (positive) or 101 - b (negative), branch free on the sign; out of 0..99 is malformed
	const unsigned offset = r.negative ? 102u : 1u;
	const unsigned flip = r.negative ? 0xFFu : 0u; // 101 - b == ~(b - 102) over 8 bits
	unsigned bad = 0;
	for (int i = 0; i < count; i++) {
		const unsigned d = ((bytes[1 + i] - offset) ^ flip) & 0xFFu;
		bad |= d > 99 ? 1u : 0u;
		r.pairs[i] = static_cast<uint8_t>(d);
	}
	if (bad) {
		throw OraduckError("malformed Oracle NUMBER (digit)");
	}
	int first = 0;
	while (first < count && r.pairs[first] == 0) {
		first++;
	}
	if (first == count) { // all digits are zero
		r.zero = true;
		r.negative = false;
		return;
	}
	while (r.pairs[count - 1] == 0) {
		count--;
	}
	r.lead = 2 * first + (r.pairs[first] < 10 ? 1 : 0);
	r.size = 2 * count - (r.pairs[count - 1] % 10 == 0 ? 1 : 0) - r.lead;
	r.point = 2 * (exponent + 1) - r.lead;
}

constexpr int kChunkDigits = 18; // a chunk of up to 18 digits fits a uint64_t

// 64 x 64 -> 128 bits, portable
void Mul64(uint64_t a, uint64_t b, uint64_t &hi, uint64_t &lo) {
	const uint64_t mask = 0xFFFFFFFFULL;
	const uint64_t a0 = a & mask, a1 = a >> 32, b0 = b & mask, b1 = b >> 32;
	const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	const uint64_t mid = (p00 >> 32) + (p01 & mask) + (p10 & mask);
	hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
	lo = (mid << 32) | (p00 & mask);
}

// Unsigned 128-bit accumulator (portable: MSVC has no __int128).
struct Acc {
	uint64_t hi = 0;
	uint64_t lo = 0;

	// this = this * multiplier + addend; false on overflow of 128 bits
	bool MulAdd(uint64_t multiplier, uint64_t addend) {
		if (hi == 0) {
			if (lo == 0) {
				lo = addend;
				return true;
			}
			uint64_t p_hi, p_lo;
			Mul64(lo, multiplier, p_hi, p_lo);
			hi = p_hi;
			lo = p_lo + addend;
			if (lo < addend && ++hi == 0) {
				return false;
			}
			return true;
		}
		uint64_t h_hi, h_lo, p_hi, p_lo;
		Mul64(hi, multiplier, h_hi, h_lo);
		if (h_hi != 0) {
			return false;
		}
		Mul64(lo, multiplier, p_hi, p_lo);
		const uint64_t new_hi = h_lo + p_hi;
		if (new_hi < h_lo) {
			return false;
		}
		const uint64_t new_lo = p_lo + addend;
		if (new_lo < addend && new_hi + 1 == 0) {
			return false;
		}
		hi = new_hi + (new_lo < addend ? 1 : 0);
		lo = new_lo;
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

// value * 10^scale as an unsigned 128-bit magnitude (the caller applies the sign and its own range).
NumberDecodeStatus ScaledMagnitude(const ParsedNumber &n, uint8_t scale, bool round, Acc &acc) {
	if (n.infinity) {
		return NumberDecodeStatus::kInfinity;
	}
	if (n.zero) {
		return NumberDecodeStatus::kOk;
	}
	const int keep = n.point + scale; // digits kept, the rest is the fractional remainder
	if (keep > 40) {
		return NumberDecodeStatus::kOverflow;
	}
	// Digits [lead, lead + taken) of the stream go into the accumulator, base 100 where aligned
	const int taken = keep < n.size ? (keep > 0 ? keep : 0) : n.size;
	// In chunks of up to 18 digits, each one a uint64_t
	for (int pos = n.lead, end = n.lead + taken; pos < end;) {
		const int digits = end - pos < kChunkDigits ? end - pos : kChunkDigits;
		if (!acc.MulAdd(kPow10[digits], n.ChunkValue(pos, digits))) {
			return NumberDecodeStatus::kOverflow;
		}
		pos += digits;
	}
	// Trailing zeros of the integer part
	for (int pad = keep - taken; pad > 0;) {
		const int step = pad < kChunkDigits ? pad : kChunkDigits;
		if (!acc.MulAdd(kPow10[step], 0)) {
			return NumberDecodeStatus::kOverflow;
		}
		pad -= step;
	}
	if (keep >= n.size) {
		return NumberDecodeStatus::kOk;
	}
	// Digits remain (the last digit is non zero, so the remainder is non zero)
	if (!round) {
		return NumberDecodeStatus::kInexact;
	}
	const bool up = keep >= 0 ? n.Digit(keep) >= 5 : false; // keep < 0: value below 0.1 unit
	if (up && !acc.Increment()) {
		return NumberDecodeStatus::kOverflow;
	}
	return NumberDecodeStatus::kOk;
}

} // namespace

NumberDecodeStatus DecodeOracleNumberScaled64(const uint8_t *bytes, size_t len, uint8_t scale, bool round,
                                              int64_t &out) {
	ParsedNumber n;
	ParseNumber(bytes, len, n);
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
	ParsedNumber n;
	ParseNumber(bytes, len, n);
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
	ParsedNumber n;
	ParseNumber(bytes, len, n);
	if (n.infinity) {
		return n.negative ? "-Infinity" : "Infinity";
	}
	if (n.zero) {
		return "0";
	}
	std::string text;
	text.reserve(static_cast<size_t>(n.size) + (n.point < 0 ? static_cast<size_t>(-n.point) : 0) + 4);
	if (n.negative) {
		text += '-';
	}
	auto append_digits = [&](int from, int to) {
		for (int i = from; i < to; i++) {
			text += static_cast<char>('0' + n.Digit(i));
		}
	};
	if (n.point <= 0) {
		text += "0.";
		text.append(static_cast<size_t>(-n.point), '0');
		append_digits(0, n.size);
	} else if (n.point >= n.size) {
		append_digits(0, n.size);
		text.append(static_cast<size_t>(n.point - n.size), '0');
	} else {
		append_digits(0, n.point);
		text += '.';
		append_digits(n.point, n.size);
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
