#pragma once

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck_scan.hpp"
#include "oraduck/describe_source.hpp"

#include <map>
#include <memory>
#include <set>

namespace duckdb {

class ExtensionLoader;
class OraduckSchemaEntry;
class OraduckTableEntry;

// Registers the storage extension oraduck: ATTACH '' AS name (TYPE oraduck, SECRET <oraduck secret>[, options]).
void RegisterOraduckStorage(ExtensionLoader &loader);

// The PermissionException of every write and DDL path of an attached Oracle database.
[[noreturn]] void ThrowOraduckReadOnly();

// An Oracle database attached read-only. Schemas are Oracle users, tables are Oracle tables, views and synonyms, read
// by oraduck_scan's engine with the ATTACH options as scan options. Descriptions are cached until DETACH; the catalog
// keeps no Oracle session open (each lookup, listing and scan opens its own).
class OraduckCatalog : public Catalog {
public:
	OraduckCatalog(AttachedDatabase &db, oraduck::OracleCredentials credentials, OraduckScanOptions options,
	               string current_schema);
	~OraduckCatalog() override;

	const oraduck::OracleCredentials &GetCredentials() const {
		return credentials;
	}
	const OraduckScanOptions &GetScanOptions() const {
		return options;
	}
	// A new session prepared for reading; throws OraduckError
	std::unique_ptr<oraduck::OciSession> OpenSession() const;

public:
	void Initialize(bool load_builtin) override;
	string GetCatalogType() override;
	optional<Identifier> GetDefaultSchema() const override;
	CatalogLookupBehavior CatalogTypeLookupRule(CatalogType type) const override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	void AlterSchema(CatalogTransaction transaction, SchemaCatalogEntry &schema, AlterSchemaInfo &info) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op) override;
	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;
	unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
	                                              unique_ptr<LogicalOperator> plan,
	                                              unique_ptr<CreateIndexInfo> create_info,
	                                              unique_ptr<AlterTableInfo> alter_info) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override;
	string GetDBPath() override;

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;
	// Reads ALL_USERS once
	void LoadUsers();
	// The Oracle user of a schema name: the exact name, else the upper-cased name; empty when neither exists
	string ResolveUser(const string &name);
	// The entry of an Oracle user (lock held)
	OraduckSchemaEntry &GetSchemaEntry(const string &user);

private:
	oraduck::OracleCredentials credentials;
	OraduckScanOptions options;
	string current_schema; // the session's current schema at ATTACH, the default schema
	mutex lock;            // guards users_loaded, users and schemas
	bool users_loaded = false;
	std::set<string> users;
	std::map<string, unique_ptr<OraduckSchemaEntry>> schemas; // by Oracle user; entries live until DETACH
};

// An Oracle user. One lookup describes one object (DescribeSource), a listing describes every table and view of the
// user at once (DescribeSchema); entries are cached and never replaced.
class OraduckSchemaEntry : public SchemaCatalogEntry {
public:
	OraduckSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);
	~OraduckSchemaEntry() override;

public:
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;
	SimilarCatalogEntry GetSimilarEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateCoordinateSystem(CatalogTransaction transaction,
	                                                  CreateCoordinateSystemInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;

private:
	OraduckCatalog &GetOraduckCatalog();
	// Describes every table and view of the user once
	void LoadTables();
	// The tables and views of the user among the cached entries (not the synonyms)
	vector<reference<CatalogEntry>> ListedTables();
	// The cached entry of name, else a new entry for the description (lock held)
	OraduckTableEntry &AddTable(const string &name, oraduck::OracleTableDescription description);

private:
	mutex lock; // guards tables_loaded and tables
	bool tables_loaded = false;
	std::map<string, unique_ptr<OraduckTableEntry>> tables; // by exact Oracle name; entries live until DETACH
};

// An Oracle table, view or synonym, read by oraduck_scan's function with bind data built from its description.
class OraduckTableEntry : public TableCatalogEntry {
public:
	OraduckTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                  oraduck::OracleTableDescription description);

	// The entry named name of a described table or view: oraduck_scan's column names and types
	static unique_ptr<OraduckTableEntry> Create(OraduckCatalog &catalog, SchemaCatalogEntry &schema, const string &name,
	                                            oraduck::OracleTableDescription description);

	const oraduck::OracleTableDescription &GetDescription() const {
		return description;
	}

public:
	const ColumnList &GetColumns() const override;
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;
	virtual_column_map_t GetVirtualColumns() const override;
	vector<column_t> GetRowIdColumns() const override;

private:
	ColumnList columns;
	oraduck::OracleTableDescription description;
};

// Transactions of an attached Oracle database do nothing: every scan opens its own sessions.
class OraduckTransactionManager : public TransactionManager {
public:
	explicit OraduckTransactionManager(AttachedDatabase &db);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	mutex lock;
	reference_map_t<Transaction, unique_ptr<Transaction>> transactions;
};

} // namespace duckdb
