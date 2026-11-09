#include "orkidea_scan.hpp"

#include "orkidea_filter.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter_set.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! What one scan asks Oracle for.
struct ScanPlan {
	string sql;
	//! Filters DuckDB evaluates on the rows Oracle returns: the index of the
	//! scanned column, and the filter expression over it.
	vector<pair<idx_t, unique_ptr<Expression>>> local_filters;
};

FilterColumn MakeFilterColumn(const OracleTable &table, column_t column, bool unicode_database) {
	FilterColumn result;
	result.sql = QuoteIdentifier(table.column_names[column]);
	result.oracle_type = table.oracle_types[column];
	result.unicode_database = unicode_database;
	return result;
}

//! Plans `SELECT <scanned columns> FROM <table> WHERE <translated filters>`.
//! A row id that DuckDB scans (for COUNT(*), for example) has no Oracle
//! column, so a NULL stands in for it and keeps the result columns aligned.
ScanPlan PlanScan(const OracleTable &table, bool unicode_database, const vector<column_t> &column_ids,
                  optional_ptr<TableFilterSet> filters) {
	vector<string> select_list;
	for (auto column : column_ids) {
		select_list.push_back(IsVirtualColumn(column) ? "NULL" : QuoteIdentifier(table.column_names[column]));
	}
	if (select_list.empty()) {
		select_list.emplace_back("NULL");
	}

	ScanPlan plan;
	vector<string> conditions;
	if (filters) {
		for (auto &entry : *filters) {
			auto scan_index = entry.GetIndex().GetIndex();
			auto &expr = *ExpressionFilter::GetExpressionFilter(entry.Filter(), "orkidea scan").expr;
			auto column = column_ids[scan_index];
			TranslatedFilter translated;
			if (!IsVirtualColumn(column)) {
				translated = TranslateFilter(MakeFilterColumn(table, column, unicode_database), expr);
			}
			if (!translated.sql.empty()) {
				conditions.push_back(std::move(translated.sql));
			}
			if (translated.evaluate_locally) {
				plan.local_filters.emplace_back(scan_index, expr.Copy());
			}
		}
	}

	plan.sql = "SELECT " + StringUtil::Join(select_list, ", ") + " FROM " + table.QualifiedName();
	if (!conditions.empty()) {
		plan.sql += " WHERE " + StringUtil::Join(conditions, " AND ");
	}
	return plan;
}

//! Reads a table outside any transaction, for DuckDB's generic Arrow scan.
//! Scans of attached tables open their streams in InitGlobal instead, on the
//! transaction's connection.
class TableScanFactory : public ArrowScanFactory {
public:
	TableScanFactory(OrkideaCatalog &catalog, shared_ptr<const OracleTable> table)
	    : catalog(catalog), table(std::move(table)) {
	}

	void GetSchema(ArrowSchema &schema) override {
		CopyArrowSchema(table->arrow_schema.arrow_schema, schema);
	}

	unique_ptr<ArrowArrayStreamWrapper> ProduceStream(ArrowStreamParameters &parameters) override {
		auto &projection = parameters.projected_columns;
		idx_t scanned = 0;
		for (auto &entry : projection.filter_to_col) {
			scanned = MaxValue<idx_t>(scanned, entry.first + 1);
		}
		vector<column_t> column_ids(scanned, COLUMN_IDENTIFIER_ROW_ID);
		for (auto &entry : projection.filter_to_col) {
			column_ids[entry.first] = entry.second;
		}
		auto plan = PlanScan(*table, catalog.GetDatabaseInfo().unicode, column_ids, parameters.filters);
		if (!plan.local_filters.empty()) {
			throw NotImplementedException("This Oracle scan has filters that DuckDB must evaluate");
		}
		auto statement = make_uniq<Statement>(catalog.AcquireConnection());
		statement->SetSqlQuery(plan.sql);
		return make_uniq<StatementStream>(std::move(statement));
	}

private:
	OrkideaCatalog &catalog;
	shared_ptr<const OracleTable> table;
};

struct TableScanGlobalState : public ArrowScanGlobalState {
	vector<pair<idx_t, unique_ptr<Expression>>> local_filters;
};

//! DuckDB's Arrow scan state, and an executor for each local filter.
struct TableScanLocalState : public LocalTableFunctionState {
	unique_ptr<LocalTableFunctionState> arrow;
	vector<pair<idx_t, unique_ptr<ExpressionExecutor>>> filters;
};

unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	auto plan =
	    PlanScan(*bind_data.table, bind_data.catalog.GetDatabaseInfo().unicode, input.column_ids, input.filters.get());

	auto result = make_uniq<TableScanGlobalState>();
	result->stream = OpenQuery(context, OrkideaTransaction::Get(context, bind_data.catalog).GetConnection(), plan.sql);
	// One stream feeds every thread; threads convert its batches in parallel.
	result->max_threads = context.db->NumberOfThreads();
	result->local_filters = std::move(plan.local_filters);
	// Columns only filters need are scanned, then left out of the output.
	if (!input.projection_ids.empty()) {
		result->projection_ids = input.projection_ids;
		for (auto column : input.column_ids) {
			result->scanned_types.push_back(IsVirtualColumn(column) ? LogicalType::ROW_TYPE
			                                                        : bind_data.all_types[column]);
		}
	}
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                              GlobalTableFunctionState *global_state) {
	auto &global = global_state->Cast<TableScanGlobalState>();
	auto result = make_uniq<TableScanLocalState>();
	result->arrow = ArrowTableFunction::ArrowScanInitLocal(context, input, global_state);
	for (auto &filter : global.local_filters) {
		result->filters.emplace_back(filter.first, make_uniq<ExpressionExecutor>(context.client, *filter.second));
	}
	return std::move(result);
}

//! Keeps the rows of `scanned`, which holds every scanned column, that pass
//! every local filter.
void ApplyLocalFilters(TableScanLocalState &state, DataChunk &scanned) {
	SelectionVector selection(STANDARD_VECTOR_SIZE);
	for (auto &filter : state.filters) {
		DataChunk column;
		column.InitializeEmpty({scanned.data[filter.first].GetType()});
		column.ReferenceColumns(scanned, {filter.first});
		auto count = filter.second->SelectExpression(column, selection);
		if (count < scanned.size()) {
			scanned.Slice(selection, count);
		}
		if (scanned.size() == 0) {
			return;
		}
	}
}

//! As DuckDB's Arrow scan, with local filters applied between converting a
//! batch and projecting it.
void Scan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.local_state->Cast<TableScanLocalState>();
	if (!state.arrow) {
		return;
	}
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	auto &global = input.global_state->Cast<TableScanGlobalState>();
	auto &arrow = state.arrow->Cast<ArrowScanLocalState>();
	auto &scanned = global.CanRemoveFilterColumns() ? arrow.all_columns : output;
	while (true) {
		auto length = NumericCast<idx_t>(arrow.chunk->arrow_array.length);
		if (arrow.chunk_offset >= length) {
			if (!ArrowTableFunction::ArrowScanParallelStateNext(context, input.bind_data.get(), arrow, global)) {
				return;
			}
			length = NumericCast<idx_t>(arrow.chunk->arrow_array.length);
		}
		auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, length - arrow.chunk_offset);
		scanned.Reset();
		scanned.SetChildCardinality(count);
		ArrowTableFunction::ArrowToDuckDB(arrow, bind_data.arrow_table.GetColumns(), scanned);
		arrow.chunk_offset += count;
		ApplyLocalFilters(state, scanned);
		// An empty chunk would end the scan, so a batch whose rows were all
		// filtered out is skipped.
		if (scanned.size() == 0) {
			continue;
		}
		if (global.CanRemoveFilterColumns()) {
			output.ReferenceColumns(arrow.all_columns, global.projection_ids);
		}
		output.Verify(context);
		return;
	}
}

bool PushdownExpression(ClientContext &, const LogicalGet &get, Expression &expr) {
	auto &bind_data = get.bind_data->Cast<TableScanBindData>();
	optional_idx scan_index;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    expr, [&](const BoundColumnRefExpression &ref) { scan_index = ref.Binding().column_index.GetIndex(); });
	if (!scan_index.IsValid()) {
		return false;
	}
	auto &column = get.GetColumnIds()[scan_index.GetIndex()];
	if (column.IsVirtualColumn()) {
		return false;
	}
	auto filter_column =
	    MakeFilterColumn(*bind_data.table, column.GetPrimaryIndex(), bind_data.catalog.GetDatabaseInfo().unicode);
	return !TranslateFilter(filter_column, expr).sql.empty();
}

bool SupportsPushdownType(const FunctionData &bind_data, idx_t column) {
	return CanCompareColumn(bind_data.Cast<TableScanBindData>().table->oracle_types[column]);
}

unique_ptr<NodeStatistics> Cardinality(ClientContext &, const FunctionData *bind_data) {
	auto rows = bind_data->Cast<TableScanBindData>().table->estimated_rows;
	if (rows < 0) {
		return nullptr;
	}
	return make_uniq<NodeStatistics>(NumericCast<idx_t>(rows));
}

} // namespace

TableScanBindData::TableScanBindData(OrkideaCatalog &catalog_p, shared_ptr<const OracleTable> table_p)
    : ArrowScanFunctionData(make_shared_ptr<TableScanFactory>(catalog_p, table_p)), catalog(catalog_p),
      table(std::move(table_p)) {
}

TableFunction TableScan::Function(ClientContext &context, OrkideaCatalog &catalog, shared_ptr<const OracleTable> table,
                                  unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<TableScanBindData>(catalog, std::move(table));
	result->factory->GetSchema(result->schema_root.arrow_schema);
	ArrowTableFunction::PopulateArrowTableSchema(context, result->arrow_table, result->schema_root.arrow_schema);
	result->all_types = result->arrow_table.GetTypes();
	bind_data = std::move(result);

	TableFunction function("orkidea_scan", {}, Scan, nullptr, InitGlobal, InitLocal);
	function.projection_pushdown = true;
	function.filter_pushdown = true;
	function.filter_prune = true;
	function.pushdown_expression = PushdownExpression;
	function.supports_pushdown_type = SupportsPushdownType;
	function.cardinality = Cardinality;
	return function;
}

} // namespace orkidea
} // namespace duckdb
