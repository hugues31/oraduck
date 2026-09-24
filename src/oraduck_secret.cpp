#include "oraduck_secret.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

namespace {

constexpr const char *kSecretType = "oraduck";

unique_ptr<BaseSecret> CreateOraduckSecret(ClientContext &, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	for (const char *key : {"user", "password", "dsn"}) {
		if (!secret->TrySetValue(key, input)) {
			throw InvalidInputException("OraDuck: the secret must define " + StringUtil::Upper(key));
		}
	}
	secret->redact_keys = {"password"};
	return std::move(secret);
}

} // namespace

void RegisterOraduckSecret(ExtensionLoader &loader) {
	SecretType type;
	type.name = kSecretType;
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = "config";
	type.extension = "oraduck";
	loader.RegisterSecretType(type);

	CreateSecretFunction function;
	function.secret_type = kSecretType;
	function.provider = "config";
	function.function = CreateOraduckSecret;
	function.named_parameters["user"] = LogicalType::VARCHAR;
	function.named_parameters["password"] = LogicalType::VARCHAR;
	function.named_parameters["dsn"] = LogicalType::VARCHAR;
	loader.RegisterFunction(function);
}

oraduck::OracleCredentials GetOraduckCredentials(ClientContext &context, const string &secret_name) {
	auto &manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto entry = manager.GetSecretByName(transaction, secret_name);
	if (!entry) {
		throw BinderException("OraDuck: secret \"" + secret_name + "\" not found (CREATE SECRET " + secret_name +
		                      " (TYPE oraduck, USER …, PASSWORD …, DSN …))");
	}
	if (entry->secret->GetType() != kSecretType) {
		throw BinderException("OraDuck: secret \"" + secret_name + "\" has type " + entry->secret->GetType() +
		                      ", not oraduck");
	}
	const auto &kv = dynamic_cast<const KeyValueSecret &>(*entry->secret);
	oraduck::OracleCredentials credentials;
	credentials.user = kv.TryGetValue("user", true).ToString();
	credentials.password = kv.TryGetValue("password", true).ToString();
	credentials.dsn = kv.TryGetValue("dsn", true).ToString();
	return credentials;
}

} // namespace duckdb
