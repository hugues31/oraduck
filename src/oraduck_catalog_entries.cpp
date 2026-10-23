#include "oraduck_catalog.hpp"

#include "duckdb/catalog/similar_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"

// OraDuck (OCI) headers after DuckDB
#include "scan_vectors.hpp"
#include "oraduck/oci_session.hpp"
#include "oraduck/read_types.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Schema entry
//===--------------------------------------------------------------------===//
OraduckSchemaEntry::OraduckSchemaEntry(Catalog &catalog, CreateSchemaInfo &info) : SchemaCatalogEntry(catalog, info) {
}

OraduckSchemaEntry::~OraduckSchemaEntry() {
}

OraduckCatalog &OraduckSchemaEntry::GetOraduckCatalog() {
	return catalog.Cast<OraduckCatalog>();
}

OraduckTableEntry &OraduckSchemaEntry::AddTable(const string &table_name, oraduck::OracleTableDescription description) {
	auto &entry = tables[table_name];
	if (!entry) {
		entry = OraduckTableEntry::Create(GetOraduckCatalog(), *this, table_name, std::move(description));
	}
	return *entry;
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                           const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	// Exact name first, then the upper-cased name (Oracle's unquoted names); a table, view or synonym of the user
	const auto &requested = lookup_info.GetEntryName();
	vector<string> candidates {requested};
	if (StringUtil::Upper(requested) != requested) {
		candidates.push_back(StringUtil::Upper(requested));
	}
	const auto owner = name.GetIdentifierName();
	std::unique_ptr<oraduck::OciSession> session;
	for (auto &candidate : candidates) {
		{
			lock_guard<mutex> guard(lock);
			auto entry = tables.find(candidate);
			if (entry != tables.end()) {
				return entry->second.get();
			}
		}
		oraduck::OracleTableDescription description;
		try {
			if (!session) {
				session = GetOraduckCatalog().OpenSession();
			}
			description = oraduck::DescribeSource(*session, oraduck::TargetName {owner, candidate});
		} catch (const oraduck::SourceNotFoundError &) {
			continue;
		} catch (const oraduck::OraduckError &e) {
			throw BinderException("OraDuck: " + string(e.what()));
		}
		lock_guard<mutex> guard(lock);
		return &AddTable(candidate, std::move(description));
	}
	return nullptr;
}

void OraduckSchemaEntry::LoadTables() {
	{
		lock_guard<mutex> guard(lock);
		if (tables_loaded) {
			return;
		}
	}
	std::vector<oraduck::OracleTableDescription> descriptions;
	try {
		auto session = GetOraduckCatalog().OpenSession();
		descriptions = oraduck::DescribeSchema(*session, name.GetIdentifierName());
	} catch (const oraduck::OraduckError &e) {
		throw IOException("OraDuck: " + string(e.what()));
	}
	lock_guard<mutex> guard(lock);
	if (tables_loaded) {
		return;
	}
	for (auto &description : descriptions) {
		auto table_name = description.name;
		AddTable(table_name, std::move(description));
	}
	tables_loaded = true;
}

vector<reference<CatalogEntry>> OraduckSchemaEntry::ListedTables() {
	vector<reference<CatalogEntry>> result;
	lock_guard<mutex> guard(lock);
	for (auto &entry : tables) {
		const auto &description = entry.second->GetDescription();
		if (description.owner == name.GetIdentifierName() && description.name == entry.first) {
			result.push_back(*entry.second);
		}
	}
	return result;
}

void OraduckSchemaEntry::Scan(ClientContext &context, CatalogType type,
                              const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	LoadTables();
	for (auto &entry : ListedTables()) {
		callback(entry.get());
	}
}

void OraduckSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	// Without a client context: the entries already described only
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	for (auto &entry : ListedTables()) {
		callback(entry.get());
	}
}

SimilarCatalogEntry OraduckSchemaEntry::GetSimilarEntry(CatalogTransaction transaction,
                                                        const EntryLookupInfo &lookup_info) {
	// Suggestions for a missing entry come from the entries already described: describing the whole schema (DuckDB
	// asks every schema of every database) would cost a dictionary query per Oracle user
	SimilarCatalogEntry result;
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return result;
	}
	for (auto &entry : ListedTables()) {
		const auto &entry_name = entry.get().name;
		auto score = StringUtil::SimilarityRating(entry_name.GetIdentifierName(), lookup_info.GetEntryName());
		if (score > result.score) {
			result.score = score;
			result.name = entry_name;
		}
	}
	return result;
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
                                                           TableCatalogEntry &table) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateFunction(CatalogTransaction transaction,
                                                              CreateFunctionInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateView(CatalogTransaction transaction, CreateViewInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateSequence(CatalogTransaction transaction,
                                                              CreateSequenceInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateTableFunction(CatalogTransaction transaction,
                                                                   CreateTableFunctionInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateCopyFunction(CatalogTransaction transaction,
                                                                  CreateCopyFunctionInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreatePragmaFunction(CatalogTransaction transaction,
                                                                    CreatePragmaFunctionInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateCollation(CatalogTransaction transaction,
                                                               CreateCollationInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateCoordinateSystem(CatalogTransaction transaction,
                                                                      CreateCoordinateSystemInfo &info) {
	ThrowOraduckReadOnly();
}

optional_ptr<CatalogEntry> OraduckSchemaEntry::CreateType(CatalogTransaction transaction, CreateTypeInfo &info) {
	ThrowOraduckReadOnly();
}

void OraduckSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	ThrowOraduckReadOnly();
}

void OraduckSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	ThrowOraduckReadOnly();
}

//===--------------------------------------------------------------------===//
// Table entry
//===--------------------------------------------------------------------===//
OraduckTableEntry::OraduckTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                     oraduck::OracleTableDescription description_p)
    : TableCatalogEntry(catalog, schema, info), columns(std::move(info.columns)),
      description(std::move(description_p)) {
}

unique_ptr<OraduckTableEntry> OraduckTableEntry::Create(OraduckCatalog &catalog, SchemaCatalogEntry &schema,
                                                        const string &table_name,
                                                        oraduck::OracleTableDescription description) {
	// The columns of oraduck_scan's bind: DuckColumnNames, the types of PlanReadColumn (unsupported types as VARCHAR,
	// refused when selected)
	CreateTableInfo info(schema, Identifier(table_name));
	const auto names = DuckColumnNames(description);
	const auto &options = catalog.GetScanOptions();
	for (idx_t i = 0; i < description.columns.size(); i++) {
		auto plan = oraduck::PlanReadColumn(description.columns[i], options.unconstrained, description.db_charset);
		info.columns.AddColumn(ColumnDefinition(Identifier(names[i]), ToLogicalType(plan.duck_type)));
	}
	return make_uniq<OraduckTableEntry>(catalog, schema, info, std::move(description));
}

const ColumnList &OraduckTableEntry::GetColumns() const {
	return columns;
}

unique_ptr<BaseStatistics> OraduckTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction OraduckTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto &oraduck_catalog = catalog.Cast<OraduckCatalog>();
	vector<LogicalType> types;
	vector<Identifier> names;
	bind_data = BindOraduckScan(context, oraduck_catalog.GetCredentials(), description,
	                            oraduck_catalog.GetScanOptions(), types, names);
	bind_data->Cast<OraduckScanBindData>().table_entry = this;
	return GetOraduckScanFunction();
}

TableStorageInfo OraduckTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	if (description.num_rows >= 0) {
		result.cardinality = idx_t(description.num_rows);
	}
	return result;
}

virtual_column_map_t OraduckTableEntry::GetVirtualColumns() const {
	// The virtual columns of oraduck_scan: no row id
	virtual_column_map_t result;
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	return result;
}

vector<column_t> OraduckTableEntry::GetRowIdColumns() const {
	return vector<column_t>();
}

} // namespace duckdb
