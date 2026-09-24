#pragma once

#include "oraduck/oci.hpp"

#include <string>
#include <vector>

namespace oraduck {

// Oracle session (OCILogon2); one instance per thread.
class OciSession {
public:
	explicit OciSession(const OracleCredentials &credentials);
	~OciSession();
	OciSession(const OciSession &) = delete;
	OciSession &operator=(const OciSession &) = delete;

	// Query with positional text binds (:1, :2...); values returned as text, NULL -> "".
	std::vector<std::vector<std::string>> QueryStrings(const std::string &sql,
	                                                   const std::vector<std::string> &binds = {});
	// Statement without result (DDL/DML) followed by a COMMIT.
	void Execute(const std::string &sql);

	OCIEnv *env() const {
		return env_;
	}
	OCISvcCtx *svc() const {
		return svc_;
	}
	OCIError *err() const {
		return err_;
	}

private:
	OCIEnv *env_ = nullptr; // owned by this session
	OCIError *err_ = nullptr;
	OCISvcCtx *svc_ = nullptr;
};

} // namespace oraduck
