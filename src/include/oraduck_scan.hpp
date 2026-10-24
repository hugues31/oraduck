#pragma once

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck/describe_source.hpp"
#include "oraduck/read_sql.hpp"
#include "oraduck/read_types.hpp"
#include "oraduck/sql_binds.hpp"

#include <vector>

namespace duckdb {

class ExtensionLoader;

// Options of oraduck_scan, also the defaults of every scan of an attached Oracle database (not duckdb::ScanOptions,
// DuckDB's storage scan options).
struct OraduckScanOptions {
	uint32_t sessions = 0; // 0: DuckDB's number of threads
	oraduck::SplitStrategy split = oraduck::SplitStrategy::kAuto;
	oraduck::UnconstrainedNumber unconstrained;
	// 'none' (SNAPSHOT_DEFAULT, docs/probe-results.md R8): AS OF SCN costs each session a hard parse of the unit
	// statement, one session after another
	oraduck::SnapshotMode snapshot = oraduck::SnapshotMode::kNone;
};

struct OraduckScanBindData : public TableFunctionData {
	oraduck::OracleCredentials credentials;
	oraduck::OracleTableDescription source;
	std::vector<oraduck::ReadColumnPlan> columns; // one per Oracle column, in column order
	OraduckScanOptions options;
	std::vector<oraduck::SqlFragment> filters;   // filters pushed down to Oracle, combined with AND
	optional_ptr<TableCatalogEntry> table_entry; // the attached table read (ATTACH), null for oraduck_scan

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

// Registers the oraduck_scan table function.
void RegisterOraduckScan(ExtensionLoader &loader);

// The oraduck_scan table function (also the scan function of attached Oracle tables).
TableFunction GetOraduckScanFunction();

// Validates and applies one option by name (sessions, split, unconstrained_number, snapshot; case-insensitive);
// throws InvalidInputException naming the accepted values.
void ApplyScanOption(OraduckScanOptions &options, const string &name, const Value &value);

// DuckDB column names: the Oracle names, deduplicated case-insensitively in column order by appending _1, _2, ...
vector<string> DuckColumnNames(const oraduck::OracleTableDescription &source);

// Bind data of a scan of a described table or view; fills the DuckDB column types and names.
unique_ptr<FunctionData> BindOraduckScan(ClientContext &context, oraduck::OracleCredentials credentials,
                                         oraduck::OracleTableDescription source, const OraduckScanOptions &options,
                                         vector<LogicalType> &return_types, vector<Identifier> &names);

} // namespace duckdb
