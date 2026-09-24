#pragma once

// DuckDB before OCI: oratypes.h defines TRUE/FALSE
#include "duckdb.hpp"

#include "oraduck/direct_path_loader.hpp"
#include "oraduck/oracle_names.hpp"

#include <string>
#include <vector>

namespace oraduck {

enum class CellRepr : uint8_t {
	kString,        // VARCHAR -> SQLT_CHR, pointer into the DuckDB vector data
	kNative,        // fixed-width value passed as is (SQLT_INT, SQLT_UIN, SQLT_FLT, SQLT_BFLOAT, SQLT_BDOUBLE)
	kNumberEncoded, // integer or DECIMAL -> SQLT_NUM encoded by OraDuck
	kDateDat,       // DATE / TIMESTAMP -> SQLT_DAT (7 bytes, seconds truncated)
	kDateTimeText,  // DATE / TIMESTAMP -> SQLT_CHR "YYYY-MM-DD HH24:MI:SS.FF6"
	kTimestampInternal, // DATE / TIMESTAMP -> internal TIMESTAMP (type 180, 7 or 11 bytes)
};

struct PlannedColumn {
	duckdb::idx_t source_index = 0;
	duckdb::LogicalType source_type;
	OracleColumn target;
	CellRepr repr = CellRepr::kString;
	DirPathColumnSpec spec;
	uint8_t scale = 0;          // kNumberEncoded: decimal scale
	uint32_t scratch_width = 0; // bytes per row in the work buffer (0 = zero copy)
	bool check_finite = false;  // FLOAT/DOUBLE to NUMBER: NaN and infinities rejected
};

// Maps the query columns to the table columns and picks their OCI representation.
// Throws duckdb::BinderException on any mismatch.
std::vector<PlannedColumn> BuildColumnPlan(const std::vector<std::string> &names,
                                           const std::vector<duckdb::LogicalType> &types,
                                           const TableDescription &table);

} // namespace oraduck
