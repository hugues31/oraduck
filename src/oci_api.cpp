#include "oraduck/oci_api.hpp"

#include "oraduck/errors.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace oraduck {

namespace {

#ifdef _WIN32
const char *const kSearchPath = "PATH";
std::vector<std::string> DefaultCandidates() {
	return {"oci.dll"};
}

void *OpenLibrary(const std::string &name, std::string &error) {
	HMODULE module = LoadLibraryA(name.c_str());
	if (module == nullptr) {
		error = "error " + std::to_string(GetLastError());
	}
	return reinterpret_cast<void *>(module);
}

void *FindSymbol(void *library, const char *name) {
	return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
}

void CloseLibrary(void *library) {
	FreeLibrary(reinterpret_cast<HMODULE>(library));
}
#else
#ifdef __APPLE__
const char *const kSearchPath = "DYLD_LIBRARY_PATH";
std::vector<std::string> DefaultCandidates() {
	return {"libclntsh.dylib"};
}
#else
const char *const kSearchPath = "LD_LIBRARY_PATH";
// libclntsh.so is the development symlink of the Instant Client zips; the versioned names cover
// installations (RPM, packages) that only ship the versioned file.
std::vector<std::string> DefaultCandidates() {
	return {"libclntsh.so", "libclntsh.so.23.1", "libclntsh.so.21.1", "libclntsh.so.19.1"};
}
#endif

void *OpenLibrary(const std::string &name, std::string &error) {
	void *library = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (library == nullptr) {
		const char *text = dlerror();
		error = text != nullptr ? text : "unknown error";
	}
	return library;
}

void *FindSymbol(void *library, const char *name) {
	return dlsym(library, name);
}

void CloseLibrary(void *library) {
	dlclose(library);
}
#endif

// Resolves every function; returns the name of the first missing one, or nullptr.
const char *Resolve(void *library, OciApi &api) {
#define ORADUCK_OCI_FUNCTION(name)                                                                                   \
	api.name = reinterpret_cast<decltype(api.name)>(FindSymbol(library, #name));                                     \
	if (api.name == nullptr) {                                                                                         \
		return #name;                                                                                                  \
	}
	ORADUCK_OCI_FUNCTION(OCIEnvNlsCreate)
	ORADUCK_OCI_FUNCTION(OCIHandleAlloc)
	ORADUCK_OCI_FUNCTION(OCIHandleFree)
	ORADUCK_OCI_FUNCTION(OCIDescriptorFree)
	ORADUCK_OCI_FUNCTION(OCIAttrGet)
	ORADUCK_OCI_FUNCTION(OCIAttrSet)
	ORADUCK_OCI_FUNCTION(OCIParamGet)
	ORADUCK_OCI_FUNCTION(OCIErrorGet)
	ORADUCK_OCI_FUNCTION(OCIClientVersion)
	ORADUCK_OCI_FUNCTION(OCILogon2)
	ORADUCK_OCI_FUNCTION(OCILogoff)
	ORADUCK_OCI_FUNCTION(OCITransCommit)
	ORADUCK_OCI_FUNCTION(OCIStmtPrepare2)
	ORADUCK_OCI_FUNCTION(OCIStmtRelease)
	ORADUCK_OCI_FUNCTION(OCIStmtExecute)
	ORADUCK_OCI_FUNCTION(OCIStmtFetch2)
	ORADUCK_OCI_FUNCTION(OCIBindByPos)
	ORADUCK_OCI_FUNCTION(OCIDefineByPos)
	ORADUCK_OCI_FUNCTION(OCIDirPathPrepare)
	ORADUCK_OCI_FUNCTION(OCIDirPathColArrayEntrySet)
	ORADUCK_OCI_FUNCTION(OCIDirPathColArrayRowGet)
	ORADUCK_OCI_FUNCTION(OCIDirPathColArrayToStream)
	ORADUCK_OCI_FUNCTION(OCIDirPathLoadStream)
	ORADUCK_OCI_FUNCTION(OCIDirPathStreamReset)
	ORADUCK_OCI_FUNCTION(OCIDirPathFinish)
	ORADUCK_OCI_FUNCTION(OCIDirPathAbort)
#undef ORADUCK_OCI_FUNCTION
	return nullptr;
}

} // namespace

OciApi LoadOciApi(const std::vector<std::string> &candidates) {
	std::string tried;
	for (const auto &name : candidates) {
		std::string error;
		void *library = OpenLibrary(name, error);
		if (library == nullptr) {
			// dlerror() names the file (or the missing dependency, such as libaio); GetLastError() does not
			const std::string entry = error.find(name) == std::string::npos ? name + ": " + error : error;
			tried += (tried.empty() ? "" : "; ") + entry;
			continue;
		}
		OciApi api {};
		if (const char *missing = Resolve(library, api)) {
			CloseLibrary(library);
			throw OraduckError(name + " is not an Oracle client library: function " + missing + " not found");
		}
		// Never unloaded: OCI stays in use until the process ends
		return api;
	}
	throw OraduckError("Oracle Instant Client not found (" + tried +
	                   "). Install Oracle Instant Client 19 or later and add its directory to " + kSearchPath +
	                   " before starting DuckDB");
}

const OciApi &Oci() {
	static const OciApi api = LoadOciApi(DefaultCandidates());
	return api;
}

} // namespace oraduck
