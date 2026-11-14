# Orkidea for DuckDB

A DuckDB extension for Oracle Database, built on the
[Orkidea ADBC driver](https://github.com/Tnsor-Labs/orkidea-adbc). It is not
affiliated with, sponsored by, or endorsed by Oracle Corporation.

**Status:** version 0.2.0 (see the [changelog](CHANGELOG.md)). Once it is published in the DuckDB community
extensions repository, install it with:

```sql
INSTALL orkidea FROM community;
LOAD orkidea;
```

Until then, build it from source (see [Building](#building)).

**Try it:** the [lab](lab/README.md) starts an Oracle database in a container
and walks through every feature below, from a first query to writes, bind
parameters, parallel scans, and Python, in short modules with expected results.
[Benchmarks](bench/README.md) on the lab's data compare it with DuckDB's
`oracle` extension: a million-row table copies into DuckDB in 0.63 s instead
of 12.6 s, and a `GROUP BY` over it runs in 0.19 s instead of 5.3 s.

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
- Listings (`duckdb_tables()`, `duckdb_columns()`, `information_schema`, and
  the DuckDB UI's sidebar) describe a schema's tables 16 to a query.
  `COLUMNS 'dictionary'` reads their columns from Oracle's data dictionary
  instead, with one query per schema, which helps with many tables or a
  distant database. Views, and tables whose columns the dictionary cannot
  describe exactly (`VECTOR`, `XMLTYPE`, object types, and virtual columns
  declared with a precision), are still described. Only listings change:
  a statement describes the tables it uses, as with the default,
  `COLUMNS 'describe'`.
- Each DuckDB transaction runs on one Oracle connection. Tables can be
  created, filled, and dropped; see [Writing](#writing).

Scans read only the columns a query needs, and push work into Oracle when
Oracle gives exactly the result DuckDB would:

- **Filters**: comparisons, `IN`, `BETWEEN`, `IS [NOT] NULL`, `IS [NOT]
  DISTINCT FROM`, `LIKE 'prefix%'`, boolean columns, `AND`, `OR`, and `NOT`,
  on numbers, strings, dates, timestamps, `RAW`, and `BOOLEAN`. Some cases
  stay in DuckDB, which evaluates them on the rows Oracle returns:
  - Oracle stores an empty string as NULL, so filters with `''` aren't sent.
  - `CHAR` compares blank-padded, so only its NULL checks are sent.
  - String ranges are sent only against ASCII constants.
  - `IN` lists of up to 10,000 values are sent, as lists of 1,000 joined by
    `OR` (Oracle's limit per list); longer ones stay in DuckDB.
- **Join keys from DuckDB**: a join of an Oracle table with a DuckDB table or
  file, as `WHERE t.receipt IN (SELECT rcx FROM 'file.csv')`, reads the
  other side first, and the scan of the Oracle table gets a filter of its
  keys: their list, up to `dynamic_or_filter_threshold` keys (50 by
  default; `SET dynamic_or_filter_threshold = 10000` sends up to 10,000), or
  else, for integer, decimal, date, and timestamp keys, their range
  (`"ID" >= 40001 AND "ID" <= 40991`). String keys past the threshold read
  the whole table: their range is not known. With 3,000 receipts out of
  100,000 rows and the threshold at 3,000, the query took 0.06 s instead of
  0.21 s with 10 ms between DuckDB and Oracle.
- **LIMIT** becomes `FETCH FIRST n ROWS ONLY`, and **ORDER BY ... LIMIT**
  sends the order too, with explicit `NULLS FIRST`/`LAST`. Either one is sent
  only when every filter on the scan runs in Oracle, and an order only on
  types Oracle sorts as DuckDB does. Strings sort the same only in an
  AL32UTF8 database.
- **Aggregates**: `GROUP BY`, `SELECT DISTINCT`, and `count(*)`,
  `count`, `count(DISTINCT ...)`, `sum`, `avg`, `min`, and `max` directly
  over a table become one Oracle query, so only the groups come back.
  `HAVING`, `ORDER BY`, and joins above it stay in DuckDB. Every group key
  and argument must be a column, and every filter on the table must run in
  Oracle. Results have DuckDB's types: Oracle's sums are cast to
  `NUMBER(38,s)`, so they are exact. An `avg`, or a `sum` of `DOUBLE`
  values, can differ from DuckDB's in the last digits: Oracle computes it
  exactly and rounds once. The aggregate stays in DuckDB for:
  - expressions (`sum(a * b)`), `FILTER (WHERE ...)`, ordered aggregates,
    and functions Oracle lacks (`median`, `list`, ...);
  - `min` and `max` of strings that sort differently (as for `ORDER BY`),
    and LOBs;
  - group keys that are unconstrained `NUMBER` columns read as `DOUBLE`,
    where distinct numbers can round to one double.

  `EXPLAIN` shows the Oracle SQL. `SET orkidea_aggregate_pushdown = false`
  turns this off. On a 10,000,000-row table, a `GROUP BY` with 50 groups
  took 1.5 s in Oracle instead of 3.2 s reading every row into DuckDB, over
  a local connection; over a network, the rows not sent count for more.
- **Joins** of tables of one attached database, in any schemas, become one
  Oracle query: inner, outer (`LEFT`, `RIGHT`, `FULL`), and the semi and
  anti joins of `IN` and `[NOT] EXISTS`, on `=`, `<>`, `<`, `<=`, `>`,
  `>=`, and `IS NOT DISTINCT FROM`. An aggregate or `DISTINCT` over the
  join goes with it, so only the groups come back. Each table brings its
  pushed filters. A join runs in Oracle when DuckDB estimates it, or the
  aggregate over it, gives fewer rows than half of its largest table, and
  when Oracle has no statistics to estimate with; a join that keeps as many
  rows is faster in DuckDB, which reads the tables in parallel. Keys compare
  as Oracle compares them:
  - `NUMBER` keys compare exactly. DuckDB reads a `NUMBER` without a
    precision as `DOUBLE`, and casts a `NUMBER(19)` key to `DOUBLE` to join
    it with one, so keys beyond 2^53 that differ, which DuckDB finds equal,
    don't match in Oracle.
  - Strings join on `=` in any character set, and on ranges as `VARCHAR2`
    in an AL32UTF8 database. `DATE` and `TIMESTAMP` join with each other,
    `TIMESTAMP WITH [LOCAL] TIME ZONE` with each other, and `RAW`,
    `BOOLEAN`, and binary floating point with their own kind.
  - The join stays in DuckDB for `CHAR` keys (Oracle compares them
    blank-padded), expression keys (`a.id + 1 = b.id`), filters DuckDB
    applies (`ILIKE`), DuckDB tables, and tables of two attached databases:
    attach once, without `SCHEMA`, to join tables of several schemas.

  `EXPLAIN` shows the Oracle SQL. `SET orkidea_join_pushdown = false` turns
  this off. On the lab's 1,000,000 orders, total sales by region, over
  orders, customers, and regions, take 0.18 s instead of 0.35 s, and 0.20 s
  instead of 0.57 s with 10 ms between DuckDB and Oracle.

### Parallel scans

A table with 500,000 rows or more (by Oracle's optimizer statistics) is read
through several sessions at once: as many as DuckDB has threads, up to 8.
Each session reads ranges of the table's ROWIDs, so together they read the
table once. All of them read `AS OF SCN` one change number taken first, so
the result is one consistent snapshot. Batches keep ROWID order, so
`CREATE TABLE ... AS` and `INSERT ... SELECT` get the rows in ROWID order.
One session reads a table in the order of its extents, which is ROWID order
when the extents lie in block order, as they usually do; without `ORDER BY`,
Oracle promises neither. On a 2,000,000-row table, copying it into DuckDB
took 2.5 s through four sessions instead of 5.9 s through one.

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
- its equality and `IN` filters leave fewer rows than
  `orkidea_parallel_scan_min_rows`, estimated from the columns' distinct
  values in Oracle's statistics, and the number of sessions is left to the
  scan;
- the user cannot call `DBMS_FLASHBACK` (`GRANT EXECUTE ON DBMS_FLASHBACK`);
- Oracle refuses the flashback read: no `FLASHBACK` privilege on another
  user's table, or a table created or altered within the last few seconds
  (ORA-01466).

A scan that runs longer than the database's undo retention can fail with
ORA-01555.

### Writing

```sql
CREATE TABLE oracle.items (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, price DECIMAL(10,2));
INSERT INTO oracle.items VALUES (1, 'tea', 3.50);
INSERT INTO oracle.items (id, name) SELECT id, name FROM 'items.parquet';
CREATE TABLE oracle.shipments AS FROM pg.logistics.shipments;   -- Postgres to Oracle
DROP TABLE oracle.items;
```

Rows go to Oracle through the driver's bulk ingest (array DML), in batches
of up to 1,048,576 rows: 900,000 rows from Postgres into a new Oracle table
took 8.3 s here. Each batch is applied whole or not at all, and a failed
statement rolls back every row it inserted.

- **Transactions.** A DuckDB transaction's writes are one Oracle
  transaction, on one session: `COMMIT` commits them, `ROLLBACK` undoes them,
  and the transaction's own queries see them (through that session, so they
  are not read in parallel). A statement outside `BEGIN` commits on its own.
- **Columns you do not name** in an `INSERT` are left to Oracle, so its
  `DEFAULT` values and identity columns apply. Virtual columns are always
  left to Oracle. (A defaulted column given NULL in every row of a batch
  also takes its default.)
- **Names.** DuckDB names that are valid unquoted Oracle names become
  upper-case Oracle names (`items` → `ITEMS`); others are kept exactly.
  Existing Oracle tables, mixed-case ones included, are written to by their
  exact names.
- **Types** of new tables follow the driver's
  [mapping](https://github.com/Tnsor-Labs/orkidea-adbc/blob/main/docs/type-support.md#column-types-for-create):
  `VARCHAR` becomes `VARCHAR2(4000)`, `BIGINT` becomes `NUMBER(19)` (which
  reads back as `DECIMAL(19,0)`), `TIMESTAMPTZ` becomes `TIMESTAMP WITH
  TIME ZONE`, and so on. `CREATE TABLE` carries `NOT NULL`, `PRIMARY KEY`,
  and `UNIQUE`; for other constraints or defaults, create the table with
  `orkidea_execute`.
- **DDL.** Oracle commits DDL immediately, so `CREATE` and `DROP` run on a
  session of their own and do not commit your transaction's other writes. A
  table created in a transaction that rolls back is dropped again. A dropped
  table goes to Oracle's recycle bin (`FLASHBACK TABLE ... TO BEFORE DROP`
  restores it); `DROP` is not undone by `ROLLBACK`.

#### Updating, deleting, and upserting

```sql
UPDATE oracle.items SET price = price * 1.1 WHERE category = 'tea';
DELETE FROM oracle.items WHERE discontinued RETURNING id, name;
INSERT INTO oracle.items (id, name) VALUES (7, 'mate') RETURNING *;   -- with Oracle's defaults and identity values
INSERT INTO oracle.items VALUES (7, 'mate', 4.0) ON CONFLICT (id) DO UPDATE SET price = excluded.price;
MERGE INTO oracle.items t USING 'prices.parquet' s ON t.id = s.id
    WHEN MATCHED THEN UPDATE SET price = s.price WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.name, s.price);
```

- **Rows are found by ROWID.** `rowid` on an attached table is the Oracle
  ROWID as text (`AAAS6/AAYAAAPpVAAA`). The scan that finds the rows an
  `UPDATE` or `DELETE` changes reads them `FOR UPDATE`, so they are locked
  from that moment until the transaction ends, and the changes go to Oracle
  as array DML (`UPDATE ... WHERE ROWID = :1`), many rows per round trip.
  DuckDB computes the new values, so any DuckDB expression works in `SET`.
  Views and index-organized tables have no ROWIDs, and cannot be updated or
  deleted from this way; use `orkidea_execute`.
- **`DEFAULT`** means Oracle's default: `SET col = DEFAULT` and the columns
  an `INSERT` leaves out are filled by Oracle. (DuckDB shows every Oracle
  column's default as `orkidea_oracle_default()`.)
- **`RETURNING`** uses Oracle's own `RETURNING ... INTO`, so it returns the
  rows as Oracle stored them: identity values, defaults, and the results of
  triggers. It runs the statement once per row, so it suits hundreds or
  thousands of rows better than millions.
- **`ON CONFLICT`** and **`MERGE INTO`** work through the table's primary
  key and unique constraints, as DuckDB plans them: DuckDB joins the new rows
  with the table, then inserts or updates. Unlike Oracle's own `MERGE`, two
  sessions inserting the same new key at once make one of them fail with a
  unique-constraint error.

#### Changing tables

`ALTER TABLE` supports `RENAME TO`, `RENAME COLUMN`, `ADD COLUMN` (with a
constant default), `DROP COLUMN`, `ALTER COLUMN ... TYPE` (Oracle converts
the data; `USING` is not supported), `SET`/`DROP NOT NULL`, `SET`/`DROP
DEFAULT` (constants), and `ADD PRIMARY KEY`/`UNIQUE`. Like `CREATE` and
`DROP`, it runs on a session of its own and commits at once.

- **Not supported:** `INSERT ... DEFAULT VALUES`, `RETURNING` on `MERGE INTO`,
  `CREATE INDEX`, and constraints other than `NOT NULL`, `PRIMARY KEY`, and
  `UNIQUE`. Use `orkidea_execute`.

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

Bind values to the query's placeholders with `params`. Values travel to Oracle
as binds, never as SQL text, so they need no quoting or escaping, and Oracle
reuses one cursor for every value. A STRUCT binds by name, and a placeholder
used more than once takes the same value each time. A `row(...)` binds by
position, one value per placeholder in the order they appear:

```sql
FROM orkidea_query('ora', 'SELECT * FROM emp WHERE deptno = :dept AND hiredate >= :since',
                   params := {'dept': 10, 'since': DATE '1981-06-01'});

FROM orkidea_query('ora', 'SELECT * FROM emp WHERE deptno = :1', params := row(10));

-- prepared statements bind new values on each execution
PREPARE emp_of AS FROM orkidea_query('ora', 'SELECT * FROM emp WHERE deptno = :d', params := {'d': $1});
EXECUTE emp_of(20);
```

Things to know:

- Oracle describes a query without its parameter values. CAST a placeholder
  that appears in the select list, as in `SELECT CAST(:d AS DATE) ...`;
  otherwise it is described as `VARCHAR2`, and a value of any other type fails
  with an error naming the column.
- A TIMESTAMP WITH TIME ZONE parameter is rejected, because the driver would
  bind it without its time zone. Bind its UTC time instead,
  `params := {'t': ts AT TIME ZONE 'UTC'}`, and convert it with
  `FROM_TZ(CAST(:t AS TIMESTAMP), 'UTC')`.

Run DDL, DML, or PL/SQL. The result is the number of rows affected:

```sql
FROM orkidea_execute('ora', 'UPDATE emp SET sal = sal * 1.1 WHERE deptno = 10');
```

`params` binds its placeholders as for `orkidea_query`. In PL/SQL a name binds
once, however often it appears. Oracle takes no binds in DDL.

```sql
FROM orkidea_execute('ora', 'UPDATE emp SET sal = sal * :raise WHERE deptno = :dept',
                     params := {'raise': 1.1, 'dept': 10});

FROM orkidea_execute('ora', 'BEGIN raise_salary(:empno, :pct); END;', params := row(7839, 5));
```

Each call opens its own connection, which commits on success.
`orkidea_execute` runs when the query executes, never when it is only bound,
so `EXPLAIN` and `PREPARE` don't run it; each `EXECUTE` of a prepared
statement runs it with that execution's values.

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
| `TIME_ZONE` | Session time zone, for Oracle SQL that depends on it, such as `CURRENT_TIMESTAMP`. `TIMESTAMP WITH LOCAL TIME ZONE` columns read as `TIMESTAMPTZ` instants whatever it is. |
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

## Releasing

`description.yml` is the extension's entry in the
[community extensions repository](https://github.com/duckdb/community-extensions):
bump `version` and set `repo.ref` to the released commit, then copy it to
`extensions/orkidea/description.yml` there. After changing `rust/Cargo.lock`,
regenerate `THIRD_PARTY_LICENSES.txt` with `scripts/third_party_licenses.py`.

## License

Apache License 2.0. See [LICENSE](LICENSE). The extension statically links
Rust crates under their own licenses; their texts are in
[THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt).
