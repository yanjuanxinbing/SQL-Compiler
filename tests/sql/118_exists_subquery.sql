-- 118_exists_subquery.sql
-- 测试目标：验证 EXISTS / NOT EXISTS 子查询（相关子查询按外层行求值）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. EXISTS（相关）：外层 id 在 e2.ref 中出现
--   2. NOT EXISTS（相关）：未出现的行
--   3. EXISTS（非相关恒假）：子查询无行 → 全表排除
-- 预期结果（e1: 1,2,3；e2.ref: 1,3）：
--   - EXISTS → 1,3
--   - NOT EXISTS → 2
--   - EXISTS (无行) → 0 行
-- 后置处理：DROP 测试表

CREATE TABLE e1(id INT PRIMARY KEY, v INT);
CREATE TABLE e2(id INT PRIMARY KEY, ref INT);

INSERT INTO e1 VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO e2 VALUES (1, 1), (2, 3);

-- 1) 相关 EXISTS
SELECT id FROM e1 WHERE EXISTS (SELECT 1 FROM e2 WHERE ref = e1.id) ORDER BY id;

-- 2) 相关 NOT EXISTS
SELECT id FROM e1 WHERE NOT EXISTS (SELECT 1 FROM e2 WHERE ref = e1.id) ORDER BY id;

-- 3) 非相关恒假 EXISTS
SELECT id FROM e1 WHERE EXISTS (SELECT 1 FROM e2 WHERE ref = 99);

-- 后置处理
DROP TABLE e2;
DROP TABLE e1;

exit;
