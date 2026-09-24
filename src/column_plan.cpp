#include "oraduck/column_plan.hpp"

#include "duckdb/common/string_util.hpp"

#include "oraduck/oracle_encoding.hpp"

#include <set>

namespace oraduck {

using duckdb::BinderException;
using duckdb::LogicalTypeId;

namespace {

// Oracle internal TIMESTAMP type, accepted by direct path (docs/probe-results.md, E2)
constexpr ub2 kInternalTimestampType = 180;

const OracleColumn &MatchColumn(const std::string &name, const TableDescription &table) {
	const OracleColumn *exact = nullptr;
	std::vector<const OracleColumn *> insensitive;
	for (const auto &column : table.columns) {
		if (column.name == name) {
			exact = &column;
		}
		if (duckdb::StringUtil::CIEquals(column.name, name)) {
			insensitive.push_back(&column);
		}
	}
	if (exact) {
		return *exact;
	}
	const std::string where = table.owner + "." + table.table;
	if (insensitive.empty()) {
		throw BinderException("OraDuck: column \"" + name + "\" does not exist in " + where);
	}
	if (insensitive.size() > 1) {
		throw BinderException("OraDuck: column \"" + name + "\" matches several columns of " + where +
		                      " (different case)");
	}
	return *insensitive[0];
}

bool IsSignedInteger(LogicalTypeId id) {
	return id == LogicalTypeId::TINYINT || id == LogicalTypeId::SMALLINT || id == LogicalTypeId::INTEGER ||
	       id == LogicalTypeId::BIGINT;
}

void PlanColumnType(PlannedColumn &pc) {
	const auto &type = pc.source_type;
	const auto family = pc.target.family;
	auto mismatch = [&]() {
		throw BinderException("OraDuck: DuckDB type " + type.ToString() +
		                      " is not compatible with Oracle column " + pc.target.name + " (" +
		                      pc.target.data_type + ")");
	};
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		if (family != OracleTypeFamily::kVarchar) {
			mismatch();
		}
		pc.repr = CellRepr::kString;
		pc.spec.external_type = SQLT_CHR;
		pc.spec.max_size = pc.target.data_length;
		return;
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		if (family != OracleTypeFamily::kNumber) {
			mismatch();
		}
		pc.repr = CellRepr::kNative;
		pc.spec.external_type = IsSignedInteger(type.id()) ? SQLT_INT : SQLT_UIN;
		pc.spec.max_size = static_cast<ub4>(duckdb::GetTypeIdSize(type.InternalType()));
		return;
	case LogicalTypeId::DECIMAL:
		if (family != OracleTypeFamily::kNumber) {
			mismatch();
		}
		pc.repr = CellRepr::kNumberEncoded;
		pc.spec.external_type = SQLT_NUM;
		pc.spec.max_size = kOracleNumberMaxBytes;
		pc.scale = duckdb::DecimalType::GetScale(type);
		pc.scratch_width = kOracleNumberMaxBytes;
		return;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE: {
		const bool is_double = type.id() == LogicalTypeId::DOUBLE;
		if (family == OracleTypeFamily::kBinaryDouble && is_double) {
			pc.spec.external_type = SQLT_BDOUBLE;
		} else if (family == OracleTypeFamily::kBinaryFloat && !is_double) {
			pc.spec.external_type = SQLT_BFLOAT;
		} else if (family == OracleTypeFamily::kNumber || family == OracleTypeFamily::kBinaryDouble ||
		           family == OracleTypeFamily::kBinaryFloat) {
			pc.spec.external_type = SQLT_FLT;
		} else {
			mismatch();
		}
		pc.repr = CellRepr::kNative;
		pc.spec.max_size = is_double ? 8 : 4;
		pc.check_finite = family == OracleTypeFamily::kNumber;
		return;
	}
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
		if (family == OracleTypeFamily::kDate) {
			pc.repr = CellRepr::kDateDat;
			pc.spec.external_type = SQLT_DAT;
			pc.spec.max_size = kOracleDateBytes;
			pc.scratch_width = kOracleDateBytes;
		} else if (family == OracleTypeFamily::kTimestamp && pc.target.scale >= 6) {
			// Internal format (type 180): 37 % faster than text (probe, E2). Limited to precisions
			// >= 6: it bypasses the rounding Oracle applies to text.
			pc.repr = CellRepr::kTimestampInternal;
			pc.spec.external_type = kInternalTimestampType;
			pc.spec.max_size = kOracleTimestampBytes;
			pc.scratch_width = kOracleTimestampBytes;
		} else if (family == OracleTypeFamily::kTimestamp) {
			pc.repr = CellRepr::kDateTimeText;
			pc.spec.external_type = SQLT_CHR;
			pc.spec.max_size = kDateTimeTextBytes;
			pc.spec.date_format = "YYYY-MM-DD HH24:MI:SS.FF6";
			pc.scratch_width = kDateTimeTextBytes;
		} else {
			mismatch();
		}
		return;
	default:
		throw BinderException("OraDuck: unsupported DuckDB type " + type.ToString() + " (column " +
		                      pc.target.name + "); convert the column with CAST");
	}
}

} // namespace

std::vector<PlannedColumn> BuildColumnPlan(const std::vector<std::string> &names,
                                           const std::vector<duckdb::LogicalType> &types,
                                           const TableDescription &table) {
	std::vector<PlannedColumn> plan;
	std::set<std::string> used;
	for (duckdb::idx_t i = 0; i < names.size(); i++) {
		PlannedColumn pc;
		pc.source_index = i;
		pc.source_type = types[i];
		pc.target = MatchColumn(names[i], table);
		if (!used.insert(pc.target.name).second) {
			throw BinderException("OraDuck: several query columns target Oracle column " +
			                      pc.target.name);
		}
		pc.spec.name = pc.target.name;
		PlanColumnType(pc);
		plan.push_back(std::move(pc));
	}
	for (const auto &column : table.columns) {
		if (!column.nullable && used.find(column.name) == used.end()) {
			throw BinderException("OraDuck: Oracle column " + column.name +
			                      " is NOT NULL but missing from the query");
		}
	}
	return plan;
}

} // namespace oraduck
