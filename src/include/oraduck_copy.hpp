#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ExtensionLoader;

// Registers the FORMAT ORADUCK copy function.
void RegisterOraduckCopy(ExtensionLoader &loader);

} // namespace duckdb
