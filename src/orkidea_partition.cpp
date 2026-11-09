#include "orkidea_partition.hpp"

#include "orkidea_util.hpp"
#include "storage/orkidea_table_entry.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace orkidea {

namespace {

//! The extents of the table's own segments (the table, or its partitions),
//! in ROWID order, each with the ROWID of its first block.
string ExtentsQuery(const OracleTable &table) {
	return "SELECT ROWIDTOCHAR(DBMS_ROWID.ROWID_CREATE(1, o.data_object_id, e.relative_fno, e.block_id, 0)), e.blocks"
	       " FROM dba_extents e JOIN dba_objects o"
	       "   ON o.owner = e.owner AND o.object_name = e.segment_name"
	       "  AND DECODE(o.subobject_name, e.partition_name, 1, 0) = 1 AND o.object_type LIKE 'TABLE%'"
	       " WHERE e.owner = " +
	       QuoteLiteral(table.schema) + " AND e.segment_name = " + QuoteLiteral(table.name) +
	       "   AND e.segment_type LIKE 'TABLE%'"
	       " ORDER BY o.data_object_id, e.relative_fno, e.block_id";
}

//! Cut points between `count` ranges of extents with about as many blocks each.
vector<string> CutsFromExtents(const vector<vector<Value>> &extents, idx_t count) {
	idx_t total = 0;
	for (auto &extent : extents) {
		total += extent[1].GetValue<idx_t>();
	}
	vector<string> cuts;
	idx_t before = 0;
	for (auto &extent : extents) {
		// An extent starts a new range when the blocks before it reach the
		// next range's share.
		if (before * count >= (cuts.size() + 1) * total && before > 0) {
			cuts.push_back(extent[0].ToString());
		}
		before += extent[1].GetValue<idx_t>();
	}
	return cuts;
}

//! Cut points spread evenly over the blocks between the first and the last
//! row. Only for a table in one segment of one file, which ROWID order lays
//! out by block number.
string InterpolatedCutsQuery(const OracleTable &table, idx_t count) {
	auto parts = to_string(count);
	return "WITH m AS (SELECT MIN(ROWID) lo, MAX(ROWID) hi FROM " + table.QualifiedName() +
	       ")"
	       " SELECT ROWIDTOCHAR(DBMS_ROWID.ROWID_CREATE(1, DBMS_ROWID.ROWID_OBJECT(lo),"
	       "   DBMS_ROWID.ROWID_RELATIVE_FNO(lo), DBMS_ROWID.ROWID_BLOCK_NUMBER(lo) + FLOOR(level *"
	       "   (DBMS_ROWID.ROWID_BLOCK_NUMBER(hi) - DBMS_ROWID.ROWID_BLOCK_NUMBER(lo) + 1) / " +
	       parts +
	       "), 0))"
	       " FROM m WHERE DBMS_ROWID.ROWID_OBJECT(lo) = DBMS_ROWID.ROWID_OBJECT(hi)"
	       "   AND DBMS_ROWID.ROWID_RELATIVE_FNO(lo) = DBMS_ROWID.ROWID_RELATIVE_FNO(hi)"
	       " CONNECT BY level < " +
	       parts;
}

string RowidLiteral(const string &rowid) {
	return "CHARTOROWID(" + QuoteLiteral(rowid) + ")";
}

} // namespace

vector<string> RowidRanges::Plan(ClientContext &context, shared_ptr<Connection> connection, const OracleTable &table,
                                 idx_t count) {
	vector<string> cuts;
	try {
		cuts = CutsFromExtents(QueryRows(context, connection, ExtentsQuery(table)), count);
	} catch (std::exception &) {
		// DBA_EXTENTS is not readable without SELECT_CATALOG_ROLE or similar.
	}
	if (cuts.empty()) {
		try {
			for (auto &row : QueryRows(context, connection, InterpolatedCutsQuery(table, count))) {
				cuts.push_back(row[0].ToString());
			}
		} catch (std::exception &) {
			// A table without ROWIDs to read, such as an external table.
		}
	}
	// Equal cut points would make empty ranges.
	cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
	if (cuts.empty()) {
		return {};
	}
	vector<string> ranges;
	ranges.push_back("ROWID < " + RowidLiteral(cuts.front()));
	for (idx_t i = 1; i < cuts.size(); i++) {
		ranges.push_back("ROWID >= " + RowidLiteral(cuts[i - 1]) + " AND ROWID < " + RowidLiteral(cuts[i]));
	}
	ranges.push_back("ROWID >= " + RowidLiteral(cuts.back()));
	return ranges;
}

optional_idx CurrentScn(ClientContext &context, shared_ptr<Connection> connection) {
	try {
		auto rows = QueryRows(context, std::move(connection),
		                      "SELECT TO_CHAR(DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER) FROM dual");
		return optional_idx(std::stoull(rows[0][0].ToString()));
	} catch (std::exception &) {
		return optional_idx();
	}
}

} // namespace orkidea
} // namespace duckdb
