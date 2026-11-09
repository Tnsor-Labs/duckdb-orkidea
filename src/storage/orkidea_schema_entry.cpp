#include "storage/orkidea_schema_entry.hpp"

#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_table_entry.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/table/arrow.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"

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
	return "SELECT table_name, num_rows, splittable FROM ("
	       " SELECT t.table_name, t.num_rows,"
	       "  CASE WHEN t.iot_type IS NULL AND t.cluster_name IS NULL AND t.temporary = 'N' THEN 'Y' ELSE 'N' END"
	       "  AS splittable FROM all_tables t"
	       "  WHERE t.owner = " +
	       owner_literal +
	       "    AND t.dropped = 'NO' AND t.secondary = 'N' AND t.nested = 'NO'"
	       "    AND (t.iot_type IS NULL OR t.iot_type = 'IOT')"
	       "    AND NOT EXISTS (SELECT 1 FROM all_mviews m WHERE m.owner = t.owner AND m.mview_name = t.table_name)"
	       " UNION ALL SELECT view_name, NULL, 'N' FROM all_views WHERE owner = " +
	       owner_literal + " UNION ALL SELECT mview_name, NULL, 'N' FROM all_mviews WHERE owner = " + owner_literal +
	       ") WHERE 1 = 1";
}

} // namespace

TableListing TableListing::FromRow(const vector<Value> &row) {
	TableListing listing;
	listing.estimated_rows = row[1].IsNull() ? -1 : row[1].GetValue<int64_t>();
	listing.splittable = row[2].ToString() == "Y";
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
	if (tables_listed) {
		return;
	}
	auto connection = OrkideaTransaction::Get(context, ParentCatalog()).GetConnection();
	for (auto &row : QueryRows(context, connection, TablesQuery(name.GetIdentifierName()))) {
		table_names[row[0].ToString()] = TableListing::FromRow(row);
	}
	tables_listed = true;
}

vector<string> OrkideaSchemaEntry::QueryTableNames(ClientContext &context, const string &condition) {
	auto sql = TablesQuery(name.GetIdentifierName()) + " AND " + condition;
	auto connection = OrkideaTransaction::Get(context, ParentCatalog()).GetConnection();
	vector<string> found;
	for (auto &row : QueryRows(context, connection, sql)) {
		table_names[row[0].ToString()] = TableListing::FromRow(row);
		found.push_back(row[0].ToString());
	}
	return found;
}

optional_ptr<OrkideaTableEntry> OrkideaSchemaEntry::FindTable(ClientContext &context, const string &table_name) {
	vector<string> known;
	for (auto &entry : table_names) {
		known.push_back(entry.first);
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
	auto cached = tables.find(oracle_name);
	if (cached != tables.end()) {
		return cached->second.get();
	}
	auto table = make_shared_ptr<OracleTable>();
	table->schema = name.GetIdentifierName();
	table->name = oracle_name;
	auto known = table_names.find(oracle_name);
	if (known != table_names.end()) {
		table->estimated_rows = known->second.estimated_rows;
		table->splittable = known->second.splittable;
	}

	auto connection = OrkideaTransaction::Get(context, ParentCatalog()).GetConnection();
	Statement statement(connection);
	statement.SetSqlQuery("SELECT * FROM " + table->QualifiedName());
	statement.ExecuteSchema(table->arrow_schema.arrow_schema);

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
		create_info.columns.AddColumn(ColumnDefinition(Identifier(duckdb_name), types[i]));
	}

	auto entry = make_uniq<OrkideaTableEntry>(ParentCatalog(), *this, create_info, std::move(table));
	auto &result = *entry;
	tables.emplace(oracle_name, std::move(entry));
	return &result;
}

optional_ptr<CatalogEntry> OrkideaSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                           const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	lock_guard<mutex> guard(lock);
	if (!transaction.context) {
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
	vector<reference<CatalogEntry>> entries;
	{
		lock_guard<mutex> guard(lock);
		ListTables(context);
		for (auto &table : table_names) {
			try {
				entries.push_back(*DescribeTable(context, table.first));
			} catch (std::exception &) {
				// An invalid view, or one the user may not read, cannot be
				// described or queried, so listings leave it out.
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

optional_ptr<CatalogEntry> OrkideaSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	ThrowNotSupported("Creating tables");
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

void OrkideaSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	ThrowNotSupported("Altering tables");
}

void OrkideaSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	ThrowNotSupported("Dropping tables");
}

} // namespace orkidea
} // namespace duckdb
