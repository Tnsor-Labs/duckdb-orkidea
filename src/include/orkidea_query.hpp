//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_query.hpp
//
// orkidea_query(target, sql [, params := ...]): runs a query on Oracle and
// returns its rows. `params` binds the query's placeholders: a STRUCT by
// name ({'id': 42} for every :id), a row(...) by position (one value per
// placeholder, in order).
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"
#include "orkidea_parameters.hpp"

#include "duckdb/function/table/arrow.hpp"

namespace duckdb {

class ExtensionLoader;

namespace orkidea {

//! Throws unless the columns of `stream` have `types`, those DuckDB planned
//! with. Oracle describes a query without its parameter values, so a bind
//! placeholder in the select list, or a table changed since, can make them
//! differ.
void CheckResultTypes(ClientContext &context, StatementStream &stream, const vector<LogicalType> &types);

//! Produces the result stream of one query on one connection, for DuckDB's
//! Arrow scan.
class QueryScanFactory : public ArrowScanFactory {
public:
	QueryScanFactory(ClientContext &context, shared_ptr<Connection> connection, string sql,
	                 Parameters parameters = Parameters());

	//! Describes the query. Oracle needs no parameter values for that.
	void GetSchema(ArrowSchema &schema) override;
	//! Runs the query with its parameters bound.
	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override;

private:
	unique_ptr<Statement> NewStatement() const;

	ClientContext &context;
	shared_ptr<Connection> connection;
	string sql;
	Parameters parameters;
	//! The column types GetSchema described.
	vector<LogicalType> types;
};

struct QueryFunction {
	static void Register(ExtensionLoader &loader);
};

} // namespace orkidea
} // namespace duckdb
