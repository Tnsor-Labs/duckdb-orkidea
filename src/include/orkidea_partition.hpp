//===----------------------------------------------------------------------===//
//                         Orkidea DuckDB extension
//
// orkidea_partition.hpp
//
// Splits a table into ROWID ranges, so that several sessions can read it at
// once, each reading only its share of the table's blocks.
//
// The ranges are cut at ROWIDs: the first range has no lower bound and the
// last no upper bound, so whatever the cut points, every row falls into
// exactly one range. Good cut points only make the ranges even. They come
// from the table's extents (DBA_EXTENTS), or, for a user who cannot read
// those, from the block range between the table's first and last row.
//===----------------------------------------------------------------------===//

#pragma once

#include "orkidea_adbc.hpp"

namespace duckdb {
namespace orkidea {

struct OracleTable;

struct RowidRanges {
	//! Oracle conditions on ROWID, one per range, together covering every row.
	//! Empty when the table cannot be split.
	static vector<string> Plan(ClientContext &context, shared_ptr<Connection> connection, const OracleTable &table,
	                           idx_t count);
};

//! The database's current system change number, for reading several sessions
//! AS OF one moment. Invalid when the user may not call DBMS_FLASHBACK.
optional_idx CurrentScn(ClientContext &context, shared_ptr<Connection> connection);

} // namespace orkidea
} // namespace duckdb
