//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// storage/orkidea_transaction.hpp
//
// A DuckDB transaction on an attached Oracle database. Its statements run on
// one connection, taken from the catalog's pool on first use.
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {
namespace orkidea {

class OrkideaCatalog;

class OrkideaTransaction : public Transaction {
public:
	OrkideaTransaction(OrkideaCatalog &catalog, TransactionManager &manager, ClientContext &context);
	~OrkideaTransaction() override;

	//! The transaction's connection.
	shared_ptr<Connection> GetConnection();

	void Commit();
	void Rollback();

	static OrkideaTransaction &Get(ClientContext &context, Catalog &catalog);

private:
	OrkideaCatalog &catalog;
	mutex lock;
	shared_ptr<Connection> connection;
};

class OrkideaTransactionManager : public TransactionManager {
public:
	OrkideaTransactionManager(AttachedDatabase &db, OrkideaCatalog &catalog);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	void Remove(Transaction &transaction);

	OrkideaCatalog &catalog;
	mutex lock;
	reference_map_t<Transaction, unique_ptr<OrkideaTransaction>> transactions;
};

} // namespace orkidea
} // namespace duckdb
