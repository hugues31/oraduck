#pragma once

#include "oraduck/oracle_decoding.hpp"
#include "oraduck/oracle_names.hpp"
#include "oraduck/sql_binds.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oraduck {

struct FilterConstant {
	enum class Kind { kInteger, kDecimal, kDouble, kString, kTimestamp, kDate } kind = Kind::kInteger;
	int64_t i64 = 0; // kInteger; kTimestamp: microseconds since epoch; kDate: days since epoch
	OracleInt128 i128 {0, 0}; // kDecimal: unscaled value
	uint8_t scale = 0;        // kDecimal
	double f64 = 0;           // kDouble
	std::string text;         // kString
};

// kDateEq: CAST(col AS DATE) = d
enum class FilterOp { kAnd, kOr, kEq, kNe, kLt, kLe, kGt, kGe, kIn, kIsNull, kIsNotNull, kDateEq };

struct FilterNode {
	FilterOp op = FilterOp::kEq;
	size_t column = 0; // index in the table's columns
	std::vector<FilterConstant> constants;
	std::vector<FilterNode> children; // kAnd, kOr
};

// Oracle SQL for the filter, or nullopt when any part is not provably equivalent in Oracle
// (the whole node or nothing). Placeholders are :b1, :b2, ... in textual order.
std::optional<SqlFragment> TranslateFilter(const FilterNode &node, const std::vector<OracleColumn> &columns,
                                           const std::string &db_charset);

} // namespace oraduck
