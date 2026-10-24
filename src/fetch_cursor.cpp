#include "oraduck/fetch_cursor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace oraduck {

namespace {

constexpr size_t kInitialLobBytes = 64 * 1024;
constexpr size_t kMaxUtf8BytesPerChar = 4; // a CLOB character read as UTF-8 takes at most 4 bytes
// Budgeted bytes per row of a LOB column: OCI holds about 4 KiB per locator, 11 KiB with a 32,768-character
// LOB prefetch (measured on small CLOBs); the values themselves are budgeted from the previous batch
constexpr uint64_t kLobRowBytes = 12 * 1024;

// Define of the constant 1 that BuildScanSql selects when no column is projected
const ReadColumnPlan &RowCounterPlan() {
	static const ReadColumnPlan plan = [] {
		ReadColumnPlan p;
		p.name = "1";
		p.decoder = FetchDecoder::kNativeInt64;
		p.dty = SQLT_INT;
		p.define_bytes = 8;
		return p;
	}();
	return plan;
}

uint64_t RowBytes(const std::vector<const ReadColumnPlan *> &columns) {
	uint64_t row_bytes = 0;
	for (const auto *column : columns) {
		row_bytes += column->lob ? kLobRowBytes : static_cast<uint64_t>(column->define_bytes) + 8;
	}
	return std::max<uint64_t>(row_bytes, 1);
}

// clamp(budget / row_bytes, 1, max_rows)
uint32_t RowsWithin(uint64_t budget, uint64_t row_bytes, uint64_t max_rows) {
	return static_cast<uint32_t>(std::min(std::max<uint64_t>(budget / row_bytes, 1), max_rows));
}

const OraText *Text(const std::string &value) {
	return reinterpret_cast<const OraText *>(value.data());
}

} // namespace

FetchCursor::FetchCursor(OciSession &session, const SqlStatement &statement,
                         std::vector<const ReadColumnPlan *> columns, const FetchOptions &options)
    : session_(session) {
	if (columns.empty()) {
		columns.push_back(&RowCounterPlan());
	}
	budget_ = options.buffer_budget_bytes;
	row_bytes_ = RowBytes(columns);
	rows_ = RowsWithin(budget_, row_bytes_, std::max<uint32_t>(options.max_rows, 1));
	next_rows_ = rows_;
	lob_read_chunk_ = options.lob_read_chunk_for_tests;
	sql_ = statement.sql;
	bind_handles_.assign(statement.binds.size(), nullptr);
	bind_ind_.assign(statement.binds.size(), 0);
	try {
		Open(columns, options);
		Execute(statement);
	} catch (...) {
		Close();
		throw;
	}
}

FetchCursor::~FetchCursor() {
	Close();
}

void FetchCursor::Open(const std::vector<const ReadColumnPlan *> &columns, const FetchOptions &options) {
	OCIError *err = session_.err();
	CheckOci(Oci().OCIStmtPrepare2(session_.svc(), &stmt_, err, Text(sql_), static_cast<ub4>(sql_.size()), nullptr,
	                               0, OCI_NTV_SYNTAX, OCI_DEFAULT),
	         err, "OCIStmtPrepare2");
	ub4 prefetch_rows = 0;
	ub4 prefetch_memory = options.prefetch_memory_bytes;
	CheckOci(Oci().OCIAttrSet(stmt_, OCI_HTYPE_STMT, &prefetch_rows, 0, OCI_ATTR_PREFETCH_ROWS, err), err,
	         "OCI_ATTR_PREFETCH_ROWS");
	CheckOci(Oci().OCIAttrSet(stmt_, OCI_HTYPE_STMT, &prefetch_memory, 0, OCI_ATTR_PREFETCH_MEMORY, err), err,
	         "OCI_ATTR_PREFETCH_MEMORY");
	columns_.resize(columns.size());
	for (size_t i = 0; i < columns.size(); i++) {
		Define(static_cast<ub4>(i + 1), *columns[i], options, columns_[i]);
		if (columns_[i].lob && !lob_data_) {
			has_lobs_ = true;
			ReserveLob(kInitialLobBytes); // never a null pointer for an empty LOB
		}
	}
}

void FetchCursor::Execute(const SqlStatement &statement) {
	if (statement.sql != sql_ || statement.binds.size() != bind_handles_.size()) {
		throw OraduckError("fetch cursor run with another statement than the one it prepared");
	}
	OCIError *err = session_.err();
	binds_ = statement.binds;
	static uint8_t empty = 0;
	for (size_t i = 0; i < binds_.size(); i++) {
		auto &bytes = binds_[i].bytes;
		bind_ind_[i] = bytes.empty() ? -1 : 0; // an empty text is NULL in Oracle
		CheckOci(Oci().OCIBindByPos(stmt_, &bind_handles_[i], err, static_cast<ub4>(i + 1),
		                            bytes.empty() ? &empty : bytes.data(), static_cast<sb4>(bytes.size()),
		                            binds_[i].dty, &bind_ind_[i], nullptr, nullptr, 0, nullptr, OCI_DEFAULT),
		         err, "OCIBindByPos");
	}
	done_ = false;
	lob_used_ = 0;
	CheckOci(Oci().OCIStmtExecute(session_.svc(), stmt_, err, 0, 0, nullptr, nullptr, OCI_DEFAULT), err,
	         "OCIStmtExecute");
}

void FetchCursor::Define(ub4 position, const ReadColumnPlan &plan, const FetchOptions &options, Column &column) {
	if (plan.decoder == FetchDecoder::kUnsupported || plan.dty == 0 || (!plan.lob && plan.define_bytes == 0)) {
		throw OraduckError("column \"" + plan.name + "\" has no fetch plan" +
		                   (plan.unsupported.empty() ? "" : ": " + plan.unsupported));
	}
	OCIError *err = session_.err();
	column.name = plan.name;
	column.dty = plan.dty;
	column.lob = plan.lob;
	column.national = plan.national;
	column.ind.assign(rows_, -1);
	column.len.assign(rows_, 0);
	column.rcode.assign(rows_, 0);
	void *values = nullptr;
	sb8 width = 0;
	if (plan.lob) {
		column.locators.assign(rows_, nullptr);
		for (auto &locator : column.locators) {
			CheckOci(Oci().OCIDescriptorAlloc(session_.env(), &locator, OCI_DTYPE_LOB, 0, nullptr), nullptr,
			         "OCIDescriptorAlloc(OCI_DTYPE_LOB)");
		}
		column.lob_offset.assign(rows_, 0);
		column.lob_length.assign(rows_, 0);
		values = column.locators.data();
		width = sizeof(void *);
	} else {
		column.width = plan.define_bytes;
		column.data.reset(new uint8_t[static_cast<size_t>(rows_) * column.width]);
		values = column.data.get();
		width = static_cast<sb8>(column.width);
	}
	CheckOci(Oci().OCIDefineByPos2(stmt_, &column.define, err, position, values, width, plan.dty, column.ind.data(),
	                               column.len.data(), column.rcode.data(), OCI_DEFAULT),
	         err, "OCIDefineByPos2");
	if (plan.national) {
		ub1 form = SQLCS_NCHAR;
		CheckOci(Oci().OCIAttrSet(column.define, OCI_HTYPE_DEFINE, &form, 0, OCI_ATTR_CHARSET_FORM, err), err,
		         "OCI_ATTR_CHARSET_FORM");
	}
	if (plan.lob) {
		if (options.lob_prefetch_chars > 0) {
			ub4 size = options.lob_prefetch_chars;
			CheckOci(Oci().OCIAttrSet(column.define, OCI_HTYPE_DEFINE, &size, 0, OCI_ATTR_LOBPREFETCH_SIZE, err), err,
			         "OCI_ATTR_LOBPREFETCH_SIZE");
		}
		int length = 1; // even without data prefetch: OCILobGetLength2 then costs no round trip
		CheckOci(Oci().OCIAttrSet(column.define, OCI_HTYPE_DEFINE, &length, 0, OCI_ATTR_LOBPREFETCH_LENGTH, err), err,
		         "OCI_ATTR_LOBPREFETCH_LENGTH");
	}
}

uint32_t FetchCursor::Fetch() {
	if (done_) {
		return 0;
	}
	OCIError *err = session_.err();
	const sword rc = Oci().OCIStmtFetch2(stmt_, err, next_rows_, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
	if (rc == OCI_NO_DATA) {
		done_ = true; // the last rows may come with OCI_NO_DATA
	} else {
		CheckOci(rc, err, "OCIStmtFetch2");
	}
	ub4 fetched = 0;
	CheckOci(Oci().OCIAttrGet(stmt_, OCI_HTYPE_STMT, &fetched, nullptr, OCI_ATTR_ROWS_FETCHED, err), err,
	         "OCI_ATTR_ROWS_FETCHED");
	CheckValues(fetched);
	ReadLobs(fetched);
	if (has_lobs_ && fetched > 0) {
		// the next batch asks for the rows that fit the budget with this batch's LOB bytes per row
		const uint64_t lob_bytes_per_row = (lob_used_ + fetched - 1) / fetched;
		next_rows_ = RowsWithin(budget_, row_bytes_ + lob_bytes_per_row, rows_);
	}
	return fetched;
}

// A truncated or unconverted value is reported per value (OCI_SUCCESS_WITH_INFO for the batch): never return it
void FetchCursor::CheckValues(uint32_t rows) const {
	for (const auto &column : columns_) {
		for (uint32_t r = 0; r < rows; r++) {
			if (column.ind[r] != -1 && (column.ind[r] != 0 || column.rcode[r] != 0)) {
				const int32_t code = column.rcode[r] != 0 ? column.rcode[r] : 1406;
				char ora[16];
				std::snprintf(ora, sizeof(ora), "ORA-%05d", static_cast<int>(code));
				throw OracleError("OCIStmtFetch2: column \"" + column.name + "\": value truncated or not converted (" +
				                      ora + ")",
				                  code);
			}
		}
	}
}

void FetchCursor::ReadLobs(uint32_t rows) {
	lob_used_ = 0;
	const size_t keep = static_cast<size_t>(std::max<uint64_t>(2 * budget_, kInitialLobBytes));
	if (lob_capacity_ > keep) {
		// release what an oversized batch (large values, or the first batch) needed beyond twice the budget
		lob_data_.reset();
		lob_capacity_ = 0;
		ReserveLob(keep);
	}
	for (auto &column : columns_) {
		if (!column.lob) {
			continue;
		}
		for (uint32_t r = 0; r < rows; r++) {
			if (column.ind[r] != -1) {
				ReadLob(column, r);
			}
		}
	}
}

// Reads the whole value into lob_data_ (from the LOB prefetch when it fits, otherwise one more round trip).
// Temporary LOBs (computed columns, such as TO_CLOB(...) in a view) are deliberately not freed here: each
// OCILobFreeTemporary costs a round trip; the session releases them when it ends.
void FetchCursor::ReadLob(Column &column, uint32_t row) {
	OCISvcCtx *svc = session_.svc();
	OCIError *err = session_.err();
	auto *locator = static_cast<OCILobLocator *>(column.locators[row]);
	const bool characters = column.dty != SQLT_BLOB;
	ub8 total = 0; // characters (CLOB, NCLOB) or bytes (BLOB)
	CheckOci(Oci().OCILobGetLength2(svc, err, locator, &total), err, "OCILobGetLength2");
	if (total > std::numeric_limits<uint32_t>::max()) {
		throw OraduckError("value of column \"" + column.name + "\" larger than 4 GiB");
	}
	const size_t capacity = static_cast<size_t>(total) * (characters ? kMaxUtf8BytesPerChar : 1);
	ReserveLob(capacity);
	uint8_t *out = lob_data_.get() + lob_used_;
	const ub1 form = column.national ? SQLCS_NCHAR : SQLCS_IMPLICIT;
	ub8 done = 0;    // characters or bytes read
	size_t used = 0; // bytes written
	while (done < total) {
		ub8 bytes = characters ? 0 : total - done;
		ub8 chars = characters ? total - done : 0;
		if (lob_read_chunk_) {
			(characters ? chars : bytes) = std::min<ub8>(characters ? chars : bytes, lob_read_chunk_);
		}
		CheckOci(Oci().OCILobRead2(svc, err, locator, &bytes, &chars, done + 1, out + used, capacity - used,
		                           OCI_ONE_PIECE, nullptr, nullptr, 0, form),
		         err, "OCILobRead2");
		const ub8 progress = characters ? chars : bytes;
		if (progress == 0 || bytes > capacity - used) {
			throw OraduckError("OCILobRead2: column \"" + column.name + "\": unexpected read of " +
			                   std::to_string(bytes) + " bytes at offset " + std::to_string(done + 1));
		}
		done += progress;
		used += static_cast<size_t>(bytes);
	}
	if (used > std::numeric_limits<uint32_t>::max()) {
		throw OraduckError("value of column \"" + column.name + "\" larger than 4 GiB");
	}
	column.lob_offset[row] = lob_used_;
	column.lob_length[row] = static_cast<uint32_t>(used);
	lob_used_ += used;
}

// Room for bytes more after lob_used_ (values are kept as offsets: the buffer may move)
void FetchCursor::ReserveLob(size_t bytes) {
	if (lob_capacity_ - lob_used_ >= bytes) {
		return;
	}
	const size_t capacity = std::max(lob_used_ + bytes, std::max(2 * lob_capacity_, kInitialLobBytes));
	std::unique_ptr<uint8_t[]> data(new uint8_t[capacity]);
	if (lob_used_) {
		std::memcpy(data.get(), lob_data_.get(), lob_used_);
	}
	lob_data_ = std::move(data);
	lob_capacity_ = capacity;
}

void FetchCursor::Close() {
	if (stmt_) {
		Oci().OCIStmtRelease(stmt_, session_.err(), nullptr, 0, OCI_DEFAULT); // frees the binds and defines
		stmt_ = nullptr;
	}
	for (auto &column : columns_) {
		for (void *locator : column.locators) {
			if (locator) {
				Oci().OCIDescriptorFree(locator, OCI_DTYPE_LOB);
			}
		}
	}
	columns_.clear();
}

} // namespace oraduck
