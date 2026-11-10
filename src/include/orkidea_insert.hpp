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
class LogicalCreateTable;
class LogicalInsert;
class PhysicalPlanGenerator;
struct BoundCreateTableInfo;

namespace orkidea {

class OrkideaCatalog;
class OrkideaTableEntry;

//! An ArrowArrayStream over the chunks of a collection, converted as they
//! are read. The collection must outlive the stream.
void CollectionStream(ClientContext &context, ColumnDataCollection &collection, vector<string> names,
                      ArrowArrayStream &stream);

class OrkideaInsert : public PhysicalOperator {
public:
	//! INSERT INTO an existing table.
	OrkideaInsert(PhysicalPlan &plan, LogicalOperator &op, OrkideaTableEntry &table);
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
};

} // namespace orkidea
} // namespace duckdb
