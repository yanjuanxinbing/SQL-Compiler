-- 03_query_basic.sql
-- 基础查询：SELECT、WHERE、ORDER BY、LIMIT

CREATE TABLE t(id INT, name VARCHAR, age INT, score FLOAT);
INSERT INTO t VALUES (1, 'Alice', 20, 88.5);
INSERT INTO t VALUES (2, 'Bob', 22, 91.0);
INSERT INTO t VALUES (3, 'Charlie', 19, 76.5);
INSERT INTO t VALUES (4, 'David', 25, 95.5);
INSERT INTO t VALUES (5, 'Eve', 21, 82.0);

-- 全部
SELECT * FROM t;

-- 条件
SELECT * FROM t WHERE age > 20;
SELECT * FROM t WHERE age > 20 AND score > 80;
SELECT * FROM t WHERE age = 22 OR name = 'Charlie';
SELECT * FROM t WHERE NOT (age < 20);

-- 排序
SELECT * FROM t ORDER BY age ASC;
SELECT * FROM t ORDER BY score DESC;
SELECT * FROM t ORDER BY age ASC, score DESC;

-- LIMIT
SELECT * FROM t ORDER BY id LIMIT 2;
SELECT * FROM t ORDER BY id LIMIT 1, 2;
SELECT * FROM t ORDER BY id LIMIT 0, 3;

-- 投影特定列
SELECT name, age FROM t;
SELECT name, score FROM t WHERE score >= 85;

exit;
