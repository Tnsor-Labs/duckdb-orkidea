#include "orkidea_secrets.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {
namespace orkidea {

namespace {

constexpr const char *SECRET_TYPE = "orkidea";
constexpr const char *SECRET_PROVIDER = "config";

//! A secret parameter passed to the driver as an ADBC database option.
struct PassThroughOption {
	const char *name;
	LogicalTypeId type;
	const char *adbc_option;
};

// clang-format off
const PassThroughOption PASS_THROUGH_OPTIONS[] = {
	{"uri",                LogicalTypeId::VARCHAR, "uri"},
	{"user",               LogicalTypeId::VARCHAR, "username"},
	{"password",           LogicalTypeId::VARCHAR, "password"},
	{"connect_string",     LogicalTypeId::VARCHAR, "oracle.connect_string"},
	{"config_dir",         LogicalTypeId::VARCHAR, "oracle.config_dir"},
	{"wallet_location",    LogicalTypeId::VARCHAR, "oracle.wallet_location"},
	{"wallet_password",    LogicalTypeId::VARCHAR, "oracle.wallet_password"},
	{"auth_mode",          LogicalTypeId::VARCHAR, "oracle.auth_mode"},
	{"connect_timeout_ms", LogicalTypeId::BIGINT,  "oracle.connect_timeout_ms"},
	{"call_timeout_ms",    LogicalTypeId::BIGINT,  "oracle.call_timeout_ms"},
	{"stmt_cache_size",    LogicalTypeId::BIGINT,  "oracle.stmt_cache_size"},
	{"time_zone",          LogicalTypeId::VARCHAR, "oracle.time_zone"},
	{"edition",            LogicalTypeId::VARCHAR, "oracle.edition"},
};
// clang-format on

//! Parameters that build the connect string: [tcps://]host[:port][/service].
const char *const HOST = "host";
const char *const PORT = "port";
const char *const SERVICE = "service";
const char *const PROTOCOL = "protocol";

//! Accepted for convenience, stored under the canonical name.
const pair<const char *, const char *> ALIASES[] = {{"username", "user"}};

//! Values that are never shown by duckdb_secrets(). A URI may embed a password.
const char *const REDACTED[] = {"password", "wallet_password", "uri"};

string CanonicalName(const string &name) {
	auto lower = StringUtil::Lower(name);
	for (auto &alias : ALIASES) {
		if (lower == alias.first) {
			return alias.second;
		}
	}
	return lower;
}

bool Has(const KeyValueSecret &secret, const char *key) {
	Value value;
	return secret.TryGetValue(Identifier(key), value) && !value.IsNull();
}

string Get(const KeyValueSecret &secret, const char *key) {
	return secret.TryGetValue(Identifier(key)).ToString();
}

void Validate(const KeyValueSecret &secret) {
	bool has_host = Has(secret, HOST);
	if (has_host && Has(secret, "connect_string")) {
		throw InvalidInputException("An orkidea secret takes HOST or CONNECT_STRING, not both");
	}
	if (!has_host) {
		for (auto key : {PORT, SERVICE, PROTOCOL}) {
			if (Has(secret, key)) {
				throw InvalidInputException("The %s of an orkidea secret needs a HOST", StringUtil::Upper(key));
			}
		}
	}
	if (Has(secret, PROTOCOL)) {
		auto protocol = StringUtil::Lower(Get(secret, PROTOCOL));
		if (protocol != "tcp" && protocol != "tcps") {
			throw InvalidInputException("The PROTOCOL of an orkidea secret is TCP or TCPS, not '%s'",
			                            Get(secret, PROTOCOL));
		}
	}
	if (!has_host && !Has(secret, "connect_string") && !Has(secret, "uri")) {
		throw InvalidInputException("An orkidea secret needs HOST, CONNECT_STRING, or URI to know where to connect");
	}
}

unique_ptr<BaseSecret> CreateFromConfig(ClientContext &, CreateSecretInput &input) {
	auto secret = make_uniq<KeyValueSecret>(input.scope, SECRET_TYPE, SECRET_PROVIDER, input.name);
	for (auto &option : input.options) {
		secret->secret_map[Identifier(CanonicalName(option.first))] = option.second;
	}
	for (auto key : REDACTED) {
		secret->redact_keys.insert(Identifier(key));
	}
	Validate(*secret);
	return std::move(secret);
}

AdbcOptions OptionsFromSecret(const KeyValueSecret &secret) {
	AdbcOptions options;
	// The URI goes first: the options after it override what it contains.
	for (auto &option : PASS_THROUGH_OPTIONS) {
		if (Has(secret, option.name)) {
			options.emplace_back(option.adbc_option, Get(secret, option.name));
		}
	}
	if (Has(secret, HOST)) {
		string connect_string;
		if (Has(secret, PROTOCOL) && StringUtil::Lower(Get(secret, PROTOCOL)) == "tcps") {
			connect_string = "tcps://";
		}
		connect_string += Get(secret, HOST);
		if (Has(secret, PORT)) {
			connect_string += ":" + Get(secret, PORT);
		}
		if (Has(secret, SERVICE)) {
			connect_string += "/" + Get(secret, SERVICE);
		}
		options.emplace_back("oracle.connect_string", connect_string);
	}
	return options;
}

} // namespace

void Secrets::Register(ExtensionLoader &loader) {
	SecretType type;
	type.name = SECRET_TYPE;
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = SECRET_PROVIDER;
	loader.RegisterSecretType(type);

	CreateSecretFunction function = {SECRET_TYPE, SECRET_PROVIDER, CreateFromConfig};
	for (auto &option : PASS_THROUGH_OPTIONS) {
		function.named_parameters[Identifier(option.name)] = LogicalType(option.type);
	}
	for (auto &alias : ALIASES) {
		function.named_parameters[Identifier(alias.first)] = LogicalType::VARCHAR;
	}
	function.named_parameters[Identifier(HOST)] = LogicalType::VARCHAR;
	function.named_parameters[Identifier(PORT)] = LogicalType::INTEGER;
	function.named_parameters[Identifier(SERVICE)] = LogicalType::VARCHAR;
	function.named_parameters[Identifier(PROTOCOL)] = LogicalType::VARCHAR;
	loader.RegisterFunction(function);
}

AdbcOptions Secrets::ResolveTarget(ClientContext &context, const string &target) {
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("Connecting to Oracle is disabled by the enable_external_access setting");
	}
	if (target.find("://") != string::npos) {
		return {{"uri", target}};
	}
	auto &manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto entry = manager.GetSecretByName(transaction, target);
	if (!entry) {
		throw InvalidInputException("No secret named '%s': pass the name of an orkidea secret, or an oracle:// URI",
		                            target);
	}
	if (entry->secret->GetType() != Identifier(SECRET_TYPE)) {
		throw InvalidInputException("Secret '%s' is of type %s, not orkidea", target,
		                            entry->secret->GetType().GetIdentifierName());
	}
	return OptionsFromSecret(entry->secret->Cast<KeyValueSecret>());
}

} // namespace orkidea
} // namespace duckdb
