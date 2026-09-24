#include "oraduck/direct_path_loader.hpp"

#include <string>

namespace oraduck {

namespace {

// Beyond this, OCI makes no progress: the row does not fit in the stream buffer.
constexpr int kMaxStreamsWithoutProgress = 4096;

// Name passed to OCI_ATTR_NAME / OCI_ATTR_SCHEMA_NAME: double-quoted, otherwise OCI upper-cases it
// (ORA-39826 on "ProbeMixed", see docs/probe-results.md, E7).
std::string DirPathIdentifier(const std::string &name) {
	return "\"" + name + "\"";
}

void SetTextAttr(void *handle, ub4 handle_type, const std::string &value, ub4 attribute, OCIError *err,
                 const char *what) {
	CheckOci(OCIAttrSet(handle, handle_type, const_cast<char *>(value.data()), static_cast<ub4>(value.size()),
	                    attribute, err),
	         err, what);
}

} // namespace

DirectPathLoader::DirectPathLoader(const OracleCredentials &credentials, const std::string &owner,
                                   const std::string &table, const std::vector<DirPathColumnSpec> &columns,
                                   const LoaderOptions &options)
    : session_(credentials), options_(options), owner_(DirPathIdentifier(owner)), table_(DirPathIdentifier(table)) {
	// Strings kept for the loader's whole life (pointers handed to OCI)
	for (const auto &column : columns) {
		column_names_.push_back(DirPathIdentifier(column.name));
		date_formats_.push_back(column.date_format);
	}
	OCIError *err = session_.err();
	try {
		CheckOci(OCIHandleAlloc(session_.env(), reinterpret_cast<void **>(&ctx_), OCI_HTYPE_DIRPATH_CTX, 0, nullptr),
		         err, "OCIHandleAlloc(DIRPATH_CTX)");
		SetTextAttr(ctx_, OCI_HTYPE_DIRPATH_CTX, table_, OCI_ATTR_NAME, err, "OCI_ATTR_NAME (table)");
		SetTextAttr(ctx_, OCI_HTYPE_DIRPATH_CTX, owner_, OCI_ATTR_SCHEMA_NAME, err, "OCI_ATTR_SCHEMA_NAME");
		ub1 parallel = options_.parallel ? 1 : 0;
		CheckOci(OCIAttrSet(ctx_, OCI_HTYPE_DIRPATH_CTX, &parallel, 0, OCI_ATTR_DIRPATH_PARALLEL, err), err,
		         "OCI_ATTR_DIRPATH_PARALLEL");
		ub4 buffer = options_.stream_buffer_bytes;
		CheckOci(OCIAttrSet(ctx_, OCI_HTYPE_DIRPATH_CTX, &buffer, 0, OCI_ATTR_BUF_SIZE, err), err,
		         "OCI_ATTR_BUF_SIZE");
		ub4 rows = options_.column_array_rows;
		CheckOci(OCIAttrSet(ctx_, OCI_HTYPE_DIRPATH_CTX, &rows, 0, OCI_ATTR_NUM_ROWS, err), err, "OCI_ATTR_NUM_ROWS");
		ub2 ncols = static_cast<ub2>(columns.size());
		CheckOci(OCIAttrSet(ctx_, OCI_HTYPE_DIRPATH_CTX, &ncols, 0, OCI_ATTR_NUM_COLS, err), err, "OCI_ATTR_NUM_COLS");

		OCIParam *column_list = nullptr;
		CheckOci(OCIAttrGet(ctx_, OCI_HTYPE_DIRPATH_CTX, &column_list, nullptr, OCI_ATTR_LIST_COLUMNS, err), err,
		         "OCI_ATTR_LIST_COLUMNS");
		for (size_t i = 0; i < columns.size(); i++) {
			OCIParam *column = nullptr;
			CheckOci(OCIParamGet(column_list, OCI_DTYPE_PARAM, err, reinterpret_cast<void **>(&column),
			                     static_cast<ub4>(i + 1)),
			         err, "OCIParamGet(column)");
			SetTextAttr(column, OCI_DTYPE_PARAM, column_names_[i], OCI_ATTR_NAME, err, "OCI_ATTR_NAME (column)");
			ub2 type = columns[i].external_type;
			CheckOci(OCIAttrSet(column, OCI_DTYPE_PARAM, &type, 0, OCI_ATTR_DATA_TYPE, err), err,
			         "OCI_ATTR_DATA_TYPE");
			ub4 size = columns[i].max_size;
			CheckOci(OCIAttrSet(column, OCI_DTYPE_PARAM, &size, 0, OCI_ATTR_DATA_SIZE, err), err,
			         "OCI_ATTR_DATA_SIZE");
			if (!date_formats_[i].empty()) {
				SetTextAttr(column, OCI_DTYPE_PARAM, date_formats_[i], OCI_ATTR_DATEFORMAT, err,
				            "OCI_ATTR_DATEFORMAT");
			}
			OCIDescriptorFree(column, OCI_DTYPE_PARAM);
		}

		CheckOci(OCIDirPathPrepare(ctx_, session_.svc(), err), err, "OCIDirPathPrepare");
		CheckOci(OCIHandleAlloc(ctx_, reinterpret_cast<void **>(&ca_), OCI_HTYPE_DIRPATH_COLUMN_ARRAY, 0, nullptr), err,
		         "OCIHandleAlloc(COLUMN_ARRAY)");
		CheckOci(OCIHandleAlloc(ctx_, reinterpret_cast<void **>(&stream_), OCI_HTYPE_DIRPATH_STREAM, 0, nullptr), err,
		         "OCIHandleAlloc(STREAM)");
		CheckOci(OCIAttrGet(ca_, OCI_HTYPE_DIRPATH_COLUMN_ARRAY, &rows_, nullptr, OCI_ATTR_NUM_ROWS, err), err,
		         "OCI_ATTR_NUM_ROWS (column array)");
		CheckOci(OCIAttrGet(ca_, OCI_HTYPE_DIRPATH_COLUMN_ARRAY, &ncols_, nullptr, OCI_ATTR_NUM_COLS, err), err,
		         "OCI_ATTR_NUM_COLS (column array)");
		DetectContiguousArrays();
	} catch (...) {
		Release(true);
		throw;
	}
}

DirectPathLoader::~DirectPathLoader() {
	Release(true);
}

void DirectPathLoader::DetectContiguousArrays() {
	contiguous_ = false;
	if (options_.force_entry_set || rows_ < 2) {
		return;
	}
	OCIError *err = session_.err();
	ub1 **v0 = nullptr, **v1 = nullptr, **vl = nullptr;
	ub4 *l0 = nullptr, *l1 = nullptr, *ll = nullptr;
	ub1 *f0 = nullptr, *f1 = nullptr, *fl = nullptr;
	CheckOci(OCIDirPathColArrayRowGet(ca_, err, 0, &v0, &l0, &f0), err, "OCIDirPathColArrayRowGet(0)");
	CheckOci(OCIDirPathColArrayRowGet(ca_, err, 1, &v1, &l1, &f1), err, "OCIDirPathColArrayRowGet(1)");
	CheckOci(OCIDirPathColArrayRowGet(ca_, err, rows_ - 1, &vl, &ll, &fl), err, "OCIDirPathColArrayRowGet(n-1)");
	const size_t last = size_t(rows_ - 1) * ncols_;
	contiguous_ = v1 == v0 + ncols_ && l1 == l0 + ncols_ && f1 == f0 + ncols_ && vl == v0 + last &&
	              ll == l0 + last && fl == f0 + last;
	if (contiguous_) {
		values_ = v0;
		lengths_ = l0;
		flags_ = f0;
	}
}

void DirectPathLoader::SetCellSlow(ub4 row, ub2 col, const void *data, ub4 len, ub1 flag) {
	CheckOci(OCIDirPathColArrayEntrySet(ca_, session_.err(), row, col, static_cast<ub1 *>(const_cast<void *>(data)),
	                                    len, flag),
	         session_.err(), "OCIDirPathColArrayEntrySet");
}

void DirectPathLoader::ConvertAndLoad(ub4 nrows) {
	OCIError *err = session_.err();
	ub4 rowoff = 0;
	int without_progress = 0;
	while (rowoff < nrows) {
		const sword rc = OCIDirPathColArrayToStream(ca_, ctx_, stream_, err, nrows, rowoff);
		if (rc == OCI_SUCCESS) {
			stream_pending_ = true;
			rows_in_stream_ += nrows - rowoff;
			break;
		}
		if (rc == OCI_CONTINUE) {
			// Stream full: send it, then resume at the first row not yet converted
			ub4 converted = 0;
			CheckOci(OCIAttrGet(ca_, OCI_HTYPE_DIRPATH_COLUMN_ARRAY, &converted, nullptr, OCI_ATTR_ROW_COUNT, err), err,
			         "OCI_ATTR_ROW_COUNT");
			without_progress = converted == 0 ? without_progress + 1 : 0;
			if (without_progress > kMaxStreamsWithoutProgress) {
				throw OraduckError("a row does not fit in the direct path stream buffer (" +
				                    std::to_string(options_.stream_buffer_bytes) + " bytes): increase STREAM_SIZE");
			}
			stream_pending_ = true;
			rows_in_stream_ += converted;
			LoadStream();
			rowoff += converted;
			continue;
		}
		int32_t code = 0;
		const std::string text = OciErrorText(err, rc, code);
		ub4 bad_row = 0;
		ub2 bad_col = 0;
		OCIAttrGet(ca_, OCI_HTYPE_DIRPATH_COLUMN_ARRAY, &bad_row, nullptr, OCI_ATTR_ROW_COUNT, err);
		OCIAttrGet(ca_, OCI_HTYPE_DIRPATH_COLUMN_ARRAY, &bad_col, nullptr, OCI_ATTR_COL_COUNT, err);
		const std::string column =
		    bad_col < column_names_.size() ? column_names_[bad_col] : "#" + std::to_string(bad_col);
		throw OracleError("direct path conversion failed (row " + std::to_string(rowoff + bad_row) +
		                      " of the batch, column " + column + "): " + text,
		                  code);
	}
	if (!options_.accumulate_stream) {
		LoadStream();
	}
}

void DirectPathLoader::LoadStream() {
	if (!stream_pending_) {
		return;
	}
	OCIError *err = session_.err();
	const sword rc = OCIDirPathLoadStream(ctx_, stream_, err);
	// OCI_NEED_DATA: the stream ends with a partial row (a row wider than the buffer);
	// the rest of it goes into the next stream.
	if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO && rc != OCI_NEED_DATA) {
		int32_t code = 0;
		const std::string text = OciErrorText(err, rc, code);
		throw OracleError("direct path stream rejected by Oracle: " + text, code);
	}
	CheckOci(OCIDirPathStreamReset(stream_, err), err, "OCIDirPathStreamReset");
	rows_loaded_ += rows_in_stream_;
	rows_in_stream_ = 0;
	stream_pending_ = false;
}

void DirectPathLoader::Flush() {
	LoadStream();
}

void DirectPathLoader::Finish() {
	if (done_) {
		throw OraduckError("direct path load already finished");
	}
	Flush();
	CheckOci(OCIDirPathFinish(ctx_, session_.err()), session_.err(), "OCIDirPathFinish");
	done_ = true; // committed: never abort from now on
	Release(false);
}

void DirectPathLoader::Abort() {
	Release(true);
}

void DirectPathLoader::Release(bool abort) {
	if (abort && ctx_ && !done_) {
		OCIDirPathAbort(ctx_, session_.err()); // status ignored: handles are freed in any case
	}
	done_ = true;
	if (ca_) {
		OCIHandleFree(ca_, OCI_HTYPE_DIRPATH_COLUMN_ARRAY);
		ca_ = nullptr;
	}
	if (stream_) {
		OCIHandleFree(stream_, OCI_HTYPE_DIRPATH_STREAM);
		stream_ = nullptr;
	}
	if (ctx_) {
		OCIHandleFree(ctx_, OCI_HTYPE_DIRPATH_CTX);
		ctx_ = nullptr;
	}
	contiguous_ = false;
	values_ = nullptr;
	lengths_ = nullptr;
	flags_ = nullptr;
}

} // namespace oraduck
