#include "orkidea_scan.hpp"

#include "orkidea_filter.hpp"
#include "orkidea_partition.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_table_entry.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
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
//! The row id is the ROWID as text. Other virtual columns, such as the empty
//! column DuckDB scans for COUNT(*), have no Oracle column, so a NULL stands
//! in for them and keeps the result columns aligned.
//! With `order_by` and `row_limit`, the first rows in that order. `source` is
//! the FROM clause (the table, or the table AS OF a change number), and
//! `range` an extra condition (a ROWID range).
ScanPlan PlanScan(const OracleTable &table, bool unicode_database, const vector<column_t> &column_ids,
                  optional_ptr<TableFilterSet> filters, const vector<ScanOrder> &order_by = {},
                  optional_idx row_limit = optional_idx(), const string &source = string(),
                  const string &range = string(), bool for_update = false) {
	vector<string> select_list;
	for (auto column : column_ids) {
		if (column == COLUMN_IDENTIFIER_ROW_ID) {
			select_list.emplace_back("ROWIDTOCHAR(ROWID)");
		} else {
			select_list.push_back(IsVirtualColumn(column) ? "NULL" : QuoteIdentifier(table.column_names[column]));
		}
	}
	if (select_list.empty()) {
		select_list.emplace_back("NULL");
	}

	ScanPlan plan;
	vector<string> conditions;
	if (!range.empty()) {
		conditions.push_back(range);
	}
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

	plan.sql =
	    "SELECT " + StringUtil::Join(select_list, ", ") + " FROM " + (source.empty() ? table.QualifiedName() : source);
	if (!conditions.empty()) {
		plan.sql += " WHERE " + StringUtil::Join(conditions, " AND ");
	}
	if (!order_by.empty()) {
		vector<string> keys;
		for (auto &order : order_by) {
			keys.push_back(QuoteIdentifier(table.column_names[order.column]) + (order.descending ? " DESC" : " ASC") +
			               (order.nulls_first ? " NULLS FIRST" : " NULLS LAST"));
		}
		plan.sql += " ORDER BY " + StringUtil::Join(keys, ", ");
	}
	if (row_limit.IsValid()) {
		plan.sql += " FETCH FIRST " + to_string(row_limit.GetIndex()) + " ROWS ONLY";
	}
	if (for_update) {
		plan.sql += " FOR UPDATE";
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

constexpr const char *PARALLEL_SCAN_SETTING = "orkidea_parallel_scan";
constexpr const char *PARALLEL_SCAN_MIN_ROWS_SETTING = "orkidea_parallel_scan_min_rows";
//! The most sessions an automatic parallel scan uses.
constexpr idx_t MAX_AUTOMATIC_SESSIONS = 8;
//! Ranges per session, so that a thread that finishes early takes another.
constexpr idx_t RANGES_PER_SESSION = 4;
//! Batch numbers within a range take the low bits of a batch index.
constexpr idx_t BATCH_BITS = 32;

//! Either one stream that every thread reads batches from (DuckDB's Arrow
//! scan state), or ROWID ranges that threads read whole, each on its own
//! session.
struct TableScanGlobalState : public ArrowScanGlobalState {
	vector<pair<idx_t, unique_ptr<Expression>>> local_filters;
	//! The statement for each range; empty for a single stream.
	vector<string> ranges;
	atomic<idx_t> next_range {0};
	//! The first range, opened in InitGlobal to check the table can be read
	//! AS OF the change number; taken by the thread that claims range 0.
	unique_ptr<StatementStream> first_range;

	bool ReadsRanges() const {
		return !ranges.empty();
	}
};

//! DuckDB's Arrow scan state, and an executor for each local filter.
struct TableScanLocalState : public LocalTableFunctionState {
	unique_ptr<ArrowScanLocalState> arrow;
	//! The scanned columns. DuckDB's Arrow conversion sees every virtual one
	//! as the row id, which it leaves out, and the scan fills them itself.
	vector<column_t> column_ids;
	//! The range this thread is reading, when the scan reads ranges, its
	//! index, and how many of its batches the thread has read.
	unique_ptr<StatementStream> range;
	idx_t range_index = 0;
	idx_t range_batches = 0;
	vector<pair<idx_t, unique_ptr<ExpressionExecutor>>> filters;
};

//! The rows a scan returns, as Oracle's statistics estimate them: the
//! table's rows, times, for each equality or IN filter Oracle applies, its
//! values over the column's distinct values (ALL_TAB_COLUMNS.NUM_DISTINCT).
//! Other filters are not counted, so this can only overestimate.
double EstimatedScanRows(ClientContext &context, const TableScanBindData &bind_data,
                         const TableFunctionInitInput &input) {
	auto &table = *bind_data.table;
	double rows = static_cast<double>(table.estimated_rows);
	if (!input.filters) {
		return rows;
	}
	unordered_map<string, idx_t> matched;
	for (auto &entry : *input.filters) {
		auto column = input.column_ids[entry.GetIndex().GetIndex()];
		if (IsVirtualColumn(column)) {
			continue;
		}
		auto &expr = *ExpressionFilter::GetExpressionFilter(entry.Filter(), "orkidea scan").expr;
		auto translated =
		    TranslateFilter(MakeFilterColumn(table, column, bind_data.catalog.GetDatabaseInfo().unicode), expr);
		auto values = MatchedValues(expr);
		if (values > 0 && !translated.sql.empty() && !translated.evaluate_locally) {
			matched[table.column_names[column]] = values;
		}
	}
	if (matched.empty()) {
		return rows;
	}
	vector<string> names;
	for (auto &entry : matched) {
		names.push_back(QuoteLiteral(entry.first));
	}
	auto sql = "SELECT column_name, num_distinct FROM all_tab_columns WHERE owner = " + QuoteLiteral(table.schema) +
	           " AND table_name = " + QuoteLiteral(table.name) + " AND column_name IN (" +
	           StringUtil::Join(names, ", ") + ")";
	auto connection = OrkideaTransaction::Get(context, bind_data.catalog).GetConnection();
	for (auto &row : QueryRows(context, connection, sql)) {
		if (row[1].IsNull()) {
			continue;
		}
		auto distinct = row[1].GetValue<double>();
		if (distinct > 0) {
			rows *= MinValue<double>(1.0, static_cast<double>(matched[row[0].ToString()]) / distinct);
		}
	}
	return rows;
}

//! How many sessions a scan of the table should use: 1 for a single stream.
idx_t ScanSessions(ClientContext &context, const TableScanBindData &bind_data, const TableFunctionInitInput &input) {
	// A pushed LIMIT or order needs one ordered stream; rows a transaction
	// wrote, and rows it locks, belong to its own session.
	if (!bind_data.table->splittable || bind_data.row_limit.IsValid() || !bind_data.order_by.empty() ||
	    bind_data.for_update || OrkideaTransaction::Get(context, bind_data.catalog).HasWritten()) {
		return 1;
	}
	Value setting;
	int64_t sessions = -1;
	if (context.TryGetCurrentSetting(PARALLEL_SCAN_SETTING, setting)) {
		sessions = setting.GetValue<int64_t>();
	}
	idx_t min_rows = 500000;
	if (context.TryGetCurrentSetting(PARALLEL_SCAN_MIN_ROWS_SETTING, setting)) {
		min_rows = setting.GetValue<idx_t>();
	}
	if (sessions == 0 || sessions == 1 || bind_data.table->estimated_rows < 0 ||
	    NumericCast<idx_t>(bind_data.table->estimated_rows) < min_rows) {
		return 1;
	}
	// A filter that leaves few rows is read faster by one query than by the
	// change number, ROWID planning, and range queries of a parallel scan.
	// A number of sessions the user set is used as set.
	if (sessions < 0 && EstimatedScanRows(context, bind_data, input) < static_cast<double>(min_rows)) {
		return 1;
	}
	auto threads = NumericCast<idx_t>(context.db->NumberOfThreads());
	if (sessions < 0) {
		return MinValue<idx_t>(threads, MAX_AUTOMATIC_SESSIONS);
	}
	return MinValue<idx_t>(threads, NumericCast<idx_t>(sessions));
}

//! Plans the scan as ROWID ranges read AS OF one change number, and opens the
//! first. Returns false, having changed nothing, when the table cannot be
//! read that way: it has no ROWID cut points, the user may not take a change
//! number, or Oracle refuses the flashback read (no FLASHBACK privilege, or
//! ORA-01466 after a recent change to the table's definition).
bool PlanRanges(ClientContext &context, const TableScanBindData &bind_data, TableFunctionInitInput &input,
                idx_t sessions, TableScanGlobalState &state) {
	auto &table = *bind_data.table;
	auto planning = bind_data.catalog.AcquireConnection();
	auto scn = CurrentScn(context, planning);
	if (!scn.IsValid()) {
		return false;
	}
	auto conditions = RowidRanges::Plan(context, planning, table, sessions * RANGES_PER_SESSION);
	if (conditions.size() < 2) {
		return false;
	}
	auto source = table.QualifiedName() + " AS OF SCN " + to_string(scn.GetIndex());
	auto unicode = bind_data.catalog.GetDatabaseInfo().unicode;
	vector<string> ranges;
	for (auto &condition : conditions) {
		auto plan =
		    PlanScan(table, unicode, input.column_ids, input.filters.get(), {}, optional_idx(), source, condition);
		ranges.push_back(std::move(plan.sql));
	}
	try {
		state.first_range = OpenQuery(context, bind_data.catalog.AcquireConnection(), ranges[0]);
	} catch (std::exception &) {
		return false;
	}
	state.ranges = std::move(ranges);
	state.max_threads = MinValue<idx_t>(sessions, state.ranges.size());
	return true;
}

//! Moves the thread to the next batch of its range, or to the next range.
//! Returns false when every range has been read.
bool NextRangeBatch(ClientContext &context, const TableScanBindData &bind_data, TableScanGlobalState &global,
                    TableScanLocalState &state) {
	while (true) {
		if (!state.range) {
			auto index = global.next_range++;
			if (index >= global.ranges.size()) {
				return false;
			}
			state.range = index == 0 ? std::move(global.first_range)
			                         : OpenQuery(context, bind_data.catalog.AcquireConnection(), global.ranges[index]);
			state.range_index = index;
			state.range_batches = 0;
		}
		auto chunk = state.range->GetNextChunk();
		if (!chunk->arrow_array.release) {
			// The range is done; its session goes back to the pool.
			state.range.reset();
			continue;
		}
		if (chunk->arrow_array.length == 0) {
			continue;
		}
		state.arrow->Reset();
		state.arrow->chunk = std::move(chunk);
		// Batches are numbered in ROWID order, range by range, so that an
		// insertion-order preserving sink can put batches from different
		// threads back in order.
		state.arrow->batch_index = ((state.range_index + 1) << BATCH_BITS) + ++state.range_batches;
		return true;
	}
}

unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	// Rows read FOR UPDATE stay locked only if the session does not commit
	// after the query, so the scan opens the transaction for writing.
	bool for_update = bind_data.for_update && !bind_data.row_limit.IsValid() && bind_data.order_by.empty();
	auto plan = PlanScan(*bind_data.table, bind_data.catalog.GetDatabaseInfo().unicode, input.column_ids,
	                     input.filters.get(), bind_data.order_by, bind_data.row_limit, string(), string(), for_update);

	auto result = make_uniq<TableScanGlobalState>();
	result->local_filters = std::move(plan.local_filters);
	auto sessions = ScanSessions(context, bind_data, input);
	if (sessions < 2 || !PlanRanges(context, bind_data, input, sessions, *result)) {
		// One stream feeds every thread; threads convert its batches in parallel.
		auto &transaction = OrkideaTransaction::Get(context, bind_data.catalog);
		auto connection = for_update ? transaction.GetWriteConnection() : transaction.GetConnection();
		result->stream = OpenQuery(context, std::move(connection), plan.sql);
		result->max_threads = context.db->NumberOfThreads();
	}
	// Columns only filters need are scanned, then left out of the output.
	if (!input.projection_ids.empty()) {
		result->projection_ids = input.projection_ids;
		for (auto column : input.column_ids) {
			if (column == COLUMN_IDENTIFIER_ROW_ID) {
				result->scanned_types.push_back(LogicalType::VARCHAR);
			} else if (IsVirtualColumn(column)) {
				result->scanned_types.push_back(LogicalType::BOOLEAN);
			} else {
				result->scanned_types.push_back(bind_data.all_types[column]);
			}
		}
	}
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> InitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                              GlobalTableFunctionState *global_state) {
	auto &global = global_state->Cast<TableScanGlobalState>();
	auto result = make_uniq<TableScanLocalState>();
	if (global.ReadsRanges()) {
		// As ArrowScanInitLocal, without taking a batch from a shared stream.
		result->arrow = make_uniq<ArrowScanLocalState>(make_uniq<ArrowArrayWrapper>(), context.client);
		result->arrow->column_ids = input.column_ids;
		result->arrow->filters = input.filters.get();
		if (global.CanRemoveFilterColumns()) {
			result->arrow->all_columns.Initialize(context.client, global.scanned_types);
		}
	} else {
		auto arrow = ArrowTableFunction::ArrowScanInitLocal(context, input, global_state);
		if (arrow) {
			result->arrow = unique_ptr_cast<LocalTableFunctionState, ArrowScanLocalState>(std::move(arrow));
		}
	}
	result->column_ids = input.column_ids;
	if (result->arrow) {
		for (auto &column : result->arrow->column_ids) {
			if (IsVirtualColumn(column)) {
				column = COLUMN_IDENTIFIER_ROW_ID;
			}
		}
	}
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

//! Fills the virtual columns of a batch, which DuckDB's Arrow conversion
//! leaves out: the row id from the ROWIDs, which Oracle sends as text
//! (ROWIDTOCHAR) in an Arrow string array, and any other with NULL.
void ConvertVirtualColumns(const TableScanLocalState &state, idx_t count, DataChunk &scanned) {
	auto &arrow = *state.arrow;
	for (idx_t column = 0; column < state.column_ids.size(); column++) {
		if (!IsVirtualColumn(state.column_ids[column])) {
			continue;
		}
		if (state.column_ids[column] != COLUMN_IDENTIFIER_ROW_ID) {
			scanned.data[column].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(scanned.data[column], true);
			continue;
		}
		auto &array = *arrow.chunk->arrow_array.children[column];
		if (array.n_buffers != 3) {
			throw InternalException("orkidea_scan: ROWIDs did not arrive as a string array");
		}
		auto validity = static_cast<const uint8_t *>(array.buffers[0]);
		auto offsets = static_cast<const int32_t *>(array.buffers[1]);
		auto data = static_cast<const char *>(array.buffers[2]);
		auto &vector = scanned.data[column];
		vector.SetVectorType(VectorType::FLAT_VECTOR);
		auto strings = FlatVector::GetDataMutable<string_t>(vector);
		auto start = NumericCast<idx_t>(arrow.chunk->arrow_array.offset + array.offset) + arrow.chunk_offset;
		for (idx_t row = 0; row < count; row++) {
			auto index = start + row;
			if (validity && !(validity[index / 8] & (1 << (index % 8)))) {
				FlatVector::SetNull(vector, row, true);
				continue;
			}
			auto length = NumericCast<uint32_t>(offsets[index + 1] - offsets[index]);
			strings[row] = StringVector::AddString(vector, data + offsets[index], length);
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
	auto &arrow = *state.arrow;
	auto &scanned = global.CanRemoveFilterColumns() ? arrow.all_columns : output;
	while (true) {
		auto length = NumericCast<idx_t>(arrow.chunk->arrow_array.length);
		if (arrow.chunk_offset >= length) {
			bool more =
			    global.ReadsRanges()
			        ? NextRangeBatch(context, bind_data, global, state)
			        : ArrowTableFunction::ArrowScanParallelStateNext(context, input.bind_data.get(), arrow, global);
			if (!more) {
				return;
			}
			length = NumericCast<idx_t>(arrow.chunk->arrow_array.length);
		}
		auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, length - arrow.chunk_offset);
		scanned.Reset();
		scanned.SetChildCardinality(count);
		ArrowTableFunction::ArrowToDuckDB(arrow, bind_data.arrow_table.GetColumns(), scanned);
		ConvertVirtualColumns(state, count, scanned);
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

optional_ptr<TableCatalogEntry> TableEntry(optional_ptr<const FunctionData> bind_data) {
	return &bind_data->Cast<TableScanBindData>().entry;
}

OperatorPartitionData PartitionData(ClientContext &, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("orkidea_scan: partition columns are not supported");
	}
	auto &state = input.local_state->Cast<TableScanLocalState>();
	return OperatorPartitionData(state.arrow ? state.arrow->batch_index : 0);
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

TableScanBindData::TableScanBindData(OrkideaCatalog &catalog_p, OrkideaTableEntry &entry_p,
                                     shared_ptr<const OracleTable> table_p)
    : ArrowScanFunctionData(make_shared_ptr<TableScanFactory>(catalog_p, table_p)), catalog(catalog_p), entry(entry_p),
      table(std::move(table_p)) {
}

void TableScan::RegisterSettings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(PARALLEL_SCAN_SETTING,
	                          "How many Oracle sessions one scan of an attached table may read through at once: -1 "
	                          "chooses (DuckDB's thread count, up to 8), 0 or 1 reads through one session",
	                          LogicalType::BIGINT, Value::BIGINT(-1));
	config.AddExtensionOption(PARALLEL_SCAN_MIN_ROWS_SETTING,
	                          "Oracle tables with fewer rows than this (by their optimizer statistics) are read "
	                          "through one session",
	                          LogicalType::UBIGINT, Value::UBIGINT(500000));
}

bool TableScan::HasLocalFilters(const LogicalGet &get) {
	auto &bind_data = get.bind_data->Cast<TableScanBindData>();
	auto unicode_database = bind_data.catalog.GetDatabaseInfo().unicode;
	for (auto &entry : get.table_filters) {
		auto &column = get.GetColumnIds()[entry.GetIndex().GetIndex()];
		auto &expr = *ExpressionFilter::GetExpressionFilter(entry.Filter(), "orkidea scan").expr;
		if (column.IsVirtualColumn()) {
			return true;
		}
		auto filter_column = MakeFilterColumn(*bind_data.table, column.GetPrimaryIndex(), unicode_database);
		if (TranslateFilter(filter_column, expr).evaluate_locally) {
			return true;
		}
	}
	return false;
}

vector<string> TableScan::Conditions(const LogicalGet &get) {
	auto &bind_data = get.bind_data->Cast<TableScanBindData>();
	auto unicode_database = bind_data.catalog.GetDatabaseInfo().unicode;
	vector<string> result;
	for (auto &entry : get.table_filters) {
		auto &column = get.GetColumnIds()[entry.GetIndex().GetIndex()];
		auto &expr = *ExpressionFilter::GetExpressionFilter(entry.Filter(), "orkidea scan").expr;
		if (column.IsVirtualColumn()) {
			throw InternalException("orkidea_scan: a filter on a virtual column has no Oracle condition");
		}
		auto translated =
		    TranslateFilter(MakeFilterColumn(*bind_data.table, column.GetPrimaryIndex(), unicode_database), expr);
		if (translated.evaluate_locally) {
			throw InternalException("orkidea_scan: a filter Oracle cannot evaluate has no Oracle condition");
		}
		if (!translated.sql.empty()) {
			result.push_back(std::move(translated.sql));
		}
	}
	return result;
}

TableFunction TableScan::Function(ClientContext &context, OrkideaCatalog &catalog, OrkideaTableEntry &entry,
                                  shared_ptr<const OracleTable> table, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<TableScanBindData>(catalog, entry, std::move(table));
	result->factory->GetSchema(result->schema_root.arrow_schema);
	ArrowTableFunction::PopulateArrowTableSchema(context, result->arrow_table, result->schema_root.arrow_schema);
	result->all_types = result->arrow_table.GetTypes();
	bind_data = std::move(result);

	TableFunction function(NAME, {}, Scan, nullptr, InitGlobal, InitLocal);
	function.projection_pushdown = true;
	function.filter_pushdown = true;
	function.filter_prune = true;
	function.pushdown_expression = PushdownExpression;
	function.supports_pushdown_type = SupportsPushdownType;
	function.cardinality = Cardinality;
	function.get_partition_data = PartitionData;
	function.get_table_entry = TableEntry;
	return function;
}

} // namespace orkidea
} // namespace duckdb
