# Orkidea for DuckDB

A DuckDB extension for Oracle Database, built on the
[Orkidea ADBC driver](https://github.com/Tnsor-Labs/orkidea-adbc). It is not
affiliated with, sponsored by, or endorsed by Oracle Corporation.

**Status:** in development. It is not yet in the DuckDB community extensions
repository, and its SQL interface may change before the first release.

- No Oracle Instant Client, OCI, or ODBC: the driver speaks Oracle's network
  protocol itself and is linked into the extension.
- Results stream into DuckDB as Arrow record batches, with exact types:
  `NUMBER(p,s)` as `DECIMAL(p,s)`, `TIMESTAMP WITH TIME ZONE` as
  `TIMESTAMPTZ`, `DATE` as `TIMESTAMP_S`, LOBs, `RAW`, intervals, `BOOLEAN`,
  `JSON` as `JSON`, and `VECTOR` as a fixed-size array.
- Targets DuckDB v2.0.0 on Linux, macOS, and Windows, on x86_64 and arm64.

## Usage

Store the connection in a secret:

```sql
LOAD orkidea;

CREATE SECRET ora (
    TYPE orkidea,
    HOST 'db.example.com', PORT 1521, SERVICE 'FREEPDB1',
    USER 'scott', PASSWORD '...'
);
```

### Attaching a database

Attach the database, and its tables and views read like DuckDB tables:

```sql
ATTACH 'ora' AS oracle (TYPE orkidea, READ_ONLY);

SELECT region, sum(amount) FROM oracle.orders JOIN oracle.hr.regions USING (region_id) GROUP BY ALL;
SHOW ALL TABLES;
```

- The path is a secret name or an `oracle://` URI. `SECRET 'name'` works
  too, with an empty path.
- Oracle schemas (users) are DuckDB schemas. The default is the connected
  user's; `SCHEMA 'HR'` limits the catalog to one. Oracle's own schemas
  (`SYS`, `XDB`, ...) are left out of listings but can be named.
- Names match case-insensitively. An exact match comes first, then the
  unquoted (upper-case) Oracle name.
- Tables are described on first use and cached until `DETACH`.
- Each DuckDB transaction runs on one Oracle connection. Writing through the
  attached database is not supported yet; use `orkidea_execute`.

Scans read only the columns a query needs, and push work into Oracle when
Oracle gives exactly the result DuckDB would:

- **Filters**: comparisons, `IN`, `BETWEEN`, `IS [NOT] NULL`, `IS [NOT]
  DISTINCT FROM`, `LIKE 'prefix%'`, boolean columns, `AND`, `OR`, and `NOT`,
  on numbers, strings, dates, timestamps, `RAW`, and `BOOLEAN`. Some cases
  stay in DuckDB, which evaluates them on the rows Oracle returns:
  - Oracle stores an empty string as NULL, so filters with `''` aren't sent.
  - `CHAR` compares blank-padded, so only its NULL checks are sent.
  - String ranges are sent only against ASCII constants.
  - `TIMESTAMP WITH LOCAL TIME ZONE` depends on the session time zone.
- **LIMIT** becomes `FETCH FIRST n ROWS ONLY`, and **ORDER BY ... LIMIT**
  sends the order too, with explicit `NULLS FIRST`/`LAST`. Either one is sent
  only when every filter on the scan runs in Oracle, and an order only on
  types Oracle sorts as DuckDB does. Strings sort the same only in an
  AL32UTF8 database.

### Parallel scans

A table with 500,000 rows or more (by Oracle's optimizer statistics) is read
through several sessions at once: as many as DuckDB has threads, up to 8.
Each session reads ranges of the table's ROWIDs, so together they read the
table once. All of them read `AS OF SCN` one change number taken first, so
the result is one consistent snapshot. Batches keep ROWID order, so
`CREATE TABLE ... AS` and `INSERT ... SELECT` keep the order a single
session would give. On a 2,000,000-row table, copying it into DuckDB took
2.5 s through four sessions instead of 5.9 s through one.

```sql
SET orkidea_parallel_scan = 4;               -- sessions per scan; 0 or 1 for one, -1 (default) to choose
SET orkidea_parallel_scan_min_rows = 100000; -- default 500000
```

The cut points come from the table's extents in `DBA_EXTENTS`. Without
access to it, they are spread over the block range between the first and
the last row, which balances the work less well. A table is read through
one session, without a warning, when:

- it is a view, or an index-organized, clustered, or temporary table;
- the query has a `LIMIT` or `ORDER BY ... LIMIT` that Oracle runs;
- the user cannot call `DBMS_FLASHBACK` (`GRANT EXECUTE ON DBMS_FLASHBACK`);
- Oracle refuses the flashback read: no `FLASHBACK` privilege on another
  user's table, or a table created or altered within the last few seconds
  (ORA-01466).

A scan that runs longer than the database's undo retention can fail with
ORA-01555.

To see the SQL sent to Oracle:

```sql
CALL enable_logging('OrkideaQuery', level = 'debug', storage = 'memory');
SELECT message FROM duckdb_logs WHERE type = 'OrkideaQuery';
```

### Federated queries

An attached Oracle database is one catalog among others. A query can join it
with Postgres, MySQL, SQLite, DuckDB files, and Parquet, CSV, JSON, Iceberg,
or Delta data, local or in object storage. Each scan pushes down what its
source can run, and DuckDB joins the results:

```sql
INSTALL postgres;
CREATE SECRET pg (TYPE postgres, HOST 'pg.example.com', DATABASE 'ops', USER 'app', PASSWORD '...');
ATTACH '' AS pg (TYPE postgres, SECRET pg);

-- Oracle orders and customers, Postgres shipments and reviews
SELECT carrier, s.late, avg(r.rating) AS rating, sum(o.amount) AS order_value
FROM pg.logistics.reviews r
JOIN pg.logistics.shipments s USING (order_id)
JOIN oracle.orders o ON o.order_id = r.order_id
GROUP BY ALL;

-- copy from Oracle into Postgres in one statement
CREATE TABLE pg.analytics.vip_customers AS
    SELECT customer_id, full_name, country FROM oracle.customers WHERE vip;
```

### Running Oracle SQL

Run a query on Oracle and use its rows like any table:

```sql
SELECT * FROM orkidea_query('ora', 'SELECT * FROM emp WHERE deptno = 10');

CREATE TABLE emp AS FROM orkidea_query('ora', 'SELECT * FROM emp');
```

Quote Oracle SQL that contains quotes with `$$`:

```sql
FROM orkidea_query('ora', $$SELECT ename FROM emp WHERE job = 'CLERK'$$);
```

Run DDL, DML, or PL/SQL. The result is the number of rows affected:

```sql
FROM orkidea_execute('ora', 'UPDATE emp SET sal = sal * 1.1 WHERE deptno = 10');
```

Each call opens its own connection, which commits on success.
`orkidea_execute` runs when the query executes, never when it is only bound,
so `EXPLAIN` and prepared statements don't run it.

In place of a secret name, both functions take an
`oracle://user:password@host:port/service` URI. Prefer secrets: their
passwords never show in `duckdb_secrets()` or in query text.

### TNS names and wallets

`CONNECT_STRING` takes a `tnsnames.ora` alias. The file is read from
`CONFIG_DIR`, or from `$TNS_ADMIN` when `CONFIG_DIR` is not set; `IFILE`
includes are followed.

```sql
CREATE SECRET sales (
    TYPE orkidea,
    CONNECT_STRING 'sales_prod', CONFIG_DIR '/etc/oracle',
    USER 'scott', PASSWORD '...'
);
```

A full descriptor works as well, without a file:

```sql
CREATE SECRET sales (
    TYPE orkidea,
    CONNECT_STRING '(DESCRIPTION=(ADDRESS=(PROTOCOL=TCP)(HOST=db)(PORT=1521))(CONNECT_DATA=(SERVICE_NAME=SALES)))',
    USER 'scott', PASSWORD '...'
);
```

For Autonomous Database, unzip the wallet, point `CONFIG_DIR` and
`WALLET_LOCATION` at it, and use one of the aliases in its `tnsnames.ora`
(for example `mydb_high`). The driver reads the PEM wallet (`ewallet.pem`);
`cwallet.sso` and `ewallet.p12` are not used.

## Secret parameters

| Parameter | Description |
| --- | --- |
| `HOST`, `PORT`, `SERVICE` | Where to connect, as the Easy Connect string `host:port/service`. |
| `PROTOCOL` | `TCP` (default), or `TCPS` for TLS. |
| `CONNECT_STRING` | In place of `HOST`: an Easy Connect string, a full TNS descriptor, or a `tnsnames.ora` alias. |
| `URI` | In place of the above: `oracle://user:password@host:port/service` or `oracle+tcps://...`. |
| `USER` (or `USERNAME`), `PASSWORD` | Credentials. They override those in `URI`. |
| `CONFIG_DIR` | Directory with `tnsnames.ora`. Defaults to `$TNS_ADMIN`. |
| `WALLET_LOCATION`, `WALLET_PASSWORD` | PEM wallet (`ewallet.pem`) for TLS and mutual TLS, as for Autonomous Database. |
| `AUTH_MODE` | Privileged connection: `SYSDBA`, `SYSOPER`, and so on. |
| `CONNECT_TIMEOUT_MS`, `CALL_TIMEOUT_MS` | Network connect timeout, and timeout per round trip. |
| `STMT_CACHE_SIZE` | Statement cache size (default 20). |
| `TIME_ZONE` | Session time zone, which determines how `TIMESTAMP WITH LOCAL TIME ZONE` values read. |
| `EDITION` | Edition for edition-based redefinition. |

`PASSWORD`, `WALLET_PASSWORD`, and `URI` are redacted in `duckdb_secrets()`.
Connecting is refused when `enable_external_access` is off.

Types map as in the driver; see its
[type support](https://github.com/Tnsor-Labs/orkidea-adbc/blob/main/docs/type-support.md).

## Building

You need CMake, Ninja, a C++17 compiler, and Rust 1.89 or newer. Corrosion
builds the driver from `rust/` and links it into the extension.

```sh
git clone --recurse-submodules https://github.com/Tnsor-Labs/duckdb-orkidea
cd duckdb-orkidea
GEN=ninja make release
```

This builds `build/release/duckdb` with the extension linked in, and the
loadable `build/release/extension/orkidea/orkidea.duckdb_extension`.

## Testing

```sh
make test
```

The tests in `test/sql/query.test` run only with a database they may create
tables in:

```sh
ORKIDEA_TEST_URI=oracle://user:password@localhost:1521/FREEPDB1 make test
```

## Updating DuckDB

Move the `duckdb` submodule to the new release tag, `extension-ci-tools` to
its matching branch, and the versions in
`.github/workflows/MainDistributionPipeline.yml` to match. Extensions build
against DuckDB's internal C++ API, which can change between releases.

## License

Apache License 2.0. See [LICENSE](LICENSE).
