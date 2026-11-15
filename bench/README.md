# Benchmarks

How orkidea compares with DuckDB's other ways to read Oracle (the
[`oracle`](https://duckdb.org/community_extensions/extensions/oracle) and
[`oracle_scanner`](https://duckdb.org/community_extensions/extensions/oracle_scanner)
community extensions, and
[`odbc_scanner`](https://duckdb.org/community_extensions/extensions/odbc_scanner)
with Oracle's ODBC driver), and how the Orkidea ADBC driver compares with
[python-oracledb](https://python-oracledb.readthedocs.io/) and
[ConnectorX](https://github.com/sfu-db/connector-x), on the
[lab](../lab/README.md) database.

## Results

Measured on 2026-11-15: orkidea 0.4.0 (driver 0.5.0), `oracle` 0.3.1 and
`odbc_scanner` with Oracle Instant Client 23.26 and its ODBC driver,
`oracle_scanner` 0.6.2, python-oracledb 26.1.0, and ConnectorX 0.4.6, in
DuckDB 2.0.0. Each figure is the median of five runs, after one to warm up.

| Query | orkidea | orkidea, 1 session | `oracle_scanner` | `oracle`, tuned | `odbc_scanner`, tuned |
| --- | ---: | ---: | ---: | ---: | ---: |
| Copy `ORDERS` into DuckDB: 1M rows, 7 columns | **0.59 s** | 1.33 s | 3.64 s | 10.80 s | 5.59 s |
| Copy 2 of the 7 columns | **0.27 s** | 0.42 s | 1.15 s | 3.76 s | 1.79 s |
| Orders of one month (8% of rows) | **0.17 s** | 0.18 s | 0.32 s | 1.07 s | 0.49 s |
| Orders of one customer (50 rows) | 35 ms | **35 ms** | 36 ms | 95 ms | 36 ms |
| `GROUP BY status`: `count`, `sum`, `avg` | 0.21 s | **0.18 s** | 2.10 s | 3.97 s | |
| `count(*)` with a filter | 38 ms | **37 ms** | 39 ms | 58 ms | |
| Top 10 by amount | 0.13 s | 0.14 s | 0.13 s | 3.61 s | |
| `ORDERS` ⋈ `CUSTOMERS` ⋈ `REGIONS`, by region | **0.19 s** | **0.19 s** | 1.41 s | fails | |
| Point lookup on `EMP`, per query | **1.6 ms** | **1.6 ms** | 1.7 ms | 11.6 ms | 2.1 ms |

- **orkidea** is the default: tables of 500,000 rows or more are read by up
  to eight sessions in ROWID ranges, and filters, aggregates, top-N queries,
  and joins of tables of one database run in Oracle.
- **orkidea, 1 session** sets `orkidea_parallel_scan = 1`.
- **`oracle_scanner`** is shown with `oracle_parallel_scan = 1`, its faster
  setting here; with its default parallel scan, the month's orders take
  0.43 s and the customer's 0.13 s, and the other figures are within 11%.
- **`oracle`, tuned** sets `oracle_array_size = 2048` (its maximum) and
  `oracle_prefetch_rows = 10000`; untuned, it is slower on the copies
  (11.46 s and 4.30 s) and faster on small results (46 ms and 3.6 ms).
  It cannot run the join: it fails on the `BOOLEAN` column of `CUSTOMERS`
  (Oracle 23ai's SQL `BOOLEAN`), even when the query does not select it.
- **`odbc_scanner`** has no catalog: it runs Oracle SQL written by hand, so it
  is only measured on the reads, as orkidea's pushdown would write them.
  Tuned, it sets a 1 MB fetch buffer (`FBS`); untuned, the copies take 6.46 s
  and 2.12 s.
- With aggregate pushdown off (`orkidea_aggregate_pushdown = false`), the
  `GROUP BY` takes 0.34 s and the join 0.32 s. `results.json` has every
  engine and setting.

What makes the difference:

- **Scans.** The Orkidea driver decodes values from the network buffer
  straight into Arrow columns. One session copies `ORDERS` almost three times
  as fast as `oracle_scanner` and eight times as fast as `oracle`, and
  parallel sessions more than halve that again.
- **Pushdown.** orkidea sends the `GROUP BY`, the top-N, and the join to
  Oracle, which returns a few rows. `oracle_scanner` is as fast on the top-N,
  but its `GROUP BY` and join take 2.10 s and 1.41 s, about the time it
  takes to read the rows into DuckDB; `oracle` takes about that long on all
  three.
- **Small results** are close: there the time is Oracle's, finding 50 rows
  in a million without an index.

From Python, reading into an Arrow table:

| Query | Orkidea ADBC 0.5.0 | ConnectorX | ConnectorX, 8 partitions | python-oracledb, `arraysize=10000` |
| --- | ---: | ---: | ---: | ---: |
| `ORDERS`: 1M rows, 7 columns | **1.19 s** | 3.22 s | 1.32 s | 2.50 s |
| 2 of the 7 columns | **0.40 s** | 1.49 s | 0.96 s | 0.81 s |

- **Orkidea ADBC** reads through one session. python-oracledb uses
  `fetch_df_all`, which also decodes Oracle's wire format straight into
  Arrow arrays; with its default `arraysize`, it takes 4.01 s and 2.28 s.
- **ConnectorX** opens its connections on every call, inside the timing; so
  does the Orkidea driver read in 8 partitions, which takes 1.49 s and 1.23 s.
  Its partitions are hash partitions (see the driver's
  [partitioned reads](https://github.com/Tnsor-Labs/orkidea-adbc/blob/main/docs/partitions.md)),
  so Oracle reads the table once for each of the eight; on a table this size,
  one session is faster.
- A second run of these engines, on a busier machine (load average 5), gave
  the same order: 1.37 s and 0.43 s for Orkidea ADBC, against 3.37 s and
  1.58 s for ConnectorX.

## Other column types

`bench/mixed.sql` creates `BENCH_MIXED`: 300,000 rows with `JSON`, a
16-dimension `FLOAT32` `VECTOR`, an interval, and an `NCLOB` beside numbers,
text, and a timestamp. Driver 0.3.0 read a query with any of those four types
row by row; the driver's `main` decodes every type into Arrow columns, and
writes `JSON` as text as it decodes Oracle's binary form (OSON), without
building a value first. `bench/mixed.py` reads it through one session. Each
figure is the mean of two medians of five runs, the two drivers run in
turns on a busy machine (load average 6 to 7):

| Query (300k rows) | Orkidea 0.3.0 | Orkidea `main` | python-oracledb, `arraysize=10000` |
| --- | ---: | ---: | ---: |
| All columns and the `ROWID` | 5.69 s | **3.98 s** | fails |
| `id`, `JSON` | 2.88 s | **1.57 s** | fails |
| `id`, `VECTOR` | 1.21 s | **1.04 s** | 1.69 s |
| `id`, interval | 0.35 s | **0.19 s** | 0.31 s |
| `id`, `NCLOB` | 1.21 s | **1.07 s** | 1.69 s |
| Numbers, text, and a timestamp | **0.30 s** | **0.30 s** | 0.64 s |

python-oracledb's `fetch_df_all` refuses `JSON` ("NotSupportedError").

- **`JSON`** reads in half the time. Its decoding takes 0.7 s of the
  client's CPU, against 1.3 s; selected as text with `JSON_SERIALIZE(doc)`,
  the same rows take 0.13 s of CPU, so writing text from OSON still costs
  something, mostly putting each object's members in name order.
- **Intervals** read in half the time.
- **`VECTOR` and `NCLOB`** values arrive as LOBs, and most of their time is
  the database's: they take 0.3 s of the client's CPU. Cast to `NVARCHAR2`,
  the `NCLOB` column takes 0.55 s.

## Over a network

`bench/network.sh` runs part of the benchmark again with delay added to every
round trip between the client and Oracle (with `tc netem`, from a container
sharing the lab container's network): 1 ms is about a data center, 5 ms a
region, 20 ms a WAN. Medians of three runs:

| Query | Delay | orkidea | orkidea, 1 session | `oracle` | `oracle`, tuned |
| --- | ---: | ---: | ---: | ---: | ---: |
| Copy 2 of `ORDERS`' 7 columns | 0 ms | **0.37 s** | 0.46 s | 5.34 s | 4.17 s |
| | 1 ms | **0.38 s** | 0.56 s | 5.65 s | 4.05 s |
| | 5 ms | **0.40 s** | 1.15 s | 9.23 s | 4.79 s |
| | 20 ms | **0.78 s** | 2.97 s | 21.37 s | 6.75 s |
| `GROUP BY status` | 0 ms | **0.19 s** | 0.19 s | 5.43 s | 4.15 s |
| | 20 ms | **0.25 s** | 0.26 s | 21.70 s | 7.60 s |
| Top 10 by amount | 0 ms | 0.15 s | **0.14 s** | 4.54 s | 3.81 s |
| | 20 ms | 0.19 s | **0.18 s** | 21.37 s | 6.21 s |
| Orders of one customer (50 rows) | 0 ms | 39 ms | **38 ms** | 64 ms | 110 ms |
| | 20 ms | 100 ms | **82 ms** | 170 ms | 230 ms |
| Point lookup on `EMP`, per query | 0 ms | **2.3 ms** | 2.4 ms | 4.0 ms | 15.0 ms |
| | 1 ms | 6.2 ms | **4.5 ms** | 12.0 ms | 18.7 ms |
| | 5 ms | 8.0 ms | **7.9 ms** | 37.1 ms | 45.1 ms |
| | 20 ms | **23.1 ms** | 23.2 ms | 130 ms | 140 ms |

- **A point lookup** takes orkidea one round trip, and `oracle` about six.
- **Pushdown counts for more** with distance: the `GROUP BY` and the top-N
  send a few rows back, so they barely change, while reading the table into
  DuckDB pays for every batch.
- **Parallel sessions** wait for their round trips at the same time: at
  20 ms, eight read the table in 0.78 s, against 2.97 s for one.
- **The single-session estimate costs a round trip.** For a filter that
  leaves few rows, orkidea first reads the columns' statistics
  (see [Parallel scans](../README.md#parallel-scans)), so at 20 ms it takes
  100 ms where one session alone takes 82 ms; without the estimate, the
  parallel plan's extra round trips would cost more.

From Python, through one session (2 columns, 1M rows):

| Delay | Orkidea ADBC 0.3.0 | python-oracledb, `arraysize=10000` |
| ---: | ---: | ---: |
| 0 ms | **0.43 s** | 0.89 s |
| 1 ms | **0.52 s** | 0.99 s |
| 5 ms | **1.05 s** | 1.44 s |
| 20 ms | 3.01 s | 3.05 s |

Both fetch about as many rows per round trip (8,192 and 10,000). Close to
the database, decoding makes the difference; at 20 ms, the round trips take
most of the time, and the two are level.

## Caveats

- One laptop (Intel Core i7-1165G7, 4 cores, 8 threads) runs both Oracle
  Database Free 23.26 in a container and the client, so they compete for
  the CPU. Apart from the [delays added](#over-a-network), there is no
  network between them, and on a real one Oracle has the machine to itself.
- `ORDERS` is small enough to stay in Oracle's buffer cache after the
  warm-up, so these are not disk reads.
- The tables have no indexes, as in the lab.

## Running them

The benchmark needs the lab database, and Oracle Instant Client for the
`oracle` extension and ConnectorX, with Oracle's ODBC package unzipped into
the same directory and unixODBC installed for `odbc_scanner` (orkidea and
the Orkidea driver need no Oracle client):

```sh
lab/setup.sh
python -m venv .venv && .venv/bin/pip install -r bench/requirements.txt
# Instant Client Basic or Basic Light, and the ODBC package, from oracle.com:
ORACLE_IC=/path/to/instantclient_23_26 .venv/bin/python bench/run.py
```

It uses orkidea from this repository's release build, or
`ORKIDEA_EXTENSION`. Each engine runs in a process of its own, and every
query's result is checked against the first engine's, so a fast wrong answer
fails the run. The run writes `bench/results.json` and prints the tables
above. `--engines` and `--queries` run part of it.

`bench/network.sh` needs Docker and the lab container; it runs the network
table's queries at each delay (default 0, 1, 5, and 20 ms) into
`bench/results-network-<delay>ms.json`, and removes the delay when it ends.

`--runs` sets the number of timed runs (default 5). A full run takes about
twenty minutes, most of it in the `oracle` extension's scans, and the
network runs half an hour.
