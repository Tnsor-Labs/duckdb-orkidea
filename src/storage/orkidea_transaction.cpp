#include "storage/orkidea_transaction.hpp"

#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"

namespace duckdb {
namespace orkidea {

OrkideaTransaction::OrkideaTransaction(OrkideaCatalog &catalog_p, TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context), catalog(catalog_p) {
}

OrkideaTransaction::~OrkideaTransaction() = default;

shared_ptr<Connection> OrkideaTransaction::GetConnection() {
	lock_guard<mutex> guard(lock);
	if (!connection) {
		connection = catalog.AcquireConnection();
	}
	return connection;
}

shared_ptr<Connection> OrkideaTransaction::GetWriteConnection() {
	auto result = GetConnection();
	lock_guard<mutex> guard(lock);
	if (!written) {
		result->SetAutocommit(false);
		written = true;
	}
	return result;
}

bool OrkideaTransaction::HasWritten() {
	lock_guard<mutex> guard(lock);
	return written;
}

void OrkideaTransaction::AddCreatedTable(string schema, string table) {
	lock_guard<mutex> guard(lock);
	created_tables.emplace_back(std::move(schema), std::move(table));
}

void OrkideaTransaction::Finish(bool commit) {
	lock_guard<mutex> guard(lock);
	if (written) {
		// The connection goes back to the pool in autocommit mode, or, if
		// ending the transaction fails, not at all.
		auto finishing = std::move(connection);
		written = false;
		finishing->reusable = false;
		if (commit) {
			finishing->Commit();
		} else {
			finishing->Rollback();
		}
		finishing->SetAutocommit(true);
		finishing->reusable = true;
	}
	connection.reset();
	if (!commit && !created_tables.empty()) {
		auto ddl = catalog.AcquireConnection();
		for (auto &table : created_tables) {
			Statement drop(ddl);
			drop.SetSqlQuery("DROP TABLE " + QuoteIdentifier(table.first) + "." + QuoteIdentifier(table.second) +
			                 " PURGE");
			drop.ExecuteUpdate();
			catalog.ForgetTable(table.first, table.second);
		}
	}
	created_tables.clear();
}

void OrkideaTransaction::Commit() {
	Finish(true);
}

void OrkideaTransaction::Rollback() {
	Finish(false);
}

OrkideaTransaction &OrkideaTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<OrkideaTransaction>();
}

OrkideaTransactionManager::OrkideaTransactionManager(AttachedDatabase &db, OrkideaCatalog &catalog_p)
    : TransactionManager(db), catalog(catalog_p) {
}

Transaction &OrkideaTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<OrkideaTransaction>(catalog, *this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData OrkideaTransactionManager::CommitTransaction(ClientContext &, Transaction &transaction) {
	ErrorData error;
	try {
		transaction.Cast<OrkideaTransaction>().Commit();
	} catch (std::exception &ex) {
		error = ErrorData(ex);
	}
	Remove(transaction);
	return error;
}

void OrkideaTransactionManager::RollbackTransaction(Transaction &transaction) {
	try {
		transaction.Cast<OrkideaTransaction>().Rollback();
	} catch (...) {
		Remove(transaction);
		throw;
	}
	Remove(transaction);
}

void OrkideaTransactionManager::Checkpoint(ClientContext &, bool) {
	// Oracle persists its own data; there is nothing to checkpoint.
}

void OrkideaTransactionManager::Remove(Transaction &transaction) {
	lock_guard<mutex> guard(lock);
	transactions.erase(transaction);
}

} // namespace orkidea
} // namespace duckdb
