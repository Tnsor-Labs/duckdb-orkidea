-- Module 1: explore an Oracle schema from DuckDB.
-- Oracle tables read like DuckDB tables: list them, describe them, join them.

-- The attached schema's tables, and their DuckDB column types.
SHOW TABLES FROM oracle;
DESCRIBE oracle.customers;

-- Names match case-insensitively: oracle.Customers is CUSTOMERS.
SELECT count(*) AS customers FROM oracle.Customers;
-- → 20000

-- Joins between Oracle tables, with DuckDB SQL (GROUP BY ALL, ORDER BY ALL).
SELECT r.name AS region, count(*) AS customers, count(*) FILTER (WHERE c.vip) AS vips
FROM oracle.customers c JOIN oracle.regions r USING (region_id)
GROUP BY ALL ORDER BY ALL;
-- → Africa 4000/85, Americas 4000/84, Asia 4000/86, Europe 4000/85, Oceania 4000/85

-- Every Oracle type has an exact DuckDB type: NUMBER(p,s) as DECIMAL or the
-- narrowest integer, DATE as TIMESTAMP_S, TIMESTAMP WITH TIME ZONE as
-- TIMESTAMPTZ, INTERVAL as INTERVAL, RAW as BLOB, JSON as JSON, BOOLEAN, and
-- VECTOR as a fixed-size array.
SELECT column_name, column_type FROM (DESCRIBE oracle.type_showcase);
FROM oracle.type_showcase;

-- They behave as DuckDB values: JSON paths, vector math, date arithmetic.
SELECT doc->>'$.tags[0]'                                        AS first_tag,
       array_cosine_similarity(embedding, [1, 2, 3]::FLOAT[3])  AS same_direction,
       day + INTERVAL 30 DAY                                    AS thirty_days_later,
       at_utc AT TIME ZONE 'UTC'                                AS utc_wall_clock,
       wait + INTERVAL 1 HOUR                                   AS a_bit_longer
FROM oracle.type_showcase;
-- → lab, 1.0, 2026-12-10 00:00:00, 2026-11-10 07:30:00, 27:03:04.5

-- The secret's password never shows.
SELECT name, type, secret_string FROM duckdb_secrets() WHERE name = 'lab';
-- → ... password=redacted ...
