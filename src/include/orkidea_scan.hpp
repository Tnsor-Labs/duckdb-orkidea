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

//! A sort key of a top-N pushed into the scan.
struct ScanOrder {
	//! The table column.
	column_t column;
	bool descending;
	bool nulls_first;
};

struct TableScanBindData : public ArrowScanFunctionData {
	TableScanBindData(OrkideaCatalog &catalog, shared_ptr<const OracleTable> table);

	OrkideaCatalog &catalog;
	shared_ptr<const OracleTable> table;
	//! The most rows the query needs from the scan (FETCH FIRST), set when a
	//! LIMIT or top-N directly above it allows. DuckDB still applies its own.
	optional_idx row_limit;
	//! The order to fetch rows in, for a top-N.
	vector<ScanOrder> order_by;
};

struct TableScan {
	static constexpr const char *NAME = "orkidea_scan";

	//! The scan function and its bind data for a table.
	static TableFunction Function(ClientContext &context, OrkideaCatalog &catalog, shared_ptr<const OracleTable> table,
	                              unique_ptr<FunctionData> &bind_data);
	//! The settings that control parallel scans.
	static void RegisterSettings(ExtensionLoader &loader);
	//! Whether DuckDB must evaluate any of the scan's filters itself, which
	//! rules out fetching only the first rows.
	static bool HasLocalFilters(const LogicalGet &get);
};

} // namespace orkidea
} // namespace duckdb
