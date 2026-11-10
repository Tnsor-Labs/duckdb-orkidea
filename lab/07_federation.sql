-- Module 7: Oracle next to everything else DuckDB reads.
-- An attached Oracle schema is one catalog among others: join it with local
-- tables and files, export it, and load files into it. (Postgres, MySQL,
-- SQLite, Iceberg, and object storage work the same way, through their
-- DuckDB extensions.)

-- Export from Oracle to Parquet and CSV; the aggregate runs in Oracle.
COPY (SELECT product_id, count(*) AS orders, sum(amount) AS revenue
      FROM oracle.orders WHERE status = 'SHIPPED' GROUP BY product_id)
TO 'out/revenue_by_product.parquet';
COPY (SELECT customer_id, full_name, country FROM oracle.customers WHERE vip)
TO 'out/vip_customers.csv' (HEADER);

-- A local file joined with Oracle tables: which categories earn most per order?
SELECT p.category, round(sum(r.revenue) / sum(r.orders), 2) AS revenue_per_order
FROM 'out/revenue_by_product.parquet' r
JOIN oracle.products p USING (product_id)
GROUP BY p.category ORDER BY revenue_per_order DESC;
-- → Books 527.45, Garden 409.96, Kitchen 322.47, Tea 209.31, Coffee 103.66

-- A local DuckDB table (targets) joined with Oracle data.
CREATE OR REPLACE TABLE targets AS
SELECT * FROM (VALUES ('Africa', 3500), ('Americas', 4200), ('Asia', 3800), ('Europe', 4500), ('Oceania', 3000))
    t(region, target_customers);
SELECT r.name AS region, count(*) AS customers, t.target_customers, count(*) >= t.target_customers AS on_target
FROM oracle.customers c JOIN oracle.regions r USING (region_id) JOIN targets t ON t.region = r.name
GROUP BY r.name, t.target_customers ORDER BY region;
-- → Africa, Asia, and Oceania on target; Americas and Europe not

-- Load a file into Oracle: a new table from a CSV, then rows appended to it.
DROP TABLE IF EXISTS oracle.lab_vip_import;
CREATE TABLE oracle.lab_vip_import AS FROM read_csv('out/vip_customers.csv');
INSERT INTO oracle.lab_vip_import SELECT customer_id + 100000, full_name || ' (copy)', country FROM 'out/vip_customers.csv' LIMIT 3;
SELECT count(*) AS imported FROM oracle.lab_vip_import;
-- → 428: the 425 VIPs, and 3 more

-- And the other way: Oracle rows into a DuckDB database file.
ATTACH 'out/lab.duckdb' AS archive;
CREATE OR REPLACE TABLE archive.returned_orders AS
SELECT * FROM oracle.orders WHERE status = 'RETURNED';
SELECT count(*) AS archived FROM archive.returned_orders;
-- → 50000
DETACH archive;

DROP TABLE oracle.lab_vip_import;
