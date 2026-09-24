#include "catch.hpp"
#include "oraduck/errors.hpp"
#include "oraduck/oci_api.hpp"

#include <string>

using namespace oraduck;

namespace {

std::string LoadError(const std::vector<std::string> &candidates) {
	try {
		LoadOciApi(candidates);
	} catch (const OraduckError &e) {
		return e.what();
	}
	return "";
}

} // namespace

TEST_CASE("OCI library not found: the error says how to install Instant Client", "[oci_api]") {
	const std::string message = LoadError({"liboraduck_no_such_client.so"});
	CHECK(message.find("Oracle Instant Client not found") != std::string::npos);
	CHECK(message.find("liboraduck_no_such_client.so") != std::string::npos);
	CHECK(message.find("LD_LIBRARY_PATH") != std::string::npos);
}

TEST_CASE("A library without the OCI functions is rejected", "[oci_api]") {
	const std::string message = LoadError({"libm.so.6"});
	CHECK(message.find("libm.so.6") != std::string::npos);
	CHECK(message.find("OCIEnvNlsCreate") != std::string::npos);
}
