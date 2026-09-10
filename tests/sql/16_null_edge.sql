-- 16_null_edge.sql
-- NULL 边界场景：算术运算、聚合、排序

CREATE TABLE t(id INT, val INT, name VARCHAR, score FLOAT);

INSERT INTO t VALUES
    (1, NULL, 'Alice',  NULL),
    (2, 10,   'Bob',    80.0),
    (3, NULL, 'Charlie',NULL),
    (4, 20,   'David',  90.0),
    (5, NULL, 'Eve',    NULL),
    (6, 30,   'Frank',  85.0);

-- IS NULL / IS NOT NULL
SELECT * FROM t WHERE val IS NULL;
SELECT * FROM t WHERE val IS NOT NULL;

-- 字符串 IS NULL
SELECT * FROM t WHERE name IS NULL;
SELECT * FROM t WHERE name IS NOT NULL;

-- 浮点 IS NULL
SELECT * FROM t WHERE score IS NULL;

-- 比较运算与 NULL：任何与 NULL 的比较都应不返回该行（除非 IS NULL）
SELECT * FROM t WHERE val = NULL;
SELECT * FROM t WHERE val != NULL;
SELECT * FROM t WHERE val > 10;

-- 算术表达式中的 NULL 应保持 NULL（验证输出列）
SELECT id, val + 0 AS val_plus_zero FROM t;
SELECT id, val * 2 AS val_doubled FROM t;

-- 聚合中 NULL 不计入
SELECT COUNT(val) AS cnt_val FROM t;
SELECT COUNT(*)   AS cnt_all FROM t;
SELECT SUM(val)   AS sum_val FROM t;
SELECT AVG(val)   AS avg_val FROM t;

-- ORDER BY 中 NULL 的位置（升序末尾 / 降序开头取决于实现）
SELECT * FROM t ORDER BY val ASC;
SELECT * FROM t ORDER BY val DESC;

-- LIMIT + NULL
SELECT * FROM t ORDER BY id LIMIT 3;

exit;