#include "oraduck/filter_sql.hpp"

#include "oraduck/errors.hpp"
#include "oraduck/oracle_encoding.hpp"
#include "oraduck/read_types.hpp"
#include "oraduck/read_sql.hpp"
#include "oraduck/sql_binds.hpp"

#include <cmath>
#include <cctype>
#include <cstdlib>

namespace oraduck {

namespace {

enum class ColumnKind { kNumber, kBinaryFloat, kBinaryDouble, kDate, kTimestamp, kChar, kLob, kOther };

constexpr size_t kMaxInList = 1000;      // Oracle: ORA-01795 above 1000 expressions
constexpr size_t kMaxBindTextBytes = 4000; // longer text binds are not compared as VARCHAR2

struct ColumnInfo {
	ColumnKind kind = ColumnKind::kOther;
	bool national = false;
	bool binary_collation = true; // string comparisons follow binary order (no column-level collation)
};

std::string Upper(const std::string &s) {
	std::string out;
	for (char c : s) {
		out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	}
	return out;
}

bool StartsWith(const std::string &s, const char *prefix) {
	return s.compare(0, std::string(prefix).size(), prefix) == 0;
}

ColumnInfo Classify(const OracleColumn &c, const std::string &db_charset) {
	ColumnInfo info;
	info.national = c.national;
	const std::string collation = Upper(c.collation);
	info.binary_collation = collation.empty() || collation == "USING_NLS_COMP" || collation == "BINARY";
	const std::string &t = c.data_type;
	if (t == "NUMBER") {
		// Pushed only when the read path maps the column to an exact type: Oracle compares exact
		// decimals, DuckDB compares the DOUBLE it receives otherwise.
		const auto id = PlanReadColumn(c, UnconstrainedNumber(), db_charset).duck_type.id;
		if (id == DuckTypeId::kSmallint || id == DuckTypeId::kInteger || id == DuckTypeId::kBigint ||
		    id == DuckTypeId::kDecimal) {
			info.kind = ColumnKind::kNumber;
		}
	} else if (t == "BINARY_FLOAT") {
		info.kind = ColumnKind::kBinaryFloat;
	} else if (t == "BINARY_DOUBLE") {
		info.kind = ColumnKind::kBinaryDouble;
	} else if (t == "DATE") {
		info.kind = ColumnKind::kDate;
	} else if (StartsWith(t, "TIMESTAMP")) {
		// TIMESTAMP(n) up to 6 fractional digits; time zones are never pushed
		if (t.find("WITH") == std::string::npos) {
			const auto open = t.find('(');
			int n = 6; // TIMESTAMP without a precision means 6
			if (open != std::string::npos) {
				n = std::atoi(t.c_str() + open + 1);
			}
			if (n <= 6) {
				info.kind = ColumnKind::kTimestamp;
			}
		}
	} else if (t == "VARCHAR2" || t == "NVARCHAR2" || t == "CHAR" || t == "NCHAR") {
		info.kind = ColumnKind::kChar;
		info.national = info.national || t[0] == 'N';
	} else if (t == "CLOB" || t == "NCLOB" || t == "BLOB") {
		info.kind = ColumnKind::kLob;
	}
	return info;
}

struct Builder {
	const std::vector<OracleColumn> &columns;
	const std::string &db_charset;
	bool utf8;
	bool al32utf8; // string ranges: UTF8 (CESU-8) sorts supplementary characters differently
	SqlFragment out;

	std::string NextPlaceholder() const {
		return ":b" + std::to_string(out.binds.size() + 1);
	}

	// Adds the bind (placeholder in sql, literal in display).
	void AddBind(BindValue bind, std::string &sql, std::string &display, const char *wrap_prefix = "",
	             const char *wrap_suffix = "", const std::string *display_override = nullptr) {
		sql += wrap_prefix + NextPlaceholder() + wrap_suffix;
		display += display_override ? *display_override : bind.display;
		out.binds.push_back(std::move(bind));
	}

	static bool IsAscii(const std::string &s) {
		for (unsigned char c : s) {
			if (c >= 0x80) {
				return false;
			}
		}
		return true;
	}

	// SQL and display of the operand for constant k compared with column c; false when not pushable.
	bool Operand(const ColumnInfo &info, FilterOp op, const FilterConstant &k, std::string &sql, std::string &display) {
		using Kind = FilterConstant::Kind;
		const bool is_range = op == FilterOp::kLt || op == FilterOp::kLe || op == FilterOp::kGt || op == FilterOp::kGe;
		switch (info.kind) {
		case ColumnKind::kNumber:
			if (k.kind == Kind::kInteger) {
				AddBind(BindNumber(k.i64), sql, display);
				return true;
			}
			if (k.kind == Kind::kDecimal) {
				AddBind(BindDecimal(k.i128, k.scale), sql, display);
				return true;
			}
			return false;
		case ColumnKind::kBinaryFloat:
		case ColumnKind::kBinaryDouble:
			if (k.kind != Kind::kDouble || !std::isfinite(k.f64)) {
				return false;
			}
			AddBind(BindDouble(k.f64), sql, display);
			return true;
		case ColumnKind::kDate: {
			int64_t micros;
			if (!DateMicros(k, micros) || micros % kMicrosPerSecond != 0) {
				return false;
			}
			AddBind(BindDate(micros), sql, display);
			return true;
		}
		case ColumnKind::kTimestamp: {
			int64_t micros;
			if (!DateMicros(k, micros)) {
				return false;
			}
			int64_t days, micros_of_day;
			SplitMicros(micros, days, micros_of_day);
			char text[kDateTimeTextBytes];
			if (!FormatDateTimeText(days, micros_of_day, text)) {
				return false;
			}
			const std::string value(text, kDateTimeTextBytes);
			const std::string shown = "TIMESTAMP " + SqlStringLiteral(value);
			AddBind(BindText(value), sql, display, "TO_TIMESTAMP(", ", 'YYYY-MM-DD HH24:MI:SS.FF6')", &shown);
			return true;
		}
		case ColumnKind::kChar:
			if (k.kind != Kind::kString || k.text.empty() || k.text.size() > kMaxBindTextBytes ||
			    k.text.find('\0') != std::string::npos || !info.binary_collation) {
				return false;
			}
			if (is_range && (!al32utf8 || info.national)) {
				return false;
			}
			// Another character set may not represent the constant: only ASCII is pushed
			if (!utf8 && !IsAscii(k.text)) {
				return false;
			}
			// Not BindText: the trailing blanks of the constant matter (non-padded comparison)
			AddBind(BindVarchar(k.text), sql, display);
			return true;
		default:
			return false;
		}
	}

	static int64_t Days(int64_t y, uint32_t m, uint32_t d) {
		return DaysFromCivil(y, m, d);
	}

	static bool DateMicros(const FilterConstant &k, int64_t &micros) {
		using Kind = FilterConstant::Kind;
		if (k.kind == Kind::kTimestamp) {
			micros = k.i64;
			return true;
		}
		if (k.kind == Kind::kDate) {
			if (k.i64 > 4000000 || k.i64 < -4000000) { // far beyond year 9999: avoid overflow
				return false;
			}
			micros = k.i64 * kMicrosPerDay;
			return true;
		}
		return false;
	}

	bool Node(const FilterNode &node, std::string &sql, std::string &display) {
		switch (node.op) {
		case FilterOp::kAnd:
		case FilterOp::kOr: {
			if (node.children.empty()) {
				return false;
			}
			const char *glue = node.op == FilterOp::kAnd ? " AND " : " OR ";
			sql = "(";
			display = "(";
			for (size_t i = 0; i < node.children.size(); i++) {
				std::string child_sql, child_display;
				if (!Node(node.children[i], child_sql, child_display)) {
					return false;
				}
				if (i > 0) {
					sql += glue;
					display += glue;
				}
				sql += child_sql;
				display += child_display;
			}
			sql += ")";
			display += ")";
			return true;
		}
		default:
			return Leaf(node, sql, display);
		}
	}

	bool Leaf(const FilterNode &node, std::string &sql, std::string &display) {
		if (node.column >= columns.size()) {
			return false;
		}
		const auto &column = columns[node.column];
		const ColumnInfo info = Classify(column, db_charset);
		const std::string quoted = QuoteIdentifier(column.name);
		if (node.op == FilterOp::kIsNull || node.op == FilterOp::kIsNotNull) {
			if (info.kind == ColumnKind::kOther && !IsUnconstrainedNumber(column)) {
				return false;
			}
			sql = display = "(" + quoted + (node.op == FilterOp::kIsNull ? " IS NULL)" : " IS NOT NULL)");
			return true;
		}
		if (info.kind == ColumnKind::kLob || info.kind == ColumnKind::kOther) {
			return false;
		}
		if (node.op == FilterOp::kDateEq) {
			using Kind = FilterConstant::Kind;
			if (info.kind != ColumnKind::kDate || node.constants.size() != 1 || node.constants[0].kind != Kind::kDate) {
				return false;
			}
			int64_t micros;
			// d + 1 would raise ORA-01841 at 9999-12-31
			if (!DateMicros(node.constants[0], micros) || node.constants[0].i64 >= Days(9999, 12, 31)) {
				return false;
			}
			std::string s1, d1, s2, d2;
			AddBind(BindDate(micros), s1, d1);
			AddBind(BindDate(micros), s2, d2);
			sql = "(" + quoted + " >= " + s1 + " AND " + quoted + " < " + s2 + " + 1)";
			display = "(" + quoted + " >= " + d1 + " AND " + quoted + " < " + d2 + " + 1)";
			return true;
		}
		if (node.op == FilterOp::kIn) {
			if (node.constants.empty() || node.constants.size() > kMaxInList) {
				return false;
			}
			sql = "(" + quoted + " IN (";
			display = sql;
			for (size_t i = 0; i < node.constants.size(); i++) {
				std::string s, d;
				if (!Operand(info, node.op, node.constants[i], s, d)) {
					return false;
				}
				const char *glue = i > 0 ? ", " : "";
				sql += glue + s;
				display += glue + d;
			}
			sql += "))";
			display += "))";
			return true;
		}
		const char *symbol = nullptr;
		switch (node.op) {
		case FilterOp::kEq:
			symbol = "=";
			break;
		case FilterOp::kNe:
			symbol = "<>";
			break;
		case FilterOp::kLt:
			symbol = "<";
			break;
		case FilterOp::kLe:
			symbol = "<=";
			break;
		case FilterOp::kGt:
			symbol = ">";
			break;
		case FilterOp::kGe:
			symbol = ">=";
			break;
		default:
			return false;
		}
		if (node.constants.size() != 1) {
			return false;
		}
		std::string s, d;
		if (!Operand(info, node.op, node.constants[0], s, d)) {
			return false;
		}
		sql = "(" + quoted + " " + symbol + " " + s + ")";
		display = "(" + quoted + " " + symbol + " " + d + ")";
		return true;
	}

	static bool IsUnconstrainedNumber(const OracleColumn &c) {
		return c.data_type == "NUMBER";
	}
};

} // namespace

std::optional<SqlFragment> TranslateFilter(const FilterNode &node, const std::vector<OracleColumn> &columns,
                                           const std::string &db_charset) {
	Builder builder {columns, db_charset, IsUtf8Charset(db_charset), Upper(db_charset) == "AL32UTF8", {}};
	try {
		std::string sql, display;
		if (!builder.Node(node, sql, display)) {
			return std::nullopt;
		}
		builder.out.sql = std::move(sql);
		builder.out.display = std::move(display);
	} catch (const OraduckError &) {
		return std::nullopt; // a constant that cannot be bound
	}
	return std::move(builder.out);
}

} // namespace oraduck
