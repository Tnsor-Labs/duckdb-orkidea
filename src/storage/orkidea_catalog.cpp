#include "storage/orkidea_catalog.hpp"

#include "orkidea_insert.hpp"
#include "orkidea_modify.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_schema_entry.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/planner/operator/logical_alter.hpp"
#include "duckdb/storage/database_size.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! Every connection compares strings byte by byte, as DuckDB does, so that
//! filters pushed into Oracle select the same rows.
constexpr const char *SESSION_SETUP = "ALTER SESSION SET NLS_COMP = BINARY NLS_SORT = BINARY";

} // namespace

ConnectionPool::ConnectionPool(shared_ptr<Database> database_p) : database(std::move(database_p)) {
}

shared_ptr<Connection> ConnectionPool::Acquire() {
	unique_ptr<Connection> connection;
	{
		lock_guard<mutex> guard(lock);
		if (!idle.empty()) {
			connection = std::move(idle.back());
			idle.pop_back();
		}
	}
	bool is_new = !connection;
	if (is_new) {
		connection = make_uniq<Connection>(database);
	}
	weak_ptr<ConnectionPool> pool = shared_from_this();
	auto result = shared_ptr<Connection>(connection.release(), [pool](Connection *returned) {
		unique_ptr<Connection> owned(returned);
		auto live = pool.lock();
		if (live && owned->reusable) {
			live->Release(std::move(owned));
		}
	});
	if (is_new) {
		Statement setup(result);
		setup.SetSqlQuery(SESSION_SETUP);
		setup.ExecuteUpdate();
	}
	return result;
}

void ConnectionPool::Release(unique_ptr<Connection> connection) {
	lock_guard<mutex> guard(lock);
	idle.push_back(std::move(connection));
}

OrkideaCatalog::OrkideaCatalog(AttachedDatabase &db, shared_ptr<ConnectionPool> pool_p, DatabaseInfo info_p,
                               string schema_filter_p, bool columns_from_dictionary_p, string path_p)
    : Catalog(db), pool(std::move(pool_p)), info(std::move(info_p)), schema_filter(std::move(schema_filter_p)),
      columns_from_dictionary(columns_from_dictionary_p), path(std::move(path_p)) {
}

OrkideaCatalog::~OrkideaCatalog() = default;

void OrkideaCatalog::Initialize(bool) {
}

optional<Identifier> OrkideaCatalog::GetDefaultSchema() const {
	return Identifier(schema_filter.empty() ? info.current_schema : schema_filter);
}

optional_ptr<CatalogEntry> OrkideaCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
	throw NotImplementedException("Oracle schemas are database users; create the user in Oracle");
}

void OrkideaCatalog::DropSchema(ClientContext &, DropInfo &) {
	throw NotImplementedException("Oracle schemas are database users; drop the user in Oracle");
}

bool OrkideaCatalog::Visible(const string &oracle_name) const {
	return schema_filter.empty() || oracle_name == schema_filter;
}

OrkideaSchemaEntry &OrkideaCatalog::AddSchema(const string &oracle_name) {
	auto entry = schemas.find(oracle_name);
	if (entry != schemas.end()) {
		return *entry->second;
	}
	CreateSchemaInfo create_info;
	create_info.SetSchema(Identifier(oracle_name));
	auto schema = make_uniq<OrkideaSchemaEntry>(*this, create_info);
	auto &result = *schema;
	schemas.emplace(oracle_name, std::move(schema));
	return result;
}

void OrkideaCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	bool listed;
	{
		lock_guard<mutex> guard(schemas_lock);
		listed = all_schemas_listed;
	}
	if (!listed) {
		// Oracle's own schemas (SYS, XDB, ...) are left out of listings, but
		// can still be named. Queried without the lock, so that lookups of
		// schemas already known do not wait for Oracle.
		string sql = "SELECT username FROM all_users WHERE ";
		sql += schema_filter.empty() ? "oracle_maintained = 'N' OR username = " + QuoteLiteral(info.current_schema)
		                             : "username = " + QuoteLiteral(schema_filter);
		auto connection = OrkideaTransaction::Get(context, *this).GetConnection();
		auto rows = QueryRows(context, connection, sql);
		lock_guard<mutex> guard(schemas_lock);
		if (!all_schemas_listed) {
			for (auto &row : rows) {
				listed_schemas.push_back(row[0].ToString());
				AddSchema(listed_schemas.back());
			}
			all_schemas_listed = true;
		}
	}
	vector<reference<OrkideaSchemaEntry>> entries;
	{
		lock_guard<mutex> guard(schemas_lock);
		// Schemas found only by name, such as SYS, stay out of listings.
		for (auto &schema : listed_schemas) {
			entries.push_back(*schemas.at(schema));
		}
	}
	for (auto &entry : entries) {
		callback(entry.get());
	}
}

optional_ptr<OrkideaSchemaEntry> OrkideaCatalog::FindSchema(optional_ptr<ClientContext> context, const string &name) {
	lock_guard<mutex> guard(schemas_lock);
	vector<string> known;
	for (auto &schema : schemas) {
		known.push_back(schema.first);
	}
	auto match = MatchName(name, known);
	auto folded = StringUtil::Upper(name);
	if (match.empty() && context && !missing_schemas.count(folded)) {
		auto sql = "SELECT username FROM all_users WHERE UPPER(username) = " + QuoteLiteral(folded);
		vector<string> found;
		auto connection = OrkideaTransaction::Get(*context, *this).GetConnection();
		for (auto &row : QueryRows(*context, connection, sql)) {
			found.push_back(row[0].ToString());
		}
		match = MatchName(name, found);
		if (match.empty()) {
			missing_schemas.insert(folded);
		} else {
			AddSchema(match);
		}
	}
	if (match.empty() || !Visible(match)) {
		return nullptr;
	}
	return schemas.at(match).get();
}

optional_ptr<SchemaCatalogEntry> OrkideaCatalog::LookupSchema(CatalogTransaction transaction,
                                                              const EntryLookupInfo &schema_lookup,
                                                              OnEntryNotFound if_not_found) {
	auto &name = schema_lookup.GetEntryName();
	auto schema = FindSchema(transaction.context, name);
	if (!schema && if_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw BinderException("Schema \"%s\" not found in Oracle database \"%s\"", name, GetName());
	}
	return schema.get();
}

PhysicalOperator &OrkideaCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                    LogicalCreateTable &op, PhysicalOperator &plan) {
	return OrkideaInsert::PlanCreateTableAs(context, planner, op, plan);
}

PhysicalOperator &OrkideaCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                             optional_ptr<PhysicalOperator> plan) {
	return OrkideaInsert::PlanInsert(context, planner, op, plan);
}

PhysicalOperator &OrkideaCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                             PhysicalOperator &plan) {
	return OrkideaModify::PlanDelete(context, planner, op, plan);
}

PhysicalOperator &OrkideaCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                             PhysicalOperator &plan) {
	return OrkideaModify::PlanUpdate(context, planner, op, plan);
}

void OrkideaCatalog::ForgetTable(const string &schema, const string &table) {
	lock_guard<mutex> guard(schemas_lock);
	auto entry = schemas.find(schema);
	if (entry != schemas.end()) {
		entry->second->ForgetTable(table);
	}
}

unique_ptr<LogicalOperator> OrkideaCatalog::BindAlterAddIndex(Binder &, TableCatalogEntry &,
                                                              unique_ptr<LogicalOperator>, unique_ptr<CreateIndexInfo>,
                                                              unique_ptr<AlterTableInfo> alter_info) {
	unique_ptr<AlterInfo> info = std::move(alter_info);
	return make_uniq<LogicalAlter>(std::move(info));
}

DatabaseSize OrkideaCatalog::GetDatabaseSize(ClientContext &) {
	return DatabaseSize();
}

CatalogLookupBehavior OrkideaCatalog::CatalogTypeLookupRule(CatalogType type) const {
	switch (type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
		return CatalogLookupBehavior::STANDARD;
	default:
		return CatalogLookupBehavior::NEVER_LOOKUP;
	}
}

} // namespace orkidea
} // namespace duckdb
