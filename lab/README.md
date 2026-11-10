# Orkidea lab

A hands-on tour of the orkidea DuckDB extension and the Orkidea ADBC driver,
against an Oracle database of your own that the lab starts in a container.
It takes about an hour; each module stands alone and takes a few minutes.

By the end you will have read, filtered, aggregated, joined, written, and
altered Oracle tables from DuckDB, run Oracle SQL and PL/SQL with bind
parameters, moved data between Oracle and files, connected through TNS
aliases, and done the same from Python.

## What you need

- **Docker** (or Podman as `docker`), with about 2 GB of memory free for
  Oracle Database Free. The image, `gvenzl/oracle-free:23-slim-faststart`,
  is pulled from Docker Hub on first use. It runs on x86_64 and arm64.
- **DuckDB v2.0.0** on your `PATH` (`duckdb --version`).
- **The orkidea extension, 0.2.0 or newer.** The lab uses features newer than
  the 0.1.0 community release (bind parameters, aggregate pushdown). Build it
  from this repository (see [Building](../README.md#building)); the lab finds
  `build/release/extension/orkidea/orkidea.duckdb_extension` on its own. To
  use another file, set `ORKIDEA_EXTENSION=/path/to/orkidea.duckdb_extension`.
  When a newer release is in the community repository, the lab installs it
  with `INSTALL orkidea FROM community` if neither is there.
- **For module 9 only:** Python 3.10 or newer.

## Set up

From the repository root:

```sh
lab/setup.sh
```

It starts Oracle Database Free on `127.0.0.1:1531` (set `ORKIDEA_LAB_PORT`
for another port), creates the user `ORKIDEA_LAB` with a generated password,
and seeds the schema. This takes under a minute once the image is pulled. The
connection details go to `lab/.lab.env`, which git ignores.

Then open DuckDB, connected:

```sh
lab/orkidea-lab.sh
```

It loads the extension and runs [`00_connect.sql`](00_connect.sql): a secret
named `lab` holds the connection (its password comes from the environment, so
it is never typed into a query), and the schema is attached as `oracle`.

## The schema

| Table | Rows | What it is |
| --- | --- | --- |
| `REGIONS` | 5 | Africa, Americas, Asia, Europe, Oceania |
| `CUSTOMERS` | 20,000 | name, email (some NULL), country, region, `vip` (`BOOLEAN`), sign-up date |
| `PRODUCTS` | 500 | name, category (Tea, Coffee, Books, Garden, Kitchen), price |
| `ORDERS` | 1,000,000 | customer, product, quantity, amount, status (`SHIPPED`, `CANCELLED`, `RETURNED`), timestamp in 2025 |
| `DEPT`, `EMP` | 4, 14 | Oracle's classic demo tables, with the procedure `RAISE_SALARY` |
| `TYPE_SHOWCASE` | 1 | one column of each kind of Oracle type: `NUMBER`, `BINARY_DOUBLE`, `CHAR`, `NVARCHAR2`, `CLOB`, `RAW`, `BOOLEAN`, `DATE`, `TIMESTAMP`, `TIMESTAMP WITH TIME ZONE`, `INTERVAL`, `JSON`, `VECTOR` |

The data is computed, not random, so the results quoted in the modules
(`-- → ...`) are what you will see.

## Modules

Run a module inside the DuckDB shell with `.read 03_aggregates.sql`, or from
your shell with `lab/orkidea-lab.sh -c ".read 03_aggregates.sql"`. Better yet,
open the file and run its statements one at a time: the comments explain each.

| Module | You will |
| --- | --- |
| [01 Explore](01_explore.sql) | list and describe Oracle tables, join them, and see how every Oracle type arrives in DuckDB (JSON paths, vector similarity, intervals) |
| [02 Pushdown](02_pushdown.sql) | log the SQL sent to Oracle, and see which filters, `LIMIT`s, and top-Ns run there and which stay in DuckDB, and why |
| [03 Aggregates](03_aggregates.sql) | push `GROUP BY`, `DISTINCT`, and aggregates into Oracle, check result types, and time a million-row `GROUP BY` with pushdown on and off |
| [04 Parallel scans](04_parallel_scans.sql) | copy a million rows into DuckDB through one, four, and eight Oracle sessions reading one snapshot |
| [05 Writing](05_writing.sql) | `CREATE TABLE`, `INSERT`, `CREATE TABLE AS`, `UPDATE`, `DELETE ... RETURNING`, `ON CONFLICT`, `INSERT OR REPLACE`, `MERGE INTO`, `ALTER TABLE`, transactions with `ROLLBACK`, and Oracle's own defaults and identity columns |
| [06 Oracle SQL](06_oracle_sql.sql) | run Oracle SQL and PL/SQL with `orkidea_query` and `orkidea_execute`, with bind parameters by name and by position, in prepared statements |
| [07 Federation](07_federation.sql) | export Oracle data to Parquet and CSV, join files and local tables with Oracle, load a CSV into Oracle, and archive Oracle rows into a DuckDB file |
| [08 Connections](08_connections.sql) | connect through a `tnsnames.ora` alias, a TNS descriptor, a read-only attachment, and a session time zone |
| [09 Python](09_python.py) | use the ADBC driver from Python: Arrow results, parameters, streaming, bulk loading, table schemas, and PL/SQL OUT parameters |

Module 9 runs outside DuckDB:

```sh
python -m venv .venv
.venv/bin/pip install "adbc-driver-orkidea[dbapi]>=0.2"
source lab/.lab.env && .venv/bin/python lab/09_python.py
```

Modules that write (05, 06, 07) create `lab_*` tables and drop them again,
and module 6 puts back the salary it raises, so every module can be run again
in any order. Files the lab writes go to `lab/out`.

## Clean up

```sh
lab/teardown.sh
```

removes the container, `lab/.lab.env`, `lab/tns`, and `lab/out`.

## When something goes wrong

- **`Container orkidea-lab already exists`**: a lab from before is still
  there. Run `lab/teardown.sh`, then `lab/setup.sh`.
- **`port is already allocated`**: pick another port, as in
  `ORKIDEA_LAB_PORT=1600 lab/setup.sh`.
- **`The database did not open`**: Oracle needs about 2 GB of memory; give
  Docker more and run setup again.
- **`Invalid named parameter "params"`** or no `Oracle SQL` in an aggregate's
  `EXPLAIN`: the extension is older than 0.2.0. Build it from this
  repository.
- **`built specifically for DuckDB version ...`**: the extension and your
  `duckdb` are different DuckDB versions. Use DuckDB v2.0.0, or set
  `ORKIDEA_DUCKDB` to the `build/release/duckdb` that the build makes. A
  local build loaded by the release CLI must be built with
  `OVERRIDE_GIT_DESCRIBE=v2.0.0 GEN=ninja make release`.
- **To see the SQL sent to Oracle** in any module:
  `CALL enable_logging('OrkideaQuery', level = 'debug', storage = 'memory');`
  then `SELECT sql, duration_ms FROM duckdb_logs_parsed('OrkideaQuery');`.
