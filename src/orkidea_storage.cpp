#include "orkidea_storage.hpp"

#include "orkidea_secrets.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! The attach target as shown in duckdb_databases(): a URI loses its password.
string DisplayPath(const string &target) {
	auto scheme_end = target.find("://");
	auto at = target.rfind('@');
	if (scheme_end == string::npos || at == string::npos || at < scheme_end) {
		return target;
	}
	auto credentials = target.substr(scheme_end + 3, at - scheme_end - 3);
	auto colon = credentials.find(':');
	if (colon == string::npos) {
		return target;
	}
	return target.substr(0, scheme_end + 3) + credentials.substr(0, colon) + target.substr(at);
}

DatabaseInfo ReadDatabaseInfo(ClientContext &context, ConnectionPool &pool) {
	auto rows = QueryRows(context, pool.Acquire(),
	                      "SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA'),"
	                      " (SELECT value FROM nls_database_parameters WHERE parameter = 'NLS_CHARACTERSET'),"
	                      " (SELECT REGEXP_SUBSTR(version, '^[0-9]+') FROM product_component_version"
	                      "   WHERE product LIKE 'Oracle%' AND ROWNUM = 1)"
	                      " FROM dual");
	DatabaseInfo info;
	info.current_schema = rows[0][0].ToString();
	auto charset = rows[0][1].IsNull() ? string() : rows[0][1].ToString();
	info.unicode = charset == "AL32UTF8" || charset == "UTF8";
	info.utf8_byte_order = charset == "AL32UTF8";
	info.major_version = rows[0][2].IsNull() ? 0 : std::stoull(rows[0][2].ToString());
	return info;
}

unique_ptr<Catalog> Attach(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                           const string &, AttachInfo &info, AttachOptions &options) {
	string secret;
	string schema;
	bool columns_from_dictionary = false;
	for (auto &option : options.options) {
		auto key = StringUtil::Lower(option.first);
		if (key == "secret") {
			secret = option.second.ToString();
		} else if (key == "schema") {
			schema = option.second.ToString();
		} else if (key == "columns") {
			auto source = StringUtil::Lower(option.second.ToString());
			if (source != "describe" && source != "dictionary") {
				throw BinderException("COLUMNS for an orkidea database is 'describe' or 'dictionary', not '%s'",
				                      option.second.ToString());
			}
			columns_from_dictionary = source == "dictionary";
		} else {
			throw BinderException("Unknown option for an orkidea database: %s (expected SECRET, SCHEMA, or COLUMNS)",
			                      option.first);
		}
	}
	if (!secret.empty() && !info.path.empty()) {
		throw BinderException("ATTACH (TYPE orkidea) takes a secret name or URI as its path, or a SECRET option, "
		                      "not both");
	}
	auto target = secret.empty() ? info.path : secret;
	if (target.empty()) {
		throw BinderException("ATTACH (TYPE orkidea) needs the name of an orkidea secret or an oracle:// URI");
	}

	auto database = make_shared_ptr<Database>(Secrets::ResolveTarget(context, target));
	auto pool = make_shared_ptr<ConnectionPool>(std::move(database));
	auto database_info = ReadDatabaseInfo(context, *pool);
	return make_uniq<OrkideaCatalog>(db, std::move(pool), std::move(database_info), std::move(schema),
	                                 columns_from_dictionary, DisplayPath(target));
}

unique_ptr<TransactionManager> CreateTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db,
                                                        Catalog &catalog) {
	return make_uniq<OrkideaTransactionManager>(db, catalog.Cast<OrkideaCatalog>());
}

class OrkideaStorageExtension : public StorageExtension {
public:
	OrkideaStorageExtension() {
		attach = Attach;
		create_transaction_manager = CreateTransactionManager;
	}
};

} // namespace

void Storage::Register(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	StorageExtension::Register(config, "orkidea", make_shared_ptr<OrkideaStorageExtension>());
}

} // namespace orkidea
} // namespace duckdb
