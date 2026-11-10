# Benchmarks

How orkidea compares with DuckDB's other Oracle extension,
[`oracle`](https://duckdb.org/community_extensions/extensions/oracle), and how
the Orkidea ADBC driver compares with
[python-oracledb](https://python-oracledb.readthedocs.io/), on the
[lab](../lab/README.md) database.

## Results

Measured on 2026-11-10: orkidea from `main` (driver 0.3.0), `oracle` 0.3.1
with Oracle Instant Client 23.26, and python-oracledb 26.1.0, in DuckDB
2.0.0. Each figure is the median of five runs, after one to warm up.

| Query | orkidea | orkidea, 1 session | `oracle` | `oracle`, tuned |
| --- | ---: | ---: | ---: | ---: |
| Copy `ORDERS` into DuckDB: 1M rows, 7 columns | **0.63 s** | 1.33 s | 12.57 s | 11.82 s |
| Copy 2 of the 7 columns | **0.30 s** | 0.45 s | 4.61 s | 3.94 s |
| Orders of one month (8% of rows) | 0.23 s | **0.18 s** | 1.09 s | 1.13 s |
| Orders of one customer (50 rows) | **39 ms** | 42 ms | 48 ms | 110 ms |
| `GROUP BY status`: `count`, `sum`, `avg` | **0.19 s** | 0.20 s | 5.30 s | 4.53 s |
| `count(*)` with a filter | **43 ms** | 43 ms | 83 ms | 62 ms |
| Top 10 by amount | **0.13 s** | 0.14 s | 4.65 s | 3.91 s |
| `ORDERS` ⋈ `CUSTOMERS` ⋈ `REGIONS`, by region | **0.34 s** | 0.48 s | fails | fails |
| Point lookup on `EMP`, per query | **2.0 ms** | 2.4 ms | 4.7 ms | 12.9 ms |

- **orkidea** is the default: tables of 500,000 rows or more are read by up
  to eight sessions in ROWID ranges, and aggregates and top-N queries run in
  Oracle. With aggregate pushdown off, the `GROUP BY` takes 0.34 s and the
  filtered `count(*)` 64 ms; the other figures do not change
  (`results.json` has every run).
- **orkidea, 1 session** sets `orkidea_parallel_scan = 1`.
- **`oracle`, tuned** sets `oracle_array_size = 2048` (its maximum) and
  `oracle_prefetch_rows = 10000`.
- **The join fails** in `oracle`, which cannot read a table with a
  `BOOLEAN` column (Oracle 23ai's SQL `BOOLEAN`), even when the query does
  not select it: "Unsupported Oracle type BOOLEAN for
  ORKIDEA_LAB.CUSTOMERS.VIP".

What makes the difference:

- **Scans.** The Orkidea driver decodes values from the network buffer
  straight into Arrow columns. One orkidea session reads `ORDERS` nine
  times as fast as `oracle` does, and parallel sessions take that to twenty.
- **Pushdown.** orkidea sends the `GROUP BY` and the top-N to Oracle, which
  returns three rows and ten; `oracle` reads the million rows into DuckDB
  first.
- **Small results** are close: there the time is Oracle's, finding 50 rows
  in a million without an index. For the month's orders, the parallel
  scan's planning costs more than its eight sessions save.

With driver 0.2.1, which built a row object for each row before converting
it, the same copies took 1.11 s (3.29 s through one session) and 0.61 s.

From Python, reading into an Arrow table through one session:

| Query | Orkidea ADBC 0.3.0 | python-oracledb | python-oracledb, `arraysize=10000` |
| --- | ---: | ---: | ---: |
| `ORDERS`: 1M rows, 7 columns | **1.60 s** | 5.01 s | 2.50 s |
| 2 of the 7 columns | **0.46 s** | 3.46 s | 1.07 s |

python-oracledb's `fetch_df_all` also decodes Oracle's wire format straight
into Arrow arrays; the Orkidea driver does so in Rust, and decodes `NUMBER`
into integers and decimals from its bytes. Driver 0.2.1 took 3.36 s and
1.38 s.

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
above. `--engines` and `--queries` run part of it.

`bench/network.sh` needs Docker and the lab container; it runs the network
table's queries at each delay (default 0, 1, 5, and 20 ms) into
`bench/results-network-<delay>ms.json`, and removes the delay when it ends.

`--runs` sets the number of timed runs (default 5). A full run takes about
fifteen minutes, most of it in the `oracle` extension's scans, and the
network runs half an hour.
