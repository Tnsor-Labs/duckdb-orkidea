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
#include "duckdb/common/mutex.hpp"

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

private:
	OrkideaCatalog &GetOrkideaCatalog();
	//! Lists the names of the schema's tables, once.
	void ListTables(ClientContext &context);
	//! Tables of the schema that meet an Oracle condition on `table_name`.
	vector<string> QueryTableNames(ClientContext &context, const string &condition);
	//! The table a DuckDB name refers to, described on first use.
	optional_ptr<OrkideaTableEntry> FindTable(ClientContext &context, const string &name);
	optional_ptr<OrkideaTableEntry> DescribeTable(ClientContext &context, const string &oracle_name);

	mutex lock;
	bool tables_listed = false;
	//! Oracle table names, as listed.
	unordered_map<string, TableListing> table_names;
	//! Described tables, keyed by Oracle name.
	unordered_map<string, unique_ptr<OrkideaTableEntry>> tables;
};

} // namespace orkidea
} // namespace duckdb
