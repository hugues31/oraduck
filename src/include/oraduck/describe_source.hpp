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

// Thrown by DescribeSource when the table or view does not exist or is not accessible.
class SourceNotFoundError : public OraduckError {
public:
	using OraduckError::OraduckError;
};

// Resolves the name (current schema when the owner is absent, synonyms followed) and describes the table or view.
// Throws SourceNotFoundError when it does not exist or is not accessible, OraduckError for other failures.
OracleTableDescription DescribeSource(OciSession &session, const TargetName &name);

// Describes every table and view of owner accessible to the session (synonyms are not listed), with the same fields
// as DescribeSource: the objects, then all their columns in one query, then the table statistics. Empty when the owner
// has none or does not exist.
std::vector<OracleTableDescription> DescribeSchema(OciSession &session, const std::string &owner);

// Session settings for reading: binary comparison and sorting and the Gregorian calendar, so pushed-down filters
// match DuckDB's semantics.
void PrepareReadSession(OciSession &session);

} // namespace oraduck
