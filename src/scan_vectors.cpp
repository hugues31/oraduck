#include "scan_vectors.hpp"

#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "utf8proc_wrapper.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck/errors.hpp"
#include "oraduck/oracle_decoding.hpp"

#include <cstring>

namespace duckdb {

namespace {

using oraduck::FetchCursor;
using oraduck::FetchDecoder;
using oraduck::NumberDecodeStatus;
using oraduck::ReadColumnPlan;

[[noreturn]] void ThrowValueError(const ReadColumnPlan &plan, const string &what) {
	throw IOException("OraDuck: column \"" + plan.name + "\": " + what);
}

string TypeText(const ReadColumnPlan &plan) {
	return ToLogicalType(plan.duck_type).ToString();
}

void CheckNumber(const ReadColumnPlan &plan, NumberDecodeStatus status) {
	switch (status) {
	case NumberDecodeStatus::kOk:
		return;
	case NumberDecodeStatus::kOverflow:
		ThrowValueError(plan, "NUMBER value out of range for " + TypeText(plan));
	case NumberDecodeStatus::kInexact:
		ThrowValueError(plan, "NUMBER value with more than " + std::to_string(plan.scale) + " decimal places for " +
		                          TypeText(plan));
	case NumberDecodeStatus::kInfinity:
		ThrowValueError(plan, "Oracle NUMBER infinity cannot be read as " + TypeText(plan));
	}
}

void CheckLength(const ReadColumnPlan &plan, uint32_t length, uint32_t expected) {
	if (length != expected) {
		ThrowValueError(plan, "unexpected value of " + std::to_string(length) + " bytes for " + TypeText(plan));
	}
}

// Writes the rows through decode(value, length), which returns a T; NULL values stay NULL
template <class T, class DECODE>
void WriteValues(const FetchCursor &cursor, size_t column, uint32_t first, uint32_t count, Vector &out, idx_t offset,
                 DECODE &&decode) {
	auto writer = FlatVector::Writer<T>(out, count, offset);
	const uint32_t end = first + count;
	for (uint32_t row = first; row < end; row++) {
		uint32_t length = 0;
		const uint8_t *value = cursor.Value(column, row, length);
		if (value) {
			writer.WriteValue(decode(value, length));
		} else {
			writer.WriteNull();
		}
	}
}

// NUMBER(p,0) with p <= 18: native 64-bit integers (SQLT_INT) or NUMBER bytes, narrowed to SMALLINT/INTEGER/BIGINT
template <class T>
void WriteIntegers(const FetchCursor &cursor, size_t column, const ReadColumnPlan &plan, uint32_t first, uint32_t count,
                   Vector &out, idx_t offset) {
	const bool native = plan.decoder == FetchDecoder::kNativeInt64;
	WriteValues<T>(cursor, column, first, count, out, offset, [&](const uint8_t *value, uint32_t length) {
		int64_t v = 0;
		if (native) {
			CheckLength(plan, length, sizeof(v));
			std::memcpy(&v, value, sizeof(v));
		} else {
			CheckNumber(plan, oraduck::DecodeOracleNumberScaled64(value, length, 0, false, v));
		}
		if (v < int64_t(NumericLimits<T>::Minimum()) || v > int64_t(NumericLimits<T>::Maximum())) {
			ThrowValueError(plan, "value " + std::to_string(v) + " out of range for " + TypeText(plan));
		}
		return T(v);
	});
}

// DECIMAL(p,s) with p <= 18, stored as T; the value must have at most p digits (a user-given DECIMAL can be narrower
// than the stored NUMBER)
template <class T>
void WriteDecimal64(const FetchCursor &cursor, size_t column, const ReadColumnPlan &plan, uint32_t first,
                    uint32_t count, Vector &out, idx_t offset) {
	const int64_t limit = NumericHelper::POWERS_OF_TEN[plan.duck_type.width];
	WriteValues<T>(cursor, column, first, count, out, offset, [&](const uint8_t *value, uint32_t length) {
		int64_t v = 0;
		CheckNumber(plan, oraduck::DecodeOracleNumberScaled64(value, length, plan.scale, plan.round, v));
		if (v >= limit || v <= -limit) {
			ThrowValueError(plan, "NUMBER value out of range for " + TypeText(plan));
		}
		return T(v);
	});
}

// DECIMAL(p,s) with 18 < p <= 38; the 128-bit decoder accepts up to 39 digits, so the width is checked here
void WriteDecimal128(const FetchCursor &cursor, size_t column, const ReadColumnPlan &plan, uint32_t first,
                     uint32_t count, Vector &out, idx_t offset) {
	const hugeint_t limit = Hugeint::POWERS_OF_TEN[plan.duck_type.width];
	const hugeint_t negative_limit = -limit;
	WriteValues<hugeint_t>(cursor, column, first, count, out, offset, [&](const uint8_t *value, uint32_t length) {
		oraduck::OracleInt128 v {0, 0};
		CheckNumber(plan, oraduck::DecodeOracleNumberScaled128(value, length, plan.scale, plan.round, v));
		const hugeint_t h(v.hi, v.lo);
		if (h >= limit || h <= negative_limit) {
			ThrowValueError(plan, "NUMBER value out of range for " + TypeText(plan));
		}
		return h;
	});
}

// Exact decimal text of NUMBER values (unconstrained_number 'VARCHAR')
void WriteNumberText(const FetchCursor &cursor, size_t column, uint32_t first, uint32_t count, Vector &out,
                     idx_t offset) {
	auto writer = FlatVector::Writer<string_t>(out, count, offset);
	const uint32_t end = first + count;
	for (uint32_t row = first; row < end; row++) {
		uint32_t length = 0;
		const uint8_t *value = cursor.Value(column, row, length);
		if (value) {
			const std::string text = oraduck::DecodeOracleNumberText(value, length);
			writer.WriteValue(string_t(text.data(), UnsafeNumericCast<uint32_t>(text.size())));
		} else {
			writer.WriteNull();
		}
	}
}

// Native IEEE values (SQLT_BDOUBLE, SQLT_BFLOAT)
template <class T>
void WriteNative(const FetchCursor &cursor, size_t column, const ReadColumnPlan &plan, uint32_t first, uint32_t count,
                 Vector &out, idx_t offset) {
	WriteValues<T>(cursor, column, first, count, out, offset, [&](const uint8_t *value, uint32_t length) {
		CheckLength(plan, length, sizeof(T));
		T v;
		std::memcpy(&v, value, sizeof(v));
		return v;
	});
}

// Binary values: copied once, into the vector's string heap
void WriteBytes(const FetchCursor &cursor, size_t column, uint32_t first, uint32_t count, Vector &out, idx_t offset) {
	WriteValues<string_t>(cursor, column, first, count, out, offset, [](const uint8_t *value, uint32_t length) {
		return string_t(const_char_ptr_cast(value), length);
	});
}

// Character values, converted to UTF-8 by OCI: a database in the same character set hands its bytes over as they are,
// so bytes stored invalid in Oracle are rejected here (a DuckDB VARCHAR must be valid UTF-8). Analyze checks 8 or 32
// ASCII bytes at a time and allocates nothing.
void WriteStrings(const FetchCursor &cursor, size_t column, const ReadColumnPlan &plan, uint32_t first, uint32_t count,
                  Vector &out, idx_t offset) {
	WriteValues<string_t>(cursor, column, first, count, out, offset, [&](const uint8_t *value, uint32_t length) {
		const auto text = const_char_ptr_cast(value);
		size_t position = 0;
		if (Utf8Proc::Analyze(text, length, nullptr, &position) == UnicodeType::INVALID) {
			ThrowValueError(plan, "invalid UTF-8 at byte " + std::to_string(position + 1) + " of a value of " +
			                          std::to_string(length) + " bytes");
		}
		return string_t(text, length);
	});
}

// DATE (7 bytes) and TIMESTAMP (internal type 180, 7 or 11 bytes) as microseconds since 1970-01-01
template <class T>
void WriteTimestamps(const FetchCursor &cursor, size_t column, const ReadColumnPlan &plan, uint32_t first,
                     uint32_t count, Vector &out, idx_t offset) {
	const bool date = plan.decoder == FetchDecoder::kDate;
	WriteValues<T>(cursor, column, first, count, out, offset, [&](const uint8_t *value, uint32_t length) {
		int64_t micros = 0;
		const bool ok = date ? length == 7 && oraduck::DecodeOracleDate(value, micros)
		                     : oraduck::DecodeOracleTimestamp(value, length, micros);
		if (!ok) {
			ThrowValueError(plan, string("invalid Oracle ") + (date ? "DATE" : "TIMESTAMP") + " value");
		}
		return T(micros);
	});
}

void WriteIntegersAs(const FetchCursor &cursor, size_t column, const ReadColumnPlan &plan, uint32_t first,
                     uint32_t count, Vector &out, idx_t offset) {
	switch (plan.duck_type.id) {
	case oraduck::DuckTypeId::kSmallint:
		return WriteIntegers<int16_t>(cursor, column, plan, first, count, out, offset);
	case oraduck::DuckTypeId::kInteger:
		return WriteIntegers<int32_t>(cursor, column, plan, first, count, out, offset);
	case oraduck::DuckTypeId::kBigint:
		return WriteIntegers<int64_t>(cursor, column, plan, first, count, out, offset);
	default:
		throw InternalException("OraDuck: column \"%s\": integer decoder for %s", plan.name, TypeText(plan));
	}
}

} // namespace

LogicalType ToLogicalType(const oraduck::DuckTypeSpec &spec) {
	switch (spec.id) {
	case oraduck::DuckTypeId::kSmallint:
		return LogicalType::SMALLINT;
	case oraduck::DuckTypeId::kInteger:
		return LogicalType::INTEGER;
	case oraduck::DuckTypeId::kBigint:
		return LogicalType::BIGINT;
	case oraduck::DuckTypeId::kDecimal:
		return LogicalType::DECIMAL(spec.width, spec.scale);
	case oraduck::DuckTypeId::kFloat:
		return LogicalType::FLOAT;
	case oraduck::DuckTypeId::kDouble:
		return LogicalType::DOUBLE;
	case oraduck::DuckTypeId::kVarchar:
		return LogicalType::VARCHAR;
	case oraduck::DuckTypeId::kTimestamp:
		return LogicalType::TIMESTAMP;
	case oraduck::DuckTypeId::kTimestampTz:
		return LogicalType::TIMESTAMP_TZ;
	case oraduck::DuckTypeId::kBlob:
		return LogicalType::BLOB;
	}
	throw InternalException("OraDuck: unknown DuckDB type id");
}

namespace {

void WriteDecoded(const FetchCursor &cursor, size_t c, const ReadColumnPlan &plan, uint32_t first_row, uint32_t count,
                  Vector &out, idx_t out_offset) {
	switch (plan.decoder) {
	case FetchDecoder::kNativeInt64:
	case FetchDecoder::kNumberInt64:
		return WriteIntegersAs(cursor, c, plan, first_row, count, out, out_offset);
	case FetchDecoder::kNumberDecimal64: {
		const auto width = plan.duck_type.width;
		if (width <= 4) {
			return WriteDecimal64<int16_t>(cursor, c, plan, first_row, count, out, out_offset);
		}
		if (width <= 9) {
			return WriteDecimal64<int32_t>(cursor, c, plan, first_row, count, out, out_offset);
		}
		return WriteDecimal64<int64_t>(cursor, c, plan, first_row, count, out, out_offset);
	}
	case FetchDecoder::kNumberDecimal128:
		return WriteDecimal128(cursor, c, plan, first_row, count, out, out_offset);
	case FetchDecoder::kNumberText:
		return WriteNumberText(cursor, c, first_row, count, out, out_offset);
	case FetchDecoder::kNativeDouble:
		return WriteNative<double>(cursor, c, plan, first_row, count, out, out_offset);
	case FetchDecoder::kNativeFloat:
		return WriteNative<float>(cursor, c, plan, first_row, count, out, out_offset);
	case FetchDecoder::kString:
		return WriteStrings(cursor, c, plan, first_row, count, out, out_offset);
	case FetchDecoder::kBinary:
		return WriteBytes(cursor, c, first_row, count, out, out_offset);
	case FetchDecoder::kDate:
	case FetchDecoder::kTimestamp:
		if (plan.duck_type.id == oraduck::DuckTypeId::kTimestampTz) {
			return WriteTimestamps<timestamp_tz_t>(cursor, c, plan, first_row, count, out, out_offset);
		}
		return WriteTimestamps<timestamp_t>(cursor, c, plan, first_row, count, out, out_offset);
	case FetchDecoder::kTimestampText:
	case FetchDecoder::kUnsupported:
		break;
	}
	ThrowValueError(plan, plan.unsupported.empty() ? "no decoder for this column" : plan.unsupported);
}

} // namespace

void WriteColumn(const FetchCursor &cursor, size_t cursor_column, const ReadColumnPlan &plan, uint32_t first_row,
                 uint32_t count, Vector &out, idx_t out_offset) {
	try {
		WriteDecoded(cursor, cursor_column, plan, first_row, count, out, out_offset);
	} catch (const oraduck::OraduckError &e) {
		// malformed NUMBER bytes
		ThrowValueError(plan, e.what());
	}
}

} // namespace duckdb
