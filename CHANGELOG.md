# Changelog

All notable changes to the orkidea DuckDB extension are documented in this
file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/). Before
1.0, a minor release may change behaviour; such changes are listed under
"Behaviour changes".

## [Unreleased]

## [0.2.0] - 2026-11-10

Orkidea 0.2.0 runs aggregates in Oracle and binds parameters in
`orkidea_query` and `orkidea_execute`, and reads tables twice as fast. It is
built on the Orkidea ADBC driver 0.3.0, and tested against Oracle Database
23ai Free. A
[hands-on lab](lab/README.md) walks through every feature.

### Behaviour changes to read before upgrading

- **Aggregates over an attached table run in Oracle** -- @hc12r. Such a
  query now sends one `SELECT ... GROUP BY` to Oracle instead of reading
  every row. Results are the same and keep DuckDB's result types; `SET
  orkidea_aggregate_pushdown = false` restores the old plan.
- **A query whose result types change after planning fails** -- @hc12r.
  When a table changes between `PREPARE` and `EXECUTE`, or a placeholder in
  the select list runs as another type, the scan stops with an error naming
  the column, instead of reading the data with the planned layout.

### Queries

- **`GROUP BY`, `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, and `DISTINCT` are pushed
  into Oracle** -- @hc12r. This happens when the aggregate is directly over an
  attached table and every filter runs in Oracle. Sums are computed as
  declared `NUMBER` types, so they stay exact. Aggregates Oracle would compute
  differently (string orderings, LOBs, `DOUBLE` group keys, expressions,
  `FILTER`) stay in DuckDB, as does any query Oracle rejects. `EXPLAIN` shows
  the Oracle SQL.

- **A selective filter is read through one session** -- @hc12r. A
  parallel scan first takes a change number and plans ROWID ranges, which
  costs more than it saves when few rows come back. A scan now uses one
  session when its equality and `IN` filters leave fewer rows than
  `orkidea_parallel_scan_min_rows`, estimated from the columns' distinct
  values in Oracle's statistics; one customer's 50 orders out of a million
  take 49 ms instead of 180 ms. A number of sessions set with
  `orkidea_parallel_scan` is used as set.

### Oracle SQL

- **`orkidea_query` and `orkidea_execute` take `params`** -- @hc12r. A
  `STRUCT` binds by name, and a `row(...)` by position. A name used twice in
  SQL takes its value for every use, and PL/SQL binds each name once.
  Prepared statements bind new values on each `EXECUTE`.
- **`HUGEINT` parameters bind as `DECIMAL(38,0)`** -- @hc12r. A value that does
  not fit is an error, not a rounded number.
- **`TIMESTAMP WITH TIME ZONE` parameters are rejected** -- @hc12r, until the
  driver binds their time zone. The error shows how to pass the value with
  `FROM_TZ`.

### Driver

- **Built on the Orkidea ADBC driver 0.3.0** -- @hc12r. 0.3.0 decodes
  results from the network buffer straight into Arrow columns: copying a
  million rows of seven columns into DuckDB takes 0.63 s, against 1.11 s
  with driver 0.2.1, and 1.33 s against 3.29 s through one session. 0.2.0 read faster than
  0.1.0, and fixed wide rows that could break a connection, schemas
  described from a stale cache, quoted names, and `tnsnames.ora` aliases
  after a comment. See the release notes of
  [0.3.0](https://github.com/Tnsor-Labs/orkidea-adbc/releases/tag/v0.3.0) and
  [0.2.0](https://github.com/Tnsor-Labs/orkidea-adbc/releases/tag/v0.2.0).

### Documentation

- **Benchmarks** -- @hc12r. `bench/run.py` compares orkidea with DuckDB's
  `oracle` extension, and the Orkidea driver with python-oracledb, on the
  lab's data; [bench/README.md](bench/README.md) has the results.
- **A hands-on lab** -- @hc12r. `lab/setup.sh` starts Oracle Database Free in a
  container with seeded data, and nine modules cover exploring, pushdown,
  aggregates, parallel scans, writes, Oracle SQL with parameters, federation,
  connections, and the driver from Python.

## [0.1.0] - 2026-11-10

The first release: secrets, `ATTACH` with filter, projection, `LIMIT`, and
top-N pushdown, parallel snapshot scans, writes inside DuckDB transactions,
`orkidea_query`, and `orkidea_execute`.

[Unreleased]: https://github.com/Tnsor-Labs/duckdb-orkidea/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/Tnsor-Labs/duckdb-orkidea/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Tnsor-Labs/duckdb-orkidea/releases/tag/v0.1.0
