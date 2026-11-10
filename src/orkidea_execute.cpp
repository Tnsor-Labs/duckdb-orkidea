#include "orkidea_execute.hpp"

#include "orkidea_adbc.hpp"
#include "orkidea_secrets.hpp"
#include "orkidea_util.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace orkidea {

namespace {

struct ExecuteBindData : public TableFunctionData {
	ExecuteBindData(AdbcOptions options_p, string sql_p, Parameters parameters_p)
	    : options(std::move(options_p)), sql(std::move(sql_p)), parameters(std::move(parameters_p)) {
	}

	AdbcOptions options;
	string sql;
	Parameters parameters;
};

struct ExecuteState : public GlobalTableFunctionState {
	bool done = false;
};

string StringArgument(const Value &value, const char *what) {
	if (value.IsNull()) {
		throw BinderException("orkidea_execute: the %s must not be NULL", what);
	}
	return StringValue::Get(value);
}

unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                              vector<Identifier> &names) {
	auto target = StringArgument(input.inputs[0], "connection target");
	auto sql = StringArgument(input.inputs[1], "statement");
	Parameters parameters;
	auto params = input.named_parameters.find("params");
	if (params != input.named_parameters.end()) {
		parameters = Parameters::FromValue(params->second, "orkidea_execute");
	}
	names.emplace_back("rows_affected");
	return_types.emplace_back(LogicalType::BIGINT);
	return make_uniq<ExecuteBindData>(Secrets::ResolveTarget(context, target), std::move(sql), std::move(parameters));
}

unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ExecuteState>();
}

//! Runs the statement on the first call, so that binding alone (EXPLAIN, a
//! prepared statement) never runs it. The connection autocommits.
void Execute(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<ExecuteState>();
	if (state.done) {
		return;
	}
	state.done = true;
	auto &bind_data = input.bind_data->Cast<ExecuteBindData>();
	auto connection = make_shared_ptr<Connection>(make_shared_ptr<Database>(bind_data.options));
	auto rows_affected = ExecuteStatement(context, std::move(connection), bind_data.sql, bind_data.parameters);
	output.data[0].Append(rows_affected < 0 ? Value(LogicalType::BIGINT) : Value::BIGINT(rows_affected));
	output.CheckCardinality(1);
}

} // namespace

void ExecuteFunction::Register(ExtensionLoader &loader) {
	TableFunction function("orkidea_execute", {LogicalType::VARCHAR, LogicalType::VARCHAR}, Execute, Bind, InitGlobal);
	// NULL by default: no parameters.
	function.GetSignature().AddKeywordOnly(Identifier("params"), LogicalType::ANY, Value());
	loader.RegisterFunction(function);
}

} // namespace orkidea
} // namespace duckdb
