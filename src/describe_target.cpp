#include "oraduck/describe_target.hpp"

#include <string>

namespace oraduck {

TableDescription DescribeTarget(OciSession &session, const TargetName &name) {
	TableDescription description;
	description.owner = name.owner;
	if (description.owner.empty()) {
		description.owner = session.QueryStrings("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM dual").at(0).at(0);
	}
	description.table = name.table;
	const auto rows = session.QueryStrings(
	    "SELECT column_name, data_type, data_length, NVL(data_precision, -1), NVL(data_scale, -1), nullable "
	    "FROM all_tab_columns WHERE owner = :1 AND table_name = :2 ORDER BY column_id",
	    {description.owner, description.table});
	if (rows.empty()) {
		throw OraduckError("Oracle table not found or not accessible: " + description.owner + "." + description.table);
	}
	for (const auto &row : rows) {
		OracleColumn column;
		column.name = row[0];
		column.data_type = row[1];
		column.family = ClassifyOracleType(row[1]);
		column.data_length = static_cast<uint32_t>(std::stoul(row[2]));
		column.precision = std::stoi(row[3]);
		column.scale = std::stoi(row[4]);
		column.nullable = row[5] == "Y";
		description.columns.push_back(std::move(column));
	}
	description.index_count = static_cast<uint32_t>(std::stoul(
	    session
	        .QueryStrings("SELECT COUNT(*) FROM all_indexes WHERE table_owner = :1 AND table_name = :2",
	                      {description.owner, description.table})
	        .at(0)
	        .at(0)));
	return description;
}

} // namespace oraduck
