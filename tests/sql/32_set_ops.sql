-- 32_set_ops.sql
-- 集合运算：UNION / UNION ALL / INTERSECT / EXCEPT
-- 要求两边的列数与类型兼容；结果列名由第一个 SELECT 决定。

CREATE TABLE t_a(
    id INT,
    name VARCHAR
);

CREATE TABLE t_b(
    id INT,
    name VARCHAR
);

CREATE TABLE t_c(
    id INT,
    name VARCHAR
);

INSERT INTO t_a VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');
INSERT INTO t_b VALUES (2, 'Bob'), (3, 'Carol'), (4, 'David');
INSERT INTO t_c VALUES (3, 'Carol'), (5, 'Eve');

-- UNION：去重并集
SELECT id, name FROM t_a
UNION
SELECT id, name FROM t_b;

-- UNION ALL：不去重并集（保留所有）
SELECT id, name FROM t_a
UNION ALL
SELECT id, name FROM t_b;

-- INTERSECT：交集
SELECT id, name FROM t_a
INTERSECT
SELECT id, name FROM t_b;

-- EXCEPT：差集（a 中有但 b 中没有）
SELECT id, name FROM t_a
EXCEPT
SELECT id, name FROM t_b;

-- 三表集合运算 + 括号控制优先级
(SELECT id, name FROM t_a
 UNION
 SELECT id, name FROM t_b)
INTERSECT
SELECT id, name FROM t_c;

-- 集合运算 + ORDER BY（作用于最终结果）
SELECT id, name FROM t_a
UNION
SELECT id, name FROM t_b
UNION
SELECT id, name FROM t_c
ORDER BY id;

-- 集合运算 + LIMIT
SELECT id, name FROM t_a
UNION ALL
SELECT id, name FROM t_b
UNION ALL
SELECT id, name FROM t_c
LIMIT 5;

-- 不同列数 / 类型时的兼容行为（应失败 / 强转）
SELECT id FROM t_a
UNION
SELECT id, name FROM t_b;  -- 列数不匹配

-- 嵌套：UNION 套 UNION
SELECT id FROM t_a WHERE id <= 2
UNION
(SELECT id FROM t_b WHERE id >= 3
 UNION
 SELECT id FROM t_c WHERE id = 5);

-- 与聚合结果做 UNION
SELECT id FROM t_a WHERE id > 2
UNION
SELECT MAX(id) FROM t_b;

exit;