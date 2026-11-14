# Changelog

All notable changes to the orkidea DuckDB extension are documented in this
file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows [Semantic Versioning](https://semver.org/). Before
1.0, a minor release may change behaviour; such changes are listed under
"Behaviour changes".

## [Unreleased]

### Queries

- **Joins run in Oracle** -- @hc12r. A join of tables of one attached
  database, in any schemas, becomes one Oracle query, with the filters
  pushed into each table: inner and outer joins, and the semi and anti
  joins of `IN` and `[NOT] EXISTS`, on comparisons Oracle makes as DuckDB
  does. An aggregate or `DISTINCT` over the join goes with it. A join runs
  in Oracle when it, or the aggregate over it, is estimated to give fewer
  rows than half of its largest table, or without Oracle statistics;
  others are faster in DuckDB, which reads the tables in parallel. On the
  lab's 1,000,000 orders, total sales by region over three tables take
  0.18 s instead of 0.35 s, and 0.20 s instead of 0.57 s with 10 ms between
  DuckDB and Oracle. `SET orkidea_join_pushdown = false` turns it off. A
  test compares every join's result with DuckDB's.
- **`NUMBER` join keys compare exactly** -- @hc12r. DuckDB casts a
  `NUMBER(19)` key to `DOUBLE` to join it with a `NUMBER` without a
  precision, read as `DOUBLE`; in Oracle, the cast is left out and the
  numbers compare exactly, so keys beyond 2^53 that differ no longer match.
- **`IN` lists longer than 1,000 values run in Oracle** -- @hc12r. They
  stayed in DuckDB, as Oracle takes at most 1,000 values per list; up to
  10,000 are now sent as lists of 1,000 joined by `OR`. That includes the
  list of keys a join with a DuckDB table or file filters the Oracle table
  with, which has up to `dynamic_or_filter_threshold` keys: with 3,000
  receipts from a CSV file out of 100,000 rows and the threshold at 3,000,
  the query takes 0.06 s instead of 0.21 s with 10 ms between DuckDB and
  Oracle.
- **The range of a join's keys runs in Oracle** -- @hc12r. When a join with
  a DuckDB table or file has more keys than that, DuckDB filters the Oracle
  table with a summary of them, which was not sent; for integer, decimal,
  date, and timestamp keys, their smallest and largest are now, as
  `key >= min AND key <= max`.
- **Filters on `FLOAT(p)` columns run in Oracle** -- @hc12r. A `FLOAT` is a
  `NUMBER`, read as `DOUBLE`, and is now filtered as one.

### Catalog

- **`COLUMNS 'dictionary'` lists tables from the data dictionary** --
  @hc12r. With the `ATTACH` option, listings read the columns of a
  schema's tables with one dictionary query instead of describing them 16
  to a query; views, and tables the dictionary cannot describe exactly
  (`VECTOR`, `XMLTYPE`, object types, virtual columns declared with a
  precision), are still described. Statements describe the tables they
  use either way. With 400 tables, on the lab's machine, listing takes
  1.9 s instead of 4.6 s with Oracle's shared pool flushed, and 0.8 s
  instead of 1.6 s with 50 ms between DuckDB and Oracle once Oracle has the
  plans. A test checks that both modes list every table of the test schema
  with the same columns. Built on the Orkidea driver's
  `oracle.table_schemas`, in its 0.5.0.
- **An altered table stays listed** -- @hc12r. After an `ALTER TABLE` from
  DuckDB, the table was left out of `duckdb_tables()` and
  `duckdb_columns()` until a statement used it; a renamed one, under its
  new name too.

## [0.3.1] - 2026-11-14

Orkidea 0.3.1 describes listed tables 16 to a query instead of 50, which
Oracle plans in a third of the time.

### Catalog

- **Listings describe tables 16 to a query** -- @hc12r. They described up
  to 50, but Oracle takes much longer to plan a join the more tables it
  has: about 5 ms a table up to 20 tables, 15 ms at 50, and 60 ms at 100,
  though the query returns no rows. With 456 tables, on the lab's machine
  and with Oracle's shared pool flushed, `duckdb_columns()` takes 7.5 s
  instead of 13 s, of which planning the batches takes 3.3 s instead of
  9 s. Listing again, once Oracle has the plans, takes 0.2 s, and 1 s with
  20 ms between DuckDB and Oracle.

## [0.3.0] - 2026-11-13

Orkidea 0.3.0 lists attached databases without holding up other queries,
and describes their tables 50 to a query, so the DuckDB UI's sidebar no
longer stalls a session. It is built on the Orkidea ADBC driver 0.4.1, adds
macOS x86_64 and Windows ARM64, and is tested against Oracle Database 23ai
Free.

### Behaviour changes to read before upgrading

- **`TIMESTAMP WITH LOCAL TIME ZONE` columns read as `TIMESTAMPTZ`** --
  @hc12r. They were `TIMESTAMP`, the wall-clock time in the database time
  zone; they are now the instant, as `TIMESTAMP WITH TIME ZONE` columns
  are. Where `DBTIMEZONE` is `+00:00` only the type changes. This follows
  the Orkidea driver 0.4.
- **`RETURNING` gives `DATE` and `TIMESTAMP` values as stored** -- @hc12r.
  They were shifted by the client's offset from UTC (with the client at
  `+02:00`, `DATE '2000-01-01'` came back as `1999-12-31 22:00:00`), and
  `TIMESTAMP` values lost their fractional seconds. Clients in UTC saw only
  the fractions go. Fixed in the driver.

### Catalog

- **Listing an attached database no longer holds up other queries** --
  @hc12r. `duckdb_tables()`, `duckdb_columns()`, and `information_schema`,
  which the DuckDB UI runs over every attached database when it shows its
  sidebar, described each table in turn, with a lock held that made any
  statement on the same schema wait until the whole listing was done. The
  lock is no longer held while Oracle is queried.
- **Listings describe tables 50 to a query** -- @hc12r. One query, which
  returns no rows, describes up to 50 tables, where each took a round trip
  of its own, and the primary and unique keys `duckdb_tables()` reports are
  read with one query per schema instead of one per table. A table that
  cannot be described, such as an invalid view, is found by describing the
  batch in halves, and left out as before. With 456 tables, on the lab's
  machine, `duckdb_columns()` takes 13 s instead of 36 s, and 7 s instead of
  about two minutes with 20 ms between DuckDB and Oracle; `duckdb_tables()`
  takes 16 s where it took several minutes.

### Queries

- **Filters on `TIMESTAMP WITH LOCAL TIME ZONE` run in Oracle** -- @hc12r.
  Now that the column reads as an instant, comparisons with a
  `TIMESTAMPTZ` are sent, as for `TIMESTAMP WITH TIME ZONE`, and so are
  `ORDER BY ... LIMIT` on it.

### Platforms

- **macOS x86_64 and Windows ARM64** -- @hc12r. Both build in the
  distribution pipeline: macOS x86_64 cross-compiled on arm64 runners, and
  Windows ARM64 with the driver's C code (aws-lc) compiled by clang-cl.

### Driver

- **Built on the Orkidea ADBC driver 0.4.1** -- @hc12r. Arrow 60,
  `oracledb` 26.0.0-beta.6, every column type decoded straight into Arrow
  columns (intervals, `ROWID`, `JSON`, `VECTOR`, and `NCLOB` included), and
  `JSON` written as text as it is decoded, in about half the time. See the
  release notes of
  [0.4.1](https://github.com/Tnsor-Labs/orkidea-adbc/releases/tag/v0.4.1) and
  [0.4.0](https://github.com/Tnsor-Labs/orkidea-adbc/releases/tag/v0.4.0).

### Testing

- **Listings are tested against describing each table** -- @hc12r.
  `test/sql/attach_listing.test` compares the columns a listing reports with
  those of tables described one by one, with an invalid view, columns whose
  names differ only in case, and a table created after the listing.

## [0.2.0] - 2026-11-11

Orkidea 0.2.0 runs aggregates in Oracle, binds parameters in
`orkidea_query` and `orkidea_execute`, and reads tables twice as fast. It is
built on the Orkidea ADBC driver 0.3.0 and tested against Oracle Database
23ai Free. A [hands-on lab](lab/README.md) walks through every feature.

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
  with driver 0.2.1, and 1.33 s against 3.29 s through one session. 0.2.0
  read faster than 0.1.0, and fixed wide rows that could break a
  connection, schemas described from a stale cache, quoted names, and
  `tnsnames.ora` aliases after a comment. See the release notes of
  [0.3.0](https://github.com/Tnsor-Labs/orkidea-adbc/releases/tag/v0.3.0) and
  [0.2.0](https://github.com/Tnsor-Labs/orkidea-adbc/releases/tag/v0.2.0).

### Testing

- **CI runs against Oracle** -- @hc12r. A workflow starts Oracle Database
  Free 23ai, runs the tests that need a database, and runs every lab module,
  so the lab is checked as users run it.

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

[Unreleased]: https://github.com/Tnsor-Labs/duckdb-orkidea/compare/v0.3.1...HEAD
[0.3.1]: https://github.com/Tnsor-Labs/duckdb-orkidea/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/Tnsor-Labs/duckdb-orkidea/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Tnsor-Labs/duckdb-orkidea/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Tnsor-Labs/duckdb-orkidea/releases/tag/v0.1.0
