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
	uint64_t buffer_budget_bytes = 67108864; // define buffers of one batch
	uint32_t prefetch_memory_bytes = 0;      // OCI_ATTR_PREFETCH_MEMORY (0: no OCI prefetch)
	uint32_t max_rows = 8192;                // rows per fetch at most
	uint32_t lob_prefetch_chars = 32768;     // LOB prefetch per value: characters (CLOB, NCLOB) or bytes (BLOB)
};

// One query on one session: array fetches into define buffers sized by a byte budget, LOBs through
// locators with LOB prefetch. Throws OracleError (OraduckError for an unusable column plan).
class FetchCursor {
public:
	// Prepares, binds, defines (columns[i] at select-list position i + 1) and executes. With no column,
	// position 1 (the constant of BuildScanSql) is defined so that Fetch() still counts rows.
	FetchCursor(OciSession &session, const SqlStatement &statement, std::vector<const ReadColumnPlan *> columns,
	            const FetchOptions &options);
	~FetchCursor();
	FetchCursor(const FetchCursor &) = delete;
	FetchCursor &operator=(const FetchCursor &) = delete;

	// Next batch: its row count, 0 at the end. Reads the batch's LOB values.
	uint32_t Fetch();

	// clamp(budget / sum(define_bytes + 8), 1, max_rows)
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

	void Open(const SqlStatement &statement, const std::vector<const ReadColumnPlan *> &columns,
	          const FetchOptions &options);
	void Define(ub4 position, const ReadColumnPlan &plan, const FetchOptions &options, Column &column);
	void CheckValues(uint32_t rows) const;
	void ReadLobs(uint32_t rows);
	void ReadLob(Column &column, uint32_t row);
	void ReserveLob(size_t bytes);
	void Close();

	OciSession &session_;
	uint32_t rows_ = 1;
	OCIStmt *stmt_ = nullptr;
	std::vector<BindValue> binds_; // bound by address: kept until the statement is released
	std::vector<sb2> bind_ind_;
	std::vector<Column> columns_;
	std::unique_ptr<uint8_t[]> lob_data_;
	size_t lob_capacity_ = 0;
	size_t lob_used_ = 0;
	bool done_ = false;
};

} // namespace oraduck
