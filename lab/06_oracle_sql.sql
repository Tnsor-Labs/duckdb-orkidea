-- Module 6: run Oracle SQL, with bind parameters.
-- orkidea_query runs a query and returns its rows as a table; orkidea_execute
-- runs DDL, DML, or PL/SQL and returns the rows it affected. Both take
-- params := ..., bound as Oracle binds: never pasted into the SQL.

-- Oracle's own SQL and functions; quote SQL that has quotes with $$.
FROM orkidea_query('lab', $$SELECT ename, job, sal, RANK() OVER (ORDER BY sal DESC) AS pay_rank
                            FROM emp WHERE job <> 'PRESIDENT' FETCH FIRST 3 ROWS ONLY$$);
-- → FORD and SCOTT 3000 (rank 1), JONES 2975 (rank 3)

-- By name: a STRUCT's fields bind the placeholders of their names, in any order.
FROM orkidea_query('lab', 'SELECT ename, sal FROM emp WHERE deptno = :dept AND sal >= :min_sal ORDER BY sal DESC',
                   params := {'min_sal': 1500, 'dept': 30});
-- → BLAKE 2850, ALLEN 1600, TURNER 1500

-- By position: row(...) gives one value per placeholder, in order.
FROM orkidea_query('lab', 'SELECT ename, hiredate FROM emp WHERE hiredate BETWEEN :1 AND :2 ORDER BY hiredate',
                   params := row(DATE '1981-09-01', DATE '1981-12-31'));
-- → TURNER, MARTIN, KING, JAMES, FORD

-- A placeholder used twice takes the same value both times.
FROM orkidea_query('lab', 'SELECT ename, empno, mgr FROM emp WHERE empno = :boss OR mgr = :boss ORDER BY empno',
                   params := {'boss': 7698});
-- → BLAKE and the five people who report to him

-- Values are data, never SQL: this finds nobody instead of everybody.
FROM orkidea_query('lab', 'SELECT count(*) AS found FROM emp WHERE ename = :name',
                   params := {'name': 'x'' OR ''1''=''1'});
-- → 0

-- Prepared statements bind new values on each execution.
PREPARE staff_of AS
    FROM orkidea_query('lab', 'SELECT ename, job FROM emp WHERE deptno = :d ORDER BY ename', params := {'d': $1});
EXECUTE staff_of(10);
EXECUTE staff_of(20);
DEALLOCATE staff_of;

-- Oracle describes a query before it sees the values, so CAST a placeholder
-- that appears in the select list:
FROM orkidea_query('lab', 'SELECT CAST(:d AS DATE) AS d, CAST(:n AS NUMBER(9)) AS n FROM dual',
                   params := {'d': DATE '2026-11-10', 'n': 42});
-- Uncast, it fails with an error naming the column (uncomment to see it):
-- FROM orkidea_query('lab', 'SELECT :d AS d FROM dual', params := {'d': DATE '2026-11-10'});

-- orkidea_execute: DDL, DML, and PL/SQL, each on its own connection, committed.
FROM orkidea_execute('lab', 'BEGIN EXECUTE IMMEDIATE ''DROP TABLE lab_bonus PURGE''; EXCEPTION WHEN OTHERS THEN NULL; END;');
FROM orkidea_execute('lab', 'CREATE TABLE lab_bonus (empno NUMBER(4), amount NUMBER(7,2), note VARCHAR2(40))');
FROM orkidea_execute('lab', 'INSERT INTO lab_bonus VALUES (:empno, :amount, :note)',
                     params := {'empno': 7788, 'amount': 250.50, 'note': 'Q4 bonus'});
FROM orkidea_execute('lab', 'INSERT INTO lab_bonus VALUES (:1, :2, :3)', params := row(7902, 300, 'Q4 bonus'));
FROM orkidea_execute('lab', 'UPDATE lab_bonus SET note = note || :tag WHERE note NOT LIKE ''%'' || :tag',
                     params := {'tag': ' (approved)'});
-- → 1, 1, 2 rows affected
FROM orkidea_query('lab', 'SELECT e.ename, b.amount, b.note FROM lab_bonus b JOIN emp e ON e.empno = b.empno ORDER BY 1');

-- PL/SQL: call a procedure with parameters.
SET VARIABLE king_sal = (SELECT sal FROM oracle.emp WHERE empno = 7839);
FROM orkidea_execute('lab', 'BEGIN raise_salary(:empno, :pct); END;', params := row(7839, 10));
SELECT sal AS now, getvariable('king_sal') AS before FROM oracle.emp WHERE empno = 7839;
-- → 5500.00 and 5000.00 on a fresh lab
-- The procedure's own error comes back as is (uncomment to see it):
-- FROM orkidea_execute('lab', 'BEGIN raise_salary(:empno, :pct); END;', params := row(1, 10));   -- ORA-20001
FROM orkidea_execute('lab', 'UPDATE emp SET sal = :sal WHERE empno = 7839', params := {'sal': getvariable('king_sal')});

FROM orkidea_execute('lab', 'DROP TABLE lab_bonus PURGE');
