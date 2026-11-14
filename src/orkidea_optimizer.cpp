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
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
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

//! A column a query over Oracle reads: its SQL, its Oracle type as the
//! driver reports it, and the DuckDB type it is read as.
struct QueryColumn {
	string sql;
	string oracle_type;
	LogicalType type;
};

//! The column an expression is, if it is one the query can read.
using ColumnResolver = std::function<bool(const Expression &expr, QueryColumn &column)>;

//! Builds `SELECT <groups>, <aggregates> FROM <from> GROUP BY <groups>`, over
//! a table or a join. Every group key and argument must be a column, and
//! every aggregate one Oracle computes as DuckDB does.
class AggregateQuery {
public:
	AggregateQuery(ColumnResolver resolve, string from, bool utf8_byte_order)
	    : resolve(std::move(resolve)), from(std::move(from)), utf8_byte_order(utf8_byte_order) {
	}

	//! Adds a group key; false when Oracle cannot group by it.
	bool AddGroup(const Expression &expr) {
		QueryColumn column;
		if (!resolve(expr, column) || !CanGroupBy(column)) {
			return false;
		}
		select_list.push_back(column.sql);
		group_by.push_back(column.sql);
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
		auto sql = "SELECT " + StringUtil::Join(select_list, ", ") + " FROM " + from;
		if (!group_by.empty()) {
			sql += " GROUP BY " + StringUtil::Join(group_by, ", ");
		}
		return sql;
	}

private:
	//! Whether Oracle groups (and DISTINCT compares) the column's values as
	//! DuckDB groups the values it reads. A NUMBER read as DOUBLE is left
	//! out: distinct numbers can round to one double.
	static bool CanGroupBy(const QueryColumn &column) {
		if (!CanGroupColumn(column.oracle_type)) {
			return false;
		}
		bool approximate = column.type.id() == LogicalTypeId::FLOAT || column.type.id() == LogicalTypeId::DOUBLE;
		return !approximate || StringUtil::StartsWith(StringUtil::Upper(column.oracle_type), "BINARY_");
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
		QueryColumn column;
		if (!resolve(*children[0], column)) {
			return string();
		}
		bool distinct = expr.IsDistinct();
		if (distinct && !CanGroupBy(column)) {
			return string();
		}
		string function;
		if (name == "count") {
			function = "COUNT";
		} else if (name == "min" || name == "max") {
			if (!CanOrderColumn(column.oracle_type, utf8_byte_order)) {
				return string();
			}
			function = name == "min" ? "MIN" : "MAX";
			// MIN and MAX of one value are that value.
			distinct = false;
		} else if (name == "sum" || name == "sum_no_overflow" || name == "avg") {
			if (!IsNumericColumn(column.oracle_type) || !column.type.IsNumeric()) {
				return string();
			}
			function = name == "avg" ? "AVG" : "SUM";
		} else {
			return string();
		}
		return Cast(function + "(" + (distinct ? "DISTINCT " : "") + column.sql + ")", expr.GetReturnType());
	}

	static string Cast(const string &sql, const LogicalType &type) {
		auto cast = ResultCast(type);
		return cast.empty() ? sql : "CAST(" + sql + " AS " + cast + ")";
	}

	ColumnResolver resolve;
	string from;
	bool utf8_byte_order;
	vector<string> select_list;
	vector<string> group_by;
};

//! The aggregate query over a table: `FROM <table> WHERE <its filters>`.
AggregateQuery TableAggregateQuery(const ScanBelow &scan) {
	auto &bind_data = scan.get->bind_data->Cast<TableScanBindData>();
	auto &table = *bind_data.table;
	auto from = table.QualifiedName();
	auto conditions = TableScan::Conditions(*scan.get);
	if (!conditions.empty()) {
		from += " WHERE " + StringUtil::Join(conditions, " AND ");
	}
	auto resolve = [&scan, &bind_data, &table](const Expression &expr, QueryColumn &column) {
		auto index = ResolveColumn(expr, scan);
		if (!index.IsValid()) {
			return false;
		}
		column = {QuoteIdentifier(table.column_names[index.GetIndex()]), table.oracle_types[index.GetIndex()],
		          bind_data.all_types[index.GetIndex()]};
		return true;
	};
	return AggregateQuery(resolve, std::move(from), bind_data.catalog.GetDatabaseInfo().utf8_byte_order);
}

OrkideaCatalog &CatalogOf(const ScanBelow &scan) {
	return scan.get->bind_data->Cast<TableScanBindData>().catalog;
}

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
//! `expected`, with a scan of the Oracle query `sql` under a projection that
//! casts its columns to those types. A query for an operator without output
//! columns returns one, which nothing reads. The replaced bindings are
//! recorded in `replacer`. Leaves the plan alone, returning false, when
//! Oracle rejects the query or returns types that do not convert exactly.
bool ReplaceWithQuery(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, OrkideaCatalog &catalog,
                      const string &sql, vector<LogicalType> expected, const vector<ColumnBinding> &old_bindings,
                      ColumnBindingReplacer &replacer) {
	unique_ptr<FunctionData> bind_data;
	vector<LogicalType> types;
	TableFunction function;
	try {
		function = RemoteQuery::Function(input.context, catalog, sql, bind_data, types);
	} catch (std::exception &) {
		// Oracle rejected the query (for example a type its aggregate does
		// not take): DuckDB computes the result instead.
		return false;
	}
	if (expected.empty() && types.size() == 1) {
		expected = types;
	}
	if (types.size() != expected.size() || (old_bindings.size() != expected.size() && !old_bindings.empty())) {
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

constexpr const char *JOIN_PUSHDOWN_SETTING = "orkidea_join_pushdown";

//! A part of the plan that one Oracle query computes: the query, and the
//! column of the query each of its output bindings is.
struct RemoteSource {
	struct Column {
		//! The column's name in the query, quoted.
		string name;
		LogicalType type;
		//! The Oracle type of the table column it is, as the driver reports it.
		string oracle_type;
	};

	optional_ptr<OrkideaCatalog> catalog;
	string sql;
	column_binding_map_t<Column> columns;
};

//! Whether a cast DuckDB puts on a key, to compare it with a value of another
//! type, can be left out in Oracle, which compares the columns themselves: a
//! timestamp to a finer one, an integer or decimal to a type with at least
//! its scale, or a number to DOUBLE, which DuckDB compares rounded and Oracle
//! exactly. Where such a cast would overflow, DuckDB fails, and Oracle gives
//! the answer.
bool CanDropKeyCast(const LogicalType &source, const LogicalType &target) {
	auto source_digits = TimestampPrecision(source);
	auto target_digits = TimestampPrecision(target);
	if (source_digits.IsValid() && target_digits.IsValid()) {
		return source_digits.GetIndex() <= target_digits.GetIndex();
	}
	if (!source.IsNumeric() || !target.IsNumeric()) {
		return false;
	}
	if (target.id() == LogicalTypeId::DOUBLE) {
		return true;
	}
	if (target.id() == LogicalTypeId::FLOAT) {
		return source.id() == LogicalTypeId::FLOAT;
	}
	auto exact = [](const LogicalType &type) {
		return type.IsIntegral() || type.id() == LogicalTypeId::DECIMAL;
	};
	auto scale = [](const LogicalType &type) {
		return type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(type) : uint8_t(0);
	};
	return exact(source) && exact(target) && scale(target) >= scale(source);
}

//! Writes one Oracle query for a tree of joins whose leaves are scans of
//! tables of one attached database. Each table is a subquery with its pushed
//! filters, so a join's semantics do not depend on where Oracle applies them,
//! and every column has a name of its own throughout the query.
class JoinQueryBuilder {
public:
	//! The query computing `op`; false when Oracle cannot compute it exactly.
	bool Build(LogicalOperator &op, RemoteSource &result) {
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
			return BuildJoin(op.Cast<LogicalComparisonJoin>(), result);
		case LogicalOperatorType::LOGICAL_PROJECTION:
			return BuildProjection(op.Cast<LogicalProjection>(), result);
		case LogicalOperatorType::LOGICAL_FILTER:
			return BuildFilter(op.Cast<LogicalFilter>(), result);
		case LogicalOperatorType::LOGICAL_GET:
			return BuildScan(op.Cast<LogicalGet>(), result);
		default:
			return false;
		}
	}

	//! A name not used elsewhere in the query, quoted.
	string NewName(const char *prefix) {
		return QuoteIdentifier(prefix + to_string(next_name++));
	}

private:
	bool BuildScan(LogicalGet &get, RemoteSource &result) {
		if (get.function.GetName() != TableScan::NAME || !get.bind_data) {
			return false;
		}
		auto &bind_data = get.bind_data->Cast<TableScanBindData>();
		if (bind_data.row_limit.IsValid() || !bind_data.order_by.empty() || bind_data.for_update ||
		    TableScan::HasLocalFilters(get)) {
			return false;
		}
		auto &table = *bind_data.table;
		vector<string> select_list;
		for (auto &binding : get.GetColumnBindings()) {
			auto &column = get.GetColumnIds()[binding.column_index.GetIndex()];
			if (column.IsVirtualColumn()) {
				return false;
			}
			auto index = column.GetPrimaryIndex();
			auto name = NewName("c");
			select_list.push_back(QuoteIdentifier(table.column_names[index]) + " " + name);
			result.columns[binding] = {name, bind_data.all_types[index], table.oracle_types[index]};
		}
		if (select_list.empty()) {
			select_list.push_back("1 " + NewName("c"));
		}
		result.catalog = &bind_data.catalog;
		result.sql = "SELECT " + StringUtil::Join(select_list, ", ") + " FROM " + table.QualifiedName();
		auto conditions = TableScan::Conditions(get);
		if (!conditions.empty()) {
			result.sql += " WHERE " + StringUtil::Join(conditions, " AND ");
		}
		return true;
	}

	//! A projection of columns, which DuckDB puts between operators to reorder
	//! or drop them.
	bool BuildProjection(LogicalProjection &projection, RemoteSource &result) {
		RemoteSource child;
		if (!Build(*projection.children[0], child)) {
			return false;
		}
		result.catalog = child.catalog;
		result.sql = std::move(child.sql);
		for (idx_t i = 0; i < projection.expressions.size(); i++) {
			auto &expr = *projection.expressions[i];
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return false;
			}
			auto column = child.columns.find(expr.Cast<BoundColumnRefExpression>().Binding());
			if (column == child.columns.end()) {
				return false;
			}
			result.columns[ColumnBinding(projection.table_index, ProjectionIndex(i))] = column->second;
		}
		return true;
	}

	//! The one column `expr` references; false when it references none, or
	//! several.
	static bool OnlyColumn(const Expression &expr, ColumnBinding &column) {
		idx_t found = 0;
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &ref) {
			if (found == 0 || column != ref.Binding()) {
				found++;
			}
			column = ref.Binding();
		});
		return found == 1;
	}

	//! Leaves out the casts DuckDB puts on columns to compare them with a
	//! number of a wider type: a NUMBER compares with a BINARY_DOUBLE literal
	//! as DuckDB compares it cast to DOUBLE, since Oracle converts it so.
	static void DropColumnCasts(unique_ptr<Expression> &expr) {
		if (BoundCastExpression::IsCast(*expr)) {
			auto &cast = expr->Cast<BoundFunctionExpression>();
			auto &child = BoundCastExpression::Child(cast);
			if (!BoundCastExpression::IsTryCast(cast) &&
			    child.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    CanDropKeyCast(child.GetReturnType(), cast.GetReturnType())) {
				expr = child.Copy();
				return;
			}
		}
		ExpressionIterator::EnumerateChildren(*expr, [](unique_ptr<Expression> &child) { DropColumnCasts(child); });
	}

	//! A filter on one column at a time, such as one DuckDB derives from a join
	//! condition and a filter on the other side, translated as a scan's filter.
	bool BuildFilter(LogicalFilter &filter, RemoteSource &result) {
		RemoteSource child;
		if (!Build(*filter.children[0], child)) {
			return false;
		}
		auto alias = NewName("t");
		vector<string> conditions;
		for (auto &expr : filter.expressions) {
			ColumnBinding binding;
			auto column = OnlyColumn(*expr, binding) ? child.columns.find(binding) : child.columns.end();
			if (column == child.columns.end()) {
				return false;
			}
			FilterColumn filter_column;
			filter_column.sql = alias + "." + column->second.name;
			filter_column.oracle_type = column->second.oracle_type;
			filter_column.unicode_database = child.catalog->GetDatabaseInfo().unicode;
			auto uncast = expr->Copy();
			DropColumnCasts(uncast);
			auto translated = TranslateFilter(filter_column, *uncast);
			if (translated.evaluate_locally || translated.sql.empty()) {
				return false;
			}
			conditions.push_back(std::move(translated.sql));
		}
		result.catalog = child.catalog;
		result.columns = std::move(child.columns);
		result.sql = "SELECT " + alias + ".* FROM (" + child.sql + ") " + alias + " WHERE " +
		             StringUtil::Join(conditions, " AND ");
		return true;
	}

	//! The column of `source` a join key is: the column, or a cast of it that
	//! Oracle can leave out.
	static optional_ptr<const RemoteSource::Column> KeyColumn(const Expression &expr, const RemoteSource &source) {
		reference<const Expression> key = expr;
		if (BoundCastExpression::IsCast(key.get())) {
			auto &cast = key.get().Cast<BoundFunctionExpression>();
			auto &child = BoundCastExpression::Child(cast);
			if (BoundCastExpression::IsTryCast(cast) || !CanDropKeyCast(child.GetReturnType(), cast.GetReturnType())) {
				return nullptr;
			}
			key = child;
		}
		if (key.get().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return nullptr;
		}
		auto column = source.columns.find(key.get().Cast<BoundColumnRefExpression>().Binding());
		return column == source.columns.end() ? nullptr : &column->second;
	}

	//! A join condition in Oracle, or empty when Oracle cannot compare its
	//! keys as DuckDB does.
	string Condition(const JoinCondition &condition, const RemoteSource &left, const string &left_alias,
	                 const RemoteSource &right, const string &right_alias) const {
		if (!condition.IsComparison()) {
			return string();
		}
		auto left_key = KeyColumn(condition.GetLHS(), left);
		auto right_key = KeyColumn(condition.GetRHS(), right);
		if (!left_key || !right_key ||
		    !CanCompareColumns(left_key->oracle_type, right_key->oracle_type, condition.GetComparisonType(),
		                       left.catalog->GetDatabaseInfo().utf8_byte_order)) {
			return string();
		}
		auto l = left_alias + "." + left_key->name;
		auto r = right_alias + "." + right_key->name;
		switch (condition.GetComparisonType()) {
		case ExpressionType::COMPARE_EQUAL:
			return l + " = " + r;
		case ExpressionType::COMPARE_NOTEQUAL:
			return l + " <> " + r;
		case ExpressionType::COMPARE_LESSTHAN:
			return l + " < " + r;
		case ExpressionType::COMPARE_GREATERTHAN:
			return l + " > " + r;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			return l + " <= " + r;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return l + " >= " + r;
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			return "(" + l + " = " + r + " OR (" + l + " IS NULL AND " + r + " IS NULL))";
		default:
			return string();
		}
	}

	bool BuildJoin(LogicalComparisonJoin &join, RemoteSource &result) {
		string join_keyword;
		switch (join.join_type) {
		case JoinType::INNER:
			join_keyword = "JOIN";
			break;
		case JoinType::LEFT:
			join_keyword = "LEFT JOIN";
			break;
		case JoinType::RIGHT:
			join_keyword = "RIGHT JOIN";
			break;
		case JoinType::OUTER:
			join_keyword = "FULL JOIN";
			break;
		case JoinType::SEMI:
		case JoinType::ANTI:
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
			break;
		default:
			// MARK and SINGLE joins, for subqueries, have no Oracle join.
			return false;
		}
		RemoteSource left;
		RemoteSource right;
		if (!Build(*join.children[0], left) || !Build(*join.children[1], right) || left.catalog != right.catalog) {
			return false;
		}
		auto left_alias = NewName("t");
		auto right_alias = NewName("t");
		vector<string> conditions;
		for (auto &condition : join.conditions) {
			auto sql = Condition(condition, left, left_alias, right, right_alias);
			if (sql.empty()) {
				return false;
			}
			conditions.push_back(std::move(sql));
		}
		if (conditions.empty()) {
			return false;
		}

		// The columns the join outputs, of either side.
		vector<string> select_list;
		for (auto &binding : join.GetColumnBindings()) {
			auto column = left.columns.find(binding);
			auto &alias = column != left.columns.end() ? left_alias : right_alias;
			if (column == left.columns.end()) {
				column = right.columns.find(binding);
				if (column == right.columns.end()) {
					return false;
				}
			}
			select_list.push_back(alias + "." + column->second.name);
			result.columns[binding] = column->second;
		}
		if (select_list.empty()) {
			select_list.push_back("1 " + NewName("c"));
		}
		result.catalog = left.catalog;
		auto left_from = "(" + left.sql + ") " + left_alias;
		auto right_from = "(" + right.sql + ") " + right_alias;
		auto on = StringUtil::Join(conditions, " AND ");
		result.sql = "SELECT " + StringUtil::Join(select_list, ", ") + " FROM ";
		switch (join.join_type) {
		case JoinType::SEMI:
		case JoinType::ANTI:
			// The rows of the left side with (or without) a match on the right.
			result.sql += left_from + " WHERE " + (join.join_type == JoinType::ANTI ? "NOT " : "") +
			              "EXISTS (SELECT 1 FROM " + right_from + " WHERE " + on + ")";
			break;
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
			result.sql += right_from + " WHERE " + (join.join_type == JoinType::RIGHT_ANTI ? "NOT " : "") +
			              "EXISTS (SELECT 1 FROM " + left_from + " WHERE " + on + ")";
			break;
		default:
			result.sql += left_from + " " + join_keyword + " " + right_from + " ON " + on;
			break;
		}
		return true;
	}

	idx_t next_name = 0;
};

//! The most rows DuckDB estimates one of the tables under `op` gives, or
//! invalid when it has no estimate for one, or Oracle no statistics.
optional_idx LargestInput(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		// A table Oracle has no statistics for has only DuckDB's guess.
		auto &get = op.Cast<LogicalGet>();
		if (get.function.GetName() == TableScan::NAME && get.bind_data &&
		    get.bind_data->Cast<TableScanBindData>().table->estimated_rows < 0) {
			return optional_idx();
		}
		return op.has_estimated_cardinality ? optional_idx(op.estimated_cardinality) : optional_idx();
	}
	idx_t largest = 0;
	for (auto &child : op.children) {
		auto rows = LargestInput(*child);
		if (!rows.IsValid()) {
			return optional_idx();
		}
		largest = MaxValue(largest, rows.GetIndex());
	}
	return largest;
}

//! Whether a join, or an aggregate over one, is estimated to give fewer rows
//! than half of its largest table: then Oracle sends fewer rows than DuckDB
//! would read. One that keeps as many is faster in DuckDB, which reads the
//! tables in parallel. Without estimates, it runs in Oracle.
bool ReducesRows(LogicalOperator &op) {
	auto largest = LargestInput(op);
	return !op.has_estimated_cardinality || !largest.IsValid() || op.estimated_cardinality * 2 <= largest.GetIndex();
}

//! The input of an aggregate as Oracle reads it: a table, or a join of
//! tables of one attached database when `joins` allows. An aggregate over a
//! table runs in Oracle; over a join, when it reduces the rows (ReducesRows).
struct AggregateInput {
	optional_ptr<OrkideaCatalog> catalog;
	unique_ptr<AggregateQuery> query;
	//! For a table: its scan, which `query` reads.
	ScanBelow scan;
	//! For a join: the query computing it, which `query` reads.
	RemoteSource source;
	JoinQueryBuilder builder;

	bool Find(LogicalOperator &op, bool joins) {
		scan = FindReplaceableScan(op);
		if (scan.get) {
			catalog = &CatalogOf(scan);
			query = make_uniq<AggregateQuery>(TableAggregateQuery(scan));
			return true;
		}
		if (!joins || !builder.Build(op, source)) {
			return false;
		}
		catalog = source.catalog;
		auto alias = builder.NewName("t");
		auto resolve = [this, alias](const Expression &expr, QueryColumn &column) {
			if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return false;
			}
			auto found = source.columns.find(expr.Cast<BoundColumnRefExpression>().Binding());
			if (found == source.columns.end()) {
				return false;
			}
			column = {alias + "." + found->second.name, found->second.oracle_type, found->second.type};
			return true;
		};
		query = make_uniq<AggregateQuery>(resolve, "(" + source.sql + ") " + alias,
		                                  catalog->GetDatabaseInfo().utf8_byte_order);
		return true;
	}
};

//! GROUP BY and aggregates over an Oracle table, or a join of tables.
bool PushAggregate(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, bool joins,
                   ColumnBindingReplacer &replacer) {
	auto &aggregate = op->Cast<LogicalAggregate>();
	if (aggregate.grouping_sets.size() > 1 || !aggregate.grouping_functions.empty() ||
	    (aggregate.groups.empty() && aggregate.expressions.empty())) {
		return false;
	}
	AggregateInput source;
	if (!source.Find(*aggregate.children[0], joins && ReducesRows(*op))) {
		return false;
	}
	auto &query = *source.query;
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
	return ReplaceWithQuery(input, op, *source.catalog, query.Build(), std::move(expected),
	                        aggregate.GetColumnBindings(), replacer);
}

//! SELECT DISTINCT over an Oracle table, or a join of tables: a GROUP BY of
//! every column.
bool PushDistinct(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, bool joins,
                  ColumnBindingReplacer &replacer) {
	auto &distinct = op->Cast<LogicalDistinct>();
	if (distinct.distinct_type != DistinctType::DISTINCT || distinct.order_by) {
		return false;
	}
	auto &child = *distinct.children[0];
	AggregateInput source;
	if (!source.Find(child, joins && ReducesRows(*op))) {
		return false;
	}
	auto bindings = child.GetColumnBindings();
	child.ResolveOperatorTypes();
	auto &query = *source.query;
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (!query.AddGroup(BoundColumnRefExpression(child.types[i], bindings[i]))) {
			return false;
		}
	}
	return ReplaceWithQuery(input, op, *source.catalog, query.Build(), child.types, bindings, replacer);
}

//! A tree of joins over tables of one attached database, as one Oracle query.
bool PushJoin(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &op, ColumnBindingReplacer &replacer) {
	if (!ReducesRows(*op)) {
		return false;
	}
	JoinQueryBuilder builder;
	RemoteSource source;
	if (!builder.Build(*op, source)) {
		return false;
	}
	op->ResolveOperatorTypes();
	return ReplaceWithQuery(input, op, *source.catalog, source.sql, op->types, op->GetColumnBindings(), replacer);
}

struct PushdownState {
	OptimizerExtensionInput &input;
	bool push_aggregates;
	bool push_joins;
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
		if (context.push_aggregates && PushAggregate(context.input, op, context.push_joins, context.replacer)) {
			return;
		}
		break;
	case LogicalOperatorType::LOGICAL_DISTINCT:
		if (context.push_aggregates && PushDistinct(context.input, op, context.push_joins, context.replacer)) {
			return;
		}
		break;
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		if (context.push_joins && PushJoin(context.input, op, context.replacer)) {
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
	auto enabled = [&](const char *name) {
		Value setting;
		return !input.context.TryGetCurrentSetting(name, setting) || BooleanValue::Get(setting);
	};
	PushdownState context {input, enabled(AGGREGATE_PUSHDOWN_SETTING), enabled(JOIN_PUSHDOWN_SETTING),
	                       ColumnBindingReplacer()};
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
	config.AddExtensionOption(JOIN_PUSHDOWN_SETTING,
	                          "Whether joins of tables of one attached Oracle database run in Oracle",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	OptimizerExtension extension;
	extension.optimize_function = Optimize;
	OptimizerExtension::Register(config, std::move(extension));
}

} // namespace orkidea
} // namespace duckdb
