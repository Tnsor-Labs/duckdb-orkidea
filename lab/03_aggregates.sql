-- Module 3: aggregates that run in Oracle.
-- GROUP BY, DISTINCT, and count/sum/avg/min/max directly over a table become
-- one Oracle query, so only the groups travel. Results keep DuckDB's types.

.timer on

-- The plan has one Orkidea Remote Query, with the GROUP BY in its Oracle SQL.
EXPLAIN SELECT status, count(*) AS orders, sum(amount) AS revenue, max(ordered_at) AS latest
FROM oracle.orders GROUP BY status;

SELECT status, count(*) AS orders, sum(amount) AS revenue, max(ordered_at) AS latest
FROM oracle.orders GROUP BY status ORDER BY status;
-- → CANCELLED  50000  11199000.00  2025-12-31 23:54:59
--   RETURNED   50000   5749500.00  2025-12-31 23:46:40
--   SHIPPED   900000 298921500.00  2025-12-31 23:59:52

-- DuckDB's types, exactly: count is BIGINT, a sum of DECIMAL(12,2) is DECIMAL(38,2).
SELECT typeof(count(*)), typeof(sum(amount)) FROM oracle.orders;

-- Filters that Oracle runs come along into the same query.
SELECT product_id, count(*) AS orders, avg(quantity) AS avg_qty
FROM oracle.orders WHERE status = 'SHIPPED' AND ordered_at >= TIMESTAMP '2025-12-01'
GROUP BY product_id ORDER BY orders DESC, product_id LIMIT 5;

-- DISTINCT, and count(DISTINCT ...).
SELECT DISTINCT country FROM oracle.customers ORDER BY country;
SELECT count(DISTINCT customer_id) AS buyers FROM oracle.orders WHERE status = 'RETURNED';

-- HAVING, ORDER BY, and joins sit above the pushed aggregate and run in DuckDB.
SELECT p.category, sum(t.revenue) AS revenue
FROM (SELECT product_id, sum(amount) AS revenue FROM oracle.orders GROUP BY product_id) t
JOIN oracle.products p USING (product_id)
GROUP BY p.category HAVING sum(t.revenue) > 0 ORDER BY revenue DESC;

-- Not pushed: expressions, FILTER, and functions Oracle lacks. DuckDB reads the
-- rows and aggregates them; the answer is the same.
EXPLAIN SELECT sum(amount * 0.9) FROM oracle.orders;                 -- an expression
EXPLAIN SELECT median(amount) FROM oracle.orders;                    -- no median pushdown

-- Compare: the same GROUP BY with pushdown off reads a million rows into DuckDB.
SELECT product_id, count(*), sum(amount) FROM oracle.orders GROUP BY product_id ORDER BY product_id LIMIT 3;
SET orkidea_aggregate_pushdown = false;
SELECT product_id, count(*), sum(amount) FROM oracle.orders GROUP BY product_id ORDER BY product_id LIMIT 3;
RESET orkidea_aggregate_pushdown;
-- → the same three rows; compare the two "Run Time" lines.

.timer off
