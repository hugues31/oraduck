#pragma once

#include "duckdb.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck/filter_sql.hpp"

#include <optional>

namespace duckdb {

// The filter node of a DuckDB filter on the columns of an oraduck_scan, or nullopt when the expression is not one
// OraDuck recognizes: comparisons, BETWEEN, IN and IS [NOT] NULL between a bare column and constants of its type,
// CAST(timestamp column AS DATE) = DATE constant, and AND / OR of those.
std::optional<oraduck::FilterNode> ExpressionToFilterNode(const Expression &expr, const LogicalGet &get);

// pushdown_complex_filter of oraduck_scan: moves each filter that translates into Oracle SQL from filters to the bind
// data's Oracle WHERE; the others stay in DuckDB.
void OraduckScanPushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
                                      vector<unique_ptr<Expression>> &filters);

} // namespace duckdb
