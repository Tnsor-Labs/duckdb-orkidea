#include "orkidea_optimizer.hpp"

#include "orkidea_filter.hpp"
#include "orkidea_scan.hpp"
#include "storage/orkidea_catalog.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! An Oracle scan under a chain of projections, which keep every row.
struct ScanBelow {
	optional_ptr<LogicalGet> get;
	vector<reference<LogicalProjection>> projections;
};

ScanBelow FindScan(LogicalOperator &op) {
	ScanBelow result;
	reference<LogicalOperator> current = op;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		result.projections.push_back(current.get().Cast<LogicalProjection>());
		current = *current.get().children[0];
	}
	if (current.get().type != LogicalOperatorType::LOGICAL_GET) {
		return result;
	}
	auto &get = current.get().Cast<LogicalGet>();
	if (get.function.GetName() != TableScan::NAME || !get.bind_data) {
		return result;
	}
	// Fetching only the first rows is right only if every filter is applied
	// in Oracle: a row DuckDB filters out afterwards would leave fewer.
	if (TableScan::HasLocalFilters(get)) {
		return result;
	}
	result.get = get;
	return result;
}

//! The table column an ORDER BY key reads, followed through the projections.
optional_idx ResolveColumn(const Expression &expr, const ScanBelow &scan) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return optional_idx();
	}
	auto binding = expr.Cast<BoundColumnRefExpression>().Binding();
	for (auto &projection_ref : scan.projections) {
		auto &projection = projection_ref.get();
		if (binding.table_index != projection.table_index) {
			return optional_idx();
		}
		auto &projected = *projection.expressions[binding.column_index.GetIndex()];
		if (projected.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return optional_idx();
		}
		binding = projected.Cast<BoundColumnRefExpression>().Binding();
	}
	if (binding.table_index != scan.get->table_index) {
		return optional_idx();
	}
	auto &column = scan.get->GetColumnIds()[binding.column_index.GetIndex()];
	if (column.IsVirtualColumn()) {
		return optional_idx();
	}
	return column.GetPrimaryIndex();
}

//! limit + offset, or invalid when it overflows.
optional_idx RowsNeeded(idx_t limit, idx_t offset) {
	if (limit > NumericLimits<idx_t>::Maximum() - offset) {
		return optional_idx();
	}
	return limit + offset;
}

void PushLimit(LogicalLimit &limit) {
	if (limit.limit_val.Type() != LimitNodeType::CONSTANT_VALUE) {
		return;
	}
	idx_t offset = 0;
	switch (limit.offset_val.Type()) {
	case LimitNodeType::UNSET:
		break;
	case LimitNodeType::CONSTANT_VALUE:
		offset = limit.offset_val.GetConstantValue();
		break;
	default:
		return;
	}
	auto scan = FindScan(*limit.children[0]);
	if (!scan.get) {
		return;
	}
	auto &bind_data = scan.get->bind_data->Cast<TableScanBindData>();
	if (!bind_data.order_by.empty() || bind_data.row_limit.IsValid()) {
		return;
	}
	bind_data.row_limit = RowsNeeded(limit.limit_val.GetConstantValue(), offset);
}

void PushTopN(LogicalTopN &top_n) {
	auto scan = FindScan(*top_n.children[0]);
	if (!scan.get) {
		return;
	}
	auto &bind_data = scan.get->bind_data->Cast<TableScanBindData>();
	if (!bind_data.order_by.empty() || bind_data.row_limit.IsValid()) {
		return;
	}
	auto utf8_byte_order = bind_data.catalog.GetDatabaseInfo().utf8_byte_order;
	vector<ScanOrder> order_by;
	for (auto &order : top_n.orders) {
		auto column = ResolveColumn(*order.expression, scan);
		if (!column.IsValid() || !CanOrderColumn(bind_data.table->oracle_types[column.GetIndex()], utf8_byte_order)) {
			return;
		}
		bool descending;
		switch (order.type) {
		case OrderType::ASCENDING:
			descending = false;
			break;
		case OrderType::DESCENDING:
			descending = true;
			break;
		default:
			return;
		}
		bool nulls_first;
		switch (order.null_order) {
		case OrderByNullType::NULLS_FIRST:
			nulls_first = true;
			break;
		case OrderByNullType::NULLS_LAST:
			nulls_first = false;
			break;
		default:
			return;
		}
		order_by.push_back(ScanOrder {column.GetIndex(), descending, nulls_first});
	}
	auto rows = RowsNeeded(top_n.limit, top_n.offset);
	if (!rows.IsValid()) {
		return;
	}
	bind_data.order_by = std::move(order_by);
	bind_data.row_limit = rows;
}

void Visit(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_LIMIT:
		PushLimit(op.Cast<LogicalLimit>());
		break;
	case LogicalOperatorType::LOGICAL_TOP_N:
		PushTopN(op.Cast<LogicalTopN>());
		break;
	default:
		break;
	}
	for (auto &child : op.children) {
		Visit(*child);
	}
}

void Optimize(OptimizerExtensionInput &, unique_ptr<LogicalOperator> &plan) {
	Visit(*plan);
}

} // namespace

void Optimizer::Register(ExtensionLoader &loader) {
	OptimizerExtension extension;
	extension.optimize_function = Optimize;
	OptimizerExtension::Register(DBConfig::GetConfig(loader.GetDatabaseInstance()), std::move(extension));
}

} // namespace orkidea
} // namespace duckdb
