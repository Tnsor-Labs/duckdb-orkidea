//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_secrets.hpp
//
// The `orkidea` secret type, and how a connection target (a secret name or an
// oracle:// URI) becomes ADBC options.
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

namespace duckdb {

class ExtensionLoader;

namespace orkidea {

struct Secrets {
	static void Register(ExtensionLoader &loader);

	//! ADBC options for a target: the name of an orkidea secret, or an
	//! oracle:// or oracle+tcps:// URI.
	static AdbcOptions ResolveTarget(ClientContext &context, const string &target);
};

} // namespace orkidea
} // namespace duckdb
