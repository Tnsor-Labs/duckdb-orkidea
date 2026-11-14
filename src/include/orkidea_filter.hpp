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

//! Whether Oracle compares columns of the two types with `comparison` as
//! DuckDB compares the values it reads of them: as strings, dates and
//! timestamps, instants, raw bytes, or binary floating point. NUMBER columns
//! compare as exact numbers, where DuckDB compares those it reads as DOUBLE
//! rounded. `utf8_byte_order`: the database character set is AL32UTF8.
bool CanCompareColumns(const string &left_type, const string &right_type, ExpressionType comparison,
                       bool utf8_byte_order);

//! Whether Oracle's ORDER BY on columns of this type (with NLS_SORT = BINARY)
//! sorts as DuckDB does. `utf8_byte_order`: the database character set is AL32UTF8.
bool CanOrderColumn(const string &oracle_type, bool utf8_byte_order);

//! Whether Oracle's GROUP BY and DISTINCT tell the column's values apart as
//! DuckDB does. CHAR and NCHAR values, all padded to the column's length,
//! are equal exactly when DuckDB finds them equal.
bool CanGroupColumn(const string &oracle_type);

//! Whether the column is a NUMBER, BINARY_FLOAT, or BINARY_DOUBLE, which
//! Oracle's SUM and AVG take.
bool IsNumericColumn(const string &oracle_type);

//! Translates a filter expression whose column references all refer to the
//! column. Filters that only prune rows (optional and dynamic filters) are
//! dropped when they cannot be translated, and never need local evaluation.
TranslatedFilter TranslateFilter(const FilterColumn &column, const Expression &expr);

//! How many values of the column a filter lets through, when it is an
//! equality (`= constant`) or an `IN` list, or an AND with one, optional or
//! not: 1 for an equality, the list's length for `IN`. 0 when the filter is
//! anything else.
idx_t MatchedValues(const Expression &expr);

} // namespace orkidea
} // namespace duckdb
