-- A table of the types Orkidea 0.3.0 read row by row (JSON, VECTOR,
-- intervals, NCLOB, ROWID) beside the common ones, for bench/mixed.py.
-- Runs as the lab user: lab/orkidea-lab.sh -c ".read ../bench/mixed.sql"
FROM orkidea_execute('lab', 'BEGIN EXECUTE IMMEDIATE ''DROP TABLE bench_mixed PURGE''; EXCEPTION WHEN OTHERS THEN NULL; END;');
FROM orkidea_execute('lab', 'CREATE TABLE bench_mixed (
    id NUMBER(10) PRIMARY KEY,
    amount NUMBER(12, 2),
    label VARCHAR2(40),
    created TIMESTAMP,
    doc JSON,
    embedding VECTOR(16, FLOAT32),
    took INTERVAL DAY(3) TO SECOND(3),
    note NCLOB)');
FROM orkidea_execute('lab', 'INSERT /*+ APPEND */ INTO bench_mixed
    SELECT level, MOD(level * 7919, 100000) / 100, ''label '' || level,
           TIMESTAMP ''2025-01-01 00:00:00'' + NUMTODSINTERVAL(level, ''SECOND''),
           JSON(''{"id":'' || level || '',"kind":"k'' || MOD(level, 9) || ''","tags":["a","b"],"score":'' || MOD(level, 1000) / 10 || ''}''),
           TO_VECTOR(''['' || MOD(level, 97) || '',1,2,3,4,5,6,7,8,9,10,11,12,13,14,'' || MOD(level, 13) || '']'', 16, FLOAT32),
           NUMTODSINTERVAL(MOD(level, 86400) + 0.25, ''SECOND''),
           TO_NCLOB(N''note '' || level)
    FROM dual CONNECT BY level <= 300000');
FROM orkidea_execute('lab', 'BEGIN DBMS_STATS.GATHER_TABLE_STATS(USER, ''BENCH_MIXED''); END;');
SELECT count(*) AS rows FROM orkidea_query('lab', 'SELECT id FROM bench_mixed');
