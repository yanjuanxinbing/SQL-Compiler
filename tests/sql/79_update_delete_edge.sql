-- 79_update_delete_edge.sql
-- 测试目标：验证 UPDATE/DELETE 在边界条件下的行为
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. UPDATE 不匹配任何行（0 行受影响，不报错）
--   2. DELETE 不匹配任何行（0 行受影响，不报错）
--   3. UPDATE 所有行（无 WHERE 子句）
--   4. DELETE 所有行（无 WHERE 子句）
--   5. UPDATE 基于列自身值的运算
--   6. UPDATE/DELETE 后 SELECT 验证结果
-- 预期结果：
--   - 不匹配的 UPDATE/DELETE：COUNT 不变，退出码 0
--   - UPDATE v = v + 1：3 行从 10/20/30 变为 11/21/31
--   - DELETE 所有行后：COUNT = 0
-- 后置处理：DROP 测试表

CREATE TABLE ud(id INT PRIMARY KEY, v INT);

INSERT INTO ud VALUES (1, 10), (2, 20), (3, 30);

-- 初始行数
SELECT COUNT(*) AS initial FROM ud;

-- UPDATE 不匹配任何行
UPDATE ud SET v = 999 WHERE id = 99;
SELECT COUNT(*) AS after_noop_update FROM ud;
SELECT SUM(v) AS sum_unchanged FROM ud;

-- DELETE 不匹配任何行
DELETE FROM ud WHERE id = 99;
SELECT COUNT(*) AS after_noop_delete FROM ud;

-- UPDATE 基于列自身运算（v = v + 1）
UPDATE ud SET v = v + 1;
SELECT id, v FROM ud ORDER BY id;

-- UPDATE 所有行（无 WHERE）
UPDATE ud SET v = 0;
SELECT id, v FROM ud ORDER BY id;

-- DELETE 所有行
DELETE FROM ud;
SELECT COUNT(*) AS after_delete_all FROM ud;

-- 后置处理
DROP TABLE ud;

exit;
