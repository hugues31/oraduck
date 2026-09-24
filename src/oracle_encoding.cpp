#include "oraduck/oracle_encoding.hpp"

namespace oraduck {

namespace {

// Decimal digits, least significant first.
size_t DecimalDigits(uint64_t magnitude, uint8_t *digits) {
	size_t n = 0;
	while (magnitude != 0) {
		digits[n++] = static_cast<uint8_t>(magnitude % 10);
		magnitude /= 10;
	}
	return n;
}

// Same for 128 bits: long division by 10 over four 32-bit limbs, most significant first.
size_t DecimalDigits(OracleUint128 magnitude, uint8_t *digits) {
	uint32_t limbs[4] = {static_cast<uint32_t>(magnitude.hi >> 32), static_cast<uint32_t>(magnitude.hi),
	                     static_cast<uint32_t>(magnitude.lo >> 32), static_cast<uint32_t>(magnitude.lo)};
	size_t n = 0;
	while ((limbs[0] | limbs[1] | limbs[2] | limbs[3]) != 0) {
		uint64_t remainder = 0;
		for (auto &limb : limbs) {
			const uint64_t current = (remainder << 32) | limb;
			limb = static_cast<uint32_t>(current / 10);
			remainder = current % 10;
		}
		digits[n++] = static_cast<uint8_t>(remainder);
	}
	return n;
}

// dec: decimal digits, least significant first; scale: digits after the decimal point.
size_t EncodeFromDigits(const uint8_t *dec, size_t n, bool negative, uint8_t scale, uint8_t *out) {
	// Align the decimal point on a digit-pair boundary (base 100)
	uint8_t aligned[48];
	size_t m = 0;
	int frac = scale;
	if (frac % 2 != 0) {
		aligned[m++] = 0;
		frac++;
	}
	for (size_t i = 0; i < n; i++) {
		aligned[m++] = dec[i];
	}
	if (m % 2 != 0) {
		aligned[m++] = 0;
	}
	const size_t pairs = m / 2;
	uint8_t base100[24];
	for (size_t i = 0; i < pairs; i++) {
		base100[i] = static_cast<uint8_t>(aligned[2 * i] + 10 * aligned[2 * i + 1]);
	}
	size_t hi = pairs;
	while (hi > 0 && base100[hi - 1] == 0) {
		hi--;
	}
	size_t lo = 0;
	while (lo < hi && base100[lo] == 0) {
		lo++;
	}
	const size_t mantissa = hi - lo;
	if (mantissa > 20) {
		return 0;
	}
	const int exponent = static_cast<int>(hi) - 1 - frac / 2;
	size_t len = 0;
	if (!negative) {
		out[len++] = static_cast<uint8_t>(0xC1 + exponent);
		for (size_t i = hi; i-- > lo;) {
			out[len++] = static_cast<uint8_t>(base100[i] + 1);
		}
	} else {
		out[len++] = static_cast<uint8_t>(0x3E - exponent);
		for (size_t i = hi; i-- > lo;) {
			out[len++] = static_cast<uint8_t>(101 - base100[i]);
		}
		if (mantissa < 20) {
			out[len++] = 102;
		}
	}
	return len;
}

inline void Put2(char *p, uint32_t v) {
	p[0] = static_cast<char>('0' + v / 10);
	p[1] = static_cast<char>('0' + v % 10);
}

} // namespace

size_t EncodeOracleNumber(uint64_t magnitude, bool negative, uint8_t scale, uint8_t *out) {
	if (magnitude == 0) {
		out[0] = 0x80;
		return 1;
	}
	uint8_t dec[24];
	const size_t n = DecimalDigits(magnitude, dec);
	return EncodeFromDigits(dec, n, negative, scale, out);
}

size_t EncodeOracleNumber(OracleUint128 magnitude, bool negative, uint8_t scale, uint8_t *out) {
	if (magnitude.hi == 0 && magnitude.lo == 0) {
		out[0] = 0x80;
		return 1;
	}
	uint8_t dec[48];
	const size_t n = DecimalDigits(magnitude, dec);
	return EncodeFromDigits(dec, n, negative, scale, out);
}

void SplitMicros(int64_t micros, int64_t &days, int64_t &micros_of_day) {
	days = micros / kMicrosPerDay;
	micros_of_day = micros % kMicrosPerDay;
	if (micros_of_day < 0) {
		micros_of_day += kMicrosPerDay;
		days -= 1;
	}
}

// Howard Hinnant's "civil_from_days" algorithm
void CivilFromDays(int64_t days, int64_t &year, uint32_t &month, uint32_t &day) {
	const int64_t z = days + 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
	const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const uint32_t mp = (5 * doy + 2) / 153;
	day = doy - (153 * mp + 2) / 5 + 1;
	month = mp < 10 ? mp + 3 : mp - 9;
	year = static_cast<int64_t>(yoe) + era * 400 + (month <= 2 ? 1 : 0);
}

bool EncodeOracleDate(int64_t days, int64_t micros_of_day, uint8_t *out) {
	int64_t year = 0;
	uint32_t month = 0, day = 0;
	CivilFromDays(days, year, month, day);
	if (year < 1 || year > 9999) {
		return false;
	}
	const int64_t seconds = micros_of_day / kMicrosPerSecond;
	out[0] = static_cast<uint8_t>(year / 100 + 100);
	out[1] = static_cast<uint8_t>(year % 100 + 100);
	out[2] = static_cast<uint8_t>(month);
	out[3] = static_cast<uint8_t>(day);
	out[4] = static_cast<uint8_t>(seconds / 3600 + 1);
	out[5] = static_cast<uint8_t>(seconds / 60 % 60 + 1);
	out[6] = static_cast<uint8_t>(seconds % 60 + 1);
	return true;
}

size_t EncodeOracleTimestamp(int64_t days, int64_t micros_of_day, uint8_t *out) {
	if (!EncodeOracleDate(days, micros_of_day, out)) {
		return 0;
	}
	const uint32_t nanos = static_cast<uint32_t>(micros_of_day % kMicrosPerSecond) * 1000u;
	if (nanos == 0) {
		return kOracleDateBytes; // Oracle's canonical form, equal to SQL literals
	}
	out[7] = static_cast<uint8_t>(nanos >> 24);
	out[8] = static_cast<uint8_t>((nanos >> 16) & 0xFF);
	out[9] = static_cast<uint8_t>((nanos >> 8) & 0xFF);
	out[10] = static_cast<uint8_t>(nanos & 0xFF);
	return kOracleTimestampBytes;
}

bool FormatDateTimeText(int64_t days, int64_t micros_of_day, char *out) {
	int64_t year = 0;
	uint32_t month = 0, day = 0;
	CivilFromDays(days, year, month, day);
	if (year < 1 || year > 9999) {
		return false;
	}
	const uint32_t seconds = static_cast<uint32_t>(micros_of_day / kMicrosPerSecond);
	const uint32_t micros = static_cast<uint32_t>(micros_of_day % kMicrosPerSecond);
	Put2(out, static_cast<uint32_t>(year / 100));
	Put2(out + 2, static_cast<uint32_t>(year % 100));
	out[4] = '-';
	Put2(out + 5, month);
	out[7] = '-';
	Put2(out + 8, day);
	out[10] = ' ';
	Put2(out + 11, seconds / 3600);
	out[13] = ':';
	Put2(out + 14, seconds / 60 % 60);
	out[16] = ':';
	Put2(out + 17, seconds % 60);
	out[19] = '.';
	Put2(out + 20, micros / 10000);
	Put2(out + 22, micros / 100 % 100);
	Put2(out + 24, micros % 100);
	return true;
}

} // namespace oraduck
