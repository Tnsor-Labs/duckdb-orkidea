//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_storage.hpp
//
// ATTACH ... (TYPE orkidea): attaches an Oracle database as a DuckDB catalog.
//===----------------------------------------------------------------------===//

#pragma once

namespace duckdb {

class ExtensionLoader;

namespace orkidea {

struct Storage {
	static void Register(ExtensionLoader &loader);
};

} // namespace orkidea
} // namespace duckdb
