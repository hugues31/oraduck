#include "oraduck/oci.hpp"

namespace oraduck {

namespace {
constexpr ub2 kAl32Utf8 = 873;
} // namespace

OCIEnv *CreateOciEnv() {
	OCIEnv *env = nullptr;
	const sword rc =
	    OCIEnvNlsCreate(&env, OCI_THREADED, nullptr, nullptr, nullptr, nullptr, 0, nullptr, kAl32Utf8, kAl32Utf8);
	if (rc != OCI_SUCCESS || env == nullptr) {
		throw OracleError("cannot create the OCI environment (Instant Client missing from LD_LIBRARY_PATH?)", 0);
	}
	return env;
}

std::string OciErrorText(OCIError *err, sword status, int32_t &ora_code) {
	ora_code = 0;
	switch (status) {
	case OCI_INVALID_HANDLE:
		return "invalid OCI handle";
	case OCI_NEED_DATA:
		return "unexpected OCI_NEED_DATA";
	case OCI_NO_DATA:
		return "unexpected OCI_NO_DATA";
	case OCI_STILL_EXECUTING:
		return "unexpected OCI_STILL_EXECUTING";
	default:
		break;
	}
	if (err == nullptr) {
		return "OCI error " + std::to_string(status);
	}
	OraText buffer[3072];
	sb4 code = 0;
	if (OCIErrorGet(err, 1, nullptr, &code, buffer, sizeof(buffer), OCI_HTYPE_ERROR) != OCI_SUCCESS) {
		return "OCI error " + std::to_string(status) + " (no text available)";
	}
	ora_code = code;
	std::string text(reinterpret_cast<char *>(buffer));
	while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
		text.pop_back();
	}
	return text;
}

void CheckOci(sword status, OCIError *err, const char *what) {
	if (status == OCI_SUCCESS || status == OCI_SUCCESS_WITH_INFO) {
		return;
	}
	int32_t code = 0;
	const std::string text = OciErrorText(err, status, code);
	throw OracleError(std::string(what) + ": " + text, code);
}

} // namespace oraduck
