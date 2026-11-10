"""Reads bench_mixed (see mixed.sql) into Arrow through one session, with
whichever adbc-driver-orkidea is installed and with python-oracledb, and
prints the median of five runs after one to warm up.

    source lab/.lab.env && .venv/bin/python bench/mixed.py
"""

import os
import statistics
import time

import adbc_driver_orkidea
import oracledb
import pyarrow as pa
from adbc_driver_orkidea import dbapi

QUERIES = {
    "all 9 columns": "SELECT ROWID AS rid, m.* FROM bench_mixed m",
    "JSON": "SELECT id, doc FROM bench_mixed",
    "VECTOR": "SELECT id, embedding FROM bench_mixed",
    "INTERVAL": "SELECT id, took FROM bench_mixed",
    "NCLOB": "SELECT id, note FROM bench_mixed",
    "common types only": "SELECT id, amount, label, created FROM bench_mixed",
}
RUNS = 5

env = os.environ
host, port, service = env["ORKIDEA_LAB_HOST"], env["ORKIDEA_LAB_PORT"], env["ORKIDEA_LAB_SERVICE"]
user, password = env["ORKIDEA_LAB_USER"], env["ORKIDEA_LAB_PASSWORD"]


def median_time(fetch):
    fetch()
    times = []
    for _ in range(RUNS):
        start = time.perf_counter()
        rows = fetch()
        times.append(time.perf_counter() - start)
    return statistics.median(times), rows


def orkidea():
    conn = dbapi.connect(f"oracle://{user}@{host}:{port}/{service}", db_kwargs={"password": password})

    def run(sql):
        def fetch():
            with conn.cursor() as cur:
                cur.execute(sql)
                return cur.fetch_arrow_table().num_rows
        return fetch
    return run


def python_oracledb():
    conn = oracledb.connect(user=user, password=password, dsn=f"{host}:{port}/{service}")

    def run(sql):
        return lambda: pa.table(conn.fetch_df_all(sql, arraysize=10000)).num_rows
    return run


drivers = {
    f"orkidea {adbc_driver_orkidea.__version__}": orkidea(),
    f"python-oracledb {oracledb.__version__} (arraysize=10000)": python_oracledb(),
}
print("| Query | " + " | ".join(drivers) + " |")
print("| --- |" + " ---: |" * len(drivers))
for name, sql in QUERIES.items():
    cells = []
    for run in drivers.values():
        try:
            seconds, rows = median_time(run(sql))
            assert rows == 300000, rows
            cells.append(f"{seconds:.2f} s")
        except Exception as error:  # a driver that cannot read the type
            cells.append(f"fails: {type(error).__name__}")
    print(f"| {name} | " + " | ".join(cells) + " |", flush=True)
