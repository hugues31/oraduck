#include "oraduck_scan_filters.hpp"

#include "duckdb/planner/expression/bound_between_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck_scan.hpp"

namespace duckdb {

namespace {

using oraduck::FilterConstant;
using oraduck::FilterNode;
using oraduck::FilterOp;

// The Oracle column (index in the table's columns) of a bare reference to a column of the scan
std::optional<size_t> ScanColumn(const Expression &expr, const LogicalGet &get) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return std::nullopt;
	}
	auto &ref = expr.Cast<BoundColumnRefExpression>();
	const auto &binding = ref.Binding();
	if (ref.Depth() != 0 || binding.table_index != get.table_index) {
		return std::nullopt;
	}
	const auto &column_ids = get.GetColumnIds();
	if (!binding.column_index.IsValid() || binding.column_index.GetIndex() >= column_ids.size()) {
		return std::nullopt;
	}
	// The empty column of count(*) and other virtual columns are not Oracle columns
	const auto &id = column_ids[binding.column_index.GetIndex()];
	if (!id.HasPrimaryIndex() || id.IsVirtualColumn() || id.HasChildren()) {
		return std::nullopt;
	}
	const auto column = id.GetPrimaryIndex();
	if (column >= get.returned_types.size() || get.returned_types[column] != expr.GetReturnType()) {
		return std::nullopt;
	}
	return column;
}

// The filter constant of a non-NULL constant of the given DuckDB type
std::optional<FilterConstant> ToConstant(const Expression &expr, const LogicalType &type) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT || expr.GetReturnType() != type) {
		return std::nullopt;
	}
	const auto &value = expr.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull() || value.type() != type) {
		return std::nullopt;
	}
	FilterConstant k;
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
		k.kind = FilterConstant::Kind::kInteger;
		k.i64 = value.GetValue<int64_t>();
		return k;
	case LogicalTypeId::DECIMAL: {
		hugeint_t unscaled;
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			unscaled = hugeint_t(value.GetValueUnsafe<int16_t>());
			break;
		case PhysicalType::INT32:
			unscaled = hugeint_t(value.GetValueUnsafe<int32_t>());
			break;
		case PhysicalType::INT64:
			unscaled = hugeint_t(value.GetValueUnsafe<int64_t>());
			break;
		case PhysicalType::INT128:
			unscaled = value.GetValueUnsafe<hugeint_t>();
			break;
		default:
			return std::nullopt;
		}
		k.kind = FilterConstant::Kind::kDecimal;
		k.i128 = {unscaled.lower, unscaled.upper};
		k.scale = DecimalType::GetScale(type);
		return k;
	}
	case LogicalTypeId::FLOAT:
		k.kind = FilterConstant::Kind::kDouble;
		k.f64 = double(FloatValue::Get(value));
		return k;
	case LogicalTypeId::DOUBLE:
		k.kind = FilterConstant::Kind::kDouble;
		k.f64 = DoubleValue::Get(value);
		return k;
	case LogicalTypeId::VARCHAR:
		// A collation changes the comparison: only plain strings
		if (!StringType::GetCollation(type).empty()) {
			return std::nullopt;
		}
		k.kind = FilterConstant::Kind::kString;
		k.text = StringValue::Get(value);
		return k;
	case LogicalTypeId::TIMESTAMP: {
		const auto ts = TimestampValue::Get(value);
		if (!ts.IsFinite()) {
			return std::nullopt;
		}
		k.kind = FilterConstant::Kind::kTimestamp;
		k.i64 = ts.value;
		return k;
	}
	case LogicalTypeId::DATE: {
		const auto date = DateValue::Get(value);
		if (!date.IsFinite()) {
			return std::nullopt;
		}
		k.kind = FilterConstant::Kind::kDate;
		k.i64 = date.days;
		return k;
	}
	default:
		return std::nullopt;
	}
}

std::optional<FilterOp> ComparisonOp(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return FilterOp::kEq;
	case ExpressionType::COMPARE_NOTEQUAL:
		return FilterOp::kNe;
	case ExpressionType::COMPARE_LESSTHAN:
		return FilterOp::kLt;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return FilterOp::kLe;
	case ExpressionType::COMPARE_GREATERTHAN:
		return FilterOp::kGt;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return FilterOp::kGe;
	default:
		return std::nullopt; // IS [NOT] DISTINCT FROM
	}
}

// constant op column is column flipped(op) constant
FilterOp Flip(FilterOp op) {
	switch (op) {
	case FilterOp::kLt:
		return FilterOp::kGt;
	case FilterOp::kLe:
		return FilterOp::kGe;
	case FilterOp::kGt:
		return FilterOp::kLt;
	case FilterOp::kGe:
		return FilterOp::kLe;
	default:
		return op;
	}
}

// column op constant
std::optional<FilterNode> ColumnComparison(FilterOp op, const Expression &column_expr, const Expression &constant_expr,
                                           const LogicalGet &get) {
	auto column = ScanColumn(column_expr, get);
	if (!column) {
		return std::nullopt;
	}
	auto constant = ToConstant(constant_expr, column_expr.GetReturnType());
	if (!constant) {
		return std::nullopt;
	}
	FilterNode node;
	node.op = op;
	node.column = *column;
	node.constants.push_back(std::move(*constant));
	return node;
}

// CAST(timestamp column AS DATE) = DATE constant
std::optional<FilterNode> DateEquality(const Expression &cast_expr, const Expression &constant_expr,
                                       const LogicalGet &get) {
	if (!BoundCastExpression::IsCast(cast_expr)) {
		return std::nullopt;
	}
	auto &cast = cast_expr.Cast<BoundFunctionExpression>();
	// TRY_CAST is another predicate
	if (!BoundCastExpression::HasValidBindData(cast) || BoundCastExpression::IsTryCast(cast) ||
	    BoundCastExpression::TargetType(cast) != LogicalType::DATE ||
	    BoundCastExpression::SourceType(cast) != LogicalType::TIMESTAMP) {
		return std::nullopt;
	}
	auto column = ScanColumn(BoundCastExpression::Child(cast), get);
	if (!column) {
		return std::nullopt;
	}
	auto constant = ToConstant(constant_expr, LogicalType::DATE);
	if (!constant) {
		return std::nullopt;
	}
	FilterNode node;
	node.op = FilterOp::kDateEq;
	node.column = *column;
	node.constants.push_back(std::move(*constant));
	return node;
}

std::optional<FilterNode> Comparison(const BoundFunctionExpression &comparison, const LogicalGet &get) {
	auto op = ComparisonOp(comparison.GetExpressionType());
	if (!op || comparison.GetChildren().size() != 2) {
		return std::nullopt;
	}
	const auto &left = BoundComparisonExpression::Left(comparison);
	const auto &right = BoundComparisonExpression::Right(comparison);
	if (auto node = ColumnComparison(*op, left, right, get)) {
		return node;
	}
	if (auto node = ColumnComparison(Flip(*op), right, left, get)) {
		return node;
	}
	if (*op != FilterOp::kEq) {
		return std::nullopt;
	}
	if (auto node = DateEquality(left, right, get)) {
		return node;
	}
	return DateEquality(right, left, get);
}

// input BETWEEN lower AND upper: input >= lower AND input <= upper (> and < when exclusive)
std::optional<FilterNode> Between(const BoundFunctionExpression &between, const LogicalGet &get) {
	if (between.GetChildren().size() != 3 || !BoundBetweenExpression::HasValidBindData(between)) {
		return std::nullopt;
	}
	const auto &input = BoundBetweenExpression::Input(between);
	auto lower = ColumnComparison(BoundBetweenExpression::LowerInclusive(between) ? FilterOp::kGe : FilterOp::kGt,
	                              input, BoundBetweenExpression::LowerBound(between), get);
	auto upper = ColumnComparison(BoundBetweenExpression::UpperInclusive(between) ? FilterOp::kLe : FilterOp::kLt,
	                              input, BoundBetweenExpression::UpperBound(between), get);
	if (!lower || !upper) {
		return std::nullopt;
	}
	FilterNode node;
	node.op = FilterOp::kAnd;
	node.children.push_back(std::move(*lower));
	node.children.push_back(std::move(*upper));
	return node;
}

std::optional<FilterNode> Operator(const BoundOperatorExpression &op, const LogicalGet &get) {
	const auto &children = op.GetChildren();
	switch (op.GetExpressionType()) {
	case ExpressionType::OPERATOR_IS_NULL:
	case ExpressionType::OPERATOR_IS_NOT_NULL: {
		if (children.size() != 1) {
			return std::nullopt;
		}
		auto column = ScanColumn(*children[0], get);
		if (!column) {
			return std::nullopt;
		}
		FilterNode node;
		node.op = op.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL ? FilterOp::kIsNull : FilterOp::kIsNotNull;
		node.column = *column;
		return node;
	}
	case ExpressionType::COMPARE_IN: {
		// children[0] IN (children[1], ...)
		if (children.size() < 2) {
			return std::nullopt;
		}
		auto column = ScanColumn(*children[0], get);
		if (!column) {
			return std::nullopt;
		}
		FilterNode node;
		node.op = FilterOp::kIn;
		node.column = *column;
		for (idx_t i = 1; i < children.size(); i++) {
			auto constant = ToConstant(*children[i], children[0]->GetReturnType());
			if (!constant) {
				return std::nullopt;
			}
			node.constants.push_back(std::move(*constant));
		}
		return node;
	}
	default:
		return std::nullopt;
	}
}

std::optional<FilterNode> Conjunction(const BoundConjunctionExpression &conjunction, const LogicalGet &get) {
	FilterNode node;
	switch (conjunction.GetExpressionType()) {
	case ExpressionType::CONJUNCTION_AND:
		node.op = FilterOp::kAnd;
		break;
	case ExpressionType::CONJUNCTION_OR:
		node.op = FilterOp::kOr;
		break;
	default:
		return std::nullopt;
	}
	for (auto &child : conjunction.GetChildren()) {
		auto converted = ExpressionToFilterNode(*child, get);
		if (!converted) {
			return std::nullopt;
		}
		node.children.push_back(std::move(*converted));
	}
	if (node.children.empty()) {
		return std::nullopt;
	}
	return node;
}

bool SameFragment(const oraduck::SqlFragment &a, const oraduck::SqlFragment &b) {
	return a.sql == b.sql && a.display == b.display;
}

} // namespace

std::optional<FilterNode> ExpressionToFilterNode(const Expression &expr, const LogicalGet &get) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CONJUNCTION:
		return Conjunction(expr.Cast<BoundConjunctionExpression>(), get);
	case ExpressionClass::BOUND_OPERATOR:
		return Operator(expr.Cast<BoundOperatorExpression>(), get);
	case ExpressionClass::BOUND_FUNCTION:
		if (BoundComparisonExpression::IsComparison(expr)) {
			return Comparison(expr.Cast<BoundFunctionExpression>(), get);
		}
		if (expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN) {
			return Between(expr.Cast<BoundFunctionExpression>(), get);
		}
		return std::nullopt;
	default:
		return std::nullopt;
	}
}

void OraduckScanPushdownComplexFilter(ClientContext &, LogicalGet &get, FunctionData *bind_data_p,
                                      vector<unique_ptr<Expression>> &filters) {
	auto &bind = bind_data_p->Cast<OraduckScanBindData>();
	for (idx_t i = 0; i < filters.size(); i++) {
		auto node = ExpressionToFilterNode(*filters[i], get);
		if (!node) {
			continue;
		}
		auto fragment = oraduck::TranslateFilter(*node, bind.source.columns, bind.source.db_charset);
		if (!fragment) {
			continue;
		}
		// A later pushdown pass can hand over a filter already pushed
		bool known = false;
		for (auto &filter : bind.filters) {
			known = known || SameFragment(filter, *fragment);
		}
		if (!known) {
			bind.filters.push_back(std::move(*fragment));
		}
		// Oracle applies it: DuckDB no longer does
		filters.erase_at(i);
		i--;
	}
}

} // namespace duckdb
