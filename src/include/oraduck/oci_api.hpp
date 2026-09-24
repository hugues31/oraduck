#pragma once

// The part of the OCI API that OraDuck uses, loaded at run time from Oracle Instant Client
// (libclntsh.so, oci.dll or libclntsh.dylib): building needs no Oracle SDK, and the extension
// loads without Instant Client until a function talks to Oracle.
//
// Types, constants and signatures follow the OCI ABI (oratypes.h, oci.h, ociap.h, oci8dp.h).

#include <cstddef>
#include <string>
#include <vector>

namespace oraduck {

typedef unsigned char ub1;
typedef unsigned short ub2;
typedef signed short sb2;
typedef unsigned int ub4;
typedef signed int sb4;
typedef signed int sword;
typedef unsigned char OraText;

// Opaque handles
struct OCIEnv;
struct OCIError;
struct OCISvcCtx;
struct OCIStmt;
struct OCIBind;
struct OCIDefine;
struct OCIParam;
struct OCISnapshot;
struct OCIDirPathCtx;
struct OCIDirPathColArray;
struct OCIDirPathStream;

// Return codes
constexpr sword OCI_SUCCESS = 0;
constexpr sword OCI_SUCCESS_WITH_INFO = 1;
constexpr sword OCI_NEED_DATA = 99;
constexpr sword OCI_NO_DATA = 100;
constexpr sword OCI_INVALID_HANDLE = -2;
constexpr sword OCI_STILL_EXECUTING = -3123;
constexpr sword OCI_CONTINUE = -24200;

// Modes
constexpr ub4 OCI_DEFAULT = 0;
constexpr ub4 OCI_THREADED = 1;
constexpr ub4 OCI_NTV_SYNTAX = 1;
constexpr ub2 OCI_FETCH_NEXT = 2;

// Handle and descriptor types
constexpr ub4 OCI_HTYPE_ENV = 1;
constexpr ub4 OCI_HTYPE_ERROR = 2;
constexpr ub4 OCI_HTYPE_STMT = 4;
constexpr ub4 OCI_HTYPE_DIRPATH_CTX = 14;
constexpr ub4 OCI_HTYPE_DIRPATH_COLUMN_ARRAY = 15;
constexpr ub4 OCI_HTYPE_DIRPATH_STREAM = 16;
constexpr ub4 OCI_DTYPE_PARAM = 53;

// Attributes
constexpr ub4 OCI_ATTR_DATA_SIZE = 1;
constexpr ub4 OCI_ATTR_DATA_TYPE = 2;
constexpr ub4 OCI_ATTR_NAME = 4;
constexpr ub4 OCI_ATTR_ROW_COUNT = 9;
constexpr ub4 OCI_ATTR_SCHEMA_NAME = 9;
constexpr ub4 OCI_ATTR_PARAM_COUNT = 18;
constexpr ub4 OCI_ATTR_DATEFORMAT = 75;
constexpr ub4 OCI_ATTR_BUF_SIZE = 77;
constexpr ub4 OCI_ATTR_DIRPATH_PARALLEL = 80;
constexpr ub4 OCI_ATTR_NUM_ROWS = 81;
constexpr ub4 OCI_ATTR_COL_COUNT = 82;
constexpr ub4 OCI_ATTR_NUM_COLS = 102;
constexpr ub4 OCI_ATTR_LIST_COLUMNS = 103;
constexpr ub4 OCI_ATTR_DIRPATH_SKIPINDEX_METHOD = 145;

// Values of OCI_ATTR_DIRPATH_SKIPINDEX_METHOD
constexpr ub1 OCI_DIRPATH_INDEX_MAINT_SKIP_ALL = 4; // sqlldr skip_index_maintenance=true: indexes left unusable

// Direct path column flags
constexpr ub1 OCI_DIRPATH_COL_COMPLETE = 0;
constexpr ub1 OCI_DIRPATH_COL_NULL = 1;

// External data types
constexpr ub2 SQLT_CHR = 1;
constexpr ub2 SQLT_NUM = 2;
constexpr ub2 SQLT_INT = 3;
constexpr ub2 SQLT_FLT = 4;
constexpr ub2 SQLT_DAT = 12;
constexpr ub2 SQLT_BFLOAT = 21;
constexpr ub2 SQLT_BDOUBLE = 22;
constexpr ub2 SQLT_UIN = 68;

struct OciApi {
	sword (*OCIEnvNlsCreate)(OCIEnv **envp, ub4 mode, void *ctxp, void *(*malocfp)(void *ctxp, size_t size),
	                         void *(*ralocfp)(void *ctxp, void *memptr, size_t newsize),
	                         void (*mfreefp)(void *ctxp, void *memptr), size_t xtramem_sz, void **usrmempp,
	                         ub2 charset, ub2 ncharset);
	sword (*OCIHandleAlloc)(const void *parenth, void **hndlpp, ub4 type, size_t xtramem_sz, void **usrmempp);
	sword (*OCIHandleFree)(void *hndlp, ub4 type);
	sword (*OCIDescriptorFree)(void *descp, ub4 type);
	sword (*OCIAttrGet)(const void *trgthndlp, ub4 trghndltyp, void *attributep, ub4 *sizep, ub4 attrtype,
	                    OCIError *errhp);
	sword (*OCIAttrSet)(void *trgthndlp, ub4 trghndltyp, void *attributep, ub4 size, ub4 attrtype, OCIError *errhp);
	sword (*OCIParamGet)(const void *hndlp, ub4 htype, OCIError *errhp, void **parmdpp, ub4 pos);
	sword (*OCIErrorGet)(void *hndlp, ub4 recordno, OraText *sqlstate, sb4 *errcodep, OraText *bufp, ub4 bufsiz,
	                     ub4 type);
	void (*OCIClientVersion)(sword *feature_release, sword *release_update, sword *release_update_revision,
	                         sword *increment, sword *ext);
	sword (*OCILogon2)(OCIEnv *envhp, OCIError *errhp, OCISvcCtx **svchp, const OraText *username, ub4 uname_len,
	                   const OraText *password, ub4 passwd_len, const OraText *dbname, ub4 dbname_len, ub4 mode);
	sword (*OCILogoff)(OCISvcCtx *svchp, OCIError *errhp);
	sword (*OCITransCommit)(OCISvcCtx *svchp, OCIError *errhp, ub4 flags);
	sword (*OCIStmtPrepare2)(OCISvcCtx *svchp, OCIStmt **stmtp, OCIError *errhp, const OraText *stmt, ub4 stmt_len,
	                         const OraText *key, ub4 key_len, ub4 language, ub4 mode);
	sword (*OCIStmtRelease)(OCIStmt *stmtp, OCIError *errhp, const OraText *key, ub4 key_len, ub4 mode);
	sword (*OCIStmtExecute)(OCISvcCtx *svchp, OCIStmt *stmtp, OCIError *errhp, ub4 iters, ub4 rowoff,
	                        const OCISnapshot *snap_in, OCISnapshot *snap_out, ub4 mode);
	sword (*OCIStmtFetch2)(OCIStmt *stmtp, OCIError *errhp, ub4 nrows, ub2 orientation, sb4 scroll_offset, ub4 mode);
	sword (*OCIBindByPos)(OCIStmt *stmtp, OCIBind **bindp, OCIError *errhp, ub4 position, void *valuep,
	                      sb4 value_sz, ub2 dty, void *indp, ub2 *alenp, ub2 *rcodep, ub4 maxarr_len, ub4 *curelep,
	                      ub4 mode);
	sword (*OCIDefineByPos)(OCIStmt *stmtp, OCIDefine **defnp, OCIError *errhp, ub4 position, void *valuep,
	                        sb4 value_sz, ub2 dty, void *indp, ub2 *rlenp, ub2 *rcodep, ub4 mode);
	sword (*OCIDirPathPrepare)(OCIDirPathCtx *dpctx, OCISvcCtx *svchp, OCIError *errhp);
	sword (*OCIDirPathColArrayEntrySet)(OCIDirPathColArray *dpca, OCIError *errhp, ub4 rownum, ub2 col_idx,
	                                    ub1 *cvalp, ub4 clen, ub1 cflg);
	sword (*OCIDirPathColArrayRowGet)(OCIDirPathColArray *dpca, OCIError *errhp, ub4 rownum, ub1 ***cvalppp,
	                                  ub4 **clenpp, ub1 **cflgpp);
	sword (*OCIDirPathColArrayToStream)(OCIDirPathColArray *dpca, OCIDirPathCtx *dpctx, OCIDirPathStream *dpstr,
	                                    OCIError *errhp, ub4 rowcnt, ub4 rowoff);
	sword (*OCIDirPathLoadStream)(OCIDirPathCtx *dpctx, OCIDirPathStream *dpstr, OCIError *errhp);
	sword (*OCIDirPathStreamReset)(OCIDirPathStream *dpstr, OCIError *errhp);
	sword (*OCIDirPathFinish)(OCIDirPathCtx *dpctx, OCIError *errhp);
	sword (*OCIDirPathAbort)(OCIDirPathCtx *dpctx, OCIError *errhp);
};

// Loads the OCI functions from the first library of candidates that opens (names or paths, searched
// like the platform's dynamic loader does). Throws OraduckError when none opens or a function is missing.
OciApi LoadOciApi(const std::vector<std::string> &candidates);

// The OCI functions of the process, loaded from Instant Client on first use (thread-safe).
// Throws OraduckError when Instant Client cannot be loaded; the next call tries again.
const OciApi &Oci();

} // namespace oraduck
