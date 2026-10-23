#include "oraduck/read_sql.hpp"

#include "oraduck/errors.hpp"

#include <cctype>

namespace oraduck {

namespace {

std::string Lower(const std::string &s) {
	std::string out;
	for (char c : s) {
		out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	return out;
}

// Rewrites :bN placeholders as :b(N + offset), outside quoted identifiers and string literals.
std::string ShiftPlaceholders(const std::string &sql, size_t offset) {
	std::string out;
	char quote = 0;
	for (size_t i = 0; i < sql.size(); i++) {
		const char c = sql[i];
		if (quote) {
			if (c == quote) {
				quote = 0;
			}
			out.push_back(c);
		} else if (c == '"' || c == '\'') {
			quote = c;
			out.push_back(c);
		} else if (c == ':' && i + 2 < sql.size() && sql[i + 1] == 'b' &&
		           std::isdigit(static_cast<unsigned char>(sql[i + 2]))) {
			size_t j = i + 2;
			size_t n = 0;
			while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j]))) {
				n = n * 10 + static_cast<size_t>(sql[j] - '0');
				j++;
			}
			out += ":b" + std::to_string(n + offset);
			i = j - 1;
		} else {
			out.push_back(c);
		}
	}
	return out;
}

} // namespace

SplitStrategy ParseSplitStrategy(const std::string &text) {
	const auto v = Lower(text);
	if (v == "auto") {
		return SplitStrategy::kAuto;
	}
	if (v == "extents") {
		return SplitStrategy::kExtents;
	}
	if (v == "hash") {
		return SplitStrategy::kHash;
	}
	if (v == "none") {
		return SplitStrategy::kNone;
	}
	throw OraduckError("invalid value for split: '" + text + "' (accepted: auto, extents, hash, none)");
}

SnapshotMode ParseSnapshotMode(const std::string &text) {
	const auto v = Lower(text);
	if (v == "auto") {
		return SnapshotMode::kAuto;
	}
	if (v == "scn") {
		return SnapshotMode::kScn;
	}
	if (v == "none") {
		return SnapshotMode::kNone;
	}
	throw OraduckError("invalid value for snapshot: '" + text + "' (accepted: auto, scn, none)");
}

std::string QuoteIdentifier(const std::string &name) {
	return "\"" + name + "\"";
}

SqlStatement BuildScanSql(const ScanSqlInput &input, const Slice &slice) {
	SqlStatement stmt;
	auto next = [&stmt]() { return ":b" + std::to_string(stmt.binds.size() + 1); };

	std::string &sql = stmt.sql;
	sql = "SELECT /* oraduck_scan */ ";
	if (input.columns.empty()) {
		sql += "1";
	}
	for (size_t i = 0; i < input.columns.size(); i++) {
		sql += (i > 0 ? ", " : "") + input.columns[i]->select_expr;
	}
	sql += " FROM " + QuoteIdentifier(input.owner) + "." + QuoteIdentifier(input.object);
	if (!input.snapshot_scn.empty()) {
		sql += " AS OF SCN TO_NUMBER(" + next() + ")";
		stmt.binds.push_back(BindText(input.snapshot_scn));
	}

	std::vector<std::string> conditions;
	for (const auto &filter : input.filters) {
		conditions.push_back(ShiftPlaceholders(filter.sql, stmt.binds.size()));
		for (const auto &bind : filter.binds) {
			stmt.binds.push_back(bind);
		}
	}
	switch (slice.kind) {
	case SliceKind::kNone:
		break;
	case SliceKind::kRowidRange: {
		const std::string lo = next();
		stmt.binds.push_back(BindText(slice.rowid_lo));
		const std::string hi = next();
		stmt.binds.push_back(BindText(slice.rowid_hi));
		if (slice.hi_inclusive) {
			conditions.push_back("ROWID BETWEEN CHARTOROWID(" + lo + ") AND CHARTOROWID(" + hi + ")");
		} else {
			conditions.push_back("ROWID >= CHARTOROWID(" + lo + ") AND ROWID < CHARTOROWID(" + hi + ")");
		}
		break;
	}
	case SliceKind::kHash: {
		const std::string buckets = next();
		stmt.binds.push_back(BindNumber(static_cast<int64_t>(slice.hash_buckets) - 1));
		const std::string bucket = next();
		stmt.binds.push_back(BindNumber(static_cast<int64_t>(slice.hash_bucket)));
		conditions.push_back("ORA_HASH(ROWID, " + buckets + ") = " + bucket);
		break;
	}
	}
	for (size_t i = 0; i < conditions.size(); i++) {
		sql += (i == 0 ? " WHERE " : " AND ") + conditions[i];
	}
	return stmt;
}

} // namespace oraduck
