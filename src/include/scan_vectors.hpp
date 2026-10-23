#pragma once

#include "duckdb.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck/fetch_cursor.hpp"
#include "oraduck/read_types.hpp"

namespace duckdb {

// DuckDB type of a read column.
LogicalType ToLogicalType(const oraduck::DuckTypeSpec &spec);

// Decodes the rows [first_row, first_row + count) of the cursor's last batch, column cursor_column, into out
// starting at out_offset. A value that the column's DuckDB type cannot hold throws IOException naming the column.
void WriteColumn(const oraduck::FetchCursor &cursor, size_t cursor_column, const oraduck::ReadColumnPlan &plan,
                 uint32_t first_row, uint32_t count, Vector &out, idx_t out_offset);

} // namespace duckdb
