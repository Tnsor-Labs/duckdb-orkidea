//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_parameters.hpp
//
// The `params` of orkidea_query and orkidea_execute: one row of values for a
// statement's bind placeholders. A STRUCT binds by name ({'id': 42} for every
// :id), a row(...) by position (one value per placeholder, in order).
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

namespace duckdb {
namespace orkidea {

struct Parameters {
	//! The parameters of a STRUCT or row(...) value; none for NULL. Errors
	//! name `function`.
	static Parameters FromValue(const Value &value, const string &function);

	bool Empty() const {
		return values.empty();
	}

	//! Binds the values to `statement` as a one-row batch. The session's
	//! Arrow export settings never change how a value binds.
	void Bind(ClientContext &context, Statement &statement) const;

	//! Matched to placeholders by name, or else by position.
	bool by_name = false;
	vector<string> names;
	vector<Value> values;
};

} // namespace orkidea
} // namespace duckdb
