#include "oraduck/oracle_names.hpp"

#include "oraduck/errors.hpp"

#include <cctype>

namespace oraduck {

namespace {

bool IsIdentifierChar(char c) {
	const auto u = static_cast<unsigned char>(c);
	return std::isalnum(u) || c == '_' || c == '$' || c == '#';
}

std::string NormalizeIdentifier(const std::string &part, const std::string &text) {
	if (part.size() >= 2 && part.front() == '"' && part.back() == '"') {
		const std::string inner = part.substr(1, part.size() - 2);
		if (inner.empty() || inner.find('"') != std::string::npos) {
			throw OraduckError("invalid quoted identifier in the target table name: " + text);
		}
		return inner;
	}
	if (part.empty() || !std::isalpha(static_cast<unsigned char>(part[0]))) {
		throw OraduckError("invalid target table name (expected TABLE or SCHEMA.TABLE): " + text);
	}
	std::string upper;
	upper.reserve(part.size());
	for (char c : part) {
		if (!IsIdentifierChar(c)) {
			throw OraduckError("invalid character in the target table name: " + text);
		}
		upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return upper;
}

} // namespace

TargetName ParseTargetName(const std::string &text) {
	const auto begin = text.find_first_not_of(" \t");
	if (begin == std::string::npos) {
		throw OraduckError("empty target table name");
	}
	const auto end = text.find_last_not_of(" \t");
	const std::string trimmed = text.substr(begin, end - begin + 1);

	std::vector<std::string> parts;
	std::string current;
	bool in_quotes = false;
	for (char c : trimmed) {
		if (c == '"') {
			in_quotes = !in_quotes;
			current += c;
		} else if (c == '.' && !in_quotes) {
			parts.push_back(current);
			current.clear();
		} else {
			current += c;
		}
	}
	if (in_quotes) {
		throw OraduckError("unterminated double quote in the target table name: " + text);
	}
	parts.push_back(current);
	if (parts.size() > 2) {
		throw OraduckError("invalid target table name (expected TABLE or SCHEMA.TABLE): " + text);
	}
	TargetName result;
	if (parts.size() == 2) {
		result.owner = NormalizeIdentifier(parts[0], text);
		result.table = NormalizeIdentifier(parts[1], text);
	} else {
		result.table = NormalizeIdentifier(parts[0], text);
	}
	return result;
}

OracleTypeFamily ClassifyOracleType(const std::string &data_type) {
	if (data_type == "NUMBER" || data_type == "FLOAT") {
		return OracleTypeFamily::kNumber;
	}
	if (data_type == "BINARY_FLOAT") {
		return OracleTypeFamily::kBinaryFloat;
	}
	if (data_type == "BINARY_DOUBLE") {
		return OracleTypeFamily::kBinaryDouble;
	}
	if (data_type == "VARCHAR2" || data_type == "CHAR") {
		return OracleTypeFamily::kVarchar;
	}
	if (data_type == "DATE") {
		return OracleTypeFamily::kDate;
	}
	if (data_type.rfind("TIMESTAMP", 0) == 0 && data_type.find("WITH") == std::string::npos) {
		return OracleTypeFamily::kTimestamp;
	}
	return OracleTypeFamily::kUnsupported;
}

} // namespace oraduck
