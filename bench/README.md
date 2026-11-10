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

## Over a network

`bench/network.sh` runs part of the benchmark again with delay added to every
round trip between the client and Oracle (with `tc netem`, from a container
sharing the lab container's network): 1 ms is about a data center, 5 ms a
region, 20 ms a WAN. Medians of three runs:

| Query | Delay | orkidea | orkidea, 1 session | `oracle` | `oracle`, tuned |
| --- | ---: | ---: | ---: | ---: | ---: |
| Copy 2 of `ORDERS`' 7 columns | 0 ms | **0.52 s** | 1.37 s | 5.46 s | 4.20 s |
| | 1 ms | **0.58 s** | 1.39 s | 6.02 s | 4.11 s |
| | 5 ms | **0.62 s** | 1.93 s | 9.35 s | 4.49 s |
| | 20 ms | **1.00 s** | 3.95 s | 21.59 s | 6.48 s |
| `GROUP BY status` | 0 ms | **0.19 s** | 0.19 s | 5.21 s | 4.40 s |
| | 20 ms | **0.27 s** | 0.24 s | 22.49 s | 6.98 s |
| Top 10 by amount | 0 ms | **0.14 s** | 0.14 s | 5.00 s | 3.98 s |
| | 20 ms | **0.18 s** | 0.18 s | 21.39 s | 6.54 s |
| Orders of one customer (50 rows) | 0 ms | **39 ms** | 42 ms | 49 ms | 120 ms |
| | 20 ms | 110 ms | **81 ms** | 170 ms | 240 ms |
| Point lookup on `EMP`, per query | 0 ms | 2.3 ms | **1.8 ms** | 5.7 ms | 15.1 ms |
| | 1 ms | **3.4 ms** | 3.5 ms | 11.7 ms | 19.4 ms |
| | 5 ms | 8.8 ms | **8.2 ms** | 37.7 ms | 44.6 ms |
| | 20 ms | 23.2 ms | **23.1 ms** | 130 ms | 140 ms |

- **A point lookup** takes orkidea one round trip, and `oracle` about six.
- **Pushdown counts for more** with distance: the `GROUP BY` and the top-N
  send a few rows back, so they barely change, while reading the table into
  DuckDB pays for every batch.
- **Parallel sessions** wait for their round trips at the same time: at
  20 ms, eight read the table in 1.0 s, against 3.95 s for one.
- **The single-session estimate costs a round trip.** For a filter that
  leaves few rows, orkidea first reads the columns' statistics
  (see [Parallel scans](../README.md#parallel-scans)), so at 20 ms it takes
  110 ms where one session alone takes 81 ms; without the estimate, the
  parallel plan's extra round trips would cost more.

From Python, through one session (2 columns, 1M rows):

| Delay | Orkidea ADBC | python-oracledb, `arraysize=10000` |
| ---: | ---: | ---: |
| 0 ms | 1.34 s | **0.98 s** |
| 1 ms | 1.33 s | **1.00 s** |
| 5 ms | 1.99 s | **1.48 s** |
| 20 ms | 3.93 s | **3.15 s** |

Both fetch about as many rows per round trip (8,192 and 10,000), so the
delay adds about the same to each; the difference is decoding, as above.

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
`bench/results-network-<delay>ms.json`, and removes the delay when it ends. `--runs` sets the number of timed runs (default 5); a full run takes
about fifteen minutes, most of it in the `oracle` extension's scans.
