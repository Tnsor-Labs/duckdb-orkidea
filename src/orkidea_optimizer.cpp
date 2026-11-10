#include "orkidea_optimizer.hpp"

#include "orkidea_filter.hpp"
#include "orkidea_remote_query.hpp"
#include "orkidea_scan.hpp"
#include "orkidea_util.hpp"
#include "storage/orkidea_catalog.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
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

constexpr const char *AGGREGATE_PUSHDOWN_SETTING = "orkidea_aggregate_pushdown";

//! The Oracle type to cast an aggregate's result to so that it arrives as
//! DuckDB's result type, or exactly: Oracle's NUMBER results have no declared
//! precision, which the driver reads as DOUBLE. Empty for no cast.
string ResultCast(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DECIMAL:
		return "NUMBER(" + to_string(DecimalType::GetWidth(type)) + "," + to_string(DecimalType::GetScale(type)) + ")";
	case LogicalTypeId::HUGEINT:
		return "NUMBER(38)";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
		return "NUMBER(18)";
	case LogicalTypeId::DOUBLE:
		return "BINARY_DOUBLE";
	case LogicalTypeId::FLOAT:
		return "BINARY_FLOAT";
	default:
		return string();
	}
}

//! How finely a timestamp type keeps time, or invalid for other types.
optional_idx TimestampPrecision(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIMESTAMP_SEC:
		return 0;
	case LogicalTypeId::TIMESTAMP_MS:
		return 3;
	case LogicalTypeId::TIMESTAMP:
		return 6;
	case LogicalTypeId::TIMESTAMP_NS:
		return 9;
	default:
		return optional_idx();
	}
}

//! Whether a value Oracle returns as `actual` converts to `expected` without
//! losing anything the DuckDB aggregate would have kept.
bool CanConvert(const LogicalType &actual, const LogicalType &expected) {
	if (actual == expected) {
		return true;
	}
	if (actual.IsNumeric() && expected.IsNumeric()) {
		auto approximate = [](const LogicalType &type) {
			return type.id() == LogicalTypeId::FLOAT || type.id() == LogicalTypeId::DOUBLE;
		};
		return !approximate(actual) || approximate(expected);
	}
	auto actual_precision = TimestampPrecision(actual);
	auto expected_precision = TimestampPrecision(expected);
	return actual_precision.IsValid() && expected_precision.IsValid() &&
	       actual_precision.GetIndex() <= expected_precision.GetIndex();
}

//! Builds `SELECT <groups>, <aggregates> FROM <table> WHERE ... GROUP BY
//! <groups>` over an Oracle scan. Every group key and argument must be a
//! column, and every aggregate one Oracle computes as DuckDB does.
class AggregateQuery {
public:
	explicit AggregateQuery(const ScanBelow &scan)
	    : scan(scan), bind_data(scan.get->bind_data->Cast<TableScanBindData>()), table(*bind_data.table) {
	}

	//! Adds a group key; false when Oracle cannot group by it.
	bool AddGroup(const Expression &expr) {
		auto column = ResolveColumn(expr, scan);
		if (!column.IsValid() || !CanGroupBy(column.GetIndex())) {
			return false;
		}
		auto name = QuoteIdentifier(table.column_names[column.GetIndex()]);
		select_list.push_back(name);
		group_by.push_back(std::move(name));
		return true;
	}

	//! Adds an aggregate; false when Oracle cannot compute it as DuckDB does.
	bool AddAggregate(const Expression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto sql = Aggregate(expr.Cast<BoundAggregateExpression>());
		if (sql.empty()) {
			return false;
		}
		select_list.push_back(std::move(sql));
		return true;
	}

	string Build() const {
		auto sql = "SELECT " + StringUtil::Join(select_list, ", ") + " FROM " + table.QualifiedName();
		auto conditions = TableScan::Conditions(*scan.get);
		if (!conditions.empty()) {
			sql += " WHERE " + StringUtil::Join(conditions, " AND ");
		}
		if (!group_by.empty()) {
			sql += " GROUP BY " + StringUtil::Join(group_by, ", ");
		}
		return sql;
	}

private:
	const LogicalType &DuckType(column_t column) const {
		return bind_data.all_types[column];
	}

	//! Whether Oracle groups (and DISTINCT compares) the column's values as
	//! DuckDB groups the values it reads. A NUMBER read as DOUBLE is left
	//! out: distinct numbers can round to one double.
	bool CanGroupBy(column_t column) const {
		auto &oracle_type = table.oracle_types[column];
		if (!CanGroupColumn(oracle_type)) {
			return false;
		}
		auto &type = DuckType(column);
		bool approximate = type.id() == LogicalTypeId::FLOAT || type.id() == LogicalTypeId::DOUBLE;
		return !approximate || StringUtil::StartsWith(StringUtil::Upper(oracle_type), "BINARY_");
	}

	string Aggregate(const BoundAggregateExpression &expr) const {
		if (expr.GetFilter() || expr.GetOrderBys()) {
			return string();
		}
		auto &name = expr.Function().GetName();
		auto &children = expr.GetChildren();
		if (name == "count_star") {
			return children.empty() ? Cast("COUNT(*)", expr.GetReturnType()) : string();
		}
		if (children.size() != 1) {
			return string();
		}
		auto column = ResolveColumn(*children[0], scan);
		if (!column.IsValid()) {
			return string();
		}
		auto &oracle_type = table.oracle_types[column.GetIndex()];
		const auto argument = QuoteIdentifier(table.column_names[column.GetIndex()]);
		bool distinct = expr.IsDistinct();
		if (distinct && !CanGroupBy(column.GetIndex())) {
			return string();
		}
		string function;
		if (name == "count") {
			function = "COUNT";
		} else if (name == "min" || name == "max") {
			if (!CanOrderColumn(oracle_type, bind_data.catalog.GetDatabaseInfo().utf8_byte_order)) {
				return string();
			}
			function = name == "min" ? "MIN" : "MAX";
			// MIN and MAX of one value are that value.
			distinct = false;
		} else if (name == "sum" || name == "sum_no_overflow" || name == "avg") {
			if (!IsNumericColumn(oracle_type) || !DuckType(column.GetIndex()).IsNumeric()) {
				return string();
			}
			function = name == "avg" ? "AVG" : "SUM";
		} else {
			return string();
		}
		return Cast(function + "(" + (distinct ? "DISTINCT " : "") + argument + ")", expr.GetReturnType());
	}

	static string Cast(const string &sql, const LogicalType &type) {
		auto cast = ResultCast(type);
		return cast.empty() ? sql : "CAST(" + sql + " AS " + cast + ")";
	}

	const ScanBelow &scan;
	TableScanBindData &bind_data;
	const OracleTable &table;
	vector<string> select_list;
	vector<string> group_by;
};

//! The scan below `op`, if it can be replaced by a query of its own.
ScanBelow FindReplaceableScan(LogicalOperator &op) {
	auto scan = FindScan(op);
	if (scan.get) {
		auto &bind_data = scan.get->bind_data->Cast<TableScanBindData>();
		if (bind_data.row_limit.IsValid() || !bind_data.order_by.empty() || bind_data.for_update) {
			scan.get = nullptr;
		}
	}
	return scan;
}

//! Replaces `op`, whose output columns are `old_bindings` of types
//! `expected`, with a scan of `query` under a projection that casts its
//! columns to those types. The replaced bindings are recorded in `replacer`.
//! Leaves the plan alone, returning false, when Oracle rejects the query or
//! returns types that do not convert exactly.
bool ReplaceWithQuery(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, const ScanBelow &scan,
                      const AggregateQuery &query, const vector<LogicalType> &expected,
                      const vector<ColumnBinding> &old_bindings, ColumnBindingReplacer &replacer) {
	auto &catalog = scan.get->bind_data->Cast<TableScanBindData>().catalog;
	unique_ptr<FunctionData> bind_data;
	vector<LogicalType> types;
	TableFunction function;
	try {
		function = RemoteQuery::Function(input.context, catalog, query.Build(), bind_data, types);
	} catch (std::exception &) {
		// Oracle rejected the query (for example a type its aggregate does
		// not take): DuckDB computes the result instead.
		return false;
	}
	if (types.size() != expected.size() || old_bindings.size() != expected.size()) {
		return false;
	}
	for (idx_t i = 0; i < types.size(); i++) {
		if (!CanConvert(types[i], expected[i])) {
			return false;
		}
	}

	vector<Identifier> names;
	for (idx_t i = 0; i < types.size(); i++) {
		names.emplace_back("c" + to_string(i));
	}
	auto get_index = input.optimizer.binder.GenerateTableIndex();
	auto get =
	    make_uniq<LogicalGet>(get_index, BoundTableFunction(function), std::move(bind_data), types, std::move(names));
	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < types.size(); i++) {
		get->AddColumnId(i);
		unique_ptr<Expression> column =
		    make_uniq<BoundColumnRefExpression>(types[i], ColumnBinding(get_index, ProjectionIndex(i)));
		if (types[i] != expected[i]) {
			column = BoundCastExpression::AddCastToType(input.context, std::move(column), expected[i]);
		}
		select_list.push_back(std::move(column));
	}
	auto projection_index = input.optimizer.binder.GenerateTableIndex();
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(select_list));
	if (op->has_estimated_cardinality) {
		get->SetEstimatedCardinality(op->estimated_cardinality);
		projection->SetEstimatedCardinality(op->estimated_cardinality);
	}
	projection->children.push_back(std::move(get));
	projection->ResolveOperatorTypes();

	for (idx_t i = 0; i < old_bindings.size(); i++) {
		replacer.replacement_bindings.emplace_back(old_bindings[i],
		                                           ColumnBinding(projection_index, ProjectionIndex(i)));
	}
	op = std::move(projection);
	return true;
}

//! GROUP BY and aggregates over an Oracle scan.
bool PushAggregate(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, ColumnBindingReplacer &replacer) {
	auto &aggregate = op->Cast<LogicalAggregate>();
	if (aggregate.grouping_sets.size() > 1 || !aggregate.grouping_functions.empty() ||
	    (aggregate.groups.empty() && aggregate.expressions.empty())) {
		return false;
	}
	auto scan = FindReplaceableScan(*aggregate.children[0]);
	if (!scan.get) {
		return false;
	}
	AggregateQuery query(scan);
	vector<LogicalType> expected;
	for (auto &group : aggregate.groups) {
		if (!query.AddGroup(*group)) {
			return false;
		}
		expected.push_back(group->GetReturnType());
	}
	for (auto &expr : aggregate.expressions) {
		if (!query.AddAggregate(*expr)) {
			return false;
		}
		expected.push_back(expr->GetReturnType());
	}
	return ReplaceWithQuery(input, op, scan, query, expected, aggregate.GetColumnBindings(), replacer);
}

//! SELECT DISTINCT over an Oracle scan: a GROUP BY of every column.
bool PushDistinct(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, ColumnBindingReplacer &replacer) {
	auto &distinct = op->Cast<LogicalDistinct>();
	if (distinct.distinct_type != DistinctType::DISTINCT || distinct.order_by) {
		return false;
	}
	auto &child = *distinct.children[0];
	auto scan = FindReplaceableScan(child);
	if (!scan.get) {
		return false;
	}
	auto bindings = child.GetColumnBindings();
	child.ResolveOperatorTypes();
	AggregateQuery query(scan);
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (!query.AddGroup(BoundColumnRefExpression(child.types[i], bindings[i]))) {
			return false;
		}
	}
	return ReplaceWithQuery(input, op, scan, query, child.types, bindings, replacer);
}

struct PushdownState {
	OptimizerExtensionInput &input;
	bool push_aggregates;
	ColumnBindingReplacer replacer;
};

void Visit(PushdownState &context, unique_ptr<LogicalOperator> &op) {
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_LIMIT:
		PushLimit(op->Cast<LogicalLimit>());
		break;
	case LogicalOperatorType::LOGICAL_TOP_N:
		PushTopN(op->Cast<LogicalTopN>());
		break;
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		if (context.push_aggregates && PushAggregate(context.input, op, context.replacer)) {
			return;
		}
		break;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		if (context.push_aggregates && PushDistinct(context.input, op, context.replacer)) {
			return;
		}
		break;
	default:
		break;
	}
	for (auto &child : op->children) {
		Visit(context, child);
	}
}

void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	Value setting;
	bool push_aggregates = true;
	if (input.context.TryGetCurrentSetting(AGGREGATE_PUSHDOWN_SETTING, setting)) {
		push_aggregates = BooleanValue::Get(setting);
	}
	PushdownState context {input, push_aggregates, ColumnBindingReplacer()};
	Visit(context, plan);
	if (!context.replacer.replacement_bindings.empty()) {
		context.replacer.VisitOperator(*plan);
	}
}

} // namespace

void Optimizer::Register(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(AGGREGATE_PUSHDOWN_SETTING,
	                          "Whether GROUP BY and aggregates (COUNT, SUM, MIN, MAX, AVG) directly over an attached "
	                          "Oracle table run in Oracle",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	OptimizerExtension extension;
	extension.optimize_function = Optimize;
	OptimizerExtension::Register(config, std::move(extension));
}

} // namespace orkidea
} // namespace duckdb
