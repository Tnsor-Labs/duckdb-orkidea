"""Benchmarks orkidea against DuckDB's `oracle` community extension, and the
Orkidea ADBC driver against python-oracledb, on the lab database.

    lab/setup.sh
    python -m venv .venv && .venv/bin/pip install -r bench/requirements.txt
    ORACLE_IC=/path/to/instantclient_23_xx .venv/bin/python bench/run.py

Each engine runs in a process of its own. Every query runs once to warm up
and then RUNS times; the median is reported. Results are compared across
engines, so a fast wrong answer fails the run.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LAB_ENV = ROOT / "lab" / ".lab.env"
ORKIDEA_BUILD = ROOT / "build" / "release" / "extension" / "orkidea" / "orkidea.duckdb_extension"
RUNS = 5

# name -> (description, SQL over the attached catalog `db`). A query that
# starts with CREATE is checked by the table it makes.
DUCKDB_QUERIES = {
    "scan_all": ("Copy ORDERS, 1M rows x 7 columns",
                 "CREATE OR REPLACE TEMP TABLE t AS FROM db.orders"),
    "scan_narrow": ("Copy 2 of ORDERS' 7 columns",
                    "CREATE OR REPLACE TEMP TABLE t AS SELECT order_id, amount FROM db.orders"),
    "filter_range": ("Orders of one month (8%)",
                     ("CREATE OR REPLACE TEMP TABLE t AS FROM db.orders "
                      "WHERE ordered_at >= TIMESTAMP '2025-03-01' AND ordered_at < TIMESTAMP '2025-04-01'")),
    "filter_point": ("Orders of one customer (50 rows)",
                     "CREATE OR REPLACE TEMP TABLE t AS FROM db.orders WHERE customer_id = 4242"),
    "group_by": ("GROUP BY status: count, sum, avg",
                 "SELECT status, count(*), sum(amount), avg(quantity) FROM db.orders GROUP BY status ORDER BY status"),
    "count_filtered": ("count(*) of a filter",
                       "SELECT count(*) FROM db.orders WHERE status = 'RETURNED'"),
    "top_n": ("Top 10 by amount",
              "SELECT order_id, amount FROM db.orders ORDER BY amount DESC, order_id LIMIT 10"),
    "join": ("ORDERS x CUSTOMERS x REGIONS, by region",
             ("SELECT r.name, count(*), sum(o.amount) FROM db.orders o "
              "JOIN db.customers c ON c.customer_id = o.customer_id "
              "JOIN db.regions r ON r.region_id = c.region_id GROUP BY r.name ORDER BY r.name")),
}
POINT_LOOKUPS = 200
POINT_SQL = "SELECT ename, sal FROM db.emp WHERE empno = 7839"

PYTHON_QUERIES = {
    "py_scan_all": ("ORDERS to an Arrow table, 1M rows x 7 columns", "SELECT * FROM orders"),
    "py_scan_narrow": ("2 columns to an Arrow table", "SELECT order_id, amount FROM orders"),
}


def lab_env():
    env = {}
    for line in LAB_ENV.read_text().splitlines():
        line = line.removeprefix("export ")
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            env[key] = value.strip("'\"")
    return env


def timed(fn, runs):
    fn()  # warm up
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        fn()
        times.append(time.perf_counter() - start)
    return times


def fingerprint(con, sql):
    if sql.startswith("CREATE"):
        return con.sql("SELECT count(*), sum(order_id)::VARCHAR, sum(amount)::VARCHAR FROM t").fetchall()
    return [tuple(str(v) for v in row) for row in con.sql(sql).fetchall()]


def duckdb_engine(engine, env, runs, queries, point_lookups):
    import duckdb

    con = duckdb.connect(config={"allow_unsigned_extensions": "true", "threads": "8"})
    # getenv() is the CLI's; variables keep the password out of the SQL text.
    for key in ("ORKIDEA_LAB_HOST", "ORKIDEA_LAB_PORT", "ORKIDEA_LAB_SERVICE", "ORKIDEA_LAB_USER",
                "ORKIDEA_LAB_PASSWORD", "ORACLE_CONNECT"):
        con.execute(f"SET VARIABLE {key} = ?", [os.environ.get(key, "")])
    if engine.startswith("orkidea"):
        extension = os.environ.get("ORKIDEA_EXTENSION", str(ORKIDEA_BUILD))
        con.execute(f"LOAD '{extension}'")
        con.execute(
            "CREATE SECRET lab (TYPE orkidea, HOST getvariable('ORKIDEA_LAB_HOST'), "
            "PORT getvariable('ORKIDEA_LAB_PORT')::INTEGER, SERVICE getvariable('ORKIDEA_LAB_SERVICE'), "
            "USER getvariable('ORKIDEA_LAB_USER'), PASSWORD getvariable('ORKIDEA_LAB_PASSWORD'))"
        )
        con.execute("ATTACH 'lab' AS db (TYPE orkidea)")
        if engine == "orkidea-1-session":
            con.execute("SET orkidea_parallel_scan = 1")
        if engine == "orkidea-no-aggregates":
            con.execute("SET orkidea_aggregate_pushdown = false")
    else:
        con.execute("INSTALL oracle FROM community")
        con.execute("LOAD oracle")
        con.execute("ATTACH getvariable('ORACLE_CONNECT') AS db (TYPE oracle)")
        if engine == "oracle-tuned":
            con.execute("SET oracle_array_size = 2048")
            con.execute("SET oracle_prefetch_rows = 10000")

    version = con.sql(
        "SELECT extension_version FROM duckdb_extensions() WHERE extension_name = ?",
        params=["orkidea" if engine.startswith("orkidea") else "oracle"],
    ).fetchall()[0][0]
    results = {}
    for name, (_, sql) in DUCKDB_QUERIES.items():
        if name not in queries:
            continue
        try:
            times = timed(lambda sql=sql: con.execute(sql).fetchall(), runs)
        except duckdb.Error as error:
            # A query the engine cannot run is a result too.
            results[name] = {"error": str(error).splitlines()[0]}
            continue
        results[name] = {"times": times, "result": fingerprint(con, sql)}
    if "point_lookup" not in queries:
        return {"version": version, "results": results}
    times = timed(lambda: [con.execute(POINT_SQL).fetchall() for _ in range(point_lookups)], runs)
    results["point_lookup"] = {
        "times": [t / point_lookups for t in times],
        "result": [tuple(str(v) for v in row) for row in con.sql(POINT_SQL).fetchall()],
    }
    return {"version": version, "results": results}


def python_engine(engine, env, runs, queries, point_lookups):
    import pyarrow as pa
    import pyarrow.compute as pc

    if engine == "orkidea-adbc":
        import adbc_driver_orkidea
        from adbc_driver_orkidea import dbapi

        uri = f"oracle://{env['ORKIDEA_LAB_USER']}@{env['ORKIDEA_LAB_HOST']}:{env['ORKIDEA_LAB_PORT']}/{env['ORKIDEA_LAB_SERVICE']}"
        conn = dbapi.connect(uri, db_kwargs={"password": env["ORKIDEA_LAB_PASSWORD"]})
        version = adbc_driver_orkidea.__version__

        def fetch(sql):
            with conn.cursor() as cur:
                cur.execute(sql)
                return cur.fetch_arrow_table()
    else:
        import oracledb

        conn = oracledb.connect(
            user=env["ORKIDEA_LAB_USER"], password=env["ORKIDEA_LAB_PASSWORD"],
            dsn=f"{env['ORKIDEA_LAB_HOST']}:{env['ORKIDEA_LAB_PORT']}/{env['ORKIDEA_LAB_SERVICE']}",
        )
        version = oracledb.__version__
        arraysize = 10000 if engine == "python-oracledb-tuned" else oracledb.defaults.arraysize

        def fetch(sql):
            return pa.table(conn.fetch_df_all(sql, arraysize=arraysize))

    results = {}
    for name, (_, sql) in PYTHON_QUERIES.items():
        if name not in queries:
            continue
        times = timed(lambda sql=sql: fetch(sql), runs)
        table = fetch(sql)
        results[name] = {
            "times": times,
            "result": [table.num_rows, str(pc.sum(table.column("ORDER_ID")).as_py()),
                       f"{pc.sum(table.column('AMOUNT').cast(pa.float64())).as_py():.2f}"],
        }
    conn.close()
    return {"version": version, "results": results}


ALL_QUERIES = [*DUCKDB_QUERIES, "point_lookup", *PYTHON_QUERIES]
DUCKDB_ENGINES = ["orkidea", "orkidea-1-session", "orkidea-no-aggregates", "oracle", "oracle-tuned"]
PYTHON_ENGINES = ["orkidea-adbc", "python-oracledb", "python-oracledb-tuned"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", help=argparse.SUPPRESS)
    parser.add_argument("--runs", type=int, default=RUNS)
    parser.add_argument("--out", type=Path, default=ROOT / "bench" / "results.json")
    parser.add_argument("--engines", default=",".join(DUCKDB_ENGINES + PYTHON_ENGINES),
                        help="comma-separated engines to run (default: all)")
    parser.add_argument("--queries", default=",".join(ALL_QUERIES),
                        help="comma-separated queries to run (default: all)")
    parser.add_argument("--point-lookups", type=int, default=POINT_LOOKUPS,
                        help="point lookups per timed run")
    args = parser.parse_args()
    engines = [e for e in DUCKDB_ENGINES + PYTHON_ENGINES if e in args.engines.split(",")]
    queries = [q for q in ALL_QUERIES if q in args.queries.split(",")]
    unknown = set(args.engines.split(",")) - set(engines) | set(args.queries.split(",")) - set(queries)
    if unknown:
        parser.error(f"unknown engines or queries: {', '.join(sorted(unknown))}")

    env = lab_env()
    if args.engine:
        os.environ.update(env)
        run = python_engine if args.engine in PYTHON_ENGINES else duckdb_engine
        json.dump(run(args.engine, env, args.runs, queries, args.point_lookups), sys.stdout)
        return

    child = dict(os.environ, **env)
    child["ORACLE_CONNECT"] = (
        f"{env['ORKIDEA_LAB_USER']}/{env['ORKIDEA_LAB_PASSWORD']}@"
        f"{env['ORKIDEA_LAB_HOST']}:{env['ORKIDEA_LAB_PORT']}/{env['ORKIDEA_LAB_SERVICE']}"
    )
    if "ORACLE_IC" in os.environ:
        child["LD_LIBRARY_PATH"] = os.environ["ORACLE_IC"]

    report = {}
    for engine in engines:
        print(f"running {engine}", file=sys.stderr, flush=True)
        out = subprocess.run(
            [sys.executable, __file__, "--engine", engine, "--runs", str(args.runs),
             "--queries", ",".join(queries), "--point-lookups", str(args.point_lookups)],
            env=child, capture_output=True, text=True, check=False,
        )
        if out.returncode:
            sys.exit(f"{engine} failed:\n{out.stderr.replace(env['ORKIDEA_LAB_PASSWORD'], '***')}")
        report[engine] = json.loads(out.stdout)

    # Every engine must return what the first of its kind returned.
    for group in (DUCKDB_ENGINES, PYTHON_ENGINES):
        group = [e for e in group if e in report]
        if not group:
            continue
        base = report[group[0]]["results"]
        for engine in group[1:]:
            for name, value in report[engine]["results"].items():
                if "error" in value or "error" in base[name]:
                    continue
                if json.dumps(value["result"]) != json.dumps(base[name]["result"]):
                    sys.exit(f"{engine} returned {value['result']} for {name}, "
                             f"{group[0]} returned {base[name]['result']}")

    args.out.write_text(json.dumps(report, indent=1) + "\n")
    print_table(report)


def print_table(report):
    def row(engines, queries):
        engines = [e for e in engines if e in report]
        queries = {n: q for n, q in queries.items() if any(n in report[e]["results"] for e in engines)}
        if not engines or not queries:
            return
        print("| Query | " + " | ".join(engines) + " |")
        print("| --- |" + " ---: |" * len(engines))
        for name, (description, _) in queries.items():
            cells = []
            for engine in engines:
                result = report[engine]["results"].get(name)
                if result is None:
                    cells.append("")
                    continue
                if "error" in result:
                    cells.append("fails")
                    continue
                median = statistics.median(result["times"])
                cells.append(f"{median * 1000:.1f} ms" if median < 0.1 else f"{median:.2f} s")
            print(f"| {description} | " + " | ".join(cells) + " |")

    print("\n" + ", ".join(f"{e} {report[e]['version']}" for e in report) + "\n")
    row(DUCKDB_ENGINES, {**DUCKDB_QUERIES, "point_lookup": ("Point lookup on EMP (per query)", "")})
    print()
    row(PYTHON_ENGINES, PYTHON_QUERIES)
    for engine, value in report.items():
        for name, result in value["results"].items():
            if "error" in result:
                print(f"\n{engine}, {name}: {result['error']}")


if __name__ == "__main__":
    main()
