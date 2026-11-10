-- Seeds the lab schema. Run by setup.sh, as the lab user, in SQL*Plus.
-- The data is computed, not random, so every run gives the same rows and the
-- results quoted in the lab hold.

WHENEVER SQLERROR EXIT FAILURE
SET FEEDBACK OFF

-- Regions, customers, products, and a million orders --------------------------

CREATE TABLE regions (
    region_id NUMBER(2) CONSTRAINT pk_regions PRIMARY KEY,
    name      VARCHAR2(20) NOT NULL
);
INSERT INTO regions VALUES (1, 'Africa');
INSERT INTO regions VALUES (2, 'Americas');
INSERT INTO regions VALUES (3, 'Asia');
INSERT INTO regions VALUES (4, 'Europe');
INSERT INTO regions VALUES (5, 'Oceania');

CREATE TABLE customers (
    customer_id NUMBER(6) CONSTRAINT pk_customers PRIMARY KEY,
    full_name   VARCHAR2(40) NOT NULL,
    email       VARCHAR2(60),
    country     VARCHAR2(30),
    region_id   NUMBER(2) REFERENCES regions,
    vip         BOOLEAN,
    signed_up   DATE
);
INSERT INTO customers
SELECT level,
       'Customer ' || level,
       CASE WHEN MOD(level, 17) = 0 THEN NULL ELSE 'customer' || level || '@example.com' END,
       CASE MOD(level, 10)
           WHEN 0 THEN 'Mozambique' WHEN 1 THEN 'South Africa' WHEN 2 THEN 'Brazil'
           WHEN 3 THEN 'Canada'     WHEN 4 THEN 'Japan'        WHEN 5 THEN 'India'
           WHEN 6 THEN 'Portugal'   WHEN 7 THEN 'Germany'      WHEN 8 THEN 'Australia'
           ELSE 'New Zealand' END,
       CASE MOD(level, 10)
           WHEN 0 THEN 1 WHEN 1 THEN 1 WHEN 2 THEN 2 WHEN 3 THEN 2 WHEN 4 THEN 3
           WHEN 5 THEN 3 WHEN 6 THEN 4 WHEN 7 THEN 4 ELSE 5 END,
       MOD(level, 47) = 0,
       DATE '2020-01-01' + MOD(level * 13, 1800)
FROM dual CONNECT BY level <= 20000;

CREATE TABLE products (
    product_id NUMBER(4) CONSTRAINT pk_products PRIMARY KEY,
    name       VARCHAR2(40) NOT NULL,
    category   VARCHAR2(20) NOT NULL,
    price      NUMBER(10,2) NOT NULL
);
INSERT INTO products
SELECT level,
       'Product ' || level,
       CASE MOD(level, 5) WHEN 0 THEN 'Tea' WHEN 1 THEN 'Coffee' WHEN 2 THEN 'Books'
                          WHEN 3 THEN 'Garden' ELSE 'Kitchen' END,
       5 + MOD(level * 37, 200) + 0.99
FROM dual CONNECT BY level <= 500;

CREATE TABLE orders (
    order_id    NUMBER(9) CONSTRAINT pk_orders PRIMARY KEY,
    customer_id NUMBER(6) NOT NULL,
    product_id  NUMBER(4) NOT NULL,
    quantity    NUMBER(3) NOT NULL,
    amount      NUMBER(12,2) NOT NULL,
    status      VARCHAR2(10) NOT NULL,
    ordered_at  TIMESTAMP(6) NOT NULL
);
INSERT /*+ APPEND */ INTO orders
SELECT n,
       MOD(n * 7919, 20000) + 1,
       MOD(n * 104729, 500) + 1,
       MOD(n, 5) + 1,
       (MOD(n, 5) + 1) * (5 + MOD((MOD(n * 104729, 500) + 1) * 37, 200) + 0.99),
       CASE MOD(n, 20) WHEN 0 THEN 'RETURNED' WHEN 1 THEN 'CANCELLED' ELSE 'SHIPPED' END,
       TIMESTAMP '2025-01-01 00:00:00' + NUMTODSINTERVAL(MOD(n * 7919, 31536000), 'SECOND')
FROM (SELECT (a.n - 1) * 1000 + b.n AS n
      FROM (SELECT level AS n FROM dual CONNECT BY level <= 1000) a,
           (SELECT level AS n FROM dual CONNECT BY level <= 1000) b);
COMMIT;

-- The classic EMP and DEPT, and a procedure --------------------------------------

CREATE TABLE dept (
    deptno NUMBER(2) CONSTRAINT pk_dept PRIMARY KEY,
    dname  VARCHAR2(14),
    loc    VARCHAR2(13)
);
INSERT INTO dept VALUES (10, 'ACCOUNTING', 'NEW YORK');
INSERT INTO dept VALUES (20, 'RESEARCH', 'DALLAS');
INSERT INTO dept VALUES (30, 'SALES', 'CHICAGO');
INSERT INTO dept VALUES (40, 'OPERATIONS', 'BOSTON');

CREATE TABLE emp (
    empno    NUMBER(4) CONSTRAINT pk_emp PRIMARY KEY,
    ename    VARCHAR2(10),
    job      VARCHAR2(9),
    mgr      NUMBER(4),
    hiredate DATE,
    sal      NUMBER(7,2),
    comm     NUMBER(7,2),
    deptno   NUMBER(2) REFERENCES dept
);
INSERT INTO emp VALUES (7839, 'KING',   'PRESIDENT', NULL, DATE '1981-11-17', 5000, NULL, 10);
INSERT INTO emp VALUES (7698, 'BLAKE',  'MANAGER',   7839, DATE '1981-05-01', 2850, NULL, 30);
INSERT INTO emp VALUES (7782, 'CLARK',  'MANAGER',   7839, DATE '1981-06-09', 2450, NULL, 10);
INSERT INTO emp VALUES (7566, 'JONES',  'MANAGER',   7839, DATE '1981-04-02', 2975, NULL, 20);
INSERT INTO emp VALUES (7788, 'SCOTT',  'ANALYST',   7566, DATE '1987-04-19', 3000, NULL, 20);
INSERT INTO emp VALUES (7902, 'FORD',   'ANALYST',   7566, DATE '1981-12-03', 3000, NULL, 20);
INSERT INTO emp VALUES (7369, 'SMITH',  'CLERK',     7902, DATE '1980-12-17',  800, NULL, 20);
INSERT INTO emp VALUES (7499, 'ALLEN',  'SALESMAN',  7698, DATE '1981-02-20', 1600,  300, 30);
INSERT INTO emp VALUES (7521, 'WARD',   'SALESMAN',  7698, DATE '1981-02-22', 1250,  500, 30);
INSERT INTO emp VALUES (7654, 'MARTIN', 'SALESMAN',  7698, DATE '1981-09-28', 1250, 1400, 30);
INSERT INTO emp VALUES (7844, 'TURNER', 'SALESMAN',  7698, DATE '1981-09-08', 1500,    0, 30);
INSERT INTO emp VALUES (7876, 'ADAMS',  'CLERK',     7788, DATE '1987-05-23', 1100, NULL, 20);
INSERT INTO emp VALUES (7900, 'JAMES',  'CLERK',     7698, DATE '1981-12-03',  950, NULL, 30);
INSERT INTO emp VALUES (7934, 'MILLER', 'CLERK',     7782, DATE '1982-01-23', 1300, NULL, 10);
COMMIT;

CREATE OR REPLACE PROCEDURE raise_salary(p_empno IN NUMBER, p_pct IN NUMBER) AS
BEGIN
    UPDATE emp SET sal = ROUND(sal * (1 + p_pct / 100), 2) WHERE empno = p_empno;
    IF SQL%ROWCOUNT = 0 THEN
        RAISE_APPLICATION_ERROR(-20001, 'No employee ' || p_empno);
    END IF;
END;
/

-- One row of each kind of Oracle type --------------------------------------------

CREATE TABLE type_showcase (
    id          NUMBER(2) CONSTRAINT pk_type_showcase PRIMARY KEY,
    small_int   NUMBER(4),
    big_int     NUMBER(18),
    money       NUMBER(12,2),
    any_number  NUMBER,
    ratio       BINARY_DOUBLE,
    label       VARCHAR2(20),
    padded      CHAR(5),
    unicode     NVARCHAR2(20),
    notes       CLOB,
    bytes       RAW(4),
    flag        BOOLEAN,
    day         DATE,
    at          TIMESTAMP(6),
    at_utc      TIMESTAMP(6) WITH TIME ZONE,
    wait        INTERVAL DAY(2) TO SECOND(3),
    doc         JSON,
    embedding   VECTOR(3, FLOAT32)
);
INSERT INTO type_showcase VALUES (
    1, 42, 9007199254740993, 1234.56, 3.14159, 0.5, 'hello', 'ab', N'olá, 世界',
    TO_CLOB('a long text'), HEXTORAW('CAFE'), TRUE, DATE '2026-11-10',
    TIMESTAMP '2026-11-10 09:30:00.123456', TIMESTAMP '2026-11-10 09:30:00 +02:00',
    INTERVAL '1 02:03:04.500' DAY TO SECOND, JSON('{"tags": ["lab", "oracle"], "level": 2}'),
    TO_VECTOR('[1, 2, 3]', 3, FLOAT32));
COMMIT;

-- Optimizer statistics, which the extension reads to choose parallel scans.
BEGIN
    DBMS_STATS.GATHER_SCHEMA_STATS(USER);
END;
/

EXIT
