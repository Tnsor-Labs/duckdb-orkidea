-- Module 0: connect. lab/orkidea-lab.sh runs this for you after loading the
-- extension; it is here to show what connecting takes.

-- A secret holds the connection. The password comes from the environment
-- (lab/.lab.env), so it never appears in a query or in duckdb_secrets().
CREATE OR REPLACE SECRET lab (
    TYPE orkidea,
    HOST getenv('ORKIDEA_LAB_HOST'),
    PORT getenv('ORKIDEA_LAB_PORT')::INTEGER,
    SERVICE getenv('ORKIDEA_LAB_SERVICE'),
    USER getenv('ORKIDEA_LAB_USER'),
    PASSWORD getenv('ORKIDEA_LAB_PASSWORD')
);

-- Attach the Oracle schema as a DuckDB catalog named `oracle`.
ATTACH IF NOT EXISTS 'lab' AS oracle (TYPE orkidea);

SELECT 'Connected to Oracle as ' || getenv('ORKIDEA_LAB_USER') || '; the lab schema is attached as `oracle`.' AS lab;
