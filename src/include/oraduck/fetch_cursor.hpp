#pragma once

#include "oraduck/oci_session.hpp"
#include "oraduck/read_types.hpp"
#include "oraduck/sql_binds.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oraduck {

// Fetch settings; the defaults are the read probe's decisions (docs/probe-results.md, "Read path decisions").
struct FetchOptions {
	uint64_t buffer_budget_bytes = 67108864; // one batch: define buffers, LOB locators (12 KiB per row) and values
	uint32_t prefetch_memory_bytes = 0;      // OCI_ATTR_PREFETCH_MEMORY (0: no OCI prefetch)
	uint32_t max_rows = 8192;                // rows per fetch at most
	uint32_t lob_prefetch_chars = 32768;     // LOB prefetch per value: characters (CLOB, NCLOB) or bytes (BLOB)
	uint32_t lob_read_chunk_for_tests = 0;   // test only: characters (bytes) per OCILobRead2 call, 0 = whole value
};

// One query on one session, run once or again with other binds: array fetches into define buffers sized by a
// byte budget, LOBs through locators with LOB prefetch. Throws OracleError (OraduckError for an unusable column
// plan).
class FetchCursor {
public:
	// Prepares, defines (columns[i] at select-list position i + 1) and runs Execute(statement). With no column,
	// position 1 (the constant of BuildScanSql) is defined so that Fetch() still counts rows.
	FetchCursor(OciSession &session, const SqlStatement &statement, std::vector<const ReadColumnPlan *> columns,
	            const FetchOptions &options);
	~FetchCursor();
	FetchCursor(const FetchCursor &) = delete;
	FetchCursor &operator=(const FetchCursor &) = delete;

	// Binds statement's values and executes; its text must be sql() (OraduckError otherwise). The cursor is parsed
	// once however often it runs: Oracle never shares a flashback (AS OF SCN) cursor between parse calls, so a
	// prepare per run costs a hard parse and a child cursor each. The rows the previous run had not fetched are
	// discarded and the values of its last batch become invalid.
	void Execute(const SqlStatement &statement);

	// Next batch: its row count, 0 at the end. Reads the batch's LOB values; when the LOB values of a batch
	// make it larger than the budget, the next batches (of this run and the next ones) ask for fewer rows.
	uint32_t Fetch();

	// The statement text, prepared once
	const std::string &sql() const {
		return sql_;
	}

	// Size of the define arrays: clamp(budget / row bytes, 1, max_rows), where a row counts define_bytes + 8 per
	// column and 12 KiB per LOB column (OCI's memory for a locator and its LOB prefetch)
	uint32_t rows_per_fetch() const {
		return rows_;
	}

	// Value of the last batch in OCI's representation for the column's dty, nullptr for NULL. Valid until
	// the next Fetch(); LOB values are contiguous in a per-batch buffer (an empty LOB has length 0).
	const uint8_t *Value(size_t column, uint32_t row, uint32_t &length) const {
		const Column &c = columns_[column];
		if (c.ind[row] == -1) {
			return nullptr;
		}
		if (c.lob) {
			length = c.lob_length[row];
			return lob_data_.get() + c.lob_offset[row];
		}
		length = c.len[row];
		return c.data.get() + static_cast<size_t>(row) * c.width;
	}

private:
	struct Column {
		std::string name;
		ub2 dty = 0;
		bool lob = false;
		bool national = false;
		size_t width = 0;                // bytes per value in data
		std::unique_ptr<uint8_t[]> data; // rows x width (inline values)
		std::vector<void *> locators;    // LOB columns: one locator per row
		std::vector<sb2> ind;
		std::vector<ub4> len;
		std::vector<ub2> rcode;
		std::vector<size_t> lob_offset; // LOB columns: value of each row in lob_data_
		std::vector<uint32_t> lob_length;
		OCIDefine *define = nullptr; // freed with the statement
	};

	void Open(const std::vector<const ReadColumnPlan *> &columns, const FetchOptions &options);
	void Define(ub4 position, const ReadColumnPlan &plan, const FetchOptions &options, Column &column);
	void CheckValues(uint32_t rows) const;
	void ReadLobs(uint32_t rows);
	void ReadLob(Column &column, uint32_t row);
	void ReserveLob(size_t bytes);
	void Close();

	OciSession &session_;
	uint64_t budget_ = 0;
	uint64_t row_bytes_ = 0; // budgeted bytes per row, without LOB values
	uint32_t rows_ = 1;      // define array size
	uint32_t next_rows_ = 1; // rows asked by the next fetch
	uint32_t lob_read_chunk_ = 0;
	bool has_lobs_ = false;
	std::string sql_;
	OCIStmt *stmt_ = nullptr;
	std::vector<BindValue> binds_; // bound by address: kept until the next run or the release of the statement
	std::vector<sb2> bind_ind_;
	std::vector<OCIBind *> bind_handles_; // freed with the statement; given back to OCI when binding again
	std::vector<Column> columns_;
	std::unique_ptr<uint8_t[]> lob_data_;
	size_t lob_capacity_ = 0;
	size_t lob_used_ = 0;
	bool done_ = false;
};

} // namespace oraduck
