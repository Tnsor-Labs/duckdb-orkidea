//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// storage/orkidea_transaction.hpp
//
// A DuckDB transaction on an attached Oracle database. Its statements run on
// one connection, taken from the catalog's pool on first use. The first
// write turns autocommit off, so that the transaction's writes commit or roll
// back together with it.
//
// Oracle DDL commits implicitly, so CREATE and DROP run on a session of their
// own, which keeps them from committing the transaction's other writes. A
// table created in a transaction that rolls back is dropped again.
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
	//! The transaction's connection, ready for writing: autocommit is off.
	shared_ptr<Connection> GetWriteConnection();
	//! Whether the transaction has written, so that only its own session
	//! sees those rows (no parallel scans on other sessions).
	bool HasWritten();
	//! Records a table the transaction created, to drop again on rollback.
	void AddCreatedTable(string schema, string table);

	void Commit();
	void Rollback();

	static OrkideaTransaction &Get(ClientContext &context, Catalog &catalog);

private:
	OrkideaCatalog &catalog;
	//! Ends the transaction on the connection, and returns it to the pool.
	void Finish(bool commit);

	mutex lock;
	shared_ptr<Connection> connection;
	bool written = false;
	//! (schema, table) as Oracle names them.
	vector<pair<string, string>> created_tables;
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
