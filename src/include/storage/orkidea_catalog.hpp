//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// storage/orkidea_catalog.hpp
//
// An attached Oracle database. Schemas are Oracle users; tables and views
// are loaded on first use and cached for the life of the attachment.
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/mutex.hpp"

namespace duckdb {
namespace orkidea {

class OrkideaSchemaEntry;

//! What the catalog learns about the database when it attaches.
struct DatabaseInfo {
	//! The connected user's current schema, the default schema in DuckDB.
	string current_schema;
	//! Whether the database character set is AL32UTF8 or UTF8.
	bool unicode = true;
};

//! Idle connections of one attached database, reused across transactions.
class ConnectionPool : public enable_shared_from_this<ConnectionPool> {
public:
	explicit ConnectionPool(shared_ptr<Database> database);

	//! An idle connection, or a new one. It returns to the pool when the last
	//! reference to it goes, unless the pool is gone by then.
	shared_ptr<Connection> Acquire();

private:
	void Release(unique_ptr<Connection> connection);

	shared_ptr<Database> database;
	mutex lock;
	vector<unique_ptr<Connection>> idle;
};

class OrkideaCatalog : public Catalog {
public:
	OrkideaCatalog(AttachedDatabase &db, shared_ptr<ConnectionPool> pool, DatabaseInfo info, string schema_filter,
	               string path);
	~OrkideaCatalog() override;

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "orkidea";
	}
	optional<Identifier> GetDefaultSchema() const override;

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return path;
	}

	CatalogLookupBehavior CatalogTypeLookupRule(CatalogType type) const override;

	shared_ptr<Connection> AcquireConnection() {
		return pool->Acquire();
	}
	const DatabaseInfo &GetDatabaseInfo() const {
		return info;
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

	//! The Oracle schema a DuckDB name refers to: the one of that exact name,
	//! else its upper-case form (an unquoted Oracle name), else the only one
	//! that matches it case-insensitively.
	optional_ptr<OrkideaSchemaEntry> FindSchema(optional_ptr<ClientContext> context, const string &name);
	OrkideaSchemaEntry &AddSchema(const string &oracle_name);
	bool Visible(const string &oracle_name) const;

	shared_ptr<ConnectionPool> pool;
	DatabaseInfo info;
	//! When set, the only schema the catalog shows.
	string schema_filter;
	//! The attach target, without credentials.
	string path;

	mutex schemas_lock;
	//! Keyed by Oracle name, which is case-sensitive.
	unordered_map<string, unique_ptr<OrkideaSchemaEntry>> schemas;
	bool all_schemas_listed = false;
	//! The schemas listings show, in order.
	vector<string> listed_schemas;
	//! Upper-cased names that match no schema, so that lookups the binder
	//! makes for other reasons do not each cost a round trip.
	unordered_set<string> missing_schemas;
};

} // namespace orkidea
} // namespace duckdb
