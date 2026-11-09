#include "storage/orkidea_table_entry.hpp"

#include "orkidea_scan.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"

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
	return TableScan::Function(context, catalog.Cast<OrkideaCatalog>(), table, bind_data);
}

TableStorageInfo OrkideaTableEntry::GetStorageInfo(ClientContext &) {
	TableStorageInfo result;
	if (table->estimated_rows >= 0) {
		result.cardinality = NumericCast<idx_t>(table->estimated_rows);
	}
	return result;
}

} // namespace orkidea
} // namespace duckdb
