-- 117_is_true_false.sql
-- 测试目标：验证 IS TRUE / IS FALSE / IS NOT TRUE / IS NOT FALSE 谓词的三值逻辑
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. flag IS TRUE：仅 1 命中（0 与 NULL 不命中）
--   2. flag IS FALSE：仅 0 命中
--   3. flag IS NOT TRUE：0 与 NULL 命中（NOT TRUE = FALSE 或 UNKNOWN）
--   4. flag IS NOT FALSE：1 与 NULL 命中
--   5. 布尔表达式 (flag = 1) 作为列输出（1/0/NULL）
--   6. (flag = 1) IS TRUE 作用于表达式结果
-- 预期结果（it: flag = 1/0/NULL）：
--   - IS TRUE → 1；IS FALSE → 2；IS NOT TRUE → 2,3；IS NOT FALSE → 1,3
--   - cmp 列：1/0/NULL
--   - (flag=1) IS TRUE → 1
-- 后置处理：DROP 测试表

CREATE TABLE it(id INT PRIMARY KEY, flag INT);

INSERT INTO it VALUES (1, 1), (2, 0), (3, NULL);

SELECT id FROM it WHERE flag IS TRUE;
SELECT id FROM it WHERE flag IS FALSE;
SELECT id FROM it WHERE flag IS NOT TRUE;
SELECT id FROM it WHERE flag IS NOT FALSE;

SELECT id, (flag = 1) AS cmp FROM it ORDER BY id;

SELECT id FROM it WHERE (flag = 1) IS TRUE ORDER BY id;

-- 后置处理
DROP TABLE it;

exit;
