#pragma once

#include "oraduck/oci_session.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

// Test database credentials (variables set by env.sh)
inline oraduck::OracleCredentials TestCredentials() {
	auto get = [](const char *name) {
		const char *value = std::getenv(name);
		if (!value || !*value) {
			throw std::runtime_error(std::string(name) + " is not set: run 'source env.sh'");
		}
		return std::string(value);
	};
	return {get("ORADUCK_ORACLE_USER"), get("ORADUCK_ORACLE_PASSWORD"), get("ORADUCK_ORACLE_DSN")};
}

// Drops the table if it exists; table may be a double-quoted name.
inline void DropTableIfExists(oraduck::OciSession &session, const std::string &table) {
	session.Execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + table +
	                " PURGE'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;");
}

inline std::string Scalar(oraduck::OciSession &session, const std::string &sql) {
	return session.QueryStrings(sql).at(0).at(0);
}
