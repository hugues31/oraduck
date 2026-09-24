#pragma once

#include "duckdb.hpp"

#include "oraduck/oci.hpp"

namespace duckdb {

class ExtensionLoader;

// Registers the oraduck secret type (USER, PASSWORD, DSN; the password is redacted).
void RegisterOraduckSecret(ExtensionLoader &loader);

// Credentials of the named secret; BinderException when it is missing or of another type.
oraduck::OracleCredentials GetOraduckCredentials(ClientContext &context, const string &secret_name);

} // namespace duckdb
