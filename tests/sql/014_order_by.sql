-- 15_order_by.sql
-- ORDER BY 各种形式：单列/多列/ASC/DESC/默认值/隐式 ASC

CREATE TABLE t(id INT, group_id INT, name VARCHAR, score FLOAT, age INT);

INSERT INTO t VALUES
    (1, 2, 'Alice',  88.5, 20),
    (2, 1, 'Bob',    91.0, 22),
    (3, 2, 'Charlie',76.5, 19),
    (4, 1, 'David',  95.5, 25),
    (5, 2, 'Eve',    82.0, 21),
    (6, 1, 'Frank',  88.5, 23);

-- 单列升序（默认）
SELECT * FROM t ORDER BY age;

-- 单列升序（显式）
SELECT * FROM t ORDER BY age ASC;

-- 单列降序
SELECT * FROM t ORDER BY score DESC;

-- 多列：先按 group_id，再按 score
SELECT * FROM t ORDER BY group_id ASC, score DESC;

-- ORDER BY + LIMIT 组合
SELECT * FROM t ORDER BY score DESC LIMIT 3;
SELECT * FROM t ORDER BY score DESC LIMIT 2, 2;

-- ORDER BY + WHERE 组合
SELECT * FROM t WHERE group_id = 1 ORDER BY age ASC;

-- 投影特定列排序
SELECT name, score FROM t ORDER BY score DESC;

-- 按别名排序（如果之前用过别名）
SELECT id, score AS s FROM t ORDER BY s DESC;

-- LIMIT 0, LIMIT 1
SELECT * FROM t ORDER BY id LIMIT 0;
SELECT * FROM t ORDER BY id LIMIT 1;

exit;