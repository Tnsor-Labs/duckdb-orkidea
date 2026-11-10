-- Module 2: what runs in Oracle.
-- A scan reads only the columns a query needs, and sends filters, LIMIT, and
-- ORDER BY ... LIMIT to Oracle when Oracle gives exactly DuckDB's answer.

-- Log the SQL the extension sends, to see it for each query below.
CALL enable_logging('OrkideaQuery', level = 'debug', storage = 'memory');
CREATE OR REPLACE MACRO oracle_sql() AS TABLE
    SELECT sql, duration_ms FROM duckdb_logs_parsed('OrkideaQuery')
    WHERE sql NOT LIKE '%all_tables%' AND sql NOT LIKE '%all_users%' AND sql NOT LIKE '%all_constraints%';

-- Filters and the columns read: EXPLAIN shows them on the Orkidea Scan.
EXPLAIN SELECT order_id, amount FROM oracle.orders WHERE status = 'RETURNED' AND amount > 200;

-- A top-N runs in Oracle: ORDER BY ... FETCH FIRST 3 ROWS ONLY.
CALL truncate_duckdb_logs();
SELECT order_id, amount FROM oracle.orders
WHERE status = 'RETURNED' AND amount > 200
ORDER BY amount DESC, order_id LIMIT 3;
-- → 20 202.99, 120 202.99, 320 202.99
FROM oracle_sql();
-- → SELECT "STATUS", "AMOUNT", "ORDER_ID" FROM "ORKIDEA_LAB"."ORDERS"
--   WHERE "STATUS" = 'RETURNED' AND ("AMOUNT" > 200.00)
--   ORDER BY "AMOUNT" DESC NULLS LAST, "ORDER_ID" ASC NULLS LAST FETCH FIRST 3 ROWS ONLY

-- IN, BETWEEN, IS NULL, dates, and prefix LIKE go to Oracle too.
CALL truncate_duckdb_logs();
SELECT count(*) FROM oracle.customers
WHERE country IN ('Mozambique', 'Portugal') AND email IS NULL
  AND signed_up BETWEEN DATE '2021-01-01' AND DATE '2022-12-31' AND full_name LIKE 'Customer 1%';
-- → 56
FROM oracle_sql();
-- → every condition in the WHERE; LIKE 'Customer 1%' as a range

-- What Oracle would answer differently stays in DuckDB, which filters the rows
-- Oracle returns. ILIKE is one, and an empty string another, because Oracle
-- stores '' as NULL.
CALL truncate_duckdb_logs();
SELECT count(*) FROM oracle.customers WHERE email ILIKE '%7@EXAMPLE.COM';   -- → 1882
SELECT count(*) FROM oracle.customers WHERE email = '';                    -- → 0
FROM oracle_sql();
-- → SELECT "EMAIL" FROM "ORKIDEA_LAB"."CUSTOMERS", twice: no WHERE, DuckDB filters.

-- DuckDB still sends what it safely can: a prefix ILIKE becomes a range that
-- holds every match, and DuckDB applies the ILIKE to what comes back.
CALL truncate_duckdb_logs();
SELECT count(*) FROM oracle.customers WHERE email ILIKE 'CUSTOMER7%';   -- → 1046
FROM oracle_sql();
-- → ... WHERE ("EMAIL" >= 'CUSTOMER7' AND "EMAIL" < 'customer8')

CALL disable_logging();
