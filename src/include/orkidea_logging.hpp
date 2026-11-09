//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_logging.hpp
//
// The SQL sent to Oracle, logged at DEBUG level as `OrkideaQuery`:
//
//   CALL enable_logging('OrkideaQuery', level = 'debug');
//   SELECT message FROM duckdb_logs WHERE type = 'OrkideaQuery';
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logging.hpp"

namespace duckdb {

class ExtensionLoader;

namespace orkidea {

class QueryLogType : public LogType {
public:
	static constexpr const char *NAME = "OrkideaQuery";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	QueryLogType();

	static string ConstructLogMessage(const string &sql, int64_t duration_ms);
	static LogicalType GetLogType();
	static void Register(ExtensionLoader &loader);
};

} // namespace orkidea
} // namespace duckdb
