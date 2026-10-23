#include "oraduck/sql_binds.hpp"

#include "oraduck/errors.hpp"
#include "oraduck/oracle_encoding.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace oraduck {

namespace {

// Decimal text of magnitude * 10^-scale (long division by 10 over four 32-bit limbs).
std::string DecimalText(OracleUint128 magnitude, bool negative, uint8_t scale) {
	uint32_t limbs[4] = {static_cast<uint32_t>(magnitude.hi >> 32), static_cast<uint32_t>(magnitude.hi),
	                     static_cast<uint32_t>(magnitude.lo >> 32), static_cast<uint32_t>(magnitude.lo)};
	std::string digits; // least significant first
	while ((limbs[0] | limbs[1] | limbs[2] | limbs[3]) != 0) {
		uint64_t remainder = 0;
		for (auto &limb : limbs) {
			const uint64_t current = (remainder << 32) | limb;
			limb = static_cast<uint32_t>(current / 10);
			remainder = current % 10;
		}
		digits.push_back(static_cast<char>('0' + remainder));
	}
	while (digits.size() <= scale) {
		digits.push_back('0');
	}
	std::string out;
	if (negative) {
		out.push_back('-');
	}
	for (size_t i = digits.size(); i-- > 0;) {
		out.push_back(digits[i]);
		if (i == scale && scale > 0) {
			out.push_back('.');
		}
	}
	return out;
}

BindValue NumberBind(OracleUint128 magnitude, bool negative, uint8_t scale) {
	uint8_t buf[kOracleNumberMaxBytes];
	const size_t n = EncodeOracleNumber(magnitude, negative, scale, buf);
	if (n == 0) {
		throw OraduckError("value not representable as an Oracle NUMBER bind");
	}
	BindValue bind;
	bind.dty = SQLT_NUM;
	bind.bytes.assign(buf, buf + n);
	bind.display = DecimalText(magnitude, negative, scale);
	return bind;
}

} // namespace

BindValue BindNumber(int64_t value) {
	const bool negative = value < 0;
	const uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
	return NumberBind({0, magnitude}, negative, 0);
}

BindValue BindDecimal(const OracleInt128 &value, uint8_t scale) {
	const bool negative = value.hi < 0;
	OracleUint128 magnitude {static_cast<uint64_t>(value.hi), value.lo};
	if (negative) {
		magnitude.lo = ~value.lo + 1;
		magnitude.hi = ~static_cast<uint64_t>(value.hi) + (magnitude.lo == 0 ? 1 : 0);
	}
	return NumberBind(magnitude, negative, scale);
}

std::string SqlStringLiteral(const std::string &value) {
	std::string out = "'";
	for (char c : value) {
		if (c == '\'') {
			out.push_back('\'');
		}
		out.push_back(c);
	}
	out.push_back('\'');
	return out;
}

BindValue BindText(const std::string &value) {
	BindValue bind;
	bind.dty = SQLT_CHR;
	bind.bytes.assign(value.begin(), value.end());
	bind.display = SqlStringLiteral(value);
	return bind;
}

// A 4-byte length then the bytes: unlike SQLT_CHR, whose input values lose their trailing blanks, the value reaches
// Oracle unchanged and compares with non-padded semantics (as VARCHAR2 up to 4,000 bytes)
BindValue BindVarchar(const std::string &value) {
	if (value.size() > static_cast<size_t>(INT32_MAX) - sizeof(int32_t)) {
		throw OraduckError("text too long for a bind");
	}
	BindValue bind;
	bind.dty = SQLT_LVC;
	const int32_t length = static_cast<int32_t>(value.size());
	bind.bytes.resize(sizeof(length) + value.size());
	std::memcpy(bind.bytes.data(), &length, sizeof(length));
	if (!value.empty()) {
		std::memcpy(bind.bytes.data() + sizeof(length), value.data(), value.size());
	}
	bind.display = SqlStringLiteral(value);
	return bind;
}

BindValue BindDate(int64_t micros) {
	int64_t days, micros_of_day;
	SplitMicros(micros, days, micros_of_day);
	uint8_t buf[kOracleDateBytes];
	if (!EncodeOracleDate(days, micros_of_day, buf)) {
		throw OraduckError("date outside the Oracle DATE range");
	}
	char text[kDateTimeTextBytes];
	FormatDateTimeText(days, micros_of_day, text);
	BindValue bind;
	bind.dty = SQLT_DAT;
	bind.bytes.assign(buf, buf + kOracleDateBytes);
	if (micros_of_day / kMicrosPerSecond == 0) {
		bind.display = "DATE '" + std::string(text, 10) + "'";
	} else {
		bind.display = "TO_DATE('" + std::string(text, 19) + "', 'YYYY-MM-DD HH24:MI:SS')";
	}
	return bind;
}

BindValue BindDouble(double value) {
	BindValue bind;
	bind.dty = SQLT_BDOUBLE;
	bind.bytes.resize(sizeof(double));
	std::memcpy(bind.bytes.data(), &value, sizeof(double));
	char text[40];
	std::snprintf(text, sizeof(text), "%.17g", value);
	bind.display = std::string(text) + "d";
	return bind;
}

} // namespace oraduck
