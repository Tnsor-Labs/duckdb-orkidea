"""Module 9: the same database from Python, through the Orkidea ADBC driver.

The DuckDB extension is built on this driver. On its own, it gives any ADBC
client (Python, R, Rust, Go, ...) Arrow data from Oracle, with no Oracle client.

    python -m venv .venv && .venv/bin/pip install "adbc-driver-orkidea[dbapi]>=0.2"
    source lab/.lab.env && .venv/bin/python lab/09_python.py
"""

import os
import time

import pyarrow as pa
import adbc_driver_orkidea
from adbc_driver_orkidea import dbapi

env = os.environ
uri = f"oracle://{env['ORKIDEA_LAB_USER']}@{env['ORKIDEA_LAB_HOST']}:{env['ORKIDEA_LAB_PORT']}/{env['ORKIDEA_LAB_SERVICE']}"


def section(title):
    print(f"\n== {title}")


with dbapi.connect(uri, db_kwargs={"password": env["ORKIDEA_LAB_PASSWORD"]}, autocommit=True) as conn:
    with conn.cursor() as cur:
        section("Query results arrive as Arrow tables")
        cur.execute("SELECT region_id, name FROM regions ORDER BY region_id")
        table = cur.fetch_arrow_table()
        print(table.schema)
        print(table.to_pylist()[:2])

        section("Parameters: a dict binds by name, and a repeated name takes one value")
        cur.execute("SELECT ename FROM emp WHERE empno = :boss OR mgr = :boss ORDER BY ename", {"boss": 7698})
        print([row[0] for row in cur.fetchall()])
        cur.execute("SELECT ename FROM emp WHERE deptno = :1 AND sal > :2 ORDER BY ename", (20, 2000))
        print([row[0] for row in cur.fetchall()])

        section("Large results stream in batches")
        start = time.perf_counter()
        cur.execute("SELECT order_id, amount, status FROM orders")
        batches = rows = 0
        for batch in cur.fetch_record_batch():
            batches += 1
            rows += batch.num_rows
        print(f"{rows} rows in {batches} batches, {time.perf_counter() - start:.2f} s")

        section("Bulk load an Arrow table into a new Oracle table, then append")
        cur.execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE \"LabReadings\" PURGE'; EXCEPTION WHEN OTHERS THEN NULL; END;")
        readings = pa.table({
            "sensor": ["a", "b", "c"] * 1000,
            "reading": pa.array([i / 10 for i in range(3000)], pa.float64()),
            "taken_at": pa.array([1_700_000_000_000_000 + i for i in range(3000)], pa.timestamp("us", "UTC")),
        })
        cur.adbc_ingest('"LabReadings"', readings, mode="create")   # quoted: the table is named LabReadings
        cur.adbc_ingest('"LabReadings"', readings.slice(0, 10), mode="append")
        cur.execute('SELECT sensor, count(*), max(reading) FROM "LabReadings" GROUP BY sensor ORDER BY sensor')
        print(cur.fetchall())

        section("Table schemas, with Oracle's own type names")
        # As in Oracle SQL, an unquoted name is upper-cased (LABREADINGS), and a
        # quoted one is taken exactly.
        schema = conn.adbc_get_table_schema('"LabReadings"')
        for field in schema:
            print(field.name, field.type, field.metadata[adbc_driver_orkidea.TYPE_NAME_KEY.encode()].decode())

        section("PL/SQL OUT parameters come back as a row")
        # An OUT parameter is passed as None; the block's outputs are the result.
        cur.execute("BEGIN SELECT ename || ' (' || job || ')' INTO :who FROM emp WHERE empno = :empno; END;",
                    {"empno": 7839, "who": None})
        print(cur.fetch_arrow_table().to_pylist())

        cur.execute('DROP TABLE "LabReadings" PURGE')
