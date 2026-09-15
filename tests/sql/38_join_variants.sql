-- 38_join_variants.sql
-- JOIN 扩展：FULL OUTER JOIN / CROSS JOIN / USING (col) / NATURAL JOIN

CREATE TABLE a(id INT, name VARCHAR);
CREATE TABLE b(id INT, name VARCHAR);

INSERT INTO a VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');
INSERT INTO b VALUES (1, 'Alice'), (2, 'David'), (4, 'Eve');

SELECT a.id, b.id FROM a INNER JOIN b ON a.id = b.id;

SELECT a.id, b.id FROM a FULL OUTER JOIN b ON a.id = b.id;

SELECT a.id, b.id FROM a CROSS JOIN b;

SELECT a.id FROM a INNER JOIN b USING (id);

SELECT a.id FROM a NATURAL JOIN b;

SELECT COUNT(*) FROM a INNER JOIN b ON a.id = b.id;

-- 派生表作为 JOIN 右操作数：(SELECT ...) [AS] alias
-- CROSS JOIN 派生表 —— 笛卡尔积
SELECT a.id, c.dummy FROM a CROSS JOIN (SELECT 1 AS dummy) c;
-- INNER JOIN 派生表 + ON 条件
SELECT a.id, c.dummy FROM a INNER JOIN (SELECT 1 AS dummy) c ON a.id <= 2;
-- LEFT JOIN 派生表 + ON 条件
SELECT a.id, c.dummy FROM a LEFT JOIN (SELECT 0 AS dummy) c ON a.id = c.dummy;
-- 多列派生表 JOIN
SELECT a.id, c.x, c.y FROM a CROSS JOIN (SELECT 1 AS x, 'hi' AS y) c;
-- 多行派生表 JOIN（笛卡尔积放大）
SELECT a.id, c.n FROM a CROSS JOIN (SELECT 1 AS n UNION ALL SELECT 2) c;
-- 不写 AS 的派生表 JOIN
SELECT a.id, c.dummy FROM a CROSS JOIN (SELECT 1 AS dummy) c;
-- 缺少别名的派生表 JOIN 应报错（SQL 标准要求派生表必须别名）
SELECT a.id FROM a CROSS JOIN (SELECT 1 AS dummy);

-- 派生表在 JOIN 左侧：(subquery) JOIN table —— 旧实现跳过 join 循环导致 JoinNode
-- 完全不出现、t1 列在外层引用全返回 NULL、Bug 13 修复后正确处理。
CREATE TABLE t1(id INT, name VARCHAR);
CREATE TABLE t2(id INT, info VARCHAR);
INSERT INTO t1 VALUES (1,'A'),(2,'B'),(3,'X');
INSERT INTO t2 VALUES (1,'P'),(2,'Q'),(4,'Z');
-- Pattern 1: (subquery) INNER JOIN table
SELECT sub.info, t1.name FROM (SELECT id, info FROM t2) AS sub INNER JOIN t1 ON sub.id = t1.id ORDER BY t1.name;
-- Pattern 2: (subquery) LEFT JOIN table —— 左表派生表时 LEFT JOIN 仍要保留左表全部行
SELECT sub.info, t1.name FROM (SELECT id, info FROM t2) AS sub LEFT JOIN t1 ON sub.id = t1.id ORDER BY sub.info;
-- Pattern 3: (subquery) CROSS JOIN table —— 派生表 3 行 × t1 3 行 = 9 行
SELECT sub.info, t1.name FROM (SELECT id, info FROM t2) AS sub CROSS JOIN t1 ORDER BY t1.id, sub.info;
-- Pattern 4: 派生表 on both sides
SELECT a.info, b.info FROM (SELECT id, info FROM t2 WHERE id <= 2) AS a INNER JOIN (SELECT id, info FROM t2 WHERE id >= 2) AS b ON a.id = b.id;
-- Pattern 5: WHERE 引用右表 t1.name（修复前 FilterNode 在 join 之前评估导致全部 NULL 过滤掉）
SELECT sub.info, t1.name FROM (SELECT id, info FROM t2) AS sub INNER JOIN t1 ON sub.id = t1.id WHERE t1.name = 'A';
-- Pattern 6: ORDER BY 引用右表 t1.name（修复前按 NULL 排序）
SELECT sub.info, t1.name FROM (SELECT id, info FROM t2) AS sub INNER JOIN t1 ON sub.id = t1.id ORDER BY t1.name;
DROP TABLE t1;
DROP TABLE t2;

exit;
