#include "orkidea_insert.hpp"

#include "orkidea_modify.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_table_entry.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! Rows buffered before they are sent to Oracle in one ingest.
constexpr idx_t FLUSH_ROWS = 1 << 20;

struct CollectionStreamData {
	CollectionStreamData(ClientContext &context, ColumnDataCollection &collection, vector<string> names_p)
	    : collection(collection), names(std::move(names_p)), properties(context.GetClientProperties()) {
	}

	//! Sets up the scan of `column_ids` (all when empty), and the output with
	//! `null_types` placeholder columns after them.
	void Initialize(Allocator &allocator, const vector<column_t> &column_ids, const vector<LogicalType> &null_types) {
		if (column_ids.empty()) {
			collection.InitializeScan(scan);
		} else {
			collection.InitializeScan(scan, column_ids);
		}
		collection.InitializeScanChunk(scan, chunk);
		auto types = chunk.GetTypes();
		types.insert(types.end(), null_types.begin(), null_types.end());
		// Allocated, not empty: the NULL columns need buffers for the Arrow export.
		output.Initialize(allocator, types);
	}

	//! The next chunk of the collection, with its placeholder columns.
	DataChunk &Next() {
		chunk.Reset();
		collection.Scan(scan, chunk);
		for (idx_t c = 0; c < chunk.ColumnCount(); c++) {
			output.data[c].Reference(chunk.data[c]);
		}
		for (idx_t c = chunk.ColumnCount(); c < output.ColumnCount(); c++) {
			output.data[c].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(output.data[c], true);
		}
		output.SetCardinalityUnsafe(chunk.size());
		return output;
	}

	ColumnDataCollection &collection;
	vector<string> names;
	ClientProperties properties;
	ColumnDataScanState scan;
	DataChunk chunk;
	DataChunk output;
	string error;
};

CollectionStreamData &StreamData(ArrowArrayStream *stream) {
	return *static_cast<CollectionStreamData *>(stream->private_data);
}

int GetSchema(ArrowArrayStream *stream, ArrowSchema *out) {
	auto &data = StreamData(stream);
	try {
		ArrowConverter::ToArrowSchema(out, data.output.GetTypes(), data.names, data.properties);
		return 0;
	} catch (std::exception &ex) {
		data.error = ex.what();
		return EIO;
	}
}

int GetNext(ArrowArrayStream *stream, ArrowArray *out) {
	auto &data = StreamData(stream);
	try {
		auto &chunk = data.Next();
		if (chunk.size() == 0) {
			out->release = nullptr;
			return 0;
		}
		ArrowConverter::ToArrowArray(chunk, out, data.properties, {});
		return 0;
	} catch (std::exception &ex) {
		data.error = ex.what();
		return EIO;
	}
}

const char *GetLastError(ArrowArrayStream *stream) {
	auto &data = StreamData(stream);
	return data.error.empty() ? nullptr : data.error.c_str();
}

void Release(ArrowArrayStream *stream) {
	delete static_cast<CollectionStreamData *>(stream->private_data);
	stream->release = nullptr;
}

void OracleDefaultFunction(DataChunk &, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, true);
}

} // namespace

void OracleDefault::Register(ExtensionLoader &loader) {
	ScalarFunction function(NAME, {}, LogicalType::SQLNULL, OracleDefaultFunction);
	// Never folded into a NULL constant, which would hide it.
	function.SetVolatile();
	loader.RegisterFunction(function);
}

unique_ptr<ParsedExpression> OracleDefault::Create() {
	return make_uniq<FunctionExpression>(NAME, vector<unique_ptr<ParsedExpression>>());
}

bool OracleDefault::Is(const Expression &expr) {
	if (BoundCastExpression::IsCast(expr)) {
		return Is(BoundCastExpression::Child(expr.Cast<BoundFunctionExpression>()));
	}
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       expr.Cast<BoundFunctionExpression>().Function().GetName() == NAME;
}

vector<bool> OracleDefault::Columns(const PhysicalOperator &plan) {
	vector<bool> result(plan.GetTypes().size(), false);
	if (plan.type != PhysicalOperatorType::PROJECTION) {
		return result;
	}
	auto &projection = plan.Cast<PhysicalProjection>();
	for (idx_t c = 0; c < projection.select_list.size() && c < result.size(); c++) {
		result[c] = Is(*projection.select_list[c]);
	}
	return result;
}

vector<ColumnRole> ReadColumnRoles(ClientContext &context, shared_ptr<Connection> connection,
                                   const OracleTable &table) {
	auto sql = "SELECT column_name, virtual_column, identity_column,"
	           " CASE WHEN default_length > 0 THEN 'YES' ELSE 'NO' END"
	           " FROM all_tab_cols WHERE owner = " +
	           QuoteLiteral(table.schema) + " AND table_name = " + QuoteLiteral(table.name) +
	           " AND hidden_column = 'NO'";
	unordered_map<string, ColumnRole> by_name;
	for (auto &row : QueryRows(context, std::move(connection), sql)) {
		auto role = ColumnRole::VALUE;
		if (row[1].ToString() == "YES") {
			role = ColumnRole::VIRTUAL;
		} else if (row[2].ToString() == "YES" || row[3].ToString() == "YES") {
			role = ColumnRole::DEFAULTED;
		}
		by_name[row[0].ToString()] = role;
	}
	vector<ColumnRole> roles;
	for (auto &name : table.column_names) {
		auto entry = by_name.find(name);
		roles.push_back(entry == by_name.end() ? ColumnRole::VALUE : entry->second);
	}
	return roles;
}

namespace {

class InsertGlobalState : public GlobalSinkState {
public:
	//! `types` are those of the rows the sink receives: the table's for an
	//! INSERT, which DuckDB casts to them, and the query's for CREATE TABLE
	//! AS, whose rows are not cast to the new table's types.
	InsertGlobalState(ClientContext &context, OrkideaTableEntry &table, vector<ColumnRole> roles_p,
	                  const vector<LogicalType> &types)
	    : context(context), table(table), roles(std::move(roles_p)),
	      connection(OrkideaTransaction::Get(context, table.ParentCatalog()).GetWriteConnection()),
	      buffer(make_uniq<ColumnDataCollection>(context, types)), any_value(roles.size(), false) {
		if (types.size() != roles.size()) {
			throw InternalException("orkidea insert: %llu input columns for %llu table columns", types.size(),
			                        roles.size());
		}
	}

	void Append(DataChunk &chunk) {
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
				                            table.GetOracleTable().column_names[c], table.GetOracleTable().name);
			}
		}
		buffer->Append(chunk);
		if (buffer->Count() >= FLUSH_ROWS) {
			Flush();
		}
	}

	//! Sends the buffered rows in one ingest, which Oracle applies whole or
	//! not at all (the driver wraps it in a savepoint).
	void Flush() {
		if (buffer->Count() == 0) {
			return;
		}
		auto &oracle_table = table.GetOracleTable();
		vector<column_t> sent;
		vector<string> names;
		for (idx_t c = 0; c < roles.size(); c++) {
			if (roles[c] == ColumnRole::VALUE || (roles[c] == ColumnRole::DEFAULTED && any_value[c])) {
				sent.push_back(c);
				// Quoted, so that the driver takes the name as Oracle has it.
				names.push_back(QuoteIdentifier(oracle_table.column_names[c]));
			}
		}
		if (sent.empty()) {
			throw NotImplementedException("Inserting rows that only take Oracle defaults is not supported; name at "
			                              "least one column of %s",
			                              oracle_table.name);
		}
		ArrowArrayStream stream;
		CollectionStream(context, *buffer, std::move(names), stream, sent);

		Statement statement(connection);
		statement.SetOption(ADBC_INGEST_OPTION_TARGET_TABLE, QuoteIdentifier(oracle_table.name));
		statement.SetOption("adbc.ingest.target_db_schema", QuoteIdentifier(oracle_table.schema));
		statement.SetOption(ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND);
		statement.BindStream(stream);
		auto rows = statement.ExecuteUpdate();
		inserted += rows < 0 ? buffer->Count() : NumericCast<idx_t>(rows);

		buffer->Reset();
		std::fill(any_value.begin(), any_value.end(), false);
	}

	ClientContext &context;
	OrkideaTableEntry &table;
	vector<ColumnRole> roles;
	shared_ptr<Connection> connection;
	unique_ptr<ColumnDataCollection> buffer;
	//! Whether a column held a non-NULL value in the rows buffered so far.
	vector<bool> any_value;
	idx_t inserted = 0;
};

} // namespace

void CollectionStream(ClientContext &context, ColumnDataCollection &collection, vector<string> names,
                      ArrowArrayStream &stream, vector<column_t> column_ids, vector<LogicalType> null_types) {
	auto data = make_uniq<CollectionStreamData>(context, collection, std::move(names));
	data->Initialize(Allocator::Get(context), column_ids, null_types);
	stream.get_schema = GetSchema;
	stream.get_next = GetNext;
	stream.get_last_error = GetLastError;
	stream.release = Release;
	stream.private_data = data.release();
}

OrkideaInsert::OrkideaInsert(PhysicalPlan &plan, LogicalOperator &op, OrkideaTableEntry &table_p,
                             vector<bool> omitted_p)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(&table_p),
      omitted(std::move(omitted_p)) {
}

OrkideaInsert::OrkideaInsert(PhysicalPlan &plan, LogicalOperator &op, SchemaCatalogEntry &schema_p,
                             unique_ptr<BoundCreateTableInfo> info_p)
    : PhysicalOperator(plan, PhysicalOperatorType::EXTENSION, op.types, 1), schema(&schema_p), info(std::move(info_p)) {
}

unique_ptr<GlobalSinkState> OrkideaInsert::GetGlobalSinkState(ClientContext &context) const {
	if (table) {
		auto connection = OrkideaTransaction::Get(context, table.get_mutable()->ParentCatalog()).GetConnection();
		auto roles = ReadColumnRoles(context, connection, table->GetOracleTable());
		for (idx_t c = 0; c < omitted.size() && c < roles.size(); c++) {
			if (omitted[c]) {
				roles[c] = ColumnRole::OMITTED;
			}
		}
		return make_uniq<InsertGlobalState>(context, *table.get_mutable(), std::move(roles),
		                                    children[0].get().GetTypes());
	}
	auto &schema_entry = *schema.get_mutable();
	auto &created =
	    schema_entry.CreateTable(schema_entry.GetCatalogTransaction(context), *info)->Cast<OrkideaTableEntry>();
	vector<ColumnRole> roles(created.GetOracleTable().column_names.size(), ColumnRole::VALUE);
	return make_uniq<InsertGlobalState>(context, created, std::move(roles), children[0].get().GetTypes());
}

SinkResultType OrkideaInsert::Sink(ExecutionContext &, DataChunk &chunk, OperatorSinkInput &input) const {
	input.global_state.Cast<InsertGlobalState>().Append(chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType OrkideaInsert::Finalize(Pipeline &, Event &, ClientContext &, OperatorSinkFinalizeInput &input) const {
	input.global_state.Cast<InsertGlobalState>().Flush();
	return SinkFinalizeType::READY;
}

SourceResultType OrkideaInsert::GetDataInternal(ExecutionContext &, DataChunk &chunk, OperatorSourceInput &) const {
	auto &state = sink_state->Cast<InsertGlobalState>();
	chunk.data[0].Append(Value::BIGINT(NumericCast<int64_t>(state.inserted)));
	chunk.CheckCardinality(1);
	return SourceResultType::FINISHED;
}

string OrkideaInsert::GetName() const {
	return table ? "ORKIDEA_INSERT" : "ORKIDEA_CREATE_TABLE_AS";
}

InsertionOrderPreservingMap<string> OrkideaInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table"] = table ? table->GetOracleTable().QualifiedName() : info->Base().GetTableName().GetIdentifierName();
	return result;
}

PhysicalOperator &OrkideaInsert::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                            optional_ptr<PhysicalOperator> plan) {
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw NotImplementedException("This form of INSERT ... ON CONFLICT is not supported for Oracle tables");
	}
	if (!plan) {
		throw NotImplementedException("INSERT ... DEFAULT VALUES is not supported for Oracle tables");
	}
	if (op.return_chunk) {
		return OrkideaModify::PlanInsertReturning(context, planner, op, *plan);
	}
	auto &insert = planner.Make<OrkideaInsert>(op, op.table.Cast<OrkideaTableEntry>(), OracleDefault::Columns(*plan));
	insert.children.push_back(*plan);
	return insert;
}

PhysicalOperator &OrkideaInsert::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &planner,
                                                   LogicalCreateTable &op, PhysicalOperator &plan) {
	auto &insert = planner.Make<OrkideaInsert>(op, op.schema, std::move(op.info));
	insert.children.push_back(plan);
	return insert;
}

} // namespace orkidea
} // namespace duckdb
