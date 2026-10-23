#pragma once

#include "oraduck/oci_api.hpp"
#include "oraduck/oracle_names.hpp"

#include <cstdint>
#include <string>

namespace oraduck {

// DuckDB types the read path produces, without depending on DuckDB.
enum class DuckTypeId { kSmallint, kInteger, kBigint, kDecimal, kFloat, kDouble, kVarchar, kTimestamp, kTimestampTz, kBlob };

struct DuckTypeSpec {
	DuckTypeId id = DuckTypeId::kVarchar;
	uint8_t width = 0; // DECIMAL only
	uint8_t scale = 0; // DECIMAL only
};

enum class FetchDecoder {
	kNumberInt64,
	kNumberDecimal64,
	kNumberDecimal128,
	kNumberText,
	kNativeInt64,
	kNativeDouble,
	kNativeFloat,
	kString,
	kBinary,
	kDate,
	kTimestamp,
	kTimestampText,
	kUnsupported
};

// Setting for NUMBER columns without precision and scale.
struct UnconstrainedNumber {
	enum class Kind { kDouble, kVarchar, kDecimal } kind = Kind::kDouble;
	uint8_t width = 0;
	uint8_t scale = 0;
};

// Accepts DOUBLE, VARCHAR, DECIMAL(p,s); throws OraduckError otherwise.
UnconstrainedNumber ParseUnconstrainedNumber(const std::string &text);

// True for AL32UTF8 and UTF8.
bool IsUtf8Charset(const std::string &charset);

struct ReadColumnPlan {
	std::string name;
	std::string select_expr;
	DuckTypeSpec duck_type;
	FetchDecoder decoder = FetchDecoder::kUnsupported;
	ub2 dty = 0;
	uint32_t define_bytes = 0;
	bool national = false;
	bool lob = false; // fetched through LOB locators with LOB prefetch
	uint8_t scale = 0;
	bool round = false;
	std::string unsupported;
};

ReadColumnPlan PlanReadColumn(const OracleColumn &column, const UnconstrainedNumber &unconstrained,
                              const std::string &db_charset);

} // namespace oraduck
