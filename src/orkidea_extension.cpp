#define DUCKDB_EXTENSION_MAIN

#include "orkidea_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/adbc/adbc.h"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"

// The Orkidea driver's ADBC entry point, linked in from the Rust static
// library (rust/src/lib.rs).
extern "C" AdbcStatusCode OrkideaDriverInit(int version, void *driver, struct AdbcError *error);

namespace duckdb {

//! Initializes the linked driver once, to prove it is present and usable.
static string DriverStatus() {
	AdbcDriver driver {};
	AdbcError error {};
	auto status = OrkideaDriverInit(ADBC_VERSION_1_1_0, &driver, &error);
	if (status != ADBC_STATUS_OK) {
		string message = error.message ? error.message : "unknown error";
		if (error.release) {
			error.release(&error);
		}
		throw IOException("Orkidea driver failed to initialize: %s", message);
	}
	if (driver.release) {
		driver.release(&driver, &error);
	}
	return "ADBC 1.1.0";
}

static void OrkideaVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	OrkideaExtension extension;
	auto text = "orkidea " + extension.Version() + " (" + DriverStatus() + ")";
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, text);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(ScalarFunction("orkidea_version", {}, LogicalType::VARCHAR, OrkideaVersionFunction));
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
