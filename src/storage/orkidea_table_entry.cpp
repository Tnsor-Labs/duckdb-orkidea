#include "storage/orkidea_table_entry.hpp"

#include "orkidea_scan.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"
#include "storage/orkidea_transaction.hpp"

#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {
namespace orkidea {

string OracleTable::QualifiedName() const {
	return QuoteIdentifier(schema) + "." + QuoteIdentifier(name);
}

OrkideaTableEntry::OrkideaTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                     shared_ptr<const OracleTable> table_p)
    : TableCatalogEntry(catalog, schema, info), columns(std::move(info.columns)), table(std::move(table_p)) {
}

const ColumnList &OrkideaTableEntry::GetColumns() const {
	return columns;
}

unique_ptr<BaseStatistics> OrkideaTableEntry::GetStatistics(ClientContext &, column_t) {
	return nullptr;
}

TableFunction OrkideaTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	return TableScan::Function(context, catalog.Cast<OrkideaCatalog>(), *this, table, bind_data);
}

virtual_column_map_t OrkideaTableEntry::GetVirtualColumns() const {
	virtual_column_map_t result;
	if (table->has_rowid) {
		result.insert(make_pair(COLUMN_IDENTIFIER_ROW_ID, TableColumn("rowid", LogicalType::VARCHAR)));
	}
	result.insert(make_pair(COLUMN_IDENTIFIER_EMPTY, TableColumn("", LogicalType::BOOLEAN)));
	return result;
}

vector<column_t> OrkideaTableEntry::GetRowIdColumns() const {
	if (!table->has_rowid) {
		throw NotImplementedException("Oracle object %s has no ROWIDs (it is a view or an index-organized table), so "
		                              "its rows cannot be updated or deleted from DuckDB; use orkidea_execute",
		                              table->QualifiedName());
	}
	return {COLUMN_IDENTIFIER_ROW_ID};
}

void OrkideaTableEntry::BindUpdateConstraints(Binder &, LogicalGet &, LogicalProjection &, LogicalUpdate &,
                                              ClientContext &) {
}

const vector<IndexInfo> &OrkideaTableEntry::GetUniqueKeys(ClientContext &context) {
	lock_guard<mutex> guard(keys_lock);
	if (keys_loaded) {
		return unique_keys;
	}
	auto sql = "SELECT c.constraint_name, c.constraint_type, cc.column_name"
	           " FROM all_constraints c JOIN all_cons_columns cc"
	           "   ON cc.owner = c.owner AND cc.constraint_name = c.constraint_name AND cc.table_name = c.table_name"
	           " WHERE c.owner = " +
	           QuoteLiteral(table->schema) + " AND c.table_name = " + QuoteLiteral(table->name) +
	           "   AND c.constraint_type IN ('P', 'U') AND c.status = 'ENABLED'"
	           " ORDER BY c.constraint_name, cc.position";
	auto connection = OrkideaTransaction::Get(context, catalog).GetConnection();
	map<string, IndexInfo> keys;
	for (auto &row : QueryRows(context, connection, sql)) {
		auto &key = keys[row[0].ToString()];
		key.is_unique = true;
		key.is_primary = row[1].ToString() == "P";
		key.is_foreign = false;
		auto &names = table->column_names;
		auto position = std::find(names.begin(), names.end(), row[2].ToString());
		if (position != names.end()) {
			key.column_set.insert(NumericCast<column_t>(position - names.begin()));
		}
	}
	for (auto &key : keys) {
		unique_keys.push_back(std::move(key.second));
	}
	keys_loaded = true;
	return unique_keys;
}

TableStorageInfo OrkideaTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	if (table->estimated_rows >= 0) {
		result.cardinality = NumericCast<idx_t>(table->estimated_rows);
	}
	result.index_info = GetUniqueKeys(context);
	return result;
}

} // namespace orkidea
} // namespace duckdb
