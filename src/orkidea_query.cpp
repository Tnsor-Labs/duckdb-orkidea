#include "orkidea_query.hpp"

#include "orkidea_secrets.hpp"

#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! The value bound for a parameter, labeled `label` in errors.
Value BindableValue(const Value &value, const string &label) {
	switch (value.type().id()) {
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT: {
		// Arrow carries them as DECIMAL(38,0), which a larger value would not fit.
		string error;
		auto result = value.DefaultTryCastAs(LogicalType::DECIMAL(38, 0), &error);
		if (!result) {
			throw BinderException("orkidea_query: parameter %s: %s", label, error);
		}
		return *result;
	}
	case LogicalTypeId::TIMESTAMP_TZ:
		throw BinderException(
		    "orkidea_query: parameter %s is a TIMESTAMP WITH TIME ZONE, which the driver binds without "
		    "its time zone; bind the UTC time as a TIMESTAMP (x AT TIME ZONE 'UTC') and convert it "
		    "in Oracle with FROM_TZ(CAST(:%s AS TIMESTAMP), 'UTC')",
		    label, label);
	default:
		return value;
	}
}

} // namespace

QueryParameters QueryParameters::FromValue(const Value &value) {
	QueryParameters result;
	if (value.IsNull()) {
		return result;
	}
	auto &type = value.type();
	if (type.id() != LogicalTypeId::STRUCT && type.id() != LogicalTypeId::TUPLE) {
		throw BinderException("orkidea_query: params must be a STRUCT, such as {'id': 42} for :id, or a row(...) of "
		                      "values for :1, :2, ..., not %s",
		                      type.ToString());
	}
	result.by_name = !StructType::IsUnnamed(type);
	auto &children = StructType::GetChildTypes(type);
	auto &values = StructValue::GetChildren(value);
	for (idx_t i = 0; i < children.size(); i++) {
		auto label = result.by_name ? children[i].first.GetIdentifierName() : to_string(i + 1);
		result.names.push_back(result.by_name ? label : "p" + label);
		result.values.push_back(BindableValue(values[i], label));
	}
	return result;
}

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
                                   QueryParameters parameters_p)
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

//! Binds the parameters as a one-row batch. The session's Arrow export
//! settings never change how a value binds.
void QueryScanFactory::BindParameters(Statement &statement) const {
	vector<LogicalType> parameter_types;
	for (auto &value : parameters.values) {
		parameter_types.push_back(value.type());
	}
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), parameter_types, 1);
	for (idx_t i = 0; i < parameters.values.size(); i++) {
		chunk.data[i].Append(parameters.values[i]);
	}
	chunk.CheckCardinality(1);

	auto properties = context.GetClientProperties();
	properties.arrow_offset_size = ArrowOffsetSize::REGULAR;
	properties.arrow_use_list_view = false;
	properties.produce_arrow_string_view = false;
	properties.arrow_lossless_conversion = false;
	ArrowSchemaWrapper schema;
	ArrowArrayWrapper values;
	ArrowConverter::ToArrowSchema(&schema.arrow_schema, parameter_types, parameters.names, properties);
	ArrowConverter::ToArrowArray(chunk, &values.arrow_array, properties, {});
	statement.SetOption("adbc.statement.bind_by_name", parameters.by_name ? "true" : "false");
	statement.Bind(values.arrow_array, schema.arrow_schema);
}

unique_ptr<ArrowArrayStreamWrapper> QueryScanFactory::ProduceStream(ArrowStreamParameters &) {
	auto statement = NewStatement();
	if (!parameters.Empty()) {
		BindParameters(*statement);
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
	QueryParameters parameters;
	auto params = input.named_parameters.find("params");
	if (params != input.named_parameters.end()) {
		parameters = QueryParameters::FromValue(params->second);
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
