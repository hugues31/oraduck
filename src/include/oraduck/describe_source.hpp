#pragma once

#include "oraduck/oci_session.hpp"
#include "oraduck/oracle_names.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oraduck {

enum class SourceKind { kTable, kView };

struct OracleTableDescription {
	std::string owner, name; // after synonym resolution
	SourceKind kind = SourceKind::kTable;
	bool partitioned = false;
	int64_t num_rows = -1; // ALL_TABLES.NUM_ROWS; -1 when unknown (views, no statistics)
	bool owned_by_session_user = false;
	std::string db_charset, nchar_charset;
	std::vector<OracleColumn> columns;
};

// Resolves the name (current schema when the owner is absent, synonyms followed) and describes the table or view.
// Throws OraduckError when it does not exist or is not accessible.
OracleTableDescription DescribeSource(OciSession &session, const TargetName &name);

// Session settings for reading: binary comparison and sorting, so pushed-down filters match DuckDB's semantics.
void PrepareReadSession(OciSession &session);

} // namespace oraduck
