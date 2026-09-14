-- 95_distinct_multi_col.sql
-- 测试目标：验证 DISTINCT 多列组合去重与 COUNT(DISTINCT col) 语义
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 完全重复行（(1,'x') x2）应合并
--   2. 部分列不同（(1,'x') vs (1,'y')）不合并
--   3. NULL 参与去重：两个 (NULL,'x') 合并为一行
--   4. 单列 DISTINCT 含 NULL：NULL 计为一个去重值
--   5. COUNT(DISTINCT col)：NULL 不计数
-- 预期结果：
--   - DISTINCT a, b → 5 行：1-x / 1-y / 2-x / 2-y / NULL-x
--   - DISTINCT a → 3 行：1 / 2 / NULL
--   - COUNT(*) = 7，COUNT(DISTINCT a) = 2（NULL 被排除）
-- 后置处理：DROP 测试表

CREATE TABLE d1(a INT, b VARCHAR);

INSERT INTO d1 VALUES
    (1, 'x'),
    (1, 'x'),
    (1, 'y'),
    (2, 'x'),
    (2, 'y'),
    (NULL, 'x'),
    (NULL, 'x');

-- 1-3) 多列组合去重（含 NULL 组合）
SELECT DISTINCT a, b FROM d1;

-- 4) 单列 DISTINCT（NULL 计为一个值）
SELECT DISTINCT a FROM d1;

-- 5) COUNT(DISTINCT) 聚合（NULL 不计数）
SELECT COUNT(*) AS all_rows, COUNT(DISTINCT a) AS distinct_a FROM d1;

-- 后置处理
DROP TABLE d1;

exit;
