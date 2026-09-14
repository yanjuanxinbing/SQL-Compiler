-- 114_between_expr_sides.sql
-- 测试目标：验证 BETWEEN 边界为表达式 / 子查询时的求值
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 两侧均为算术表达式：v BETWEEN 5+5 AND 10+10 → [10, 20]
--   2. 左操作数为表达式：v * 2 BETWEEN 20 AND 39 → v ∈ [10, 19.5]
--   3. 两侧为标量子查询：v BETWEEN (SELECT MIN...) AND (SELECT MAX...-5)
-- 预期结果（bt: 5/10/15/20）：
--   - [10,20] → 2,3,4
--   - v*2 ∈ [20,39] → v ∈ [10,19.5] → 2,3
--   - [MIN=5, MAX-5=15] → 1,2,3
-- 后置处理：DROP 测试表

CREATE TABLE bt(id INT PRIMARY KEY, v INT);

INSERT INTO bt VALUES (1, 5), (2, 10), (3, 15), (4, 20);

-- 1) 两侧算术表达式
SELECT id FROM bt WHERE v BETWEEN 5 + 5 AND 10 + 10 ORDER BY id;

-- 2) 左操作数为表达式
SELECT id FROM bt WHERE v * 2 BETWEEN 20 AND 39 ORDER BY id;

-- 3) 两侧标量子查询
SELECT id FROM bt
WHERE v BETWEEN (SELECT MIN(v) FROM bt) AND (SELECT MAX(v) - 5 FROM bt)
ORDER BY id;

-- 后置处理
DROP TABLE bt;

exit;
