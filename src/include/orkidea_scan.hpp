//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_scan.hpp
//
// Reads an attached Oracle table. Only the columns DuckDB needs are selected,
// filters that Oracle evaluates exactly the same way become a WHERE clause,
// and DuckDB evaluates the rest on the rows that come back.
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/orkidea_table_entry.hpp"

#include "duckdb/function/table/arrow.hpp"

namespace duckdb {
class ExtensionLoader;

namespace orkidea {

class OrkideaCatalog;
class OrkideaTableEntry;

//! A sort key of a top-N pushed into the scan.
struct ScanOrder {
	//! The table column.
	column_t column;
	bool descending;
	bool nulls_first;
};

struct TableScanBindData : public ArrowScanFunctionData {
	TableScanBindData(OrkideaCatalog &catalog, OrkideaTableEntry &entry, shared_ptr<const OracleTable> table);

	OrkideaCatalog &catalog;
	//! The catalog entry scanned, which DuckDB asks for to bind the table's
	//! virtual columns (rowid) and to match UPDATE and DELETE to their scan.
	OrkideaTableEntry &entry;
	shared_ptr<const OracleTable> table;
	//! The most rows the query needs from the scan (FETCH FIRST), set when a
	//! LIMIT or top-N directly above it allows. DuckDB still applies its own.
	optional_idx row_limit;
	//! The order to fetch rows in, for a top-N.
	vector<ScanOrder> order_by;
	//! Set for the scan that finds the rows an UPDATE or DELETE changes: it
	//! reads FOR UPDATE, in the writing transaction, so that the rows are
	//! locked from the moment they are read until the transaction ends.
	bool for_update = false;
};

struct TableScan {
	static constexpr const char *NAME = "orkidea_scan";

	//! The scan function and its bind data for a table.
	static TableFunction Function(ClientContext &context, OrkideaCatalog &catalog, OrkideaTableEntry &entry,
	                              shared_ptr<const OracleTable> table, unique_ptr<FunctionData> &bind_data);
	//! The settings that control parallel scans.
	static void RegisterSettings(ExtensionLoader &loader);
	//! Whether DuckDB must evaluate any of the scan's filters itself, which
	//! rules out fetching only the first rows.
	static bool HasLocalFilters(const LogicalGet &get);
	//! The scan's filters as Oracle conditions, for a scan without local
	//! filters: the WHERE clause of a query that replaces it.
	static vector<string> Conditions(const LogicalGet &get);
};

} // namespace orkidea
} // namespace duckdb
