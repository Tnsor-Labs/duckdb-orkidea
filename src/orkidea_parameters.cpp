#include "orkidea_parameters.hpp"

#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! The value bound for a parameter, labeled `label` in errors.
Value BindableValue(const Value &value, const string &label, const string &function) {
	switch (value.type().id()) {
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT: {
		// Arrow carries them as DECIMAL(38,0), which a larger value would not fit.
		string error;
		auto result = value.DefaultTryCastAs(LogicalType::DECIMAL(38, 0), &error);
		if (!result) {
			throw BinderException("%s: parameter %s: %s", function, label, error);
		}
		return *result;
	}
	case LogicalTypeId::TIMESTAMP_TZ:
		throw BinderException("%s: parameter %s is a TIMESTAMP WITH TIME ZONE, which the driver binds without its "
		                      "time zone; bind the UTC time as a TIMESTAMP (x AT TIME ZONE 'UTC') and convert it in "
		                      "Oracle with FROM_TZ(CAST(:%s AS TIMESTAMP), 'UTC')",
		                      function, label, label);
	default:
		return value;
	}
}

} // namespace

Parameters Parameters::FromValue(const Value &value, const string &function) {
	Parameters result;
	if (value.IsNull()) {
		return result;
	}
	auto &type = value.type();
	if (type.id() != LogicalTypeId::STRUCT && type.id() != LogicalTypeId::TUPLE) {
		throw BinderException("%s: params must be a STRUCT, such as {'id': 42} for :id, or a row(...) of values for "
		                      ":1, :2, ..., not %s",
		                      function, type.ToString());
	}
	result.by_name = !StructType::IsUnnamed(type);
	auto &children = StructType::GetChildTypes(type);
	auto &values = StructValue::GetChildren(value);
	for (idx_t i = 0; i < children.size(); i++) {
		auto label = result.by_name ? children[i].first.GetIdentifierName() : to_string(i + 1);
		result.names.push_back(result.by_name ? label : "p" + label);
		result.values.push_back(BindableValue(values[i], label, function));
	}
	return result;
}

void Parameters::Bind(ClientContext &context, Statement &statement) const {
	vector<LogicalType> types;
	for (auto &value : values) {
		types.push_back(value.type());
	}
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), types, 1);
	for (idx_t i = 0; i < values.size(); i++) {
		chunk.data[i].Append(values[i]);
	}
	chunk.CheckCardinality(1);

	auto properties = context.GetClientProperties();
	properties.arrow_offset_size = ArrowOffsetSize::REGULAR;
	properties.arrow_use_list_view = false;
	properties.produce_arrow_string_view = false;
	properties.arrow_lossless_conversion = false;
	ArrowSchemaWrapper schema;
	ArrowArrayWrapper array;
	ArrowConverter::ToArrowSchema(&schema.arrow_schema, types, names, properties);
	ArrowConverter::ToArrowArray(chunk, &array.arrow_array, properties, {});
	statement.SetOption("adbc.statement.bind_by_name", by_name ? "true" : "false");
	statement.Bind(array.arrow_array, schema.arrow_schema);
}

} // namespace orkidea
} // namespace duckdb
