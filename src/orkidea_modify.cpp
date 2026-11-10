#include "orkidea_modify.hpp"

#include "orkidea_insert.hpp"
#include "orkidea_logging.hpp"
#include "orkidea_scan.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_table_entry.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"

#include <chrono>

namespace duckdb {
namespace orkidea {

namespace {

//! Rows buffered before they are sent: array DML takes many rows per round
//! trip; RETURNING runs once per row, and its results are buffered too.
constexpr idx_t FLUSH_ROWS = 1 << 20;
constexpr idx_t FLUSH_ROWS_RETURNING = 10000;

string Placeholder(idx_t number) {
	return ":" + to_string(number);
}

class ModifyGlobalState : public GlobalSinkState {
public:
	ModifyGlobalState(ClientContext &context, const OrkideaModify &op, const vector<LogicalType> &input_types)
	    : context(context), op(op), table(op.table.GetOracleTable()),
	      connection(OrkideaTransaction::Get(context, op.table.ParentCatalog()).GetWriteConnection()),
	      buffer(make_uniq<ColumnDataCollection>(context, input_types)) {
		if (op.return_chunk) {
			// What Oracle returns: every column, and for a DELETE the ROWID,
			// which DuckDB's RETURNING output carries after the columns.
			auto returned_types = op.table.GetTypes();
			returns_rowid = op.kind == ModifyKind::DELETE && table.has_rowid;
			if (returns_rowid) {
				returned_types.push_back(LogicalType::VARCHAR);
			}
			returned = make_uniq<ColumnDataCollection>(context, returned_types);
			for (idx_t c = 0; c < op.types.size(); c++) {
				output_map.emplace_back(c < table.column_names.size() ? optional_idx(c) : optional_idx());
			}
			if (op.kind == ModifyKind::DELETE) {
				// The virtual columns follow, in the order DuckDB lists them.
				idx_t position = table.column_names.size();
				for (auto &entry : op.table.GetVirtualColumns()) {
					if (position < output_map.size() && entry.first == COLUMN_IDENTIFIER_ROW_ID && returns_rowid) {
						output_map[position] = table.column_names.size();
					}
					position++;
				}
			}
		}
		if (op.kind == ModifyKind::INSERT) {
			roles = ReadColumnRoles(context, connection, table);
			for (idx_t c = 0; c < op.omitted.size() && c < roles.size(); c++) {
				if (op.omitted[c]) {
					roles[c] = ColumnRole::OMITTED;
				}
			}
			any_value.assign(roles.size(), false);
		}
	}

	void Append(DataChunk &chunk) {
		if (op.kind == ModifyKind::INSERT) {
			for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
				if (any_value[c]) {
					continue;
				}
				UnifiedVectorFormat format;
				chunk.data[c].ToUnifiedFormat(format);
				for (idx_t r = 0; r < chunk.size() && !any_value[c]; r++) {
					any_value[c] = format.validity.RowIsValid(format.sel->get_index(r));
				}
				if (any_value[c] && roles[c] == ColumnRole::VIRTUAL) {
					throw InvalidInputException("Cannot insert into the virtual column %s of Oracle table %s",
					                            table.column_names[c], table.name);
				}
			}
		}
		buffer->Append(chunk);
		if (buffer->Count() >= (op.return_chunk ? FLUSH_ROWS_RETURNING : FLUSH_ROWS)) {
			Flush();
		}
	}

	void Flush() {
		if (buffer->Count() == 0) {
			return;
		}
		vector<column_t> parameters;
		auto sql = Statement(parameters);
		vector<string> names;
		for (idx_t i = 0; i < parameters.size(); i++) {
			names.push_back("p" + to_string(i + 1));
		}
		vector<LogicalType> out_types;
		if (op.return_chunk) {
			out_types = returned->Types();
			for (idx_t i = 0; i < out_types.size(); i++) {
				names.push_back("r" + to_string(i + 1));
			}
		}
		ArrowArrayStream stream;
		CollectionStream(context, *buffer, std::move(names), stream, parameters, out_types);

		auto start = std::chrono::steady_clock::now();
		auto statement = make_uniq<orkidea::Statement>(connection);
		statement->SetSqlQuery(sql);
		statement->SetOption("adbc.statement.bind_by_name", "false");
		statement->BindStream(stream);
		if (op.return_chunk) {
			StatementStream results(std::move(statement));
			auto before = returned->Count();
			ReadStream(context, results, *returned);
			affected += returned->Count() - before;
		} else {
			auto rows = statement->ExecuteUpdate();
			affected += rows < 0 ? buffer->Count() : NumericCast<idx_t>(rows);
		}
		auto elapsed = std::chrono::steady_clock::now() - start;
		DUCKDB_LOG(context, QueryLogType, sql, std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());

		buffer->Reset();
		std::fill(any_value.begin(), any_value.end(), false);
	}

	ClientContext &context;
	const OrkideaModify &op;
	const OracleTable &table;
	shared_ptr<Connection> connection;
	unique_ptr<ColumnDataCollection> buffer;
	//! The rows Oracle returned, and for each output column the returned
	//! column it is (invalid: NULL, as for the empty virtual column).
	unique_ptr<ColumnDataCollection> returned;
	vector<optional_idx> output_map;
	bool returns_rowid = false;
	vector<ColumnRole> roles;
	vector<bool> any_value;
	idx_t affected = 0;

private:
	//! The Oracle statement for the buffered rows, and the input columns bound
	//! to its placeholders, in order.
	string Statement(vector<column_t> &parameters) const {
		auto target = table.QualifiedName();
		string sql;
		switch (op.kind) {
		case ModifyKind::INSERT: {
			vector<string> columns;
			vector<string> values;
			for (idx_t c = 0; c < roles.size(); c++) {
				if (roles[c] == ColumnRole::VALUE || (roles[c] == ColumnRole::DEFAULTED && any_value[c])) {
					parameters.push_back(c);
					columns.push_back(QuoteIdentifier(table.column_names[c]));
					values.push_back(Placeholder(parameters.size()));
				}
			}
			if (columns.empty()) {
				throw NotImplementedException("Inserting rows that only take Oracle defaults is not supported; name at "
				                              "least one column of %s",
				                              table.name);
			}
			sql = "INSERT INTO " + target + " (" + StringUtil::Join(columns, ", ") + ") VALUES (" +
			      StringUtil::Join(values, ", ") + ")";
			break;
		}
		case ModifyKind::UPDATE: {
			vector<string> assignments;
			for (idx_t i = 0; i < op.update_columns.size(); i++) {
				auto column = QuoteIdentifier(table.column_names[op.update_columns[i].index]);
				if (!op.value_columns[i].IsValid()) {
					assignments.push_back(column + " = DEFAULT");
					continue;
				}
				parameters.push_back(op.value_columns[i].GetIndex());
				assignments.push_back(column + " = " + Placeholder(parameters.size()));
			}
			parameters.push_back(op.row_id_column);
			sql = "UPDATE " + target + " SET " + StringUtil::Join(assignments, ", ") + " WHERE ROWID = CHARTOROWID(" +
			      Placeholder(parameters.size()) + ")";
			break;
		}
		case ModifyKind::DELETE:
			parameters.push_back(op.row_id_column);
			sql = "DELETE FROM " + target + " WHERE ROWID = CHARTOROWID(" + Placeholder(1) + ")";
			break;
		}
		if (op.return_chunk) {
			vector<string> columns;
			for (auto &name : table.column_names) {
				columns.push_back(QuoteIdentifier(name));
			}
			if (returns_rowid) {
				columns.emplace_back("ROWIDTOCHAR(ROWID)");
			}
			vector<string> outputs;
			for (idx_t c = 0; c < columns.size(); c++) {
				outputs.push_back(":r" + to_string(c + 1));
			}
			sql += " RETURNING " + StringUtil::Join(columns, ", ") + " INTO " + StringUtil::Join(outputs, ", ");
		}
		return sql;
	}
};

class ModifySourceState : public GlobalSourceState {
public:
	ColumnDataScanState scan;
	DataChunk rows;
	bool initialized = false;
};

//! Marks the scans of `table` under `op` to read FOR UPDATE: the rows an
//! UPDATE or DELETE finds stay locked until its transaction ends, so no
//! other session changes them in between.
void LockScannedRows(PhysicalOperator &op, const OrkideaTableEntry &table) {
	if (op.type == PhysicalOperatorType::TABLE_SCAN) {
		auto &scan = op.Cast<PhysicalTableScan>();
		if (scan.function.GetName() == TableScan::NAME && scan.bind_data) {
			auto &bind_data = scan.bind_data->Cast<TableScanBindData>();
			if (&bind_data.entry == &table) {
				bind_data.for_update = true;
			}
		}
	}
	for (auto &child : op.children) {
		LockScannedRows(child.get(), table);
	}
}

} // namespace

OrkideaModify::OrkideaModify(PhysicalPlan &plan, LogicalOperator &op, ModifyKind kind_p, OrkideaTableEntry &table_p,
                             bool return_chunk_p, idx_t row_id_column_p, vector<PhysicalIndex> update_columns_p,
                             vector<optional_idx> value_columns_p)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), kind(kind_p), table(table_p),
      return_chunk(return_chunk_p), row_id_column(row_id_column_p), update_columns(std::move(update_columns_p)),
      value_columns(std::move(value_columns_p)) {
}

unique_ptr<GlobalSinkState> OrkideaModify::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<ModifyGlobalState>(context, *this, children[0].get().GetTypes());
}

SinkResultType OrkideaModify::Sink(ExecutionContext &, DataChunk &chunk, OperatorSinkInput &input) const {
	input.global_state.Cast<ModifyGlobalState>().Append(chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType OrkideaModify::Finalize(Pipeline &, Event &, ClientContext &, OperatorSinkFinalizeInput &input) const {
	input.global_state.Cast<ModifyGlobalState>().Flush();
	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSourceState> OrkideaModify::GetGlobalSourceState(ClientContext &) const {
	return make_uniq<ModifySourceState>();
}

SourceResultType OrkideaModify::GetDataInternal(ExecutionContext &, DataChunk &chunk,
                                                OperatorSourceInput &input) const {
	auto &state = sink_state->Cast<ModifyGlobalState>();
	if (!return_chunk) {
		chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(state.affected)));
		chunk.CheckCardinality(1);
		return SourceResultType::FINISHED;
	}
	auto &source = input.global_state.Cast<ModifySourceState>();
	if (!source.initialized) {
		state.returned->InitializeScan(source.scan);
		state.returned->InitializeScanChunk(source.scan, source.rows);
		source.initialized = true;
	}
	source.rows.Reset();
	state.returned->Scan(source.scan, source.rows);
	for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
		auto column = state.output_map[c];
		if (column.IsValid()) {
			chunk.data[c].Reference(source.rows.data[column.GetIndex()]);
		} else {
			chunk.data[c].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(chunk.data[c], true);
		}
	}
	chunk.SetCardinalityUnsafe(source.rows.size());
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

string OrkideaModify::GetName() const {
	switch (kind) {
	case ModifyKind::INSERT:
		return "ORKIDEA_INSERT_RETURNING";
	case ModifyKind::UPDATE:
		return "ORKIDEA_UPDATE";
	default:
		return "ORKIDEA_DELETE";
	}
}

InsertionOrderPreservingMap<string> OrkideaModify::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table"] = table.GetOracleTable().QualifiedName();
	return result;
}

PhysicalOperator &OrkideaModify::PlanDelete(ClientContext &, PhysicalPlanGenerator &planner, LogicalDelete &op,
                                            PhysicalOperator &plan) {
	auto &table = op.table.Cast<OrkideaTableEntry>();
	auto row_id = op.expressions[0]->Cast<BoundReferenceExpression>().Index();
	LockScannedRows(plan, table);
	auto &result = planner.Make<OrkideaModify>(op, ModifyKind::DELETE, table, op.return_chunk, row_id,
	                                           vector<PhysicalIndex>(), vector<optional_idx>());
	result.children.push_back(plan);
	return result;
}

PhysicalOperator &OrkideaModify::PlanUpdate(ClientContext &, PhysicalPlanGenerator &planner, LogicalUpdate &op,
                                            PhysicalOperator &plan) {
	auto &table = op.table.Cast<OrkideaTableEntry>();
	// SET col = DEFAULT reaches the plan as the column's DuckDB default, the
	// Oracle default marker, which becomes DEFAULT in Oracle's UPDATE.
	auto defaults = OracleDefault::Columns(plan);
	vector<optional_idx> value_columns;
	for (auto &expression : op.expressions) {
		if (expression->GetExpressionType() == ExpressionType::VALUE_DEFAULT) {
			value_columns.emplace_back();
			continue;
		}
		auto index = expression->Cast<BoundReferenceExpression>().Index();
		value_columns.emplace_back(defaults[index] ? optional_idx() : optional_idx(index));
	}
	// The row id is the last column of the input.
	auto row_id = plan.GetTypes().size() - 1;
	LockScannedRows(plan, table);
	auto &result = planner.Make<OrkideaModify>(op, ModifyKind::UPDATE, table, op.return_chunk, row_id,
	                                           std::move(op.columns), std::move(value_columns));
	result.children.push_back(plan);
	return result;
}

PhysicalOperator &OrkideaModify::PlanInsertReturning(ClientContext &, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                                     PhysicalOperator &plan) {
	auto &table = op.table.Cast<OrkideaTableEntry>();
	auto &result = planner.Make<OrkideaModify>(op, ModifyKind::INSERT, table, true, DConstants::INVALID_INDEX,
	                                           vector<PhysicalIndex>(), vector<optional_idx>());
	result.Cast<OrkideaModify>().omitted = OracleDefault::Columns(plan);
	result.children.push_back(plan);
	return result;
}

} // namespace orkidea
} // namespace duckdb
