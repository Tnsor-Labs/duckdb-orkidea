//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_util.hpp
//
// Oracle SQL quoting, Arrow schema copies, and reading small results.
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

namespace duckdb {

class ColumnDataCollection;

namespace orkidea {

//! A quoted Oracle identifier. Oracle names cannot contain double quotes.
string QuoteIdentifier(const string &name);

//! A quoted Oracle string literal.
string QuoteLiteral(const string &value);

//! The Oracle name for a name given in DuckDB, quoted: a name valid unquoted
//! (`items`) becomes upper-case (`"ITEMS"`), as the driver's ingest does, and
//! any other name is kept exactly.
string NewOracleName(const string &name);

//! The Oracle column type the driver's ingest creates for a DuckDB type (see
//! its type-support documentation). `native_boolean`: Oracle 23ai or later.
string OracleColumnType(const LogicalType &type, bool native_boolean);

//! The Oracle name a DuckDB name refers to among `candidates`: the exact
//! name, else its upper-case form (how Oracle stores unquoted names), else the
//! only candidate that matches case-insensitively. Empty when none matches.
string MatchName(const string &name, const vector<string> &candidates);

//! The value of a key in Arrow C schema metadata, or empty when absent.
string ArrowMetadataValue(const char *metadata, const string &key);

//! Copies a schema, with its children, metadata, and dictionaries, into an
//! independently released one.
void CopyArrowSchema(const ArrowSchema &source, ArrowSchema &target);

//! Runs a query and returns its result stream. The SQL is logged as OrkideaQuery.
unique_ptr<StatementStream> OpenQuery(ClientContext &context, shared_ptr<Connection> connection, const string &sql);

//! Runs a statement without a result set and returns the affected row count,
//! or -1 if unknown. The SQL is logged as OrkideaQuery.
int64_t ExecuteStatement(ClientContext &context, shared_ptr<Connection> connection, const string &sql);

//! Reads every row of a result stream into `rows`, cast to the collection's
//! types, which must be as many as the stream's columns.
void ReadStream(ClientContext &context, StatementStream &stream, ColumnDataCollection &rows);

//! Runs a query and returns all its rows. For dictionary queries and other
//! small results.
vector<vector<Value>> QueryRows(ClientContext &context, shared_ptr<Connection> connection, const string &sql);

} // namespace orkidea
} // namespace duckdb
