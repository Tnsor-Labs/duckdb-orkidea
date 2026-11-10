//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_execute.hpp
//
// orkidea_execute(target, sql [, params := ...]): runs a statement without a
// result set (DDL, DML, PL/SQL) on Oracle and returns the number of rows it
// affected. `params` binds its placeholders, as for orkidea_query.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class ExtensionLoader;

namespace orkidea {

struct ExecuteFunction {
	static void Register(ExtensionLoader &loader);
};

} // namespace orkidea
} // namespace duckdb
