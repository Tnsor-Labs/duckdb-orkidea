//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_query.hpp
//
// orkidea_query(target, sql): runs a query on Oracle and returns its rows.
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

#include "duckdb/function/table/arrow.hpp"

namespace duckdb {

class ExtensionLoader;

namespace orkidea {

//! Produces the result stream of one query on one connection, for DuckDB's
//! Arrow scan.
class QueryScanFactory : public ArrowScanFactory {
public:
	QueryScanFactory(shared_ptr<Connection> connection, string sql);

	void GetSchema(ArrowSchema &schema) override;
	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override;

private:
	unique_ptr<Statement> NewStatement() const;

	shared_ptr<Connection> connection;
	string sql;
};

struct QueryFunction {
	static void Register(ExtensionLoader &loader);
};

} // namespace orkidea
} // namespace duckdb
