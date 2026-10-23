#pragma once

#include "oraduck/oci_session.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

inline std::string RequiredEnv(const char *name) {
	const char *value = std::getenv(name);
	if (!value || !*value) {
		throw std::runtime_error(std::string(name) + " is not set: run 'source env.sh'");
	}
	return std::string(value);
}

// Test database credentials (variables set by env.sh)
inline oraduck::OracleCredentials TestCredentials() {
	return {RequiredEnv("ORADUCK_ORACLE_USER"), RequiredEnv("ORADUCK_ORACLE_PASSWORD"),
	        RequiredEnv("ORADUCK_ORACLE_DSN")};
}

// Credentials of the reader user: CREATE SESSION only, no dictionary access
inline oraduck::OracleCredentials ReaderCredentials() {
	return {RequiredEnv("ORADUCK_ORACLE_READER_USER"), RequiredEnv("ORADUCK_ORACLE_READER_PASSWORD"),
	        RequiredEnv("ORADUCK_ORACLE_DSN")};
}

// Drops the table if it exists; table may be a double-quoted name.
inline void DropTableIfExists(oraduck::OciSession &session, const std::string &table) {
	session.Execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + table +
	                " PURGE'; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -942 THEN RAISE; END IF; END;");
}

inline std::string Scalar(oraduck::OciSession &session, const std::string &sql) {
	return session.QueryStrings(sql).at(0).at(0);
}
