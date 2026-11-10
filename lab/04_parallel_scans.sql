-- Module 4: parallel scans.
-- A table with 500,000 rows or more (by Oracle's statistics) is read through
-- several Oracle sessions at once, each reading ranges of its ROWIDs. All of
-- them read AS OF one SCN, so together they see one consistent snapshot.

.timer on

-- One session, then four, then the default (as many as DuckDB has threads, up to 8).
SET orkidea_parallel_scan = 1;
CREATE OR REPLACE TABLE orders_one AS FROM oracle.orders;
SET orkidea_parallel_scan = 4;
CREATE OR REPLACE TABLE orders_four AS FROM oracle.orders;
RESET orkidea_parallel_scan;
CREATE OR REPLACE TABLE orders_local AS FROM oracle.orders;
-- → on a laptop: about 1.7 s, 1.2 s, and 0.8 s for a million rows

.timer off

-- The same rows, and here in the same order: several sessions give ROWID
-- order, and one session the order of the table's extents, which is the same
-- when the extents lie in block order, as they usually do. Without ORDER BY,
-- Oracle promises neither.
SELECT (SELECT count(*) FROM orders_one) AS one, (SELECT count(*) FROM orders_four) AS four,
       (SELECT count(*) FROM orders_local) AS default_sessions,
       (SELECT bool_and(a.order_id = b.order_id)
        FROM (SELECT order_id, row_number() OVER () AS r FROM orders_one) a
        JOIN (SELECT order_id, row_number() OVER () AS r FROM orders_four) b USING (r)) AS same_order;
-- → 1000000, 1000000, 1000000, true

-- Smaller tables use one session; lower the threshold to split them too.
SET orkidea_parallel_scan_min_rows = 10000;
CREATE OR REPLACE TABLE customers_local AS FROM oracle.customers;
RESET orkidea_parallel_scan_min_rows;

-- The local copy is a plain DuckDB table now: no round trips at all.
SELECT status, count(*) FROM orders_local GROUP BY status ORDER BY status;

DROP TABLE orders_one;
DROP TABLE orders_four;
