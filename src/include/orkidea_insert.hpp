//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_insert.hpp
//
// INSERT INTO and CREATE TABLE ... AS for attached Oracle tables. Rows are
// buffered and sent through the driver's bulk ingest (array DML), on the
// transaction's session with autocommit off.
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class ColumnDataCollection;
class ExtensionLoader;
class ParsedExpression;
class LogicalCreateTable;
class LogicalInsert;
class PhysicalPlanGenerator;
struct BoundCreateTableInfo;

namespace orkidea {

class OrkideaCatalog;
class OrkideaTableEntry;
struct OracleTable;

//! An ArrowArrayStream over the chunks of a collection, converted as they
//! are read. The collection must outlive the stream. `column_ids` selects the
//! columns (all when empty), and `null_types` adds that many NULL columns of
//! those types after them: typed placeholders for OUT parameters.
void CollectionStream(ClientContext &context, ColumnDataCollection &collection, vector<string> names,
                      ArrowArrayStream &stream, vector<column_t> column_ids = {}, vector<LogicalType> null_types = {});

//! How an Oracle column takes part in an insert.
enum class ColumnRole {
	//! Always sent.
	VALUE,
	//! Has a default or is an identity column: left out of a batch in which
	//! it is NULL in every row, which is what an INSERT that does not name it
	//! gives, so that Oracle fills it in.
	DEFAULTED,
	//! A virtual column, which Oracle computes: never sent.
	VIRTUAL,
	//! Not named by the INSERT: never sent, so Oracle fills it in.
	OMITTED,
};

vector<ColumnRole> ReadColumnRoles(ClientContext &context, shared_ptr<Connection> connection, const OracleTable &table);

//! The DuckDB default of every column of an Oracle table: a function that
//! stands for "whatever Oracle's default is". It evaluates to NULL; INSERT and
//! UPDATE recognize it in their input and leave the column to Oracle.
struct OracleDefault {
	static constexpr const char *NAME = "orkidea_oracle_default";

	static void Register(ExtensionLoader &loader);
	static unique_ptr<ParsedExpression> Create();
	//! Whether an expression is the marker, possibly cast to the column type.
	static bool Is(const Expression &expr);
	//! For each column of `plan`'s output, whether it is the marker: the
	//! columns an INSERT did not name or an UPDATE set to DEFAULT. All false
	//! when `plan` is not a projection that shows it.
	static vector<bool> Columns(const PhysicalOperator &plan);
};

class OrkideaInsert : public PhysicalOperator {
public:
	//! INSERT INTO an existing table. `omitted` marks the columns the INSERT
	//! leaves to Oracle.
	OrkideaInsert(PhysicalPlan &plan, LogicalOperator &op, OrkideaTableEntry &table, vector<bool> omitted);
	//! CREATE TABLE ... AS: the table is created when the insert starts.
	OrkideaInsert(PhysicalPlan &plan, LogicalOperator &op, SchemaCatalogEntry &schema,
	              unique_ptr<BoundCreateTableInfo> info);

	static PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                                    optional_ptr<PhysicalOperator> plan);
	static PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
	                                           LogicalCreateTable &op, PhysicalOperator &plan);

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

private:
	optional_ptr<OrkideaTableEntry> table;
	optional_ptr<SchemaCatalogEntry> schema;
	unique_ptr<BoundCreateTableInfo> info;
	vector<bool> omitted;
};

} // namespace orkidea
} // namespace duckdb
