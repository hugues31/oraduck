#include "oraduck/read_types.hpp"

#include "oraduck/errors.hpp"

#include <cctype>
#include <cstdlib>

namespace oraduck {

namespace {

std::string Upper(const std::string &s) {
	std::string out;
	for (char c : s) {
		out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	}
	return out;
}

std::string StripSpaces(const std::string &s) {
	std::string out;
	for (char c : s) {
		if (!std::isspace(static_cast<unsigned char>(c))) {
			out.push_back(c);
		}
	}
	return out;
}

bool StartsWith(const std::string &s, const std::string &prefix) {
	return s.compare(0, prefix.size(), prefix) == 0;
}

[[noreturn]] void ThrowBadUnconstrained(const std::string &text) {
	throw OraduckError("invalid value for the unconstrained NUMBER setting: '" + text +
	                   "' (accepted: DOUBLE, VARCHAR, DECIMAL(p,s) with 1 <= p <= 38 and 0 <= s <= p)");
}

bool ParseUint(const std::string &s, int &out) {
	if (s.empty() || s.size() > 4) {
		return false;
	}
	for (char c : s) {
		if (!std::isdigit(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	out = std::atoi(s.c_str());
	return true;
}

ReadColumnPlan Base(const OracleColumn &c) {
	ReadColumnPlan plan;
	plan.name = c.name;
	plan.select_expr = "\"" + c.name + "\"";
	return plan;
}

// Decimal with the given width and scale: 64-bit decoder up to 18 digits, 128-bit above.
void SetDecimal(ReadColumnPlan &plan, int width, int scale) {
	plan.duck_type = {DuckTypeId::kDecimal, static_cast<uint8_t>(width), static_cast<uint8_t>(scale)};
	plan.decoder = width <= 18 ? FetchDecoder::kNumberDecimal64 : FetchDecoder::kNumberDecimal128;
	plan.dty = SQLT_NUM;
	plan.define_bytes = 22;
	plan.scale = static_cast<uint8_t>(scale);
}

void SetDouble(ReadColumnPlan &plan) {
	plan.duck_type = {DuckTypeId::kDouble, 0, 0};
	plan.decoder = FetchDecoder::kNativeDouble;
	plan.dty = SQLT_BDOUBLE;
	plan.define_bytes = 8;
}

void PlanNumber(ReadColumnPlan &plan, const OracleColumn &c, const UnconstrainedNumber &u, bool is_float) {
	// NUMBER without precision and scale, or FLOAT(b): scale -1 is NULL (NUMBER(p,-1) has a precision)
	if (is_float || (c.precision < 0 && c.scale < 0)) {
		switch (u.kind) {
		case UnconstrainedNumber::Kind::kDouble:
			SetDouble(plan);
			break;
		case UnconstrainedNumber::Kind::kVarchar:
			plan.duck_type = {DuckTypeId::kVarchar, 0, 0};
			plan.decoder = FetchDecoder::kNumberText;
			plan.dty = SQLT_NUM;
			plan.define_bytes = 22;
			break;
		case UnconstrainedNumber::Kind::kDecimal:
			SetDecimal(plan, u.width, u.scale);
			plan.round = true;
			break;
		}
		return;
	}
	const int s = c.scale;
	const int p = c.precision > 0 ? c.precision : 38; // NUMBER(*,s)
	if (s < 0) {
		if (p - s <= 38) {
			SetDecimal(plan, p - s, 0);
		} else {
			SetDouble(plan);
		}
		return;
	}
	if (s > p) {
		if (s <= 38) {
			SetDecimal(plan, s, s);
		} else {
			SetDouble(plan);
		}
		return;
	}
	if (s == 0 && c.precision > 0 && c.precision <= 18) {
		plan.duck_type.id = p <= 4 ? DuckTypeId::kSmallint : (p <= 9 ? DuckTypeId::kInteger : DuckTypeId::kBigint);
		plan.decoder = FetchDecoder::kNativeInt64;
		plan.dty = SQLT_INT;
		plan.define_bytes = 8;
		return;
	}
	SetDecimal(plan, p, s);
}

} // namespace

UnconstrainedNumber ParseUnconstrainedNumber(const std::string &text) {
	const std::string t = Upper(StripSpaces(text));
	UnconstrainedNumber out;
	if (t == "DOUBLE") {
		out.kind = UnconstrainedNumber::Kind::kDouble;
		return out;
	}
	if (t == "VARCHAR") {
		out.kind = UnconstrainedNumber::Kind::kVarchar;
		return out;
	}
	if (StartsWith(t, "DECIMAL(") && t.back() == ')') {
		const std::string inner = t.substr(8, t.size() - 9);
		const auto comma = inner.find(',');
		int p = 0, s = 0;
		if (comma != std::string::npos && ParseUint(inner.substr(0, comma), p) &&
		    ParseUint(inner.substr(comma + 1), s) && p >= 1 && p <= 38 && s >= 0 && s <= p) {
			out.kind = UnconstrainedNumber::Kind::kDecimal;
			out.width = static_cast<uint8_t>(p);
			out.scale = static_cast<uint8_t>(s);
			return out;
		}
	}
	ThrowBadUnconstrained(text);
}

bool IsUtf8Charset(const std::string &charset) {
	const std::string c = Upper(charset);
	return c == "AL32UTF8" || c == "UTF8";
}

ReadColumnPlan PlanReadColumn(const OracleColumn &column, const UnconstrainedNumber &unconstrained,
                              const std::string &db_charset) {
	ReadColumnPlan plan = Base(column);
	const std::string type = Upper(column.data_type);
	const std::string q = plan.select_expr;

	if (type == "NUMBER" || type == "FLOAT") {
		PlanNumber(plan, column, unconstrained, type == "FLOAT");
	} else if (type == "BINARY_FLOAT") {
		plan.duck_type = {DuckTypeId::kFloat, 0, 0};
		plan.decoder = FetchDecoder::kNativeFloat;
		plan.dty = SQLT_BFLOAT;
		plan.define_bytes = 4;
	} else if (type == "BINARY_DOUBLE") {
		SetDouble(plan);
	} else if (type == "DATE") {
		plan.duck_type = {DuckTypeId::kTimestamp, 0, 0};
		plan.decoder = FetchDecoder::kDate;
		plan.dty = SQLT_DAT;
		plan.define_bytes = 7;
	} else if (StartsWith(type, "TIMESTAMP")) {
		plan.decoder = FetchDecoder::kTimestamp;
		plan.dty = kInternalTimestamp;
		plan.define_bytes = 11;
		if (type.find("WITH LOCAL TIME ZONE") != std::string::npos) {
			plan.duck_type = {DuckTypeId::kTimestampTz, 0, 0};
			plan.select_expr = "SYS_EXTRACT_UTC(CAST(" + q + " AS TIMESTAMP WITH TIME ZONE))";
		} else if (type.find("WITH TIME ZONE") != std::string::npos) {
			plan.duck_type = {DuckTypeId::kTimestampTz, 0, 0};
			plan.select_expr = "SYS_EXTRACT_UTC(" + q + ")";
		} else {
			plan.duck_type = {DuckTypeId::kTimestamp, 0, 0};
		}
	} else if (type == "VARCHAR2" || type == "CHAR") {
		plan.duck_type = {DuckTypeId::kVarchar, 0, 0};
		plan.decoder = FetchDecoder::kString;
		plan.dty = SQLT_CHR;
		plan.define_bytes = IsUtf8Charset(db_charset) ? column.data_length : column.data_length * 3;
	} else if (type == "NVARCHAR2" || type == "NCHAR") {
		plan.duck_type = {DuckTypeId::kVarchar, 0, 0};
		plan.decoder = FetchDecoder::kString;
		plan.dty = SQLT_CHR;
		plan.national = true;
		plan.define_bytes = (column.data_length * 3 + 1) / 2;
	} else if (type == "CLOB" || type == "NCLOB") {
		plan.duck_type = {DuckTypeId::kVarchar, 0, 0};
		plan.decoder = FetchDecoder::kString;
		plan.dty = SQLT_CLOB;
		plan.lob = true;
		plan.define_bytes = 8;
		plan.national = type == "NCLOB";
	} else if (type == "RAW") {
		plan.duck_type = {DuckTypeId::kBlob, 0, 0};
		plan.decoder = FetchDecoder::kBinary;
		plan.dty = SQLT_BIN;
		plan.define_bytes = column.data_length;
	} else if (type == "BLOB") {
		plan.duck_type = {DuckTypeId::kBlob, 0, 0};
		plan.decoder = FetchDecoder::kBinary;
		plan.dty = SQLT_BLOB;
		plan.lob = true;
		plan.define_bytes = 8;
	} else if (type == "ROWID" || type == "UROWID") {
		plan.duck_type = {DuckTypeId::kVarchar, 0, 0};
		plan.decoder = FetchDecoder::kString;
		plan.dty = SQLT_CHR;
		plan.define_bytes = 4000;
		plan.select_expr = type == "ROWID" ? "ROWIDTOCHAR(" + q + ")" : "CAST(" + q + " AS VARCHAR2(4000))";
	} else {
		plan.duck_type = {DuckTypeId::kVarchar, 0, 0};
		plan.decoder = FetchDecoder::kUnsupported;
		plan.unsupported =
		    "column " + q + " has Oracle type " + column.data_type + ", which oraduck_scan does not read";
	}
	return plan;
}

} // namespace oraduck
