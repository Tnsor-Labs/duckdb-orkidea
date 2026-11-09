//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_filter.hpp
//
// Translates DuckDB filters on a column into Oracle conditions. A filter is
// translated only when Oracle selects exactly the rows DuckDB would, given
// the session settings the catalog's connections use (NLS_COMP = BINARY).
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

class Expression;

namespace orkidea {

//! The column a filter applies to.
struct FilterColumn {
	//! The quoted Oracle column reference.
	string sql;
	//! The declared Oracle type, as the driver reports it (`oracle.type-name`).
	string oracle_type;
	//! Whether the database character set is Unicode (AL32UTF8 or UTF8).
	bool unicode_database = true;
};

struct TranslatedFilter {
	//! The Oracle condition, or empty when nothing could be translated.
	string sql;
	//! Whether DuckDB must still evaluate the filter on the rows Oracle returns:
	//! true when `sql` is empty or selects a superset of the rows.
	bool evaluate_locally = true;
};

//! Whether comparisons on columns of this Oracle type can be pushed down.
bool CanCompareColumn(const string &oracle_type);

//! Whether Oracle's ORDER BY on columns of this type (with NLS_SORT = BINARY)
//! sorts as DuckDB does. `utf8_byte_order`: the database character set is AL32UTF8.
bool CanOrderColumn(const string &oracle_type, bool utf8_byte_order);

//! Translates a filter expression whose column references all refer to the
//! column. Filters that only prune rows (optional and dynamic filters) are
//! dropped when they cannot be translated, and never need local evaluation.
TranslatedFilter TranslateFilter(const FilterColumn &column, const Expression &expr);

} // namespace orkidea
} // namespace duckdb
