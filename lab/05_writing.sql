-- Module 5: write to Oracle with DuckDB SQL.
-- CREATE, INSERT, UPDATE, DELETE, upserts, MERGE, and ALTER TABLE, inside
-- DuckDB transactions. Everything here works on lab_* tables it creates, so
-- the module can be run again.

DROP TABLE IF EXISTS oracle.lab_items;
DROP TABLE IF EXISTS oracle.lab_vips;
FROM orkidea_execute('lab', 'BEGIN EXECUTE IMMEDIATE ''DROP TABLE lab_tickets PURGE''; EXCEPTION WHEN OTHERS THEN NULL; END;');

-- Create a table: DuckDB types become Oracle types (VARCHAR → VARCHAR2(4000),
-- DECIMAL(10,2) → NUMBER(10,2), ...), with NOT NULL and PRIMARY KEY.
CREATE TABLE oracle.lab_items (id INTEGER PRIMARY KEY, name VARCHAR NOT NULL, category VARCHAR, price DECIMAL(10,2));

-- Insert values, and rows from anywhere DuckDB reads: here, a generated series.
INSERT INTO oracle.lab_items VALUES (1, 'Rooibos', 'Tea', 4.50), (2, 'Espresso beans', 'Coffee', 12.00);
INSERT INTO oracle.lab_items
SELECT 100 + i, 'Notebook ' || i, 'Books', 2.50 + i FROM range(1, 6) t(i);
SELECT * FROM oracle.lab_items ORDER BY id;
-- → 7 rows: 1, 2, 101..105

-- CREATE TABLE ... AS: from Oracle to Oracle, through DuckDB, in one statement.
CREATE TABLE oracle.lab_vips AS
SELECT c.customer_id, c.full_name, r.name AS region
FROM oracle.customers c JOIN oracle.regions r USING (region_id) WHERE c.vip;
SELECT region, count(*) FROM oracle.lab_vips GROUP BY region ORDER BY region;
-- → Africa 85, Americas 84, Asia 86, Europe 85, Oceania 85

-- UPDATE and DELETE find their rows by Oracle ROWID; any DuckDB expression
-- works in SET. RETURNING gives the rows as Oracle stored them.
UPDATE oracle.lab_items SET price = round(price * 1.10, 2) WHERE category = 'Books';
DELETE FROM oracle.lab_items WHERE id = 105 RETURNING id, name;
-- → 105, Notebook 5
SELECT rowid, id, price FROM oracle.lab_items WHERE category = 'Books' ORDER BY id;
-- → rowid is the Oracle ROWID as text (AAAS...)

-- Upserts, through the primary key.
INSERT INTO oracle.lab_items VALUES (1, 'Rooibos', 'Tea', 3.99)
ON CONFLICT (id) DO UPDATE SET price = excluded.price;
INSERT OR REPLACE INTO oracle.lab_items VALUES (2, 'Espresso beans, 1 kg', 'Coffee', 21.00);
SELECT id, name, price FROM oracle.lab_items WHERE id IN (1, 2) ORDER BY id;
-- → 1 Rooibos 3.99; 2 Espresso beans, 1 kg 21.00

-- MERGE INTO, from local data.
MERGE INTO oracle.lab_items t
USING (VALUES (1, 'Rooibos', 'Tea', 4.25), (3, 'Green tea', 'Tea', 5.00)) s(id, name, category, price)
ON t.id = s.id
WHEN MATCHED THEN UPDATE SET price = s.price
WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.name, s.category, s.price);
SELECT id, name, price FROM oracle.lab_items WHERE category = 'Tea' ORDER BY id;
-- → 1 Rooibos 4.25; 3 Green tea 5.00

-- Transactions: the writes are one Oracle transaction. Its queries see them;
-- ROLLBACK undoes them.
BEGIN;
DELETE FROM oracle.lab_items WHERE category = 'Books';
SELECT count(*) AS books_inside_the_transaction FROM oracle.lab_items WHERE category = 'Books';
ROLLBACK;
SELECT count(*) AS books_after_rollback FROM oracle.lab_items WHERE category = 'Books';
-- → 0, then 4

-- ALTER TABLE: add, rename, retype, and constrain columns. Oracle commits DDL
-- at once, so it runs on a session of its own.
ALTER TABLE oracle.lab_items ADD COLUMN in_stock BOOLEAN DEFAULT true;
ALTER TABLE oracle.lab_items RENAME COLUMN name TO title;
ALTER TABLE oracle.lab_items ALTER COLUMN price TYPE DECIMAL(12,2);
ALTER TABLE oracle.lab_items ALTER COLUMN category SET NOT NULL;
DESCRIBE oracle.lab_items;

-- Oracle's own defaults and identity columns: columns an INSERT leaves out
-- are filled by Oracle, and RETURNING shows what it chose.
FROM orkidea_execute('lab', $$CREATE TABLE lab_tickets (
    id NUMBER(9) GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    subject VARCHAR2(80) NOT NULL,
    status VARCHAR2(10) DEFAULT 'OPEN' NOT NULL,
    opened TIMESTAMP DEFAULT SYSTIMESTAMP)$$);
INSERT INTO oracle.lab_tickets (subject) VALUES ('Printer on fire'), ('Coffee machine empty') RETURNING id, subject, status;
-- → 1 Printer on fire OPEN; 2 Coffee machine empty OPEN
UPDATE oracle.lab_tickets SET status = 'CLOSED' WHERE id = 2;
UPDATE oracle.lab_tickets SET status = DEFAULT WHERE id = 2;
SELECT id, status FROM oracle.lab_tickets ORDER BY id;
-- → both OPEN again: DEFAULT means Oracle's default

-- Clean up. DROP sends the table to Oracle's recycle bin.
DROP TABLE oracle.lab_items;
DROP TABLE oracle.lab_vips;
DROP TABLE oracle.lab_tickets;
