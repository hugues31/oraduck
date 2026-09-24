#define DUCKDB_EXTENSION_MAIN

#include "oraduck_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "oraduck_copy.hpp"
#include "oraduck_secret.hpp"

// OCI headers after DuckDB: oratypes.h defines TRUE/FALSE
#include <oci.h>

namespace duckdb {

// Version of the loaded OCI client library (installation diagnostics)
static void OraduckOciVersion(DataChunk &args, ExpressionState &state, Vector &result) {
	sword major = 0, minor = 0, update = 0, patch = 0, port_update = 0;
	OCIClientVersion(&major, &minor, &update, &patch, &port_update);
	auto text = StringUtil::Format("%d.%d.%d.%d.%d", major, minor, update, patch, port_update);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, text);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(ScalarFunction("oraduck_oci_version", {}, LogicalType::VARCHAR, OraduckOciVersion));
	RegisterOraduckSecret(loader);
	RegisterOraduckCopy(loader);
}

void OraduckExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string OraduckExtension::Name() {
	return "oraduck";
}

std::string OraduckExtension::Version() const {
#ifdef EXT_VERSION_ORADUCK
	return EXT_VERSION_ORADUCK;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(oraduck, loader) {
	duckdb::LoadInternal(loader);
}
}
