#include "oraduck/oci_session.hpp"

namespace oraduck {

namespace {

struct StmtGuard {
	OCIStmt *stmt;
	OCIError *err;
	~StmtGuard() {
		if (stmt) {
			OCIStmtRelease(stmt, err, nullptr, 0, OCI_DEFAULT);
		}
	}
};

const OraText *Text(const std::string &value) {
	return reinterpret_cast<const OraText *>(value.data());
}

} // namespace

OciSession::OciSession(const OracleCredentials &credentials) : env_(CreateOciEnv()) {
	const sword alloc = OCIHandleAlloc(env_, reinterpret_cast<void **>(&err_), OCI_HTYPE_ERROR, 0, nullptr);
	if (alloc != OCI_SUCCESS) {
		OCIHandleFree(env_, OCI_HTYPE_ENV);
		throw OracleError("OCIHandleAlloc(OCIError): OCI error " + std::to_string(alloc), 0);
	}
	const sword rc = OCILogon2(env_, err_, &svc_, Text(credentials.user), static_cast<ub4>(credentials.user.size()),
	                           Text(credentials.password), static_cast<ub4>(credentials.password.size()),
	                           Text(credentials.dsn), static_cast<ub4>(credentials.dsn.size()), OCI_DEFAULT);
	if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
		int32_t code = 0;
		const std::string text = OciErrorText(err_, rc, code);
		OCIHandleFree(err_, OCI_HTYPE_ERROR);
		err_ = nullptr;
		OCIHandleFree(env_, OCI_HTYPE_ENV);
		env_ = nullptr;
		throw OracleError("cannot connect to Oracle (" + credentials.user + "@" + credentials.dsn + "): " + text,
		                  code);
	}
}

OciSession::~OciSession() {
	if (svc_) {
		OCILogoff(svc_, err_);
	}
	if (err_) {
		OCIHandleFree(err_, OCI_HTYPE_ERROR);
	}
	if (env_) {
		OCIHandleFree(env_, OCI_HTYPE_ENV);
	}
}

std::vector<std::vector<std::string>> OciSession::QueryStrings(const std::string &sql,
                                                               const std::vector<std::string> &binds) {
	OCIStmt *stmt = nullptr;
	CheckOci(OCIStmtPrepare2(svc_, &stmt, err_, Text(sql), static_cast<ub4>(sql.size()), nullptr, 0, OCI_NTV_SYNTAX,
	                         OCI_DEFAULT),
	         err_, "OCIStmtPrepare2");
	StmtGuard guard {stmt, err_};
	for (size_t i = 0; i < binds.size(); i++) {
		OCIBind *bind = nullptr;
		CheckOci(OCIBindByPos(stmt, &bind, err_, static_cast<ub4>(i + 1), const_cast<char *>(binds[i].data()),
		                      static_cast<sb4>(binds[i].size()), SQLT_CHR, nullptr, nullptr, nullptr, 0, nullptr,
		                      OCI_DEFAULT),
		         err_, "OCIBindByPos");
	}
	CheckOci(OCIStmtExecute(svc_, stmt, err_, 0, 0, nullptr, nullptr, OCI_DEFAULT), err_, "OCIStmtExecute");
	ub4 ncols = 0;
	CheckOci(OCIAttrGet(stmt, OCI_HTYPE_STMT, &ncols, nullptr, OCI_ATTR_PARAM_COUNT, err_), err_,
	         "OCI_ATTR_PARAM_COUNT");
	constexpr sb4 kWidth = 4001;
	std::vector<std::vector<char>> buffers(ncols, std::vector<char>(kWidth));
	std::vector<sb2> indicators(ncols);
	std::vector<ub2> lengths(ncols);
	for (ub4 c = 0; c < ncols; c++) {
		OCIDefine *define = nullptr;
		CheckOci(OCIDefineByPos(stmt, &define, err_, c + 1, buffers[c].data(), kWidth, SQLT_CHR, &indicators[c],
		                        &lengths[c], nullptr, OCI_DEFAULT),
		         err_, "OCIDefineByPos");
	}
	std::vector<std::vector<std::string>> rows;
	while (true) {
		const sword rc = OCIStmtFetch2(stmt, err_, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
		if (rc == OCI_NO_DATA) {
			break;
		}
		CheckOci(rc, err_, "OCIStmtFetch2");
		std::vector<std::string> row;
		row.reserve(ncols);
		for (ub4 c = 0; c < ncols; c++) {
			row.emplace_back(indicators[c] == -1 ? std::string() : std::string(buffers[c].data(), lengths[c]));
		}
		rows.push_back(std::move(row));
	}
	return rows;
}

void OciSession::Execute(const std::string &sql) {
	OCIStmt *stmt = nullptr;
	CheckOci(OCIStmtPrepare2(svc_, &stmt, err_, Text(sql), static_cast<ub4>(sql.size()), nullptr, 0, OCI_NTV_SYNTAX,
	                         OCI_DEFAULT),
	         err_, "OCIStmtPrepare2");
	StmtGuard guard {stmt, err_};
	const sword rc = OCIStmtExecute(svc_, stmt, err_, 1, 0, nullptr, nullptr, OCI_DEFAULT);
	if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
		int32_t code = 0;
		const std::string text = OciErrorText(err_, rc, code);
		throw OracleError("failed \"" + sql.substr(0, 120) + "\": " + text, code);
	}
	CheckOci(OCITransCommit(svc_, err_, OCI_DEFAULT), err_, "OCITransCommit");
}

} // namespace oraduck
