#include "oraduck/chunk_writer.hpp"

#include "oraduck/oracle_encoding.hpp"

#include <cmath>
#include <type_traits>

namespace oraduck {

using duckdb::date_t;
using duckdb::hugeint_t;
using duckdb::idx_t;
using duckdb::InvalidInputException;
using duckdb::LogicalTypeId;
using duckdb::PhysicalType;
using duckdb::string_t;
using duckdb::timestamp_t;
using duckdb::UnifiedVectorFormat;

namespace {

[[noreturn]] void ThrowOutOfRange(const PlannedColumn &column) {
	throw InvalidInputException("OraDuck: value outside the Oracle range (0001-01-01 to 9999-12-31) in column " +
	                            column.target.name);
}

void WriteString(const UnifiedVectorFormat &format, idx_t offset, idx_t n, ub2 col, DirectPathLoader &loader) {
	auto data = UnifiedVectorFormat::GetData<string_t>(format);
	for (idx_t r = 0; r < n; r++) {
		const auto idx = format.sel->get_index(offset + r);
		if (!format.validity.RowIsValid(idx)) {
			loader.SetNull(static_cast<ub4>(r), col);
			continue;
		}
		// A reference, not a copy: a short string (<= 12 bytes) is stored inside the string_t
		// itself, so the pointer must target the vector's memory.
		const string_t &value = data[idx];
		const auto size = value.GetSize();
		if (size == 0) {
			loader.SetNull(static_cast<ub4>(r), col); // Oracle: '' is NULL
			continue;
		}
		loader.SetCell(static_cast<ub4>(r), col, value.GetData(), static_cast<ub4>(size));
	}
}

template <class T>
void WriteNative(const PlannedColumn &column, const UnifiedVectorFormat &format, idx_t offset, idx_t n, ub2 col,
                 DirectPathLoader &loader) {
	auto data = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t r = 0; r < n; r++) {
		const auto idx = format.sel->get_index(offset + r);
		if (!format.validity.RowIsValid(idx)) {
			loader.SetNull(static_cast<ub4>(r), col);
			continue;
		}
		if constexpr (std::is_floating_point<T>::value) {
			if (column.check_finite && !std::isfinite(data[idx])) {
				throw InvalidInputException("OraDuck: NaN or infinity cannot be stored in NUMBER (column " +
				                            column.target.name + ")");
			}
		}
		loader.SetCell(static_cast<ub4>(r), col, &data[idx], sizeof(T));
	}
}

template <class T>
void WriteSignedNumber(const PlannedColumn &column, const UnifiedVectorFormat &format, idx_t offset, idx_t n, ub2 col,
                       uint8_t *scratch, DirectPathLoader &loader) {
	auto data = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t r = 0; r < n; r++) {
		const auto idx = format.sel->get_index(offset + r);
		if (!format.validity.RowIsValid(idx)) {
			loader.SetNull(static_cast<ub4>(r), col);
			continue;
		}
		const int64_t v = data[idx];
		const uint64_t magnitude = v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v);
		uint8_t *out = scratch + r * kOracleNumberMaxBytes;
		const size_t len = EncodeOracleNumber(magnitude, v < 0, column.scale, out);
		loader.SetCell(static_cast<ub4>(r), col, out, static_cast<ub4>(len));
	}
}

template <class T>
void WriteUnsignedNumber(const PlannedColumn &column, const UnifiedVectorFormat &format, idx_t offset, idx_t n,
                         ub2 col, uint8_t *scratch, DirectPathLoader &loader) {
	auto data = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t r = 0; r < n; r++) {
		const auto idx = format.sel->get_index(offset + r);
		if (!format.validity.RowIsValid(idx)) {
			loader.SetNull(static_cast<ub4>(r), col);
			continue;
		}
		uint8_t *out = scratch + r * kOracleNumberMaxBytes;
		const size_t len = EncodeOracleNumber(static_cast<uint64_t>(data[idx]), false, column.scale, out);
		loader.SetCell(static_cast<ub4>(r), col, out, static_cast<ub4>(len));
	}
}

void WriteHugeintNumber(const PlannedColumn &column, const UnifiedVectorFormat &format, idx_t offset, idx_t n, ub2 col,
                        uint8_t *scratch, DirectPathLoader &loader) {
	auto data = UnifiedVectorFormat::GetData<hugeint_t>(format);
	for (idx_t r = 0; r < n; r++) {
		const auto idx = format.sel->get_index(offset + r);
		if (!format.validity.RowIsValid(idx)) {
			loader.SetNull(static_cast<ub4>(r), col);
			continue;
		}
		const hugeint_t &v = data[idx];
		const bool negative = v.upper < 0;
		OracleUint128 magnitude {static_cast<uint64_t>(v.upper), v.lower};
		if (negative) { // two's complement negation
			magnitude.lo = ~magnitude.lo + 1;
			magnitude.hi = ~magnitude.hi + (magnitude.lo == 0 ? 1 : 0);
		}
		uint8_t *out = scratch + r * kOracleNumberMaxBytes;
		const size_t len = EncodeOracleNumber(magnitude, negative, column.scale, out);
		if (len == 0) {
			throw InvalidInputException("OraDuck: value too large for NUMBER (column " + column.target.name +
			                            ")");
		}
		loader.SetCell(static_cast<ub4>(r), col, out, static_cast<ub4>(len));
	}
}

void WriteNumberEncoded(const PlannedColumn &column, const UnifiedVectorFormat &format, idx_t offset, idx_t n,
                        ub2 col, uint8_t *scratch, DirectPathLoader &loader) {
	switch (column.source_type.InternalType()) {
	case PhysicalType::INT8:
		return WriteSignedNumber<int8_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::INT16:
		return WriteSignedNumber<int16_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::INT32:
		return WriteSignedNumber<int32_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::INT64:
		return WriteSignedNumber<int64_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::BOOL:
		return WriteUnsignedNumber<bool>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::UINT8:
		return WriteUnsignedNumber<uint8_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::UINT16:
		return WriteUnsignedNumber<uint16_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::UINT32:
		return WriteUnsignedNumber<uint32_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::UINT64:
		return WriteUnsignedNumber<uint64_t>(column, format, offset, n, col, scratch, loader);
	case PhysicalType::INT128:
		return WriteHugeintNumber(column, format, offset, n, col, scratch, loader);
	default:
		throw duckdb::InternalException("OraDuck: unexpected physical type for NUMBER (column " +
		                                column.target.name + ")");
	}
}

void WriteDateTime(const PlannedColumn &column, const UnifiedVectorFormat &format, idx_t offset, idx_t n, ub2 col,
                   uint8_t *scratch, DirectPathLoader &loader) {
	const bool is_timestamp = column.source_type.id() == LogicalTypeId::TIMESTAMP;
	const auto repr = column.repr;
	// DuckDB checks the physical type: read the vector only as its own type
	const date_t *dates = is_timestamp ? nullptr : UnifiedVectorFormat::GetData<date_t>(format);
	const timestamp_t *timestamps = is_timestamp ? UnifiedVectorFormat::GetData<timestamp_t>(format) : nullptr;
	for (idx_t r = 0; r < n; r++) {
		const auto idx = format.sel->get_index(offset + r);
		if (!format.validity.RowIsValid(idx)) {
			loader.SetNull(static_cast<ub4>(r), col);
			continue;
		}
		int64_t days = 0, micros = 0;
		if (is_timestamp) {
			SplitMicros(timestamps[idx].value, days, micros);
		} else {
			days = dates[idx].days;
		}
		uint8_t *out = scratch + r * column.scratch_width;
		size_t len;
		if (repr == CellRepr::kDateTimeText) {
			len = FormatDateTimeText(days, micros, reinterpret_cast<char *>(out)) ? column.spec.max_size : 0;
		} else if (repr == CellRepr::kTimestampInternal) {
			len = EncodeOracleTimestamp(days, micros, out); // 7 bytes without fraction, 11 otherwise
		} else {
			len = EncodeOracleDate(days, micros, out) ? kOracleDateBytes : 0;
		}
		if (len == 0) {
			ThrowOutOfRange(column);
		}
		loader.SetCell(static_cast<ub4>(r), col, out, static_cast<ub4>(len));
	}
}

} // namespace

void WriteColumn(const PlannedColumn &column, ub2 col, const UnifiedVectorFormat &format, idx_t offset, idx_t n,
                 uint8_t *scratch, DirectPathLoader &loader) {
	switch (column.repr) {
	case CellRepr::kString:
		return WriteString(format, offset, n, col, loader);
	case CellRepr::kNumberEncoded:
		return WriteNumberEncoded(column, format, offset, n, col, scratch, loader);
	case CellRepr::kDateDat:
	case CellRepr::kDateTimeText:
	case CellRepr::kTimestampInternal:
		return WriteDateTime(column, format, offset, n, col, scratch, loader);
	case CellRepr::kNative:
		switch (column.source_type.InternalType()) {
		case PhysicalType::BOOL:
			return WriteNative<bool>(column, format, offset, n, col, loader);
		case PhysicalType::INT8:
			return WriteNative<int8_t>(column, format, offset, n, col, loader);
		case PhysicalType::INT16:
			return WriteNative<int16_t>(column, format, offset, n, col, loader);
		case PhysicalType::INT32:
			return WriteNative<int32_t>(column, format, offset, n, col, loader);
		case PhysicalType::INT64:
			return WriteNative<int64_t>(column, format, offset, n, col, loader);
		case PhysicalType::UINT8:
			return WriteNative<uint8_t>(column, format, offset, n, col, loader);
		case PhysicalType::UINT16:
			return WriteNative<uint16_t>(column, format, offset, n, col, loader);
		case PhysicalType::UINT32:
			return WriteNative<uint32_t>(column, format, offset, n, col, loader);
		case PhysicalType::UINT64:
			return WriteNative<uint64_t>(column, format, offset, n, col, loader);
		case PhysicalType::FLOAT:
			return WriteNative<float>(column, format, offset, n, col, loader);
		case PhysicalType::DOUBLE:
			return WriteNative<double>(column, format, offset, n, col, loader);
		default:
			break;
		}
		break;
	}
	throw duckdb::InternalException("OraDuck: unhandled representation for column " + column.target.name);
}

} // namespace oraduck
