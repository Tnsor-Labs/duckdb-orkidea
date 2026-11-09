#include "orkidea_query.hpp"

#include "orkidea_secrets.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace orkidea {

QueryScanFactory::QueryScanFactory(shared_ptr<Connection> connection_p, string sql_p)
    : connection(std::move(connection_p)), sql(std::move(sql_p)) {
}

unique_ptr<Statement> QueryScanFactory::NewStatement() const {
	auto statement = make_uniq<Statement>(connection);
	statement->SetSqlQuery(sql);
	return statement;
}

void QueryScanFactory::GetSchema(ArrowSchema &schema) {
	NewStatement()->ExecuteSchema(schema);
}

unique_ptr<ArrowArrayStreamWrapper> QueryScanFactory::ProduceStream(ArrowStreamParameters &) {
	return make_uniq<StatementStream>(NewStatement());
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

	auto database = make_shared_ptr<Database>(Secrets::ResolveTarget(context, target));
	auto connection = make_shared_ptr<Connection>(std::move(database));
	auto factory = make_shared_ptr<QueryScanFactory>(std::move(connection), std::move(sql));

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
	function.projection_pushdown = false;
	function.filter_pushdown = false;
	function.parallelism = TableFunctionParallelism::SEQUENTIAL;
	loader.RegisterFunction(function);
}

} // namespace orkidea
} // namespace duckdb
