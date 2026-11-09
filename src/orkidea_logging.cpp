#include "orkidea_logging.hpp"

#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace orkidea {

constexpr LogLevel QueryLogType::LEVEL;

QueryLogType::QueryLogType() : LogType(NAME, LEVEL, GetLogType()) {
}

string QueryLogType::ConstructLogMessage(const string &sql, int64_t duration_ms) {
	child_list_t<Value> fields = {{"sql", Value(sql)}, {"duration_ms", Value::BIGINT(duration_ms)}};
	return Value::STRUCT(std::move(fields)).ToString();
}

LogicalType QueryLogType::GetLogType() {
	child_list_t<LogicalType> fields = {{"sql", LogicalType::VARCHAR}, {"duration_ms", LogicalType::BIGINT}};
	return LogicalType::STRUCT(std::move(fields));
}

void QueryLogType::Register(ExtensionLoader &loader) {
	loader.GetDatabaseInstance().GetLogManager().RegisterLogType(make_uniq<QueryLogType>());
}

} // namespace orkidea
} // namespace duckdb
