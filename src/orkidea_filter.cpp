#include "orkidea_filter.hpp"

#include "orkidea_util.hpp"

#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

#include <cmath>

namespace duckdb {
namespace orkidea {

namespace {

//! Longest string literal Oracle accepts in SQL without extended strings.
constexpr idx_t MAX_LITERAL_BYTES = 4000;
//! Oracle allows at most 1000 expressions in an IN list.
constexpr idx_t MAX_IN_LIST = 1000;

enum class ColumnKind {
	//! Not filtered in Oracle.
	NONE,
	//! Only NULL checks are pushed: CHAR and NCHAR compare blank-padded, which DuckDB does not.
	NULL_CHECKS_ONLY,
	NUMBER,
	BINARY_FLOAT,
	BINARY_DOUBLE,
	VARCHAR,
	NVARCHAR,
	DATE,
	TIMESTAMP,
	TIMESTAMP_TZ,
	RAW,
	BOOLEAN,
};

ColumnKind Classify(const string &oracle_type) {
	auto type = StringUtil::Upper(oracle_type);
	auto is_sized = [&](const char *prefix) {
		return StringUtil::StartsWith(type, prefix) && StringUtil::EndsWith(type, ")");
	};
	if (type == "NUMBER" || is_sized("NUMBER(")) {
		return ColumnKind::NUMBER;
	}
	if (type == "BINARY_FLOAT") {
		return ColumnKind::BINARY_FLOAT;
	}
	if (type == "BINARY_DOUBLE") {
		return ColumnKind::BINARY_DOUBLE;
	}
	if (is_sized("VARCHAR2(")) {
		return ColumnKind::VARCHAR;
	}
	if (is_sized("NVARCHAR2(")) {
		return ColumnKind::NVARCHAR;
	}
	if (type == "DATE") {
		return ColumnKind::DATE;
	}
	if (StringUtil::StartsWith(type, "TIMESTAMP")) {
		// TIMESTAMP WITH LOCAL TIME ZONE reads in the session time zone, so it is left out.
		if (StringUtil::EndsWith(type, " WITH LOCAL TIME ZONE")) {
			return ColumnKind::NONE;
		}
		return StringUtil::EndsWith(type, " WITH TIME ZONE") ? ColumnKind::TIMESTAMP_TZ : ColumnKind::TIMESTAMP;
	}
	if (is_sized("RAW(")) {
		return ColumnKind::RAW;
	}
	if (type == "BOOLEAN") {
		return ColumnKind::BOOLEAN;
	}
	if (is_sized("CHAR(") || is_sized("NCHAR(") || type == "ROWID") {
		return ColumnKind::NULL_CHECKS_ONLY;
	}
	return ColumnKind::NONE;
}

bool IsAscii(const string &value) {
	for (auto c : value) {
		if (static_cast<unsigned char>(c) >= 0x80) {
			return false;
		}
	}
	return true;
}

bool IsDigits(const string &text, idx_t begin, idx_t end) {
	for (idx_t i = begin; i < end; i++) {
		if (!StringUtil::CharacterIsDigit(text[i])) {
			return false;
		}
	}
	return true;
}

//! Whether text is `YYYY-MM-DD`, with a year from 1 to 9999.
bool IsOracleDate(const string &text) {
	return text.size() == 10 && IsDigits(text, 0, 4) && text[4] == '-' && IsDigits(text, 5, 7) && text[7] == '-' &&
	       IsDigits(text, 8, 10) && text.substr(0, 4) != "0000";
}

//! Whether text is `YYYY-MM-DD HH:MM:SS[.fraction]`, with a year from 1 to 9999.
bool IsOracleTimestamp(const string &text) {
	if (text.size() < 19 || !IsOracleDate(text.substr(0, 10)) || text[10] != ' ' || !IsDigits(text, 11, 13) ||
	    text[13] != ':' || !IsDigits(text, 14, 16) || text[16] != ':' || !IsDigits(text, 17, 19)) {
		return false;
	}
	if (text.size() == 19) {
		return true;
	}
	return text[19] == '.' && text.size() > 20 && text.size() <= 29 && IsDigits(text, 20, text.size());
}

//! A finite floating-point literal, as DuckDB prints it, followed by the
//! Oracle suffix for its binary type.
string FloatingLiteral(const Value &value, double number, const char *special_prefix, char suffix) {
	if (std::isnan(number)) {
		return string(special_prefix) + "_NAN";
	}
	if (std::isinf(number)) {
		return string(number < 0 ? "-" : "") + special_prefix + "_INFINITY";
	}
	auto text = value.ToString();
	for (auto c : text) {
		if (!StringUtil::CharacterIsDigit(c) && c != '.' && c != '-' && c != '+' && c != 'e' && c != 'E') {
			return string();
		}
	}
	return text + suffix;
}

//! The Oracle literal for a constant compared with a column, or empty when
//! Oracle would compare it differently from DuckDB. `ordered` is true for
//! range comparisons, where strings must also sort the same way.
string WriteConstant(ColumnKind kind, const FilterColumn &column, const Value &value, bool ordered) {
	if (value.IsNull()) {
		return string();
	}
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		if (kind != ColumnKind::BOOLEAN) {
			return string();
		}
		return BooleanValue::Get(value) ? "TRUE" : "FALSE";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DECIMAL:
		if (kind != ColumnKind::NUMBER) {
			return string();
		}
		return value.ToString();
	case LogicalTypeId::DOUBLE:
		// An unconstrained NUMBER reads as DOUBLE. Oracle converts it to
		// BINARY_DOUBLE to compare it with a BINARY_DOUBLE literal, as DuckDB does.
		if (kind != ColumnKind::NUMBER && kind != ColumnKind::BINARY_DOUBLE) {
			return string();
		}
		return FloatingLiteral(value, DoubleValue::Get(value), "BINARY_DOUBLE", 'd');
	case LogicalTypeId::FLOAT:
		if (kind != ColumnKind::BINARY_FLOAT) {
			return string();
		}
		return FloatingLiteral(value, FloatValue::Get(value), "BINARY_FLOAT", 'f');
	case LogicalTypeId::VARCHAR: {
		if (kind != ColumnKind::VARCHAR && kind != ColumnKind::NVARCHAR) {
			return string();
		}
		auto &text = StringValue::Get(value);
		// Oracle stores an empty string as NULL, and an ASCII-only constant
		// sorts against any value the same way in every ASCII-based character set.
		bool ascii = IsAscii(text);
		if (text.empty() || text.size() > MAX_LITERAL_BYTES || text.find('\0') != string::npos || (ordered && !ascii) ||
		    (!ascii && !column.unicode_database)) {
			return string();
		}
		return (kind == ColumnKind::NVARCHAR ? "N" : "") + QuoteLiteral(text);
	}
	case LogicalTypeId::BLOB: {
		auto &bytes = StringValue::Get(value);
		if (kind != ColumnKind::RAW || bytes.empty() || bytes.size() * 2 > MAX_LITERAL_BYTES) {
			return string();
		}
		string hex;
		for (auto byte : bytes) {
			hex += Blob::HEX_TABLE[static_cast<uint8_t>(byte) >> 4];
			hex += Blob::HEX_TABLE[static_cast<uint8_t>(byte) & 0x0F];
		}
		return "HEXTORAW('" + hex + "')";
	}
	case LogicalTypeId::DATE: {
		auto text = value.ToString();
		if ((kind != ColumnKind::DATE && kind != ColumnKind::TIMESTAMP) || !IsOracleDate(text)) {
			return string();
		}
		return "DATE '" + text + "'";
	}
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS: {
		auto text = value.ToString();
		if ((kind != ColumnKind::DATE && kind != ColumnKind::TIMESTAMP) || !IsOracleTimestamp(text)) {
			return string();
		}
		return "TIMESTAMP '" + text + "'";
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		// The instant in UTC; Oracle compares TIMESTAMP WITH TIME ZONE values as instants.
		auto instant = value.GetValue<timestamp_tz_t>();
		if (kind != ColumnKind::TIMESTAMP_TZ || !instant.IsFinite()) {
			return string();
		}
		auto text = Timestamp::ToString(timestamp_t(instant.value));
		if (!IsOracleTimestamp(text)) {
			return string();
		}
		return "TIMESTAMP '" + text + " +00:00'";
	}
	default:
		return string();
	}
}

const char *ComparisonOperator(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "<>";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		return nullptr;
	}
}

bool IsColumn(const Expression &expr) {
	auto expression_class = expr.GetExpressionClass();
	return expression_class == ExpressionClass::BOUND_REF || expression_class == ExpressionClass::BOUND_COLUMN_REF;
}

//! Whether a cast keeps every value of its source and orders them the same
//! way, so that comparing the cast column is comparing the column: a
//! timestamp to a finer one, or an integer or decimal to a numeric type with
//! at least its scale that cannot overflow. DuckDB adds such casts to compare
//! columns with constants of a wider type, as a TIMESTAMP(3) column with a
//! TIMESTAMP literal. Casts to floating point are left out: they round.
bool IsWideningCast(const LogicalType &source, const LogicalType &target) {
	if (target.id() == LogicalTypeId::TIMESTAMP) {
		// DuckDB can throw here for years past microsecond range, which an
		// Oracle timestamp (year 9999 at most) never reaches.
		return source.id() == LogicalTypeId::TIMESTAMP_SEC || source.id() == LogicalTypeId::TIMESTAMP_MS;
	}
	if (BoundCastExpression::CastCanThrow(source, target, false)) {
		return false;
	}
	switch (target.id()) {
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::DECIMAL: {
		if (!source.IsIntegral() && source.id() != LogicalTypeId::DECIMAL) {
			return false;
		}
		auto scale = [](const LogicalType &type) {
			return type.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(type) : uint8_t(0);
		};
		return scale(target) >= scale(source);
	}
	default:
		return false;
	}
}

//! A column, or a widening cast of one (see IsWideningCast).
bool IsColumnOrWidened(const Expression &expr) {
	if (IsColumn(expr)) {
		return true;
	}
	if (!BoundCastExpression::IsCast(expr)) {
		return false;
	}
	auto &cast = expr.Cast<BoundFunctionExpression>();
	return !BoundCastExpression::IsTryCast(cast) && IsColumn(BoundCastExpression::Child(cast)) &&
	       IsWideningCast(BoundCastExpression::SourceType(cast), BoundCastExpression::TargetType(cast));
}

bool IsInternal(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       TableFilterFunctions::IsTableFilterFunction(expr.Cast<BoundFunctionExpression>().Function());
}

//! A filter Oracle applies exactly.
TranslatedFilter Exact(string sql) {
	return TranslatedFilter {std::move(sql), false};
}

const TranslatedFilter LOCAL_ONLY {string(), true};

class Translator {
public:
	explicit Translator(const FilterColumn &column) : column(column), kind(Classify(column.oracle_type)) {
	}

	TranslatedFilter Translate(const Expression &expr) const {
		if (IsInternal(expr)) {
			return TranslateInternal(expr.Cast<BoundFunctionExpression>());
		}
		if (BoundComparisonExpression::IsComparison(expr)) {
			return TranslateComparison(expr.Cast<BoundFunctionExpression>());
		}
		// A BOOLEAN column on its own, as in WHERE flag, selects its TRUE rows.
		if (IsColumn(expr)) {
			return kind == ColumnKind::BOOLEAN ? Exact(column.sql + " = TRUE") : LOCAL_ONLY;
		}
		switch (expr.GetExpressionClass()) {
		case ExpressionClass::BOUND_CONJUNCTION:
			return TranslateConjunction(expr.Cast<BoundConjunctionExpression>());
		case ExpressionClass::BOUND_OPERATOR: {
			auto &op = expr.Cast<BoundOperatorExpression>();
			if (op.GetExpressionType() == ExpressionType::OPERATOR_NOT) {
				return TranslateNot(op);
			}
			return TranslateOperator(op);
		}
		default:
			return LOCAL_ONLY;
		}
	}

private:
	//! Optional filters may be applied or not, so they never need local
	//! evaluation. Other internal filters (dynamic, Bloom) are not pushed.
	TranslatedFilter TranslateInternal(const BoundFunctionExpression &func) const {
		auto &name = func.Function().GetName();
		optional_ptr<const Expression> child;
		if (name == OptionalFilterScalarFun::NAME && func.BindInfo()) {
			child = func.BindInfo()->Cast<OptionalFilterFunctionData>().child_filter_expr.get();
		} else if (name == SelectivityOptionalFilterScalarFun::NAME && func.BindInfo()) {
			child = func.BindInfo()->Cast<SelectivityOptionalFilterFunctionData>().child_filter_expr.get();
		}
		if (!child) {
			return TranslatedFilter {string(), false};
		}
		return TranslatedFilter {Translate(*child).sql, false};
	}

	TranslatedFilter TranslateComparison(const BoundFunctionExpression &comparison) const {
		if (kind == ColumnKind::NONE || kind == ColumnKind::NULL_CHECKS_ONLY) {
			return LOCAL_ONLY;
		}
		auto type = comparison.GetExpressionType();
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		const Value *constant;
		if (IsColumnOrWidened(left) && right.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant = &right.Cast<BoundConstantExpression>().GetValue();
		} else if (IsColumnOrWidened(right) && left.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			constant = &left.Cast<BoundConstantExpression>().GetValue();
			type = FlipComparisonExpression(type);
		} else {
			return LOCAL_ONLY;
		}
		bool ordered = type != ExpressionType::COMPARE_EQUAL && type != ExpressionType::COMPARE_NOTEQUAL &&
		               type != ExpressionType::COMPARE_DISTINCT_FROM &&
		               type != ExpressionType::COMPARE_NOT_DISTINCT_FROM;
		auto literal = WriteConstant(kind, column, *constant, ordered);
		if (literal.empty()) {
			return LOCAL_ONLY;
		}
		// DECODE treats two NULLs as equal, which makes it a portable IS [NOT] DISTINCT FROM.
		if (type == ExpressionType::COMPARE_DISTINCT_FROM) {
			return Exact(StringUtil::Format("DECODE(%s, %s, 0, 1) = 1", column.sql, literal));
		}
		if (type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return Exact(StringUtil::Format("DECODE(%s, %s, 0, 1) = 0", column.sql, literal));
		}
		auto op = ComparisonOperator(type);
		if (!op) {
			return LOCAL_ONLY;
		}
		return Exact(StringUtil::Format("%s %s %s", column.sql, op, literal));
	}

	TranslatedFilter TranslateConjunction(const BoundConjunctionExpression &conjunction) const {
		auto type = conjunction.GetExpressionType();
		if (type != ExpressionType::CONJUNCTION_AND && type != ExpressionType::CONJUNCTION_OR) {
			return LOCAL_ONLY;
		}
		bool is_and = type == ExpressionType::CONJUNCTION_AND;
		vector<string> parts;
		bool evaluate_locally = false;
		for (auto &child : conjunction.GetChildren()) {
			auto part = Translate(*child);
			if (part.sql.empty()) {
				if (!is_and) {
					// Without one of its terms, an OR would select fewer rows.
					return LOCAL_ONLY;
				}
				evaluate_locally = evaluate_locally || part.evaluate_locally;
				continue;
			}
			evaluate_locally = evaluate_locally || part.evaluate_locally;
			parts.push_back(std::move(part.sql));
		}
		if (parts.empty()) {
			return TranslatedFilter {string(), evaluate_locally};
		}
		return TranslatedFilter {"(" + StringUtil::Join(parts, is_and ? " AND " : " OR ") + ")", evaluate_locally};
	}

	//! NOT of a condition Oracle evaluates exactly is exact too: both follow
	//! SQL's three-valued logic, so NULL stays NULL. NOT of a superset is not.
	TranslatedFilter TranslateNot(const BoundOperatorExpression &op) const {
		// Optional filters translate to a superset while reporting no local
		// evaluation, so their negation cannot be pushed.
		if (op.GetChildren().size() != 1 || IsInternal(*op.GetChildren()[0])) {
			return LOCAL_ONLY;
		}
		auto inner = Translate(*op.GetChildren()[0]);
		if (inner.sql.empty() || inner.evaluate_locally) {
			return LOCAL_ONLY;
		}
		return Exact("NOT (" + inner.sql + ")");
	}

	TranslatedFilter TranslateOperator(const BoundOperatorExpression &op) const {
		auto &children = op.GetChildren();
		if (children.empty() || !IsColumnOrWidened(*children[0]) || kind == ColumnKind::NONE) {
			return LOCAL_ONLY;
		}
		switch (op.GetExpressionType()) {
		case ExpressionType::OPERATOR_IS_NULL:
			return children.size() == 1 ? Exact(column.sql + " IS NULL") : LOCAL_ONLY;
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return children.size() == 1 ? Exact(column.sql + " IS NOT NULL") : LOCAL_ONLY;
		case ExpressionType::COMPARE_IN: {
			if (kind == ColumnKind::NULL_CHECKS_ONLY || children.size() - 1 > MAX_IN_LIST) {
				return LOCAL_ONLY;
			}
			vector<string> literals;
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					return LOCAL_ONLY;
				}
				auto literal =
				    WriteConstant(kind, column, children[i]->Cast<BoundConstantExpression>().GetValue(), false);
				if (literal.empty()) {
					return LOCAL_ONLY;
				}
				literals.push_back(std::move(literal));
			}
			return Exact(column.sql + " IN (" + StringUtil::Join(literals, ", ") + ")");
		}
		default:
			return LOCAL_ONLY;
		}
	}

	const FilterColumn &column;
	const ColumnKind kind;
};

} // namespace

bool CanCompareColumn(const string &oracle_type) {
	auto kind = Classify(oracle_type);
	return kind != ColumnKind::NONE && kind != ColumnKind::NULL_CHECKS_ONLY;
}

bool CanOrderColumn(const string &oracle_type, bool utf8_byte_order) {
	switch (Classify(oracle_type)) {
	case ColumnKind::NUMBER:
	case ColumnKind::BINARY_FLOAT:
	case ColumnKind::BINARY_DOUBLE:
	case ColumnKind::DATE:
	case ColumnKind::TIMESTAMP:
	case ColumnKind::TIMESTAMP_TZ:
	case ColumnKind::RAW:
	case ColumnKind::BOOLEAN:
		return true;
	case ColumnKind::VARCHAR:
		// With NLS_SORT = BINARY, Oracle sorts AL32UTF8 bytes, which is UTF-8
		// code point order, as DuckDB does. UTF8 (CESU-8) and the national
		// character set (UTF-16) sort supplementary characters differently.
		return utf8_byte_order;
	default:
		return false;
	}
}

bool CanGroupColumn(const string &oracle_type) {
	return Classify(oracle_type) != ColumnKind::NONE;
}

bool IsNumericColumn(const string &oracle_type) {
	switch (Classify(oracle_type)) {
	case ColumnKind::NUMBER:
	case ColumnKind::BINARY_FLOAT:
	case ColumnKind::BINARY_DOUBLE:
		return true;
	default:
		return false;
	}
}

TranslatedFilter TranslateFilter(const FilterColumn &column, const Expression &expr) {
	return Translator(column).Translate(expr);
}

} // namespace orkidea
} // namespace duckdb
