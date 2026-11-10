//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_modify.hpp
//
// UPDATE and DELETE on attached Oracle tables, and INSERT ... RETURNING.
// DuckDB finds the rows (by the ROWIDs its scan reads, FOR UPDATE) and
// computes new values; the changes go to Oracle as array DML by ROWID, on the
// transaction's session. With RETURNING, Oracle's own RETURNING ... INTO
// gives the rows as it stored them.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/index_vector.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {

class LogicalDelete;
class LogicalOperator;
class LogicalInsert;
class LogicalUpdate;
class PhysicalPlanGenerator;

namespace orkidea {

class OrkideaTableEntry;

enum class ModifyKind { INSERT, UPDATE, DELETE };

class OrkideaModify : public PhysicalOperator {
public:
	//! For an UPDATE, `update_columns` are the columns set and `value_columns`
	//! the input column of each new value (invalid for DEFAULT). The row id is
	//! input column `row_id_column` (unused for an INSERT).
	OrkideaModify(PhysicalPlan &plan, LogicalOperator &op, ModifyKind kind, OrkideaTableEntry &table, bool return_chunk,
	              idx_t row_id_column, vector<PhysicalIndex> update_columns, vector<optional_idx> value_columns);

	static PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                                    PhysicalOperator &plan);
	static PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                                    PhysicalOperator &plan);
	static PhysicalOperator &PlanInsertReturning(ClientContext &context, PhysicalPlanGenerator &planner,
	                                             LogicalInsert &op, PhysicalOperator &plan);

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

	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	ModifyKind kind;
	OrkideaTableEntry &table;
	bool return_chunk;
	idx_t row_id_column;
	vector<PhysicalIndex> update_columns;
	vector<optional_idx> value_columns;
	//! For an INSERT, the columns it leaves to Oracle.
	vector<bool> omitted;
};

} // namespace orkidea
} // namespace duckdb
