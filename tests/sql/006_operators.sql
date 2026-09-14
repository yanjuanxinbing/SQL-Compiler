-- 06_operators.sql
-- 操作符：LIKE / IN / BETWEEN / IS NULL / IS NOT NULL

CREATE TABLE t(id INT, name VARCHAR, age INT, status VARCHAR);

INSERT INTO t VALUES (1, 'Alice', 20, 'active');
INSERT INTO t VALUES (2, 'Bob', 25, 'inactive');
INSERT INTO t VALUES (3, 'Charlie', 30, NULL);
INSERT INTO t VALUES (4, 'David', 35, 'active');
INSERT INTO t VALUES (5, 'Eve', NULL, 'pending');

-- LIKE
SELECT * FROM t WHERE name LIKE 'A%';
SELECT * FROM t WHERE name LIKE '%e';
SELECT * FROM t WHERE name LIKE '%li%';
SELECT * FROM t WHERE name LIKE '____e';  -- 5 字符结尾 e

-- IN
SELECT * FROM t WHERE id IN (1, 3, 5);
SELECT * FROM t WHERE age IN (20, 25, NULL);
SELECT * FROM t WHERE name IN ('Alice', 'Bob');

-- BETWEEN
SELECT * FROM t WHERE age BETWEEN 20 AND 30;
SELECT * FROM t WHERE age BETWEEN 25 AND 35;
SELECT * FROM t WHERE id BETWEEN 1 AND 3;

-- IS NULL / IS NOT NULL
SELECT * FROM t WHERE status IS NULL;
SELECT * FROM t WHERE age IS NULL;
SELECT * FROM t WHERE status IS NOT NULL;

-- 组合
SELECT * FROM t WHERE (name LIKE 'A%' OR name LIKE 'E%') AND age >= 20;

exit;
