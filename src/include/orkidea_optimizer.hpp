//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_optimizer.hpp
//
// Pushes LIMIT, and the ORDER BY of a top-N, into Oracle scans, so that
// Oracle returns only the rows the query can use (ORDER BY ... FETCH FIRST n
// ROWS ONLY). DuckDB's own LIMIT or top-N stays in the plan, so a pushed limit
// only ever saves fetching rows.
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
