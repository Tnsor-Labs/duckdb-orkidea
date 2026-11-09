cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME orkidea)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Orkidea driver is Rust, built by Corrosion as a static library and
# linked into the extension. Build it for the platform DuckDB is built for,
# which in cross builds (macOS x86_64 on arm64, for example) is not the host.
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
  if("${OS_ARCH}" STREQUAL "arm64")
    set(Rust_CARGO_TARGET "aarch64-pc-windows-msvc")
  else()
    set(Rust_CARGO_TARGET "x86_64-pc-windows-msvc")
  endif()
endif()

add_subdirectory(corrosion EXCLUDE_FROM_ALL)
corrosion_import_crate(
  MANIFEST_PATH ${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml
  CRATES orkidea-duckdb
  CRATE_TYPES staticlib
  PROFILE release
  LOCKED)

include_directories(src/include)

set(EXTENSION_SOURCES src/orkidea_extension.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} orkidea_duckdb)
target_link_libraries(${LOADABLE_EXTENSION_NAME} orkidea_duckdb)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
