#include "storage/orkidea_transaction.hpp"

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

void OrkideaTransaction::Commit() {
	lock_guard<mutex> guard(lock);
	connection.reset();
}

void OrkideaTransaction::Rollback() {
	lock_guard<mutex> guard(lock);
	connection.reset();
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
