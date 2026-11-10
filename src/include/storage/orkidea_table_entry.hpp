//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// storage/orkidea_table_entry.hpp
//
// An Oracle table or view, as DuckDB sees it.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/arrow/arrow_wrapper.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {
namespace orkidea {

//! How an Oracle table is described: its names and types as the driver reads
//! them. Immutable once built, and shared with the scans that read it.
struct OracleTable {
	string schema;
	string name;
	//! Column names as Oracle stores them.
	vector<string> column_names;
	//! Declared Oracle types (`oracle.type-name`), for example `VARCHAR2(20)`.
	vector<string> oracle_types;
	//! The schema of `SELECT * FROM` the table.
	ArrowSchemaWrapper arrow_schema;
	//! The optimizer's row count (ALL_TABLES.NUM_ROWS), or -1 when unknown.
	int64_t estimated_rows = -1;
	//! Whether several sessions can read it in ROWID ranges: a heap table.
	bool splittable = false;
	//! Whether its rows have ROWIDs, which DuckDB sees as the `rowid` column
	//! and UPDATE and DELETE use to find them.
	bool has_rowid = false;

	//! The quoted, schema-qualified name.
	string QualifiedName() const;
};

class OrkideaTableEntry : public TableCatalogEntry {
public:
	OrkideaTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                  shared_ptr<const OracleTable> table);

	const ColumnList &GetColumns() const override;
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	//! `rowid`: the Oracle ROWID as text, for tables that have ROWIDs; and
	//! the empty column DuckDB scans when a query needs no column.
	virtual_column_map_t GetVirtualColumns() const override;
	vector<column_t> GetRowIdColumns() const override;
	//! Nothing to add: Oracle checks its own constraints, updates key columns
	//! in place (not as a delete and an insert), and returns the whole row for
	//! RETURNING, so an UPDATE sets only the columns it names.
	void BindUpdateConstraints(Binder &binder, LogicalGet &get, LogicalProjection &proj, LogicalUpdate &update,
	                           ClientContext &context) override;

	const OracleTable &GetOracleTable() const {
		return *table;
	}

private:
	//! The table's primary and unique keys, as sets of column indexes, read
	//! from Oracle on first use (ON CONFLICT needs them).
	const vector<IndexInfo> &GetUniqueKeys(ClientContext &context);

	ColumnList columns;
	shared_ptr<const OracleTable> table;
	mutex keys_lock;
	bool keys_loaded = false;
	vector<IndexInfo> unique_keys;
};

} // namespace orkidea
} // namespace duckdb
