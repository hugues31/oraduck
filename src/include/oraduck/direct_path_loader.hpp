#pragma once

#include "oraduck/oci_session.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oraduck {

struct DirPathColumnSpec {
	std::string name;        // exact column name in the Oracle dictionary
	ub2 external_type = 0;   // SQLT_CHR, SQLT_INT, SQLT_UIN, SQLT_NUM, SQLT_DAT, SQLT_FLT, SQLT_BFLOAT, SQLT_BDOUBLE
	ub4 max_size = 0;        // maximum size of a value, in bytes
	std::string date_format; // Oracle mask for SQLT_CHR into DATE/TIMESTAMP, empty otherwise
};

struct LoaderOptions {
	ub4 stream_buffer_bytes = 4u << 20; // OCI_ATTR_BUF_SIZE (best probe result, E4)
	ub4 column_array_rows = 2048;       // requested OCI_ATTR_NUM_ROWS
	bool parallel = true;               // OCI_ATTR_DIRPATH_PARALLEL
	bool accumulate_stream = true;      // convert several column arrays into one stream before sending it (E3: 16 % faster)
	bool force_entry_set = false;       // tests: ignore direct access to contiguous column arrays
};

// Direct path load of one table through a dedicated Oracle session. One instance per thread.
class DirectPathLoader {
public:
	DirectPathLoader(const OracleCredentials &credentials, const std::string &owner, const std::string &table,
	                 const std::vector<DirPathColumnSpec> &columns, const LoaderOptions &options);
	~DirectPathLoader();
	DirectPathLoader(const DirectPathLoader &) = delete;
	DirectPathLoader &operator=(const DirectPathLoader &) = delete;

	// Actual column array capacity allocated by OCI.
	ub4 Rows() const {
		return rows_;
	}
	ub2 Columns() const {
		return ncols_;
	}
	bool contiguous_arrays() const {
		return contiguous_;
	}
	uint64_t rows_loaded() const {
		return rows_loaded_;
	}

	// The memory data points to must stay valid until ConvertAndLoad returns.
	void SetCell(ub4 row, ub2 col, const void *data, ub4 len) {
		if (contiguous_) {
			const size_t i = size_t(row) * ncols_ + col;
			values_[i] = static_cast<ub1 *>(const_cast<void *>(data));
			lengths_[i] = len;
			flags_[i] = OCI_DIRPATH_COL_COMPLETE;
		} else {
			SetCellSlow(row, col, data, len, OCI_DIRPATH_COL_COMPLETE);
		}
	}
	void SetNull(ub4 row, ub2 col) {
		if (contiguous_) {
			const size_t i = size_t(row) * ncols_ + col;
			values_[i] = nullptr;
			lengths_[i] = 0;
			flags_[i] = OCI_DIRPATH_COL_NULL;
		} else {
			SetCellSlow(row, col, nullptr, 0, OCI_DIRPATH_COL_NULL);
		}
	}

	// Converts rows [0, nrows) of the column array and sends full streams.
	void ConvertAndLoad(ub4 nrows);
	// Sends the pending stream content.
	void Flush();
	// Flush then OCIDirPathFinish: commits this session's data.
	void Finish();
	// Aborts: nothing is committed.
	void Abort();

private:
	void SetCellSlow(ub4 row, ub2 col, const void *data, ub4 len, ub1 flag);
	void DetectContiguousArrays();
	void LoadStream();
	void Release(bool abort);

	OciSession session_;
	LoaderOptions options_;
	std::string owner_;
	std::string table_;
	std::vector<std::string> column_names_;
	std::vector<std::string> date_formats_;
	OCIDirPathCtx *ctx_ = nullptr;
	OCIDirPathColArray *ca_ = nullptr;
	OCIDirPathStream *stream_ = nullptr;
	ub4 rows_ = 0;
	ub2 ncols_ = 0;
	bool contiguous_ = false;
	ub1 **values_ = nullptr;
	ub4 *lengths_ = nullptr;
	ub1 *flags_ = nullptr;
	bool done_ = false;
	bool stream_pending_ = false;
	uint64_t rows_in_stream_ = 0;
	uint64_t rows_loaded_ = 0;
};

} // namespace oraduck
