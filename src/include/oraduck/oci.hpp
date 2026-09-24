#pragma once

#include "oraduck/errors.hpp"

#include <oci.h>

#include <string>

namespace oraduck {

struct OracleCredentials {
	std::string user;
	std::string password;
	std::string dsn; // EZConnect host:port/service
};

// New OCI environment (threaded mode, AL32UTF8 client), freed with OCIHandleFree(env, OCI_HTYPE_ENV).
// One environment per session: concurrent allocations on a shared environment
// corrupt its heap (ORA-21500 KGHALO4, SIGSEGV), even with OCI_THREADED.
OCIEnv *CreateOciEnv();

// Text of the last OCI error; ora_code receives the ORA code (0 if none).
std::string OciErrorText(OCIError *err, sword status, int32_t &ora_code);

// Throws OracleError unless status is OCI_SUCCESS or OCI_SUCCESS_WITH_INFO.
void CheckOci(sword status, OCIError *err, const char *what);

} // namespace oraduck
