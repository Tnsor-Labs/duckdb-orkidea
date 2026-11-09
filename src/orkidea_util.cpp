#include "orkidea_util.hpp"

#include "orkidea_logging.hpp"

#include "duckdb/common/exception.hpp"
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
	auto copy = make_uniq<SchemaCopy>();
	copy->format = source.format ? source.format : "";
	copy->name = source.name ? source.name : "";
	copy->metadata.assign(source.metadata ? source.metadata : "", MetadataLength(source.metadata));

	auto child_count = NumericCast<idx_t>(source.n_children);
	copy->children = unique_ptr<ArrowSchema[]>(new ArrowSchema[child_count]);
	for (idx_t i = 0; i < child_count; i++) {
		CopyArrowSchema(*source.children[i], copy->children[i]);
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
	target.n_children = source.n_children;
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

int64_t ExecuteStatement(ClientContext &context, shared_ptr<Connection> connection, const string &sql) {
	auto start = std::chrono::steady_clock::now();
	Statement statement(std::move(connection));
	statement.SetSqlQuery(sql);
	auto rows_affected = statement.ExecuteUpdate();
	DUCKDB_LOG(context, QueryLogType, sql, ElapsedMilliseconds(start));
	return rows_affected;
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
