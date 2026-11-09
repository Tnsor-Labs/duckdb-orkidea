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
namespace orkidea {

class OrkideaCatalog;

struct TableScanBindData : public ArrowScanFunctionData {
	TableScanBindData(OrkideaCatalog &catalog, shared_ptr<const OracleTable> table);

	OrkideaCatalog &catalog;
	shared_ptr<const OracleTable> table;
};

struct TableScan {
	//! The scan function and its bind data for a table.
	static TableFunction Function(ClientContext &context, OrkideaCatalog &catalog, shared_ptr<const OracleTable> table,
	                              unique_ptr<FunctionData> &bind_data);
};

} // namespace orkidea
} // namespace duckdb
