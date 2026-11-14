//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_optimizer.hpp
//
// Pushes work into Oracle: LIMIT, and the ORDER BY of a top-N, into scans
// (ORDER BY ... FETCH FIRST n ROWS ONLY), with DuckDB's own LIMIT or top-N
// kept in the plan; and aggregates, DISTINCT, and joins of tables of one
// attached database, which become queries of their own (orkidea_remote_query)
// when Oracle computes them as DuckDB would.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class ExtensionLoader;

namespace orkidea {

struct Optimizer {
	static void Register(ExtensionLoader &loader);
};

} // namespace orkidea
} // namespace duckdb
