#pragma once

#include "oraduck/column_plan.hpp"

namespace oraduck {

// Writes rows [offset, offset + n) of the vector into rows [0, n) of the column array.
// scratch: the column's work buffer (Rows() * scratch_width bytes).
void WriteColumn(const PlannedColumn &column, ub2 col, const duckdb::UnifiedVectorFormat &format, duckdb::idx_t offset,
                 duckdb::idx_t n, uint8_t *scratch, DirectPathLoader &loader);

} // namespace oraduck
