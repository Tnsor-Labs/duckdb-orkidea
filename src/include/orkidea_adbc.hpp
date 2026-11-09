//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_adbc.hpp
//
// Owning wrappers around the ADBC handles of the linked Orkidea driver. Every
// failed call throws a DuckDB exception carrying the driver's message.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/adbc/adbc.h"
#include "duckdb/common/arrow/arrow_wrapper.hpp"

namespace duckdb {
namespace orkidea {

//! ADBC database options, set in order.
using AdbcOptions = vector<pair<string, string>>;

//! The driver's function table, initialized once per process.
const AdbcDriver &Driver();

//! An ADBC database: the connection settings shared by its connections.
class Database {
public:
	explicit Database(const AdbcOptions &options);
	~Database();
	Database(const Database &) = delete;
	Database &operator=(const Database &) = delete;

	AdbcDatabase handle;
};

//! An open session with Oracle. One statement runs on it at a time.
class Connection {
public:
	explicit Connection(shared_ptr<Database> database);
	~Connection();
	Connection(const Connection &) = delete;
	Connection &operator=(const Connection &) = delete;

	AdbcConnection handle;

private:
	shared_ptr<Database> database;
};

class Statement {
public:
	explicit Statement(shared_ptr<Connection> connection);
	~Statement();
	Statement(const Statement &) = delete;
	Statement &operator=(const Statement &) = delete;

	void SetSqlQuery(const string &sql);
	void SetOption(const string &key, const string &value);
	//! Describes the result set of the query without running it.
	void ExecuteSchema(ArrowSchema &schema);
	//! Runs the query. The stream must be released before the statement.
	void ExecuteQuery(ArrowArrayStream &stream);
	//! Runs a statement without a result set. Returns the affected row count, or -1 if unknown.
	int64_t ExecuteUpdate();

	AdbcStatement handle;

private:
	shared_ptr<Connection> connection;
};

//! A result stream that keeps its statement, and so its connection, open.
class StatementStream : public ArrowArrayStreamWrapper {
public:
	explicit StatementStream(unique_ptr<Statement> statement);
	~StatementStream() override;

	//! The next batch, or one with a null release callback at the end.
	shared_ptr<ArrowArrayWrapper> GetNextChunk() override;

private:
	unique_ptr<Statement> statement;
};

} // namespace orkidea
} // namespace duckdb
