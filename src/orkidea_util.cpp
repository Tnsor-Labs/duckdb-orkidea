#include "orkidea_util.hpp"

#include "orkidea_logging.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/function/table/arrow.hpp"

#include <chrono>
#include <cstring>

namespace duckdb {
namespace orkidea {

string QuoteIdentifier(const string &name) {
	if (name.find('"') != string::npos) {
		throw InvalidInputException("Oracle names cannot contain double quotes: %s", name);
	}
	return "\"" + name + "\"";
}

string QuoteLiteral(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

string NewOracleName(const string &name) {
	bool simple = !name.empty() && name.size() <= 128 && StringUtil::CharacterIsAlpha(name[0]);
	for (auto c : name) {
		simple = simple && (StringUtil::CharacterIsAlphaNumeric(c) || c == '_' || c == '$' || c == '#');
	}
	return QuoteIdentifier(simple ? StringUtil::Upper(name) : name);
}

string OracleColumnType(const LogicalType &type, bool native_boolean) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return native_boolean ? "BOOLEAN" : "NUMBER(1)";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return "NUMBER(3)";
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		return "NUMBER(5)";
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
		return "NUMBER(10)";
	case LogicalTypeId::BIGINT:
		return "NUMBER(19)";
	case LogicalTypeId::UBIGINT:
		return "NUMBER(20)";
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return "NUMBER(38)";
	case LogicalTypeId::FLOAT:
		return "BINARY_FLOAT";
	case LogicalTypeId::DOUBLE:
		return "BINARY_DOUBLE";
	case LogicalTypeId::DECIMAL:
		return StringUtil::Format("NUMBER(%d,%d)", DecimalType::GetWidth(type), DecimalType::GetScale(type));
	case LogicalTypeId::VARCHAR:
		return "VARCHAR2(4000)";
	case LogicalTypeId::BLOB:
		return "RAW(2000)";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIMESTAMP_SEC:
		return "TIMESTAMP(0)";
	case LogicalTypeId::TIMESTAMP_MS:
		return "TIMESTAMP(3)";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP(6)";
	case LogicalTypeId::TIMESTAMP_NS:
		return "TIMESTAMP(9)";
	case LogicalTypeId::TIMESTAMP_TZ:
		return "TIMESTAMP(6) WITH TIME ZONE";
	default:
		throw NotImplementedException("A %s column cannot be added to an Oracle table from DuckDB; use orkidea_execute",
		                              type.ToString());
	}
}

string MatchName(const string &name, const vector<string> &candidates) {
	auto upper = StringUtil::Upper(name);
	string upper_match;
	vector<string> folded_matches;
	for (auto &candidate : candidates) {
		if (candidate == name) {
			return candidate;
		}
		if (candidate == upper) {
			upper_match = candidate;
		} else if (StringUtil::CIEquals(candidate, name)) {
			folded_matches.push_back(candidate);
		}
	}
	if (!upper_match.empty()) {
		return upper_match;
	}
	if (folded_matches.size() > 1) {
		throw BinderException("\"%s\" is ambiguous in Oracle, which has %s; quote the exact name", name,
		                      StringUtil::Join(folded_matches, ", "));
	}
	return folded_matches.empty() ? string() : folded_matches[0];
}

namespace {

int32_t ReadInt32(const char *data) {
	int32_t value;
	memcpy(&value, data, sizeof(value));
	return value;
}

} // namespace

string ArrowMetadataValue(const char *metadata, const string &key) {
	if (!metadata) {
		return string();
	}
	auto pairs = ReadInt32(metadata);
	auto position = metadata + sizeof(int32_t);
	for (int32_t i = 0; i < pairs; i++) {
		auto key_length = ReadInt32(position);
		string current_key(position + sizeof(int32_t), NumericCast<size_t>(key_length));
		position += sizeof(int32_t) + key_length;
		auto value_length = ReadInt32(position);
		if (current_key == key) {
			return string(position + sizeof(int32_t), NumericCast<size_t>(value_length));
		}
		position += sizeof(int32_t) + value_length;
	}
	return string();
}

namespace {

//! Owns everything a copied schema points to.
struct SchemaCopy {
	string format;
	string name;
	string metadata;
	unique_ptr<ArrowSchema[]> children;
	vector<ArrowSchema *> child_pointers;
	unique_ptr<ArrowSchema> dictionary;
};

//! The byte length of Arrow C metadata: an int32 count, then that many
//! int32-length-prefixed keys and values.
size_t MetadataLength(const char *metadata) {
	if (!metadata) {
		return 0;
	}
	size_t length = sizeof(int32_t);
	auto pairs = ReadInt32(metadata);
	for (int32_t i = 0; i < pairs * 2; i++) {
		length += sizeof(int32_t) + NumericCast<size_t>(ReadInt32(metadata + length));
	}
	return length;
}

void ReleaseCopy(ArrowSchema *schema) {
	auto copy = static_cast<SchemaCopy *>(schema->private_data);
	for (auto child : copy->child_pointers) {
		if (child->release) {
			child->release(child);
		}
	}
	if (copy->dictionary && copy->dictionary->release) {
		copy->dictionary->release(copy->dictionary.get());
	}
	delete copy;
	schema->release = nullptr;
}

} // namespace

void CopyArrowSchema(const ArrowSchema &source, ArrowSchema &target) {
	CopyArrowSchemaColumns(source, 0, NumericCast<idx_t>(source.n_children), target);
}

void CopyArrowSchemaColumns(const ArrowSchema &source, idx_t offset, idx_t count, ArrowSchema &target) {
	if (offset + count > NumericCast<idx_t>(source.n_children)) {
		throw InternalException("Arrow schema has %d children, not %d", source.n_children, offset + count);
	}
	auto copy = make_uniq<SchemaCopy>();
	copy->format = source.format ? source.format : "";
	copy->name = source.name ? source.name : "";
	copy->metadata.assign(source.metadata ? source.metadata : "", MetadataLength(source.metadata));

	copy->children = unique_ptr<ArrowSchema[]>(new ArrowSchema[count]);
	for (idx_t i = 0; i < count; i++) {
		CopyArrowSchema(*source.children[offset + i], copy->children[i]);
		copy->child_pointers.push_back(&copy->children[i]);
	}
	if (source.dictionary) {
		copy->dictionary = make_uniq<ArrowSchema>();
		CopyArrowSchema(*source.dictionary, *copy->dictionary);
	}

	target.format = copy->format.c_str();
	target.name = source.name ? copy->name.c_str() : nullptr;
	target.metadata = source.metadata ? copy->metadata.data() : nullptr;
	target.flags = source.flags;
	target.n_children = NumericCast<int64_t>(count);
	target.children = copy->child_pointers.empty() ? nullptr : copy->child_pointers.data();
	target.dictionary = copy->dictionary.get();
	target.release = ReleaseCopy;
	target.private_data = copy.release();
}

namespace {

int64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point start) {
	auto elapsed = std::chrono::steady_clock::now() - start;
	return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

} // namespace

unique_ptr<StatementStream> OpenQuery(ClientContext &context, shared_ptr<Connection> connection, const string &sql) {
	auto start = std::chrono::steady_clock::now();
	auto statement = make_uniq<Statement>(std::move(connection));
	statement->SetSqlQuery(sql);
	auto stream = make_uniq<StatementStream>(std::move(statement));
	DUCKDB_LOG(context, QueryLogType, sql, ElapsedMilliseconds(start));
	return stream;
}

int64_t ExecuteStatement(ClientContext &context, shared_ptr<Connection> connection, const string &sql,
                         const Parameters &parameters) {
	auto start = std::chrono::steady_clock::now();
	Statement statement(std::move(connection));
	statement.SetSqlQuery(sql);
	if (!parameters.Empty()) {
		parameters.Bind(context, statement);
	}
	auto rows_affected = statement.ExecuteUpdate();
	DUCKDB_LOG(context, QueryLogType, sql, ElapsedMilliseconds(start));
	return rows_affected;
}

void ReadStream(ClientContext &context, StatementStream &stream, ColumnDataCollection &rows) {
	ArrowSchemaWrapper schema;
	stream.GetSchema(schema);
	ArrowTableSchema table;
	ArrowTableFunction::PopulateArrowTableSchema(context, table, schema.arrow_schema);
	auto source_types = table.GetTypes();
	auto &target_types = rows.Types();
	if (source_types.size() != target_types.size()) {
		throw InternalException("Oracle returned %llu columns where %llu were expected", source_types.size(),
		                        target_types.size());
	}
	vector<column_t> column_ids;
	for (idx_t i = 0; i < source_types.size(); i++) {
		column_ids.push_back(i);
	}
	DataChunk source;
	source.Initialize(Allocator::Get(context), source_types);
	DataChunk target;
	target.Initialize(Allocator::Get(context), target_types);
	while (true) {
		ArrowScanLocalState state(nullptr, context);
		state.chunk = stream.GetNextChunk();
		if (!state.chunk->arrow_array.release) {
			return;
		}
		state.column_ids = column_ids;
		auto length = NumericCast<idx_t>(state.chunk->arrow_array.length);
		while (state.chunk_offset < length) {
			auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, length - state.chunk_offset);
			source.Reset();
			source.SetChildCardinality(count);
			ArrowTableFunction::ArrowToDuckDB(state, table.GetColumns(), source);
			target.Reset();
			for (idx_t c = 0; c < target_types.size(); c++) {
				if (source_types[c] == target_types[c]) {
					target.data[c].Reference(source.data[c]);
				} else {
					VectorOperations::Cast(context, source.data[c], target.data[c], count);
				}
			}
			target.SetCardinalityUnsafe(count);
			rows.Append(target);
			state.chunk_offset += count;
		}
	}
}

vector<vector<Value>> QueryRows(ClientContext &context, shared_ptr<Connection> connection, const string &sql) {
	auto query = OpenQuery(context, std::move(connection), sql);
	auto &stream = *query;

	ArrowSchemaWrapper schema;
	stream.GetSchema(schema);
	ArrowTableSchema table;
	ArrowTableFunction::PopulateArrowTableSchema(context, table, schema.arrow_schema);
	auto types = table.GetTypes();

	vector<column_t> column_ids;
	for (idx_t i = 0; i < types.size(); i++) {
		column_ids.push_back(i);
	}
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), types);

	vector<vector<Value>> rows;
	while (true) {
		ArrowScanLocalState state(nullptr, context);
		state.chunk = stream.GetNextChunk();
		if (!state.chunk->arrow_array.release) {
			return rows;
		}
		state.column_ids = column_ids;
		auto length = NumericCast<idx_t>(state.chunk->arrow_array.length);
		while (state.chunk_offset < length) {
			auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, length - state.chunk_offset);
			chunk.Reset();
			chunk.SetChildCardinality(count);
			ArrowTableFunction::ArrowToDuckDB(state, table.GetColumns(), chunk);
			for (idx_t row = 0; row < count; row++) {
				vector<Value> values;
				for (idx_t column = 0; column < types.size(); column++) {
					values.push_back(chunk.GetValue(column, row));
				}
				rows.push_back(std::move(values));
			}
			state.chunk_offset += count;
		}
	}
}

} // namespace orkidea
} // namespace duckdb
