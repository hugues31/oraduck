#pragma once

#include "oraduck/oci_api.hpp"
#include "oraduck/oracle_decoding.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oraduck {

// A bind variable: Oracle external type, encoded bytes and the SQL literal shown by EXPLAIN.
struct BindValue {
	ub2 dty = 0;
	std::vector<uint8_t> bytes;
	std::string display;
};

// SQL text with placeholders :b1, :b2, ... and the matching binds (positional, in textual order).
struct SqlFragment {
	std::string sql;
	std::vector<BindValue> binds;
	std::string display; // same text with the literals inlined
};

struct SqlStatement {
	std::string sql;
	std::vector<BindValue> binds;
};

// All throw OraduckError when the value cannot be represented.
BindValue BindNumber(int64_t value);                                 // SQLT_NUM
BindValue BindDecimal(const OracleInt128 &value, uint8_t scale);     // SQLT_NUM, value * 10^-scale
BindValue BindText(const std::string &value);                        // SQLT_CHR
BindValue BindDate(int64_t micros);                                  // SQLT_DAT (7 bytes), seconds truncated
BindValue BindDouble(double value);                                  // SQLT_BDOUBLE

// Quotes a text as an SQL string literal.
std::string SqlStringLiteral(const std::string &value);

} // namespace oraduck
