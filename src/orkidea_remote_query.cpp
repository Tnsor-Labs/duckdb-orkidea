#include "orkidea_remote_query.hpp"

#include "orkidea_query.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_transaction.hpp"

namespace duckdb {
namespace orkidea {

namespace {

unique_ptr<GlobalTableFunctionState> InitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RemoteQueryBindData>();
	auto result = make_uniq<ArrowScanGlobalState>();
	result->stream =
	    OpenQuery(context, OrkideaTransaction::Get(context, bind_data.catalog).GetConnection(), bind_data.sql);
	result->max_threads = context.db->NumberOfThreads();
	return std::move(result);
}

InsertionOrderPreservingMap<string> ToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	result["Oracle SQL"] = input.bind_data->Cast<RemoteQueryBindData>().sql;
	return result;
}

} // namespace

RemoteQueryBindData::RemoteQueryBindData(OrkideaCatalog &catalog_p, shared_ptr<ArrowScanFactory> factory, string sql_p)
    : ArrowScanFunctionData(std::move(factory)), catalog(catalog_p), sql(std::move(sql_p)) {
}

TableFunction RemoteQuery::Function(ClientContext &context, OrkideaCatalog &catalog, string sql,
                                    unique_ptr<FunctionData> &bind_data, vector<LogicalType> &types) {
	auto connection = OrkideaTransaction::Get(context, catalog).GetConnection();
	auto result =
	    make_uniq<RemoteQueryBindData>(catalog, make_shared_ptr<QueryScanFactory>(std::move(connection), sql), sql);
	result->factory->GetSchema(result->schema_root.arrow_schema);
	ArrowTableFunction::PopulateArrowTableSchema(context, result->arrow_table, result->schema_root.arrow_schema);
	types = result->arrow_table.GetTypes();
	result->all_types = types;
	bind_data = std::move(result);

	TableFunction function(NAME, {}, ArrowTableFunction::ArrowScanFunction, nullptr, InitGlobal,
	                       ArrowTableFunction::ArrowScanInitLocal);
	function.to_string = ToString;
	return function;
}

} // namespace orkidea
} // namespace duckdb
