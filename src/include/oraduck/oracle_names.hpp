#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oraduck {

// Target table: normalized names (upper case except for double-quoted identifiers).
// Empty owner = the session's current schema.
struct TargetName {
	std::string owner;
	std::string table;
};

// "TABLE", "SCHEMA.TABLE", "\"Schema\".\"Table\"" ; throws OraduckError when invalid.
TargetName ParseTargetName(const std::string &text);

enum class OracleTypeFamily { kNumber, kBinaryFloat, kBinaryDouble, kVarchar, kDate, kTimestamp, kUnsupported };

// Family of an Oracle type as reported by ALL_TAB_COLUMNS.DATA_TYPE.
OracleTypeFamily ClassifyOracleType(const std::string &data_type);

struct OracleColumn {
	std::string name;      // exact dictionary name
	std::string data_type; // ex. "TIMESTAMP(6)"
	OracleTypeFamily family = OracleTypeFamily::kUnsupported;
	uint32_t data_length = 0; // in bytes
	int32_t precision = -1;   // -1 when NULL
	int32_t scale = -1;       // -1 when NULL
	bool nullable = true;
};

struct TableDescription {
	std::string owner;
	std::string table;
	std::vector<OracleColumn> columns;
	uint32_t index_count = 0;
};

} // namespace oraduck
