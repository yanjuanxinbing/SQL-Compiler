-- 103_condition_precedence.sql
-- 测试目标：验证 AND/OR 逻辑优先级、NOT 求反与括号覆盖
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 无括号混用：a=1 OR b=1 AND id=3 —— AND 优先级高于 OR
--   2. 加括号：(a=1 OR b=1) AND id=3 —— 改变结合
--   3. NOT (a=1) 求反
--   4. a=1 AND b=1 OR id=4 —— AND 先结合
-- 预期结果（p1: (1,1,1)(2,1,0)(3,0,1)(4,0,0)）：
--   - 1: a=1 OR (b=1 AND id=3) → 1,2,3
--   - 2: (a=1 OR b=1) AND id=3 → 3
--   - 3: NOT(a=1) → 3,4
--   - 4: (a=1 AND b=1) OR id=4 → 1,4
-- 后置处理：DROP 测试表

CREATE TABLE p1(id INT PRIMARY KEY, a INT, b INT);

INSERT INTO p1 VALUES (1, 1, 1), (2, 1, 0), (3, 0, 1), (4, 0, 0);

-- 1) AND 优先于 OR
SELECT id FROM p1 WHERE a = 1 OR b = 1 AND id = 3 ORDER BY id;

-- 2) 括号改变结合
SELECT id FROM p1 WHERE (a = 1 OR b = 1) AND id = 3 ORDER BY id;

-- 3) NOT 求反
SELECT id FROM p1 WHERE NOT (a = 1) ORDER BY id;

-- 4) AND 先结合再 OR
SELECT id FROM p1 WHERE a = 1 AND b = 1 OR id = 4 ORDER BY id;

-- 后置处理
DROP TABLE p1;

exit;
