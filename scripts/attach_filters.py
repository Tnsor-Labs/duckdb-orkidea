"""Writes test/sql/attach_filters.test: pushed-down filters against DuckDB's own evaluation."""

import sys

TABLE = "ORKIDEA_DUCK_FILTERS"
COLUMNS = """id NUMBER(10), n NUMBER(12,3), nu NUMBER, bd BINARY_DOUBLE, bf BINARY_FLOAT,
 vc VARCHAR2(20), nv NVARCHAR2(20), ch CHAR(5), d DATE, ts TIMESTAMP(6),
 tstz TIMESTAMP(6) WITH TIME ZONE, tsltz TIMESTAMP WITH LOCAL TIME ZONE, r RAW(8), b BOOLEAN, cl CLOB"""
# One INSERT per row; NULL where a value is absent. Oracle stores '' as NULL.
ROWS = [
    "1, 1.5, 0.1, 0.1d, 1.5f, 'abc', N'abc', 'ab', TIMESTAMP '2024-01-15 10:30:00', TIMESTAMP '2024-01-15 10:30:00.123456', TIMESTAMP '2024-01-15 10:30:00 +02:00', TIMESTAMP '2024-01-15 10:30:00 +02:00', HEXTORAW('00FF'), TRUE, TO_CLOB('x')",
    "2, -2, -1e10, -BINARY_DOUBLE_INFINITY, BINARY_FLOAT_NAN, 'ABC', N'Ünïcödé', 'a', TIMESTAMP '1999-12-31 23:59:59', TIMESTAMP '1999-12-31 23:59:59.999999', TIMESTAMP '2000-01-01 00:00:00 -05:00', NULL, HEXTORAW('FF'), FALSE, NULL",
    "3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL",
    "4, 0.001, 1e30, BINARY_DOUBLE_NAN, -0f, 'abc ', N'zz', 'abc', TIMESTAMP '2024-01-15 00:00:00', TIMESTAMP '2024-01-15 00:00:00', TIMESTAMP '2024-01-15 08:30:00 +00:00', TIMESTAMP '2024-01-15 08:30:00 +00:00', HEXTORAW('00'), TRUE, NULL",
    "5, 0, 0, BINARY_DOUBLE_INFINITY, 0f, '', N'', 'é', TIMESTAMP '0001-01-01 00:00:00', TIMESTAMP '9999-12-31 23:59:59.999999', TIMESTAMP '9999-12-31 23:59:59 +00:00', NULL, NULL, NULL, EMPTY_CLOB()",
    "6, 999999999.999, 12345678901234567890, 1e300d, 3.4e38f, 'Zebra', N'日本', 'Z', TIMESTAMP '2100-02-28 12:00:00', TIMESTAMP '2024-01-15 10:30:00.123457', TIMESTAMP '2024-01-15 10:30:00.000001 +02:00', NULL, HEXTORAW('0000'), FALSE, NULL",
    "7, -0.001, -0.1, -0.1d, -1.5f, 'a''b', N'O''Neil', 'a b', TIMESTAMP '2024-01-15 10:29:59', TIMESTAMP '2024-01-15 10:29:59.999999', TIMESTAMP '2024-01-15 03:30:00 -05:00', NULL, HEXTORAW('01'), TRUE, NULL",
    "8, 1.5, 0.30000000000000004, 0.30000000000000004d, 1.5f, 'é', N'é', 'É', TIMESTAMP '2024-01-15 10:30:00', TIMESTAMP '2024-01-15 10:30:00', TIMESTAMP '2024-01-15 10:30:00 +02:00', NULL, HEXTORAW('00FF00'), NULL, NULL",
]
PREDICATES = [
    # integers and decimals
    "id = 3",
    "id <> 3",
    "id > 2 AND id < 6",
    "id IN (1, 3, 5)",
    "id NOT IN (1, 3, 5)",
    "id BETWEEN 2 AND 4",
    "id IS DISTINCT FROM 2",
    "id IS NOT DISTINCT FROM 2",
    "id = 1 OR id = 7",
    "NOT (id = 3)",
    "n = 1.5",
    "n > 0",
    "n < -1",
    "n BETWEEN -2 AND 1.5",
    "n IN (0, 1.5)",
    "n = 999999999.999",
    "n >= 0.001",
    "n IS NULL",
    "n IS NOT NULL",
    # unconstrained NUMBER, read as DOUBLE
    "nu = 0.1",
    "nu > 1e20",
    "nu < 0",
    "nu = 0.30000000000000004",
    "nu = 12345678901234567890",
    "nu <> 0",
    # binary floating point, with NaN and infinities
    "bd = 0.1",
    "bd > 0",
    "bd < 0",
    "bd = 'nan'::DOUBLE",
    "bd <> 'nan'::DOUBLE",
    "bd >= 'nan'::DOUBLE",
    "bd = 'inf'::DOUBLE",
    "bd < 'inf'::DOUBLE",
    "bd <= '-inf'::DOUBLE",
    "bd > 1e299",
    "bd IN (0.1, 'inf'::DOUBLE)",
    "bf = 1.5::FLOAT",
    "bf = 0::FLOAT",
    "bf < 0::FLOAT",
    "bf > 1e38::FLOAT",
    "bf = 'nan'::FLOAT",
    "bf > 'nan'::FLOAT",
    # strings: empty strings, trailing spaces, case, quotes, non-ASCII
    "vc = 'abc'",
    "vc = 'abc '",
    "vc <> 'abc'",
    "vc > 'B'",
    "vc < 'a'",
    "vc >= 'abc'",
    "vc <= 'Zebra'",
    "vc = ''",
    "vc <> ''",
    "vc > ''",
    "vc IN ('abc', 'Zebra')",
    "vc IN ('abc', '')",
    "vc = 'a''b'",
    "vc = 'é'",
    "vc > 'é'",
    "vc < 'é'",
    "vc IS NULL",
    "vc IS DISTINCT FROM 'abc'",
    "vc LIKE 'a%'",
    "vc ILIKE 'A%'",
    "id > 1 AND vc LIKE 'a%'",
    "vc = 'abc' OR vc LIKE 'Z%'",
    "lower(vc) = 'abc'",
    "COALESCE(vc, '') = ''",
    "nv = 'Ünïcödé'",
    "nv = '日本'",
    "nv > 'a'",
    "nv < 'zz'",
    "nv IN ('zz', '日本')",
    "nv = 'O''Neil'",
    "nv = ''",
    # CHAR is blank-padded in Oracle; only NULL checks are pushed
    "ch = 'ab'",
    "ch = 'ab   '",
    "ch > 'a'",
    "ch IS NULL",
    "ch IS NOT NULL",
    "ch = 'É'",
    # DATE (read as TIMESTAMP_S) and TIMESTAMP
    "d = TIMESTAMP '2024-01-15 10:30:00'",
    "d >= TIMESTAMP '2024-01-15'",
    "d < TIMESTAMP '2000-01-01'",
    "d > TIMESTAMP '2024-01-15 10:29:59.5'",
    "d = DATE '2024-01-15'",
    "d > DATE '2024-01-15'",
    "d <= TIMESTAMP '0001-01-01'",
    "d > TIMESTAMP '2100-01-01'",
    "d BETWEEN TIMESTAMP '1999-12-31' AND TIMESTAMP '2024-01-15 10:30:00'",
    "ts = TIMESTAMP '2024-01-15 10:30:00.123456'",
    "ts > TIMESTAMP '2024-01-15 10:30:00.123456'",
    "ts > TIMESTAMP '2024-01-15'",
    "ts <= TIMESTAMP '9999-12-31 23:59:59.999999'",
    "ts = 'infinity'::TIMESTAMP",
    "ts < 'infinity'::TIMESTAMP",
    "ts > '-infinity'::TIMESTAMP",
    "ts IN (TIMESTAMP '2024-01-15 10:30:00', TIMESTAMP '2024-01-15 00:00:00')",
    # TIMESTAMP WITH TIME ZONE compares instants; WITH LOCAL TIME ZONE is not pushed
    "tstz = TIMESTAMPTZ '2024-01-15 08:30:00+00'",
    "tstz < TIMESTAMPTZ '2001-01-01 00:00:00+00'",
    "tstz > TIMESTAMPTZ '2000-01-01 04:59:59+00'",
    "tstz >= TIMESTAMPTZ '2000-01-01 05:00:00+00'",
    "tstz > TIMESTAMPTZ '2024-01-15 08:30:00+00'",
    "tstz IS NULL",
    "tsltz > TIMESTAMP '2020-01-01'",
    "tsltz IS NULL",
    # RAW, BOOLEAN, CLOB
    "r = '\\x00\\xFF'::BLOB",
    "r > '\\x00'::BLOB",
    "r < '\\x00\\xFF'::BLOB",
    "r = '\\x00'::BLOB",
    "r IS NULL",
    "b",
    "NOT b",
    "b = true",
    "b IS NULL",
    "b IS DISTINCT FROM true",
    "cl = 'x'",
    "cl IS NULL",
    "cl IS NOT NULL",
    # several columns at once
    "n > 0 OR bd > 0",
    "id > 1 AND n > 0 AND vc IS NOT NULL",
    "(id = 1 OR id = 2) AND b",
]


def main(path):
    out = [
        "# name: test/sql/attach_filters.test",
        "# description: Filters pushed into Oracle select the same rows as DuckDB evaluating them itself.",
        "# group: [sql]",
        "",
        "# Generated by scripts/attach_filters.py; edit the script, not this file.",
        "",
        "require orkidea",
        "",
        "require-env ORKIDEA_TEST_URI",
        "",
        "statement ok",
        "CREATE SECRET ora (TYPE orkidea, URI '{ORKIDEA_TEST_URI}');",
        "",
        "statement ok",
        f"FROM orkidea_execute('ora', 'BEGIN EXECUTE IMMEDIATE ''DROP TABLE {TABLE} PURGE''; EXCEPTION WHEN OTHERS THEN NULL; END;');",
        "",
        "statement ok",
        f"FROM orkidea_execute('ora', 'CREATE TABLE {TABLE} ({' '.join(COLUMNS.split())})');",
        "",
    ]
    for row in ROWS:
        sql = f"INSERT INTO {TABLE} VALUES ({row})".replace("'", "''")
        out += ["statement ok", f"FROM orkidea_execute('ora', '{sql}');", ""]
    out += [
        "statement ok",
        "ATTACH 'ora' AS o (TYPE orkidea, READ_ONLY);",
        "",
        "# DuckDB evaluates every filter on this copy itself.",
        "statement ok",
        f"CREATE TABLE local_copy AS FROM orkidea_query('ora', 'SELECT * FROM {TABLE}');",
        "",
    ]
    for predicate in PREDICATES:
        pushed = f"(SELECT list(id ORDER BY id) FROM o.{TABLE} WHERE {predicate})"
        local = f"(SELECT list(id ORDER BY id) FROM local_copy WHERE {predicate})"
        quoted = predicate.replace("'", "''")
        out += [
            "query III",
            f"SELECT '{quoted}', pushed, local FROM (SELECT {pushed} AS pushed, {local} AS local) WHERE pushed IS DISTINCT FROM local;",
            "----",
            "",
        ]
    out += [
        "statement ok",
        "DETACH o;",
        "",
        "statement ok",
        f"FROM orkidea_execute('ora', 'DROP TABLE {TABLE} PURGE');",
        "",
    ]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
