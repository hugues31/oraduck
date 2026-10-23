#include "oraduck_catalog.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/storage_extension.hpp"

// OraDuck (OCI) headers after DuckDB
#include "oraduck_secret.hpp"
#include "oraduck/oci_session.hpp"

namespace duckdb {

void ThrowOraduckReadOnly() {
	// \xE2\x80\xA6: an ellipsis (UTF-8)
	throw PermissionException("OraDuck: attached Oracle databases are read-only; load data with COPY \xE2\x80\xA6"
	                          " TO '<SCHEMA.TABLE>' (FORMAT ORADUCK, CONNECTION '<secret>')");
}

//===--------------------------------------------------------------------===//
// Catalog
//===--------------------------------------------------------------------===//
OraduckCatalog::OraduckCatalog(AttachedDatabase &db, oraduck::OracleCredentials credentials_p,
                               OraduckScanOptions options_p, string current_schema_p)
    : Catalog(db), credentials(std::move(credentials_p)), options(options_p),
      current_schema(std::move(current_schema_p)) {
}

OraduckCatalog::~OraduckCatalog() {
}

std::unique_ptr<oraduck::OciSession> OraduckCatalog::OpenSession() const {
	return std::make_unique<oraduck::OciSession>(credentials);
}

void OraduckCatalog::Initialize(bool load_builtin) {
}

string OraduckCatalog::GetCatalogType() {
	return "oraduck";
}

optional<Identifier> OraduckCatalog::GetDefaultSchema() const {
	return Identifier(current_schema);
}

CatalogLookupBehavior OraduckCatalog::CatalogTypeLookupRule(CatalogType type) const {
	// Only tables live in Oracle: functions, types, ... are never looked up here (no round trip)
	return type == CatalogType::TABLE_ENTRY ? CatalogLookupBehavior::STANDARD : CatalogLookupBehavior::NEVER_LOOKUP;
}

void OraduckCatalog::LoadUsers() {
	{
		lock_guard<mutex> guard(lock);
		if (users_loaded) {
			return;
		}
	}
	std::set<string> loaded;
	try {
		auto session = OpenSession();
		for (auto &row : session->QueryStrings("SELECT username FROM all_users")) {
			loaded.insert(row[0]);
		}
	} catch (const oraduck::OraduckError &e) {
		throw BinderException("OraDuck: " + string(e.what()));
	}
	lock_guard<mutex> guard(lock);
	if (!users_loaded) {
		users = std::move(loaded);
		users_loaded = true;
	}
}

string OraduckCatalog::ResolveUser(const string &name) {
	if (name == current_schema) {
		return name;
	}
	LoadUsers();
	lock_guard<mutex> guard(lock);
	if (users.count(name)) {
		return name;
	}
	auto upper = StringUtil::Upper(name);
	return users.count(upper) ? upper : string();
}

OraduckSchemaEntry &OraduckCatalog::GetSchemaEntry(const string &user) {
	auto &entry = schemas[user];
	if (!entry) {
		CreateSchemaInfo info;
		info.SetQualifiedName(QualifiedName(vector<Identifier> {Identifier(user)}, Identifier()));
		entry = make_uniq<OraduckSchemaEntry>(*this, info);
	}
	return *entry;
}

optional_ptr<SchemaCatalogEntry> OraduckCatalog::LookupSchema(CatalogTransaction transaction,
                                                              const EntryLookupInfo &schema_lookup,
                                                              OnEntryNotFound if_not_found) {
	return LookupSchemaPath(transaction, schema_lookup, if_not_found,
	                        [&](const EntryLookupInfo &lookup) -> optional_ptr<CatalogEntry> {
		                        auto user = ResolveUser(lookup.GetEntryName());
		                        if (user.empty()) {
			                        return nullptr;
		                        }
		                        lock_guard<mutex> guard(lock);
		                        return &GetSchemaEntry(user);
	                        });
}

void OraduckCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	// The default schema and the schemas already named, without Oracle round trip: DuckDB lists the schemas of every
	// database for SHOW TABLES, information_schema and the suggestions of any missing-entry error
	vector<reference<SchemaCatalogEntry>> entries;
	{
		lock_guard<mutex> guard(lock);
		GetSchemaEntry(current_schema);
		for (auto &entry : schemas) {
			entries.push_back(*entry.second);
		}
	}
	for (auto &entry : entries) {
		callback(entry.get());
	}
}

optional_ptr<CatalogEntry> OraduckCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	ThrowOraduckReadOnly();
}

void OraduckCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	ThrowOraduckReadOnly();
}

void OraduckCatalog::AlterSchema(CatalogTransaction transaction, SchemaCatalogEntry &schema, AlterSchemaInfo &info) {
	ThrowOraduckReadOnly();
}

PhysicalOperator &OraduckCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                    LogicalCreateTable &op, PhysicalOperator &plan) {
	ThrowOraduckReadOnly();
}

PhysicalOperator &OraduckCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                             optional_ptr<PhysicalOperator> plan) {
	ThrowOraduckReadOnly();
}

PhysicalOperator &OraduckCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                             PhysicalOperator &plan) {
	ThrowOraduckReadOnly();
}

PhysicalOperator &OraduckCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                             LogicalDelete &op) {
	ThrowOraduckReadOnly();
}

PhysicalOperator &OraduckCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                             PhysicalOperator &plan) {
	ThrowOraduckReadOnly();
}

PhysicalOperator &OraduckCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                             LogicalUpdate &op) {
	ThrowOraduckReadOnly();
}

PhysicalOperator &OraduckCatalog::PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner,
                                                LogicalMergeInto &op, PhysicalOperator &plan) {
	ThrowOraduckReadOnly();
}

unique_ptr<LogicalOperator> OraduckCatalog::BindCreateIndex(Binder &binder, CreateStatement &stmt,
                                                            TableCatalogEntry &table,
                                                            unique_ptr<LogicalOperator> plan) {
	ThrowOraduckReadOnly();
}

unique_ptr<LogicalOperator> OraduckCatalog::BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
                                                              unique_ptr<LogicalOperator> plan,
                                                              unique_ptr<CreateIndexInfo> create_info,
                                                              unique_ptr<AlterTableInfo> alter_info) {
	ThrowOraduckReadOnly();
}

DatabaseSize OraduckCatalog::GetDatabaseSize(ClientContext &context) {
	return DatabaseSize();
}

bool OraduckCatalog::InMemory() {
	return false;
}

string OraduckCatalog::GetDBPath() {
	return string();
}

//===--------------------------------------------------------------------===//
// Transactions
//===--------------------------------------------------------------------===//
namespace {

class OraduckTransaction : public Transaction {
public:
	OraduckTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {
	}
};

} // namespace

OraduckTransactionManager::OraduckTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
}

Transaction &OraduckTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<OraduckTransaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData OraduckTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> guard(lock);
	transactions.erase(transaction);
	return ErrorData();
}

void OraduckTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(lock);
	transactions.erase(transaction);
}

void OraduckTransactionManager::Checkpoint(ClientContext &context, bool force) {
}

//===--------------------------------------------------------------------===//
// Storage extension
//===--------------------------------------------------------------------===//
namespace {

const char *const kAttachOptions = "secret, sessions, split, snapshot, unconstrained_number";

unique_ptr<Catalog> OraduckAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                                  const string &, AttachInfo &info, AttachOptions &attach_options) {
	string secret_name;
	for (auto &entry : attach_options.options) {
		if (StringUtil::Lower(entry.first) == "secret") {
			secret_name = entry.second.ToString();
		}
	}
	if (secret_name.empty()) {
		throw BinderException("OraDuck: ATTACH needs SECRET (name of an oraduck secret)");
	}
	if (!info.path.empty()) {
		throw BinderException("OraDuck: the ATTACH path must be empty; the connection comes from SECRET");
	}
	OraduckScanOptions options;
	for (auto &entry : attach_options.options) {
		const auto name = StringUtil::Lower(entry.first);
		if (name == "secret") {
			continue;
		}
		if (name != "sessions" && name != "split" && name != "snapshot" && name != "unconstrained_number") {
			throw BinderException("OraDuck: unknown ATTACH option \"%s\" (accepted: %s)", name, kAttachOptions);
		}
		ApplyScanOption(options, name, entry.second);
	}
	auto credentials = GetOraduckCredentials(context, secret_name);
	// One session checks the credentials and reads the default schema; nothing else is read before a query
	string current_schema;
	try {
		oraduck::OciSession session(credentials);
		current_schema = session.QueryStrings("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM dual").at(0).at(0);
	} catch (const oraduck::OraduckError &e) {
		throw IOException("OraDuck: " + string(e.what()));
	}
	return make_uniq<OraduckCatalog>(db, std::move(credentials), options, std::move(current_schema));
}

unique_ptr<TransactionManager> OraduckCreateTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
                                                               Catalog &) {
	return make_uniq<OraduckTransactionManager>(db);
}

class OraduckStorageExtension : public StorageExtension {
public:
	OraduckStorageExtension() {
		attach = OraduckAttach;
		create_transaction_manager = OraduckCreateTransactionManager;
	}
};

} // namespace

void RegisterOraduckStorage(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "oraduck", make_shared_ptr<OraduckStorageExtension>());
}

} // namespace duckdb
