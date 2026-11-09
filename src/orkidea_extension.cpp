#define DUCKDB_EXTENSION_MAIN

#include "orkidea_extension.hpp"

#include "orkidea_adbc.hpp"
#include "orkidea_execute.hpp"
#include "orkidea_logging.hpp"
#include "orkidea_optimizer.hpp"
#include "orkidea_query.hpp"
#include "orkidea_scan.hpp"
#include "orkidea_secrets.hpp"
#include "orkidea_storage.hpp"

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

static void OrkideaVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// Initializing the driver proves the linked library is present and usable.
	orkidea::Driver();
	OrkideaExtension extension;
	auto text = "orkidea " + extension.Version() + " (ADBC 1.1.0)";
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, text);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(ScalarFunction("orkidea_version", {}, LogicalType::VARCHAR, OrkideaVersionFunction));
	orkidea::Secrets::Register(loader);
	orkidea::QueryFunction::Register(loader);
	orkidea::ExecuteFunction::Register(loader);
	orkidea::Storage::Register(loader);
	orkidea::QueryLogType::Register(loader);
	orkidea::Optimizer::Register(loader);
	orkidea::TableScan::RegisterSettings(loader);
}

void OrkideaExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string OrkideaExtension::Name() {
	return "orkidea";
}

std::string OrkideaExtension::Version() const {
#ifdef EXT_VERSION_ORKIDEA
	return EXT_VERSION_ORKIDEA;
#else
	return "dev";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(orkidea, loader) {
	duckdb::LoadInternal(loader);
}
}
