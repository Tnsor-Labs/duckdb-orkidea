# Benchmarks

How orkidea compares with DuckDB's other Oracle extension,
[`oracle`](https://duckdb.org/community_extensions/extensions/oracle), and how
the Orkidea ADBC driver compares with
[python-oracledb](https://python-oracledb.readthedocs.io/), on the
[lab](../lab/README.md) database.

## Results

Measured on 2026-11-10: orkidea from `main` (133736c, driver 0.2.1),
`oracle` 0.3.1 with Oracle Instant Client 23.26, and python-oracledb 26.1.0,
in DuckDB 2.0.0. Each figure is the median of five runs, after one to warm
up.

| Query | orkidea | orkidea, 1 session | `oracle` | `oracle`, tuned |
| --- | ---: | ---: | ---: | ---: |
| Copy `ORDERS` into DuckDB: 1M rows, 7 columns | **1.11 s** | 3.29 s | 13.40 s | 12.63 s |
| Copy 2 of the 7 columns | **0.61 s** | 1.39 s | 4.93 s | 4.15 s |
| Orders of one month (8% of rows) | **0.26 s** | 0.37 s | 1.19 s | 1.26 s |
| Orders of one customer (50 rows) | 49 ms | **40 ms** | 49 ms | 120 ms |
| `GROUP BY status`: `count`, `sum`, `avg` | **0.19 s** | 0.19 s | 6.16 s | 4.70 s |
| `count(*)` with a filter | **42 ms** | 43 ms | 92 ms | 66 ms |
| Top 10 by amount | **0.14 s** | 0.14 s | 4.85 s | 4.12 s |
| `ORDERS` ⋈ `CUSTOMERS` ⋈ `REGIONS`, by region | **0.66 s** | 1.42 s | fails | fails |
| Point lookup on `EMP`, per query | **2.3 ms** | 2.2 ms | 5.3 ms | 8.3 ms |

- **orkidea** is the default: tables of 500,000 rows or more are read by up
  to eight sessions in ROWID ranges, and aggregates and top-N queries run in
  Oracle. With aggregate pushdown off, the `GROUP BY` takes 0.69 s and the
  filtered `count(*)` 82 ms; the other figures do not change
  (`results.json` has every run).
- **orkidea, 1 session** sets `orkidea_parallel_scan = 1`.
- **`oracle`, tuned** sets `oracle_array_size = 2048` (its maximum) and
  `oracle_prefetch_rows = 10000`.
- **The join fails** in `oracle`, which cannot read a table with a
  `BOOLEAN` column (Oracle 23ai's SQL `BOOLEAN`), even when the query does
  not select it: "Unsupported Oracle type BOOLEAN for
  ORKIDEA_LAB.CUSTOMERS.VIP".

What makes the difference:

- **Scans.** A single orkidea session reads `ORDERS` four times as fast as
  `oracle` does, and parallel sessions take that to twelve.
- **Pushdown.** orkidea sends the `GROUP BY` and the top-N to Oracle, which
  returns three rows and ten; `oracle` reads the million rows into DuckDB
  first.
- **Small results** are close: there the time is Oracle's, finding 50 rows
  in a million without an index.

From Python, reading into an Arrow table through one session:

| Query | Orkidea ADBC | python-oracledb | python-oracledb, `arraysize=10000` |
| --- | ---: | ---: | ---: |
| `ORDERS`: 1M rows, 7 columns | 3.36 s | 4.98 s | **2.69 s** |
| 2 of the 7 columns | 1.38 s | 2.90 s | **0.98 s** |

python-oracledb's `fetch_df_all` decodes Oracle's wire format straight into
Arrow arrays. With a large `arraysize` it is faster than the Orkidea driver,
which builds each row before it appends it to Arrow arrays: reading
one `NUMBER` column, the driver spends 0.50 s of CPU per million rows to
python-oracledb's 0.30 s. With python-oracledb's default `arraysize` (100),
the Orkidea driver is faster. In DuckDB, orkidea's parallel sessions more
than make up for the difference.

## Caveats

- One laptop (Intel Core i7-1165G7, 4 cores, 8 threads) runs both Oracle
  Database Free 23.26 in a container and the client, so they compete for
  the CPU, and there is no network between them. Over a real network,
  fetching fewer, larger batches matters more, and Oracle has the machine
  to itself.
- `ORDERS` is small enough to stay in Oracle's buffer cache after the
  warm-up, so these are not disk reads.
- The tables have no indexes, as in the lab.

## Running them

The benchmark needs the lab database, and Oracle Instant Client for the
`oracle` extension (orkidea and the Orkidea driver need no Oracle client):

```sh
lab/setup.sh
python -m venv .venv && .venv/bin/pip install -r bench/requirements.txt
# Instant Client Basic or Basic Light, from oracle.com, unzipped anywhere:
ORACLE_IC=/path/to/instantclient_23_26 .venv/bin/python bench/run.py
```

It uses orkidea from this repository's release build, or
`ORKIDEA_EXTENSION`. Each engine runs in a process of its own, and every
query's result is checked against the first engine's, so a fast wrong answer
fails the run. The run writes `bench/results.json` and prints the tables
above. `--runs` sets the number of timed runs (default 5); a full run takes
about fifteen minutes, most of it in the `oracle` extension's scans.
