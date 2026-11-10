-- Module 8: other ways to connect.
-- A secret can name its database by host, port, and service (module 0), by a
-- tnsnames.ora alias, by a full TNS descriptor, or by a URI.

-- A tnsnames.ora alias. setup.sh wrote lab/tns/tnsnames.ora, with a comment
-- line before the alias (aliases after comments are found since driver 0.2.0).
CREATE OR REPLACE SECRET lab_tns (
    TYPE orkidea,
    CONNECT_STRING 'orkidea_lab',
    CONFIG_DIR getenv('ORKIDEA_LAB_TNS_ADMIN'),
    USER getenv('ORKIDEA_LAB_USER'),
    PASSWORD getenv('ORKIDEA_LAB_PASSWORD')
);
FROM orkidea_query('lab_tns', 'SELECT sys_context(''USERENV'', ''SERVICE_NAME'') AS service, user AS who FROM dual');
-- → freepdb1, ORKIDEA_LAB

-- A full descriptor, without any file.
CREATE OR REPLACE SECRET lab_descriptor (
    TYPE orkidea,
    CONNECT_STRING '(DESCRIPTION=(ADDRESS=(PROTOCOL=TCP)(HOST=' || getenv('ORKIDEA_LAB_HOST') || ')(PORT='
        || getenv('ORKIDEA_LAB_PORT') || '))(CONNECT_DATA=(SERVICE_NAME=' || getenv('ORKIDEA_LAB_SERVICE') || ')))',
    USER getenv('ORKIDEA_LAB_USER'),
    PASSWORD getenv('ORKIDEA_LAB_PASSWORD')
);
FROM orkidea_query('lab_descriptor', 'SELECT count(*) AS products FROM products');
-- → 500

-- A second, read-only attachment of the same schema, through the alias.
ATTACH 'lab_tns' AS oracle_ro (TYPE orkidea, READ_ONLY);
SELECT count(*) FROM oracle_ro.regions;
-- Writes are refused (uncomment to see it):
-- INSERT INTO oracle_ro.regions VALUES (9, 'Antarctica');
DETACH oracle_ro;

-- Session settings from the secret: TIME_ZONE sets Oracle's session time zone.
CREATE OR REPLACE SECRET lab_tokyo (
    TYPE orkidea,
    HOST getenv('ORKIDEA_LAB_HOST'), PORT getenv('ORKIDEA_LAB_PORT')::INTEGER,
    SERVICE getenv('ORKIDEA_LAB_SERVICE'),
    USER getenv('ORKIDEA_LAB_USER'), PASSWORD getenv('ORKIDEA_LAB_PASSWORD'),
    TIME_ZONE 'Asia/Tokyo'
);
FROM orkidea_query('lab_tokyo', 'SELECT sessiontimezone AS tz FROM dual');
-- → Asia/Tokyo

-- No passwords in any of them.
SELECT name, secret_string FROM duckdb_secrets() WHERE type = 'orkidea' ORDER BY name;

DROP SECRET lab_tns;
DROP SECRET lab_descriptor;
DROP SECRET lab_tokyo;
