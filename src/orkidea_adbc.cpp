#include "orkidea_adbc.hpp"

#include "duckdb/common/exception.hpp"

// The Orkidea driver's ADBC entry point, linked in from the Rust static
// library (rust/src/lib.rs).
extern "C" AdbcStatusCode OrkideaDriverInit(int version, void *driver, struct AdbcError *error);

namespace duckdb {
namespace orkidea {

namespace {

//! Collects the error of one ADBC call and releases it.
class ErrorScope {
public:
	ErrorScope() : error {} {
	}
	~ErrorScope() {
		if (error.release) {
			error.release(&error);
		}
	}
	ErrorScope(const ErrorScope &) = delete;
	ErrorScope &operator=(const ErrorScope &) = delete;

	AdbcError *Get() {
		return &error;
	}

	//! Throws the DuckDB exception matching an ADBC status.
	[[noreturn]] void Throw(AdbcStatusCode status, const char *action) const {
		string message = error.message && *error.message ? error.message : "no further details";
		auto text = StringUtil::Format("Orkidea: %s failed: %s", action, message);
		switch (status) {
		case ADBC_STATUS_INVALID_ARGUMENT:
		case ADBC_STATUS_INVALID_STATE:
			throw InvalidInputException(text);
		case ADBC_STATUS_NOT_IMPLEMENTED:
			throw NotImplementedException(text);
		case ADBC_STATUS_UNAUTHENTICATED:
		case ADBC_STATUS_UNAUTHORIZED:
			throw PermissionException(text);
		case ADBC_STATUS_CANCELLED:
			throw InterruptException();
		default:
			throw IOException(text);
		}
	}

private:
	AdbcError error;
};

void Check(AdbcStatusCode status, ErrorScope &error, const char *action) {
	if (status != ADBC_STATUS_OK) {
		error.Throw(status, action);
	}
}

AdbcDriver InitDriver() {
	AdbcDriver driver {};
	ErrorScope error;
	Check(OrkideaDriverInit(ADBC_VERSION_1_1_0, &driver, error.Get()), error, "initializing the driver");
	return driver;
}

} // namespace

const AdbcDriver &Driver() {
	// Never released: the driver holds no resources of its own, and its
	// handles may outlive static destruction at exit.
	static const AdbcDriver driver = InitDriver();
	return driver;
}

Database::Database(const AdbcOptions &options) : handle {} {
	auto &driver = Driver();
	ErrorScope error;
	Check(driver.DatabaseNew(&handle, error.Get()), error, "creating a database");
	try {
		for (auto &option : options) {
			Check(driver.DatabaseSetOption(&handle, option.first.c_str(), option.second.c_str(), error.Get()), error,
			      "setting a connection option");
		}
		Check(driver.DatabaseInit(&handle, error.Get()), error, "opening the database");
	} catch (...) {
		ErrorScope release_error;
		driver.DatabaseRelease(&handle, release_error.Get());
		throw;
	}
}

Database::~Database() {
	ErrorScope error;
	Driver().DatabaseRelease(&handle, error.Get());
}

Connection::Connection(shared_ptr<Database> database_p) : handle {}, database(std::move(database_p)) {
	auto &driver = Driver();
	ErrorScope error;
	Check(driver.ConnectionNew(&handle, error.Get()), error, "creating a connection");
	auto status = driver.ConnectionInit(&handle, &database->handle, error.Get());
	if (status != ADBC_STATUS_OK) {
		ErrorScope release_error;
		driver.ConnectionRelease(&handle, release_error.Get());
		error.Throw(status, "connecting");
	}
}

void Connection::SetOption(const string &key, const string &value) {
	ErrorScope error;
	Check(Driver().ConnectionSetOption(&handle, key.c_str(), value.c_str(), error.Get()), error,
	      "setting a connection option");
}

void Connection::SetAutocommit(bool autocommit) {
	SetOption(ADBC_CONNECTION_OPTION_AUTOCOMMIT, autocommit ? ADBC_OPTION_VALUE_ENABLED : ADBC_OPTION_VALUE_DISABLED);
}

void Connection::Commit() {
	ErrorScope error;
	Check(Driver().ConnectionCommit(&handle, error.Get()), error, "committing");
}

void Connection::Rollback() {
	ErrorScope error;
	Check(Driver().ConnectionRollback(&handle, error.Get()), error, "rolling back");
}

Connection::~Connection() {
	ErrorScope error;
	Driver().ConnectionRelease(&handle, error.Get());
}

Statement::Statement(shared_ptr<Connection> connection_p) : handle {}, connection(std::move(connection_p)) {
	ErrorScope error;
	Check(Driver().StatementNew(&connection->handle, &handle, error.Get()), error, "creating a statement");
}

Statement::~Statement() {
	ErrorScope error;
	Driver().StatementRelease(&handle, error.Get());
}

void Statement::SetSqlQuery(const string &sql) {
	ErrorScope error;
	Check(Driver().StatementSetSqlQuery(&handle, sql.c_str(), error.Get()), error, "setting the query");
}

void Statement::SetOption(const string &key, const string &value) {
	ErrorScope error;
	Check(Driver().StatementSetOption(&handle, key.c_str(), value.c_str(), error.Get()), error,
	      "setting a statement option");
}

void Statement::BindStream(ArrowArrayStream &stream) {
	ErrorScope error;
	Check(Driver().StatementBindStream(&handle, &stream, error.Get()), error, "binding rows");
}

void Statement::Bind(ArrowArray &values, ArrowSchema &schema) {
	ErrorScope error;
	Check(Driver().StatementBind(&handle, &values, &schema, error.Get()), error, "binding parameters");
}

void Statement::ExecuteSchema(ArrowSchema &schema) {
	ErrorScope error;
	Check(Driver().StatementExecuteSchema(&handle, &schema, error.Get()), error, "describing the query");
}

void Statement::ExecuteQuery(ArrowArrayStream &stream) {
	ErrorScope error;
	Check(Driver().StatementExecuteQuery(&handle, &stream, nullptr, error.Get()), error, "running the query");
}

int64_t Statement::ExecuteUpdate() {
	ErrorScope error;
	int64_t rows_affected = -1;
	Check(Driver().StatementExecuteQuery(&handle, nullptr, &rows_affected, error.Get()), error,
	      "running the statement");
	return rows_affected;
}

StatementStream::StatementStream(unique_ptr<Statement> statement_p) : statement(std::move(statement_p)) {
	statement->ExecuteQuery(arrow_array_stream);
}

shared_ptr<ArrowArrayWrapper> StatementStream::GetNextChunk() {
	auto chunk = make_shared_ptr<ArrowArrayWrapper>();
	if (arrow_array_stream.get_next(&arrow_array_stream, &chunk->arrow_array) != 0) {
		auto message = arrow_array_stream.get_last_error(&arrow_array_stream);
		throw IOException("Orkidea: reading rows failed: %s", message && *message ? message : "no further details");
	}
	return chunk;
}

StatementStream::~StatementStream() {
	// The stream goes before the statement that produced it.
	if (arrow_array_stream.release) {
		arrow_array_stream.release(&arrow_array_stream);
		arrow_array_stream.release = nullptr;
	}
}

} // namespace orkidea
} // namespace duckdb
