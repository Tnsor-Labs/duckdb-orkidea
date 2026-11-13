#include "storage/orkidea_schema_entry.hpp"

#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_table_entry.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "orkidea_insert.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"

namespace duckdb {
namespace orkidea {

namespace {

constexpr const char *TYPE_NAME_KEY = "oracle.type-name";

//! The schema's tables, views, and materialized views, with the optimizer's
//! row count for tables. Left out, as by the driver's catalog: dropped tables
//! in the recycle bin, nested tables, IOT overflow segments, and the
//! container tables of materialized views.
string TablesQuery(const string &owner) {
	// The owner filter goes into each branch: on the union as a whole, Oracle
	// reads every owner's objects first.
	auto owner_literal = QuoteLiteral(owner);
	// Only heap tables can be read in ROWID ranges by several sessions: not
	// index-organized or clustered tables, and not temporary ones, whose rows
	// each session sees on its own.
	// Rows of a table that is not index-organized have ROWIDs, by which
	// UPDATE and DELETE find them.
	return "SELECT table_name, num_rows, splittable, has_rowid FROM ("
	       " SELECT t.table_name, t.num_rows,"
	       "  CASE WHEN t.iot_type IS NULL AND t.cluster_name IS NULL AND t.temporary = 'N' THEN 'Y' ELSE 'N' END"
	       "  AS splittable, CASE WHEN t.iot_type IS NULL THEN 'Y' ELSE 'N' END AS has_rowid FROM all_tables t"
	       "  WHERE t.owner = " +
	       owner_literal +
	       "    AND t.dropped = 'NO' AND t.secondary = 'N' AND t.nested = 'NO'"
	       "    AND (t.iot_type IS NULL OR t.iot_type = 'IOT')"
	       "    AND NOT EXISTS (SELECT 1 FROM all_mviews m WHERE m.owner = t.owner AND m.mview_name = t.table_name)"
	       " UNION ALL SELECT view_name, NULL, 'N', 'N' FROM all_views WHERE owner = " +
	       owner_literal +
	       " UNION ALL SELECT mview_name, NULL, 'N', 'N' FROM all_mviews WHERE owner = " + owner_literal +
	       ") WHERE 1 = 1";
}

} // namespace

TableListing TableListing::FromRow(const vector<Value> &row) {
	TableListing listing;
	listing.estimated_rows = row[1].IsNull() ? -1 : row[1].GetValue<int64_t>();
	listing.splittable = row[2].ToString() == "Y";
	listing.has_rowid = row[3].ToString() == "Y";
	return listing;
}

namespace {

[[noreturn]] void ThrowNotSupported(const char *what) {
	throw NotImplementedException("%s in an attached Oracle database is not supported", what);
}

} // namespace

OrkideaSchemaEntry::OrkideaSchemaEntry(OrkideaCatalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

OrkideaSchemaEntry::~OrkideaSchemaEntry() = default;

OrkideaCatalog &OrkideaSchemaEntry::GetOrkideaCatalog() {
	return ParentCatalog().Cast<OrkideaCatalog>();
}

void OrkideaSchemaEntry::ListTables(ClientContext &context) {
	{
		lock_guard<mutex> guard(lock);
		if (tables_listed) {
			return;
		}
	}
	// Queried without the lock, so that lookups of tables already described
	// do not wait for Oracle.
	auto connection = OrkideaTransaction::Get(context, ParentCatalog()).GetConnection();
	auto rows = QueryRows(context, connection, TablesQuery(name.GetIdentifierName()));
	lock_guard<mutex> guard(lock);
	for (auto &row : rows) {
		table_names[row[0].ToString()] = TableListing::FromRow(row);
	}
	tables_listed = true;
}

vector<string> OrkideaSchemaEntry::QueryTableNames(ClientContext &context, const string &condition) {
	auto sql = TablesQuery(name.GetIdentifierName()) + " AND " + condition;
	auto connection = OrkideaTransaction::Get(context, ParentCatalog()).GetConnection();
	auto rows = QueryRows(context, connection, sql);
	vector<string> found;
	lock_guard<mutex> guard(lock);
	for (auto &row : rows) {
		table_names[row[0].ToString()] = TableListing::FromRow(row);
		found.push_back(row[0].ToString());
	}
	return found;
}

optional_ptr<OrkideaTableEntry> OrkideaSchemaEntry::FindTable(ClientContext &context, const string &table_name) {
	vector<string> known;
	{
		lock_guard<mutex> guard(lock);
		for (auto &entry : table_names) {
			known.push_back(entry.first);
		}
	}
	auto match = MatchName(table_name, known);
	if (match.empty()) {
		// Not seen yet, or created since: look up the name as given and as
		// an unquoted Oracle name, then any name that differs only in case.
		auto upper = StringUtil::Upper(table_name);
		match = MatchName(table_name, QueryTableNames(context, "table_name IN (" + QuoteLiteral(table_name) + ", " +
		                                                           QuoteLiteral(upper) + ")"));
		if (match.empty()) {
			match = MatchName(table_name, QueryTableNames(context, "UPPER(table_name) = " + QuoteLiteral(upper)));
		}
	}
	if (match.empty()) {
		return nullptr;
	}
	return DescribeTable(context, match);
}

optional_ptr<OrkideaTableEntry> OrkideaSchemaEntry::DescribeTable(ClientContext &context, const string &oracle_name) {
	{
		lock_guard<mutex> guard(lock);
		auto cached = tables.find(oracle_name);
		if (cached != tables.end()) {
			return cached->second.get();
		}
	}
	ArrowSchemaWrapper described;
	Statement statement(OrkideaTransaction::Get(context, ParentCatalog()).GetConnection());
	statement.SetSqlQuery("SELECT * FROM " + QuoteIdentifier(name.GetIdentifierName()) + "." +
	                      QuoteIdentifier(oracle_name));
	statement.ExecuteSchema(described.arrow_schema);
	return AddTable(context, oracle_name, described.arrow_schema, 0,
	                NumericCast<idx_t>(described.arrow_schema.n_children));
}

optional_ptr<OrkideaTableEntry> OrkideaSchemaEntry::AddTable(ClientContext &context, const string &oracle_name,
                                                             const ArrowSchema &described, idx_t offset, idx_t count) {
	auto table = make_shared_ptr<OracleTable>();
	table->schema = name.GetIdentifierName();
	table->name = oracle_name;
	CopyArrowSchemaColumns(described, offset, count, table->arrow_schema.arrow_schema);

	ArrowTableSchema arrow_table;
	ArrowTableFunction::PopulateArrowTableSchema(context, arrow_table, table->arrow_schema.arrow_schema);
	auto types = arrow_table.GetTypes();

	CreateTableInfo create_info(*this, Identifier(oracle_name));
	// Oracle can hold columns whose names differ only in case, which DuckDB
	// cannot; later ones get a suffix, as in DuckDB's own Postgres extension.
	case_insensitive_set_t used_names;
	auto &schema = table->arrow_schema.arrow_schema;
	for (idx_t i = 0; i < types.size(); i++) {
		auto &child = *schema.children[i];
		string column_name = child.name;
		table->column_names.push_back(column_name);
		table->oracle_types.push_back(ArrowMetadataValue(child.metadata, TYPE_NAME_KEY));
		auto duckdb_name = column_name;
		for (idx_t suffix = 1; used_names.count(duckdb_name); suffix++) {
			duckdb_name = column_name + ":" + to_string(suffix);
		}
		used_names.insert(duckdb_name);
		ColumnDefinition column(Identifier(duckdb_name), types[i]);
		column.SetDefaultValue(OracleDefault::Create());
		create_info.columns.AddColumn(std::move(column));
	}

	lock_guard<mutex> guard(lock);
	// Another statement may have described it meanwhile: the first entry is
	// kept, since statements may already use it.
	auto cached = tables.find(oracle_name);
	if (cached != tables.end()) {
		return cached->second.get();
	}
	auto known = table_names.find(oracle_name);
	if (known != table_names.end()) {
		table->estimated_rows = known->second.estimated_rows;
		table->splittable = known->second.splittable;
		table->has_rowid = known->second.has_rowid;
	}
	auto entry = make_uniq<OrkideaTableEntry>(ParentCatalog(), *this, create_info, std::move(table));
	auto &result = *entry;
	tables.emplace(oracle_name, std::move(entry));
	return &result;
}

namespace {

//! Tables described by one query, at most: enough to save most round trips,
//! few enough to keep the query cheap to parse.
constexpr idx_t MAX_BATCH_TABLES = 50;

//! The column put between two tables' columns in a batch, to tell them apart.
string BatchMarker(idx_t index) {
	return "orkidea$" + to_string(index);
}

} // namespace

void OrkideaSchemaEntry::DescribeEach(ClientContext &context, const vector<string> &oracle_names) {
	for (auto &oracle_name : oracle_names) {
		try {
			DescribeTable(context, oracle_name);
		} catch (std::exception &) {
			// An invalid view, or one the user may not read, cannot be
			// described or queried, so listings leave it out.
		}
	}
}

void OrkideaSchemaEntry::DescribeTables(ClientContext &context, const vector<string> &oracle_names) {
	for (idx_t start = 0; start < oracle_names.size(); start += MAX_BATCH_TABLES) {
		auto end = MinValue<idx_t>(start + MAX_BATCH_TABLES, oracle_names.size());
		DescribeBatch(context, vector<string>(oracle_names.begin() + NumericCast<int64_t>(start),
		                                      oracle_names.begin() + NumericCast<int64_t>(end)));
	}
}

void OrkideaSchemaEntry::DescribeBatch(ClientContext &context, const vector<string> &oracle_names) {
	if (oracle_names.size() < 2) {
		DescribeEach(context, oracle_names);
		return;
	}
	// One parse, and one round trip, for every table of the batch: a query
	// over all of them that returns no rows, with a marker column between
	// the columns of one table and the next. The hint spares Oracle from
	// choosing a join order.
	string select_list;
	string from;
	for (idx_t i = 0; i < oracle_names.size(); i++) {
		auto alias = "t" + to_string(i);
		if (i > 0) {
			select_list += ", CAST(NULL AS NUMBER(1)) AS " + QuoteIdentifier(BatchMarker(i)) + ", ";
			from += " CROSS JOIN ";
		}
		select_list += alias + ".*";
		from += QuoteIdentifier(name.GetIdentifierName()) + "." + QuoteIdentifier(oracle_names[i]) + " " + alias;
	}
	auto sql = "SELECT /*+ ORDERED */ " + select_list + " FROM " + from + " WHERE 1 = 0";

	// Each table's columns, as offsets into the described schema.
	ArrowSchemaWrapper described;
	vector<pair<idx_t, idx_t>> ranges;
	try {
		Statement statement(OrkideaTransaction::Get(context, ParentCatalog()).GetConnection());
		statement.SetSqlQuery(sql);
		statement.ExecuteSchema(described.arrow_schema);
		auto &schema = described.arrow_schema;
		idx_t start = 0;
		for (idx_t i = 0; i < NumericCast<idx_t>(schema.n_children); i++) {
			auto column = schema.children[i]->name;
			if (column && BatchMarker(ranges.size() + 1) == column) {
				ranges.emplace_back(start, i - start);
				start = i + 1;
			}
		}
		ranges.emplace_back(start, NumericCast<idx_t>(schema.n_children) - start);
	} catch (std::exception &) {
		// A table that cannot be described, or more columns than a query may
		// have, fails the batch: each half is tried on its own.
		ranges.clear();
	}
	if (ranges.size() != oracle_names.size()) {
		auto half = oracle_names.size() / 2;
		DescribeBatch(context, vector<string>(oracle_names.begin(), oracle_names.begin() + NumericCast<int64_t>(half)));
		DescribeBatch(context, vector<string>(oracle_names.begin() + NumericCast<int64_t>(half), oracle_names.end()));
		return;
	}
	for (idx_t i = 0; i < oracle_names.size(); i++) {
		try {
			AddTable(context, oracle_names[i], described.arrow_schema, ranges[i].first, ranges[i].second);
		} catch (std::exception &) {
			// A type DuckDB cannot read: left out of listings, as when the
			// table is described alone.
		}
	}
}

bool OrkideaSchemaEntry::ListedKeys(ClientContext &context, const string &oracle_name, vector<vector<Value>> &rows) {
	auto covered = [&]() {
		if (!keys_cover.count(oracle_name)) {
			return false;
		}
		auto found = listed_keys.find(oracle_name);
		if (found != listed_keys.end()) {
			rows = found->second;
		}
		return true;
	};
	{
		lock_guard<mutex> guard(lock);
		if (!tables_listed) {
			return false;
		}
		if (keys_listed) {
			return covered();
		}
	}
	// A listing reports every table's keys (duckdb_tables), so they are read
	// for all the schema's tables at once, without the lock.
	auto connection = OrkideaTransaction::Get(context, ParentCatalog()).GetConnection();
	auto result = QueryRows(context, connection,
	                        "SELECT c.table_name, c.constraint_name, c.constraint_type, cc.column_name"
	                        " FROM all_constraints c JOIN all_cons_columns cc"
	                        "   ON cc.owner = c.owner AND cc.constraint_name = c.constraint_name"
	                        "  AND cc.table_name = c.table_name"
	                        " WHERE c.owner = " +
	                            QuoteLiteral(name.GetIdentifierName()) +
	                            " AND c.constraint_type IN ('P', 'U') AND c.status = 'ENABLED'"
	                            " ORDER BY c.table_name, c.constraint_name, cc.position");
	lock_guard<mutex> guard(lock);
	if (!keys_listed) {
		for (auto &table : table_names) {
			keys_cover.insert(table.first);
		}
		for (auto &row : result) {
			listed_keys[row[0].ToString()].push_back({row[1], row[2], row[3]});
		}
		keys_listed = true;
	}
	return covered();
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                           const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	if (!transaction.context) {
		lock_guard<mutex> guard(lock);
		auto cached = tables.find(lookup_info.GetEntryName());
		return cached == tables.end() ? nullptr : cached->second.get();
	}
	return FindTable(*transaction.context, lookup_info.GetEntryName()).get();
}

void OrkideaSchemaEntry::Scan(ClientContext &context, CatalogType type,
                              const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	ListTables(context);
	vector<string> undescribed;
	{
		lock_guard<mutex> guard(lock);
		for (auto &table : table_names) {
			if (!tables.count(table.first)) {
				undescribed.push_back(table.first);
			}
		}
	}
	std::sort(undescribed.begin(), undescribed.end());
	DescribeTables(context, undescribed);

	vector<reference<CatalogEntry>> entries;
	{
		lock_guard<mutex> guard(lock);
		for (auto &table : table_names) {
			auto described = tables.find(table.first);
			if (described != tables.end()) {
				entries.push_back(*described->second);
			}
		}
	}
	for (auto &entry : entries) {
		callback(entry.get());
	}
}

void OrkideaSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	vector<reference<CatalogEntry>> entries;
	{
		lock_guard<mutex> guard(lock);
		for (auto &table : tables) {
			entries.push_back(*table.second);
		}
	}
	for (auto &entry : entries) {
		callback(entry.get());
	}
}

namespace {

//! Refuses what a CREATE TABLE in Oracle would not carry over.
void CheckCreatable(const CreateTableInfo &info) {
	for (auto &column : info.columns.Logical()) {
		if (column.Generated()) {
			throw NotImplementedException("Generated columns are not supported in Oracle tables created from DuckDB; "
			                              "create the table with orkidea_execute");
		}
		if (column.HasDefaultValue()) {
			throw NotImplementedException("Column defaults are not supported in Oracle tables created from DuckDB; "
			                              "create the table with orkidea_execute");
		}
	}
	for (auto &constraint : info.constraints) {
		if (constraint->type != ConstraintType::NOT_NULL && constraint->type != ConstraintType::UNIQUE) {
			throw NotImplementedException("Only NOT NULL, PRIMARY KEY, and UNIQUE constraints are supported in Oracle "
			                              "tables created from DuckDB; create the table with orkidea_execute");
		}
	}
}

//! The ALTER TABLE statements that add the table's constraints, with column
//! names as Oracle stores them.
vector<string> ConstraintStatements(const CreateTableInfo &info, const OracleTable &table) {
	vector<string> statements;
	auto prefix = "ALTER TABLE " + table.QualifiedName();
	for (auto &constraint : info.constraints) {
		if (constraint->type == ConstraintType::NOT_NULL) {
			auto &not_null = constraint->Cast<NotNullConstraint>();
			statements.push_back(prefix + " MODIFY (" + QuoteIdentifier(table.column_names[not_null.index.index]) +
			                     " NOT NULL)");
			continue;
		}
		auto &unique = constraint->Cast<UniqueConstraint>();
		vector<string> columns;
		for (auto &index : unique.GetLogicalIndexes(info.columns)) {
			columns.push_back(QuoteIdentifier(table.column_names[index.index]));
		}
		statements.push_back(prefix + (unique.IsPrimaryKey() ? " ADD PRIMARY KEY (" : " ADD UNIQUE (") +
		                     StringUtil::Join(columns, ", ") + ")");
	}
	return statements;
}

} // namespace

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                           BoundCreateTableInfo &bound_info) {
	auto &context = transaction.GetContext();
	auto &info = bound_info.Base();
	auto table_name = info.GetTableName().GetIdentifierName();
	CheckCreatable(info);

	lock_guard<mutex> guard(ddl_lock);
	auto existing = FindTable(context, table_name);
	if (existing) {
		switch (info.on_conflict) {
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return existing.get();
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			DropTable(context, *existing, false);
			break;
		default:
			throw CatalogException("Table \"%s\" already exists in Oracle schema %s", table_name,
			                       name.GetIdentifierName());
		}
	}

	// The driver creates the table from an Arrow schema, with the type
	// mapping it documents. DDL commits, so it runs on a session of its own.
	vector<LogicalType> types;
	vector<string> names;
	for (auto &column : info.columns.Physical()) {
		types.push_back(column.Type());
		names.push_back(column.GetName().GetIdentifierName());
	}
	ColumnDataCollection no_rows(context, types);
	ArrowArrayStream stream;
	CollectionStream(context, no_rows, std::move(names), stream);
	auto ddl = GetOrkideaCatalog().AcquireConnection();
	Statement create(ddl);
	create.SetOption(ADBC_INGEST_OPTION_TARGET_TABLE, table_name);
	create.SetOption("adbc.ingest.target_db_schema", QuoteIdentifier(name.GetIdentifierName()));
	create.SetOption(ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_CREATE);
	create.BindStream(stream);
	create.ExecuteUpdate();

	auto created = FindTable(context, table_name);
	if (!created) {
		throw InternalException("Oracle table %s was created but cannot be found", table_name);
	}
	auto &oracle_table = created->GetOracleTable();
	OrkideaTransaction::Get(context, ParentCatalog()).AddCreatedTable(oracle_table.schema, oracle_table.name);
	try {
		for (auto &statement : ConstraintStatements(info, oracle_table)) {
			ExecuteStatement(context, ddl, statement);
		}
	} catch (...) {
		DropTable(context, *created, true);
		throw;
	}
	return created.get();
}

void OrkideaSchemaEntry::DropTable(ClientContext &context, OrkideaTableEntry &table, bool purge) {
	auto oracle_name = table.GetOracleTable().name;
	ExecuteStatement(context, GetOrkideaCatalog().AcquireConnection(),
	                 "DROP TABLE " + table.GetOracleTable().QualifiedName() + (purge ? " PURGE" : ""));
	Retire(oracle_name);
}

void OrkideaSchemaEntry::ForgetTable(const string &oracle_name) {
	Retire(oracle_name);
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	ThrowNotSupported("Creating functions");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) {
	ThrowNotSupported("Creating indexes");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	ThrowNotSupported("Creating views");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	ThrowNotSupported("Creating sequences");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	ThrowNotSupported("Creating table functions");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	ThrowNotSupported("Creating copy functions");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	ThrowNotSupported("Creating pragma functions");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	ThrowNotSupported("Creating collations");
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	ThrowNotSupported("Creating types");
}

void OrkideaSchemaEntry::Retire(const string &oracle_name) {
	lock_guard<mutex> guard(lock);
	keys_cover.erase(oracle_name);
	auto entry = tables.find(oracle_name);
	if (entry != tables.end()) {
		// DuckDB may still hold the entry for the statement that runs; it
		// stays alive until the database is detached.
		retired.push_back(std::move(entry->second));
		tables.erase(entry);
	}
	table_names.erase(oracle_name);
}

namespace {

//! The Oracle name of a table column, given its DuckDB name. Empty when there
//! is no such column.
string OracleColumnName(OrkideaTableEntry &table, const Identifier &name) {
	if (!table.ColumnExists(name)) {
		return string();
	}
	auto lookup = name;
	auto index = table.GetColumnIndex(lookup).index;
	return table.GetOracleTable().column_names[index];
}

string RequireColumn(OrkideaTableEntry &table, const Identifier &name) {
	auto column = OracleColumnName(table, name);
	if (column.empty()) {
		throw CatalogException("Table %s has no column \"%s\"", table.GetOracleTable().QualifiedName(),
		                       name.GetIdentifierName());
	}
	return QuoteIdentifier(column);
}

//! An Oracle literal for a constant DEFAULT; only numbers, strings, and
//! booleans, which read the same in both.
string DefaultLiteral(const ParsedExpression &expression) {
	if (expression.GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw NotImplementedException("Only constant defaults can be set on Oracle columns from DuckDB; use "
		                              "orkidea_execute");
	}
	auto value = expression.Cast<ConstantExpression>().GetLiteral().ToValue();
	if (value.IsNull()) {
		return "NULL";
	}
	if (value.type().IsNumeric()) {
		return value.ToString();
	}
	switch (value.type().id()) {
	case LogicalTypeId::VARCHAR:
		return QuoteLiteral(StringValue::Get(value));
	case LogicalTypeId::BOOLEAN:
		return BooleanValue::Get(value) ? "TRUE" : "FALSE";
	default:
		throw NotImplementedException("Only number, string, and boolean defaults can be set on Oracle columns from "
		                              "DuckDB; use orkidea_execute");
	}
}

} // namespace

void OrkideaSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_TABLE) {
		ThrowNotSupported("Altering anything but tables");
	}
	auto &alter = info.Cast<AlterTableInfo>();
	auto &context = transaction.GetContext();
	auto table_name = info.GetQualifiedName().Name().GetIdentifierName();
	lock_guard<mutex> guard(ddl_lock);
	auto table = FindTable(context, table_name);
	if (!table) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table \"%s\" does not exist in Oracle schema %s", table_name, name.GetIdentifierName());
	}
	auto &oracle_table = table->GetOracleTable();
	auto native_boolean = GetOrkideaCatalog().GetDatabaseInfo().major_version >= 23;
	auto prefix = "ALTER TABLE " + oracle_table.QualifiedName();
	string sql;
	switch (alter.alter_table_type) {
	case AlterTableType::RENAME_TABLE:
		sql = prefix + " RENAME TO " + NewOracleName(alter.Cast<RenameTableInfo>().new_table_name.GetIdentifierName());
		break;
	case AlterTableType::RENAME_COLUMN: {
		auto &rename = alter.Cast<RenameColumnInfo>();
		sql = prefix + " RENAME COLUMN " + RequireColumn(*table, rename.old_name) + " TO " +
		      NewOracleName(rename.new_name.GetIdentifierName());
		break;
	}
	case AlterTableType::ADD_COLUMN: {
		auto &add = alter.Cast<AddColumnInfo>();
		auto &column = add.new_column;
		if (!OracleColumnName(*table, column.GetName()).empty()) {
			if (add.if_column_not_exists) {
				return;
			}
			throw CatalogException("Table %s already has a column \"%s\"", oracle_table.QualifiedName(),
			                       column.GetName().GetIdentifierName());
		}
		if (column.Generated()) {
			throw NotImplementedException("Generated columns cannot be added to Oracle tables from DuckDB; use "
			                              "orkidea_execute");
		}
		sql = prefix + " ADD (" + NewOracleName(column.GetName().GetIdentifierName()) + " " +
		      OracleColumnType(column.Type(), native_boolean);
		if (column.HasDefaultValue()) {
			sql += " DEFAULT " + DefaultLiteral(column.DefaultValue());
		}
		sql += ")";
		break;
	}
	case AlterTableType::REMOVE_COLUMN: {
		auto &remove = alter.Cast<RemoveColumnInfo>();
		auto column = OracleColumnName(*table, remove.removed_column);
		if (column.empty() && remove.if_column_exists) {
			return;
		}
		sql = prefix + " DROP COLUMN " + RequireColumn(*table, remove.removed_column) +
		      (remove.cascade ? " CASCADE CONSTRAINTS" : "");
		break;
	}
	case AlterTableType::ALTER_COLUMN_TYPE: {
		auto &change = alter.Cast<ChangeColumnTypeInfo>();
		if (change.column_path.size() != 1) {
			ThrowNotSupported("Changing the type of a nested field");
		}
		if (change.expression && change.expression->GetExpressionClass() != ExpressionClass::COLUMN_REF &&
		    change.expression->GetExpressionClass() != ExpressionClass::CAST) {
			throw NotImplementedException(
			    "ALTER ... TYPE ... USING is not supported for Oracle tables; Oracle converts "
			    "the column itself");
		}
		sql = prefix + " MODIFY (" + RequireColumn(*table, change.column_path[0]) + " " +
		      OracleColumnType(change.target_type, native_boolean) + ")";
		break;
	}
	case AlterTableType::SET_DEFAULT: {
		auto &set_default = alter.Cast<SetDefaultInfo>();
		sql = prefix + " MODIFY (" + RequireColumn(*table, set_default.column_path[0]) + " DEFAULT " +
		      (set_default.expression ? DefaultLiteral(*set_default.expression) : "NULL") + ")";
		break;
	}
	case AlterTableType::SET_NOT_NULL:
		sql = prefix + " MODIFY (" + RequireColumn(*table, alter.Cast<SetNotNullInfo>().column_path[0]) + " NOT NULL)";
		break;
	case AlterTableType::DROP_NOT_NULL:
		sql = prefix + " MODIFY (" + RequireColumn(*table, alter.Cast<DropNotNullInfo>().column_path[0]) + " NULL)";
		break;
	case AlterTableType::ADD_CONSTRAINT: {
		auto &constraint = *alter.Cast<AddConstraintInfo>().constraint;
		if (constraint.type != ConstraintType::UNIQUE) {
			ThrowNotSupported("Adding constraints other than PRIMARY KEY and UNIQUE");
		}
		auto &unique = constraint.Cast<UniqueConstraint>();
		vector<string> columns;
		for (auto &index : unique.GetLogicalIndexes(table->GetColumns())) {
			columns.push_back(QuoteIdentifier(oracle_table.column_names[index.index]));
		}
		sql = prefix + (unique.IsPrimaryKey() ? " ADD PRIMARY KEY (" : " ADD UNIQUE (") +
		      StringUtil::Join(columns, ", ") + ")";
		break;
	}
	default:
		ThrowNotSupported("This ALTER TABLE");
	}
	// Oracle commits DDL, so it runs on a session of its own. The table is
	// described again when it is next used.
	auto oracle_name = oracle_table.name;
	ExecuteStatement(context, GetOrkideaCatalog().AcquireConnection(), sql);
	Retire(oracle_name);
}

void OrkideaSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.type != CatalogType::TABLE_ENTRY) {
		ThrowNotSupported("Dropping anything but tables");
	}
	auto table_name = info.GetQualifiedName().Name().GetIdentifierName();
	lock_guard<mutex> guard(ddl_lock);
	auto table = FindTable(context, table_name);
	if (!table) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table \"%s\" does not exist in Oracle schema %s", table_name, name.GetIdentifierName());
	}
	// Not purged: a dropped table stays in Oracle's recycle bin, from which
	// FLASHBACK TABLE ... TO BEFORE DROP restores it.
	auto oracle_name = table->GetOracleTable().name;
	auto sql = "DROP TABLE " + table->GetOracleTable().QualifiedName() + (info.cascade ? " CASCADE CONSTRAINTS" : "");
	ExecuteStatement(context, GetOrkideaCatalog().AcquireConnection(), sql);
	Retire(oracle_name);
}

} // namespace orkidea
} // namespace duckdb
