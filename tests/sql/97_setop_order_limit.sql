-- 97_setop_order_limit.sql
-- 测试目标：验证集合运算（UNION）与 ORDER BY / LIMIT / OFFSET 的组合语义
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. UNION + ORDER BY DESC：去重后整体排序
--   2. UNION + ORDER BY + LIMIT：排序后截断
--   3. UNION + ORDER BY + LIMIT + OFFSET：排序后跳过再截断
--   4. 跨表 UNION 的去重（两表各自插入，结果合并去重）
-- 预期结果：
--   - u1{3,1} UNION u2{2,5} → {1,2,3,5}；ORDER BY id DESC → 5,3,2,1
--   - ORDER BY id LIMIT 2 → 1,2
--   - ORDER BY id LIMIT 2 OFFSET 1 → 2,3
-- 后置处理：DROP 测试表

CREATE TABLE u1(id INT PRIMARY KEY);
CREATE TABLE u2(id INT PRIMARY KEY);

INSERT INTO u1 VALUES (3), (1);
INSERT INTO u2 VALUES (2), (5);

-- 1) UNION + ORDER BY DESC
SELECT id FROM u1 UNION SELECT id FROM u2 ORDER BY id DESC;

-- 2) UNION + ORDER BY + LIMIT
SELECT id FROM u1 UNION SELECT id FROM u2 ORDER BY id LIMIT 2;

-- 3) UNION + ORDER BY + LIMIT + OFFSET
SELECT id FROM u1 UNION SELECT id FROM u2 ORDER BY id LIMIT 2 OFFSET 1;

-- 4) 跨表去重后再插入重复值，确认 UNION 仍去重
INSERT INTO u1 VALUES (2);

SELECT id FROM u1 UNION SELECT id FROM u2 ORDER BY id;

-- 后置处理
DROP TABLE u1;
DROP TABLE u2;

exit;
