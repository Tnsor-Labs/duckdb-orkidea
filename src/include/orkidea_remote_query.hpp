//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_remote_query.hpp
//
// A scan of a query the optimizer writes for Oracle, such as an aggregate it
// pushes down. It runs on the transaction's session, so it sees the
// transaction's own writes.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table/arrow.hpp"

namespace duckdb {
namespace orkidea {

class OrkideaCatalog;

struct RemoteQueryBindData : public ArrowScanFunctionData {
	RemoteQueryBindData(OrkideaCatalog &catalog, shared_ptr<ArrowScanFactory> factory, string sql);

	OrkideaCatalog &catalog;
	string sql;
};

struct RemoteQuery {
	static constexpr const char *NAME = "orkidea_remote_query";

	//! The scan of `sql` and its bind data, with the types of the columns it
	//! returns. Oracle describes the query, so this fails, throwing, when
	//! Oracle rejects it.
	static TableFunction Function(ClientContext &context, OrkideaCatalog &catalog, string sql,
	                              unique_ptr<FunctionData> &bind_data, vector<LogicalType> &types);
};

} // namespace orkidea
} // namespace duckdb
