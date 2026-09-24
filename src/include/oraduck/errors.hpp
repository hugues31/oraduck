#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace oraduck {

// OraDuck error (message without the "OraDuck: " prefix).
class OraduckError : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

// Error returned by Oracle or OCI; ora_code is 0 when there is no ORA code.
class OracleError : public OraduckError {
public:
	OracleError(const std::string &message, int32_t ora_code_p) : OraduckError(message), ora_code(ora_code_p) {
	}

	int32_t ora_code;
};

} // namespace oraduck
