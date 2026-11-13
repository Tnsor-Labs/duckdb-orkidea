//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// storage/orkidea_schema_entry.hpp
//
// An Oracle schema. Its tables, views, and materialized views appear as
// DuckDB tables, described on first use.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/arrow/arrow.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {
namespace orkidea {

class OrkideaCatalog;
class OrkideaTableEntry;

//! A table as the schema's listing shows it, before it is described.
struct TableListing {
	//! The optimizer's row count, or -1 when unknown.
	int64_t estimated_rows = -1;
	//! Whether several sessions can read it in ROWID ranges.
	bool splittable = false;
	//! Whether its rows have ROWIDs (a table that is not index-organized).
	bool has_rowid = false;

	static TableListing FromRow(const vector<Value> &row);
};

class OrkideaSchemaEntry : public SchemaCatalogEntry {
public:
	OrkideaSchemaEntry(OrkideaCatalog &catalog, CreateSchemaInfo &info);
	~OrkideaSchemaEntry() override;

	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;

	//! Drops a table from the cache, after it was dropped in Oracle.
	void ForgetTable(const string &oracle_name);

	//! The primary and unique key columns of a table, as rows of constraint
	//! name, type ('P' or 'U'), and column name, in key order: for a listed
	//! schema, from one query for all its tables. False when they must be
	//! read for the table alone: the schema was not listed, or the table was
	//! created or altered since.
	bool ListedKeys(ClientContext &context, const string &oracle_name, vector<vector<Value>> &rows);

private:
	OrkideaCatalog &GetOrkideaCatalog();
	//! Lists the names of the schema's tables, once.
	void ListTables(ClientContext &context);
	//! Tables of the schema that meet an Oracle condition on `table_name`.
	vector<string> QueryTableNames(ClientContext &context, const string &condition);
	//! The table a DuckDB name refers to, described on first use.
	optional_ptr<OrkideaTableEntry> FindTable(ClientContext &context, const string &name);
	//! The table's entry, described on first use.
	optional_ptr<OrkideaTableEntry> DescribeTable(ClientContext &context, const string &oracle_name);
	//! Adds the entry of a table whose columns are `count` children of
	//! `described` from `offset`; or returns the entry added meanwhile.
	optional_ptr<OrkideaTableEntry> AddTable(ClientContext &context, const string &oracle_name,
	                                         const ArrowSchema &described, idx_t offset, idx_t count);
	//! Describes tables for a listing, several to a query; ones that cannot be
	//! described are left out.
	void DescribeTables(ClientContext &context, const vector<string> &oracle_names);
	//! Describes tables with one query; when that fails, each half on its own.
	void DescribeBatch(ClientContext &context, const vector<string> &oracle_names);
	//! Describes tables one by one, leaving out ones that cannot be described.
	void DescribeEach(ClientContext &context, const vector<string> &oracle_names);
	//! Drops a table in Oracle and from the cache.
	void DropTable(ClientContext &context, OrkideaTableEntry &table, bool purge);
	//! Removes a table from the cache, keeping its entry alive.
	void Retire(const string &oracle_name);

	//! Guards the maps below. It is never held while Oracle is queried, so a
	//! listing does not hold up statements on tables already described.
	mutex lock;
	//! Makes CREATE, ALTER, and DROP TABLE run one at a time.
	mutex ddl_lock;
	bool tables_listed = false;
	//! Oracle table names, as listed.
	unordered_map<string, TableListing> table_names;
	//! Described tables, keyed by Oracle name.
	unordered_map<string, unique_ptr<OrkideaTableEntry>> tables;
	//! Entries of tables dropped or altered since, which statements may still use.
	vector<unique_ptr<OrkideaTableEntry>> retired;
	//! Key columns of the listed tables, by table, once read.
	bool keys_listed = false;
	unordered_map<string, vector<vector<Value>>> listed_keys;
	//! The tables `listed_keys` covers: those listed when it was read, less
	//! the ones altered or dropped since.
	unordered_set<string> keys_cover;
};

} // namespace orkidea
} // namespace duckdb
