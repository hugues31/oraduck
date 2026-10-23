#include "oraduck/describe_source.hpp"

#include <map>
#include <string>

namespace oraduck {

static const int MAX_SYNONYM_DEPTH = 8;

// Columns of ALL_TAB_COLUMNS read for an OracleColumn, in ColumnFromRow's order
static const char *const COLUMN_FIELDS =
    "column_name, data_type, data_length, NVL(data_precision, -1), NVL(data_scale, -1), nullable, char_length, "
    "character_set_name, collation";

// The column described by row[first], row[first + 1], ... (COLUMN_FIELDS)
static OracleColumn ColumnFromRow(const std::vector<std::string> &row, size_t first) {
	OracleColumn column;
	column.name = row[first];
	column.data_type = row[first + 1];
	column.family = ClassifyOracleType(column.data_type);
	column.data_length = static_cast<uint32_t>(std::stoul(row[first + 2]));
	column.precision = std::stoi(row[first + 3]);
	column.scale = std::stoi(row[first + 4]);
	column.nullable = row[first + 5] == "Y";
	column.char_length = static_cast<uint32_t>(std::stoul(row[first + 6]));
	column.national = row[first + 7] == "NCHAR_CS";
	column.collation = row[first + 8];
	return column;
}

static std::string SessionUser(OciSession &session) {
	return session.QueryStrings("SELECT USER FROM dual").at(0).at(0);
}

static void ReadCharsets(OciSession &session, OracleTableDescription &description) {
	for (const auto &row : session.QueryStrings(
	         "SELECT parameter, value FROM nls_database_parameters "
	         "WHERE parameter IN ('NLS_CHARACTERSET', 'NLS_NCHAR_CHARACTERSET')")) {
		(row[0] == "NLS_CHARACTERSET" ? description.db_charset : description.nchar_charset) = row[1];
	}
}

void PrepareReadSession(OciSession &session) {
	// Gregorian calendar: TIMESTAMP filter constants are bound as text through TO_TIMESTAMP
	session.Execute("ALTER SESSION SET NLS_COMP = 'BINARY' NLS_SORT = 'BINARY' NLS_CALENDAR = 'GREGORIAN'");
}

// Looks the object up in ALL_OBJECTS; returns its type ("" when absent).
static std::string FindObjectType(OciSession &session, const std::string &owner, const std::string &name) {
	const auto rows = session.QueryStrings(
	    "SELECT object_type FROM all_objects WHERE owner = :1 AND object_name = :2 "
	    "AND object_type IN ('TABLE', 'VIEW', 'MATERIALIZED VIEW', 'SYNONYM')",
	    {owner, name});
	if (rows.empty()) {
		return "";
	}
	return rows[0][0];
}

OracleTableDescription DescribeSource(OciSession &session, const TargetName &name) {
	OracleTableDescription description;
	const bool implicit_owner = name.owner.empty();
	std::string owner = name.owner;
	if (implicit_owner) {
		owner = session.QueryStrings("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM dual").at(0).at(0);
	}
	const std::string requested_owner = owner;
	std::string object = name.table;
	bool public_allowed = implicit_owner;

	std::string type;
	for (int depth = 0;; depth++) {
		type = FindObjectType(session, owner, object);
		if (type.empty() && public_allowed) {
			type = FindObjectType(session, "PUBLIC", object);
			if (!type.empty()) {
				owner = "PUBLIC";
			}
		}
		public_allowed = false;
		if (type.empty()) {
			throw SourceNotFoundError("Oracle table or view not found or not accessible: " + requested_owner + "." +
			                          name.table);
		}
		if (type != "SYNONYM") {
			break;
		}
		if (depth >= MAX_SYNONYM_DEPTH) {
			throw OraduckError("Oracle synonym chain too deep or circular: " + name.table);
		}
		const auto rows = session.QueryStrings(
		    "SELECT table_owner, table_name, db_link FROM all_synonyms WHERE owner = :1 AND synonym_name = :2",
		    {owner, object});
		if (rows.empty()) {
			throw SourceNotFoundError("Oracle table or view not found or not accessible: " + requested_owner + "." +
			                          name.table);
		}
		if (!rows[0][2].empty()) {
			throw OraduckError("Oracle synonym over a database link is not supported: " + owner + "." + object);
		}
		owner = rows[0][0];
		object = rows[0][1];
	}

	description.owner = owner;
	description.name = object;
	description.kind = type == "VIEW" ? SourceKind::kView : SourceKind::kTable;
	description.owned_by_session_user = owner == SessionUser(session);

	const auto rows = session.QueryStrings(std::string("SELECT ") + COLUMN_FIELDS +
	                                           " FROM all_tab_columns WHERE owner = :1 AND table_name = :2 "
	                                           "ORDER BY column_id",
	                                       {owner, object});
	for (const auto &row : rows) {
		description.columns.push_back(ColumnFromRow(row, 0));
	}

	if (description.kind == SourceKind::kTable) {
		const auto table_rows = session.QueryStrings(
		    "SELECT partitioned, NVL(num_rows, -1) FROM all_tables WHERE owner = :1 AND table_name = :2",
		    {owner, object});
		if (!table_rows.empty()) {
			description.partitioned = table_rows[0][0] == "YES";
			description.num_rows = std::stoll(table_rows[0][1]);
		}
	}

	ReadCharsets(session, description);
	return description;
}

std::vector<OracleTableDescription> DescribeSchema(OciSession &session, const std::string &owner) {
	// A materialized view is listed twice (MATERIALIZED VIEW and its TABLE): read as a table, like DescribeSource.
	// System-generated objects (IOT overflow segments, ...) cannot be read on their own.
	std::map<std::string, OracleTableDescription> objects;
	for (const auto &row :
	     session.QueryStrings("SELECT object_name, object_type FROM all_objects WHERE owner = :1 "
	                          "AND object_type IN ('TABLE', 'VIEW', 'MATERIALIZED VIEW') AND generated = 'N'",
	                          {owner})) {
		auto &object = objects[row[0]];
		object.name = row[0];
		object.kind = row[1] == "VIEW" ? SourceKind::kView : SourceKind::kTable;
	}
	if (objects.empty()) {
		return {};
	}
	OracleTableDescription common;
	common.owned_by_session_user = owner == SessionUser(session);
	ReadCharsets(session, common);
	for (auto &object : objects) {
		object.second.owner = owner;
		object.second.owned_by_session_user = common.owned_by_session_user;
		object.second.db_charset = common.db_charset;
		object.second.nchar_charset = common.nchar_charset;
	}

	for (const auto &row : session.QueryStrings(std::string("SELECT table_name, ") + COLUMN_FIELDS +
	                                                " FROM all_tab_columns WHERE owner = :1 "
	                                                "ORDER BY table_name, column_id",
	                                            {owner})) {
		auto object = objects.find(row[0]);
		if (object != objects.end()) {
			object->second.columns.push_back(ColumnFromRow(row, 1));
		}
	}

	for (const auto &row : session.QueryStrings(
	         "SELECT table_name, partitioned, NVL(num_rows, -1), dropped FROM all_tables WHERE owner = :1", {owner})) {
		auto object = objects.find(row[0]);
		if (object == objects.end() || object->second.kind != SourceKind::kTable) {
			continue;
		}
		if (row[3] == "YES") {
			// in the recycle bin
			objects.erase(object);
			continue;
		}
		object->second.partitioned = row[1] == "YES";
		object->second.num_rows = std::stoll(row[2]);
	}

	std::vector<OracleTableDescription> result;
	result.reserve(objects.size());
	for (auto &object : objects) {
		result.push_back(std::move(object.second));
	}
	return result;
}

} // namespace oraduck
