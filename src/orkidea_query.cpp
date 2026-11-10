#include "orkidea_query.hpp"

#include "orkidea_secrets.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace orkidea {

void CheckResultTypes(ClientContext &context, StatementStream &stream, const vector<LogicalType> &types) {
	ArrowSchemaWrapper schema;
	auto &arrow = stream.arrow_array_stream;
	if (arrow.get_schema(&arrow, &schema.arrow_schema) != 0) {
		auto message = arrow.get_last_error(&arrow);
		throw IOException("Orkidea: reading the result schema failed: %s",
		                  message && *message ? message : "no further details");
	}
	auto count = NumericCast<idx_t>(schema.arrow_schema.n_children);
	if (count != types.size()) {
		throw InvalidInputException("Orkidea: the query returns %llu columns, but returned %llu when it was planned; "
		                            "has a table it reads changed?",
		                            count, types.size());
	}
	for (idx_t i = 0; i < count; i++) {
		auto &column = *schema.arrow_schema.children[i];
		auto type = ArrowType::GetArrowLogicalType(context, column)->GetDuckType(true);
		if (type != types[i]) {
			throw InvalidInputException(
			    "Orkidea: column %s of the query is %s when it runs, but was %s when it was planned. A bind "
			    "placeholder in the select list takes its type from its value: CAST it, as in CAST(:x AS DATE)",
			    column.name ? column.name : to_string(i + 1), type.ToString(), types[i].ToString());
		}
	}
}

QueryScanFactory::QueryScanFactory(ClientContext &context_p, shared_ptr<Connection> connection_p, string sql_p,
                                   Parameters parameters_p)
    : context(context_p), connection(std::move(connection_p)), sql(std::move(sql_p)),
      parameters(std::move(parameters_p)) {
}

unique_ptr<Statement> QueryScanFactory::NewStatement() const {
	auto statement = make_uniq<Statement>(connection);
	statement->SetSqlQuery(sql);
	return statement;
}

void QueryScanFactory::GetSchema(ArrowSchema &schema) {
	NewStatement()->ExecuteSchema(schema);
	ArrowTableSchema table;
	ArrowTableFunction::PopulateArrowTableSchema(context, table, schema);
	types = table.GetTypes();
}

unique_ptr<ArrowArrayStreamWrapper> QueryScanFactory::ProduceStream(ArrowStreamParameters &) {
	auto statement = NewStatement();
	if (!parameters.Empty()) {
		parameters.Bind(context, *statement);
	}
	auto stream = make_uniq<StatementStream>(std::move(statement));
	CheckResultTypes(context, *stream, types);
	return std::move(stream);
}

namespace {

string StringArgument(const Value &value, const char *what) {
	if (value.IsNull()) {
		throw BinderException("orkidea_query: the %s must not be NULL", what);
	}
	return StringValue::Get(value);
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto target = StringArgument(input.inputs[0], "connection target");
	auto sql = StringArgument(input.inputs[1], "query");
	Parameters parameters;
	auto params = input.named_parameters.find("params");
	if (params != input.named_parameters.end()) {
		parameters = Parameters::FromValue(params->second, "orkidea_query");
	}

	auto database = make_shared_ptr<Database>(Secrets::ResolveTarget(context, target));
	auto connection = make_shared_ptr<Connection>(std::move(database));
	auto factory =
	    make_shared_ptr<QueryScanFactory>(context, std::move(connection), std::move(sql), std::move(parameters));

	auto result = make_uniq<ArrowScanFunctionData>(factory);
	factory->GetSchema(result->schema_root.arrow_schema);
	if (result->schema_root.arrow_schema.n_children == 0) {
		throw BinderException("orkidea_query: the statement returns no rows; run it with orkidea_execute");
	}
	ArrowTableFunction::PopulateArrowTableSchema(context, result->arrow_table, result->schema_root.arrow_schema);
	names = StringsToIdentifiers(result->arrow_table.GetNames());
	return_types = result->arrow_table.GetTypes();
	result->all_types = return_types;
	// The query is the user's own SQL, so DuckDB reads every column it returns.
	result->projection_pushdown_enabled = false;
	return std::move(result);
}

} // namespace

void QueryFunction::Register(ExtensionLoader &loader) {
	TableFunction function("orkidea_query", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                       ArrowTableFunction::ArrowScanFunction, Bind, ArrowTableFunction::ArrowScanInitGlobal,
	                       ArrowTableFunction::ArrowScanInitLocal);
	// NULL by default: no parameters.
	function.GetSignature().AddKeywordOnly(Identifier("params"), LogicalType::ANY, Value());
	function.projection_pushdown = false;
	function.filter_pushdown = false;
	function.parallelism = TableFunctionParallelism::SEQUENTIAL;
	loader.RegisterFunction(function);
}

} // namespace orkidea
} // namespace duckdb
