//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_execute.hpp
//
// orkidea_execute(target, sql): runs a statement without a result set (DDL,
// DML, PL/SQL) on Oracle and returns the number of rows it affected.
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
