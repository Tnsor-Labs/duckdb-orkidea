cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME orkidea)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Orkidea driver is Rust, built by Corrosion as a static library and linked
# into the extension. Build it for the platform DuckDB is built for, which in
# cross builds (macOS x86_64 on arm64, for example) is not the host.
set(Rust_CARGO_TARGET "")
if("${OS_NAME}" STREQUAL "linux")
  if("${OS_ARCH}" STREQUAL "arm64")
    set(Rust_CARGO_TARGET "aarch64-unknown-linux-gnu")
  else()
    set(Rust_CARGO_TARGET "x86_64-unknown-linux-gnu")
  endif()
elseif("${OS_NAME}" STREQUAL "osx")
  if("${OSX_BUILD_ARCH}" STREQUAL "x86_64")
    set(Rust_CARGO_TARGET "x86_64-apple-darwin")
  elseif("${OSX_BUILD_ARCH}" STREQUAL "arm64" OR "${OS_ARCH}" STREQUAL "arm64")
    set(Rust_CARGO_TARGET "aarch64-apple-darwin")
  else()
    set(Rust_CARGO_TARGET "x86_64-apple-darwin")
  endif()
elseif(WIN32)
  if("${OS_ARCH}" STREQUAL "arm64" OR "${CMAKE_SYSTEM_PROCESSOR}" MATCHES
                                       "^(ARM64|arm64|aarch64)$")
    set(Rust_CARGO_TARGET "aarch64-pc-windows-msvc")
  else()
    set(Rust_CARGO_TARGET "x86_64-pc-windows-msvc")
  endif()
endif()

# A toolchain installed for the host has only the host's standard library, so
# a cross build needs its target added first.
find_program(RUSTUP rustup)
if(Rust_CARGO_TARGET AND RUSTUP)
  execute_process(COMMAND ${RUSTUP} target add ${Rust_CARGO_TARGET}
                  RESULT_VARIABLE RUSTUP_RESULT)
  if(NOT RUSTUP_RESULT EQUAL 0)
    message(WARNING "rustup could not add the ${Rust_CARGO_TARGET} target")
  endif()
endif()

add_subdirectory(corrosion EXCLUDE_FROM_ALL)
corrosion_import_crate(
  MANIFEST_PATH
  ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml
  CRATES
  orkidea-duckdb
  CRATE_TYPES
  staticlib
  PROFILE
  release
  LOCKED)

# DuckDB links the C runtime statically on Windows (CMAKE_MSVC_RUNTIME_LIBRARY
# is MultiThreaded), so the driver and the C code it builds (aws-lc) must too.
if(MSVC)
  corrosion_add_target_rustflags(orkidea_duckdb -Ctarget-feature=+crt-static)
endif()

include_directories(src/include)

set(EXTENSION_SOURCES
    src/orkidea_extension.cpp
    src/orkidea_adbc.cpp
    src/orkidea_execute.cpp
    src/orkidea_filter.cpp
    src/orkidea_insert.cpp
    src/orkidea_logging.cpp
    src/orkidea_modify.cpp
    src/orkidea_optimizer.cpp
    src/orkidea_partition.cpp
    src/orkidea_parameters.cpp
    src/orkidea_query.cpp
    src/orkidea_remote_query.cpp
    src/orkidea_scan.cpp
    src/orkidea_secrets.cpp
    src/orkidea_storage.cpp
    src/orkidea_util.cpp
    src/storage/orkidea_catalog.cpp
    src/storage/orkidea_schema_entry.cpp
    src/storage/orkidea_table_entry.cpp
    src/storage/orkidea_transaction.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The static extension links Corrosion's imported library target, which (unlike
# the interface target) can appear in DuckDB's export set.
set(ORKIDEA_PLATFORM_LIBS "")
if(APPLE)
  set(ORKIDEA_PLATFORM_LIBS "-framework CoreFoundation" "-framework Security")
endif()
target_link_libraries(${EXTENSION_NAME} orkidea_duckdb-static
                      ${ORKIDEA_PLATFORM_LIBS})
target_link_libraries(${LOADABLE_EXTENSION_NAME} orkidea_duckdb
                      ${ORKIDEA_PLATFORM_LIBS})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
