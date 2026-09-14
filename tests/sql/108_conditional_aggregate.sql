-- 108_conditional_aggregate.sql
-- 测试目标：验证聚合函数内的条件表达式（条件计数 / 条件求和 / 表达式聚合）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 全表混合聚合：COUNT(*) vs COUNT(col)、SUM(CASE WHEN...)、
--      COUNT(CASE WHEN ... THEN 1 END)、AVG(表达式)
--   2. 分组条件计数：GROUP BY cat + SUM(CASE WHEN ... THEN 1 ELSE 0 END)
-- 预期结果（ca: a=10/20, b=5/NULL, c=30）：
--   - all_cnt=5, amt_cnt=4（NULL 不计）
--   - big_sum = 20+30 = 50（amt>10 求和），big_cnt = 2
--   - avg_doubled = (20+40+10+60)/4 = 32.5（NULL 行不计）
--   - 分组 hit：a=2, b=0, c=1
-- 后置处理：DROP 测试表

CREATE TABLE ca(id INT PRIMARY KEY, cat VARCHAR, amt INT);

INSERT INTO ca VALUES
    (1, 'a', 10),
    (2, 'a', 20),
    (3, 'b', 5),
    (4, 'b', NULL),
    (5, 'c', 30);

-- 1) 全表混合聚合
SELECT COUNT(*) AS all_cnt,
       COUNT(amt) AS amt_cnt,
       SUM(CASE WHEN amt > 10 THEN amt ELSE 0 END) AS big_sum,
       COUNT(CASE WHEN amt > 10 THEN 1 END) AS big_cnt,
       AVG(amt * 2) AS avg_doubled
FROM ca;

-- 2) 分组条件计数
SELECT cat,
       SUM(CASE WHEN amt > 8 THEN 1 ELSE 0 END) AS hit
FROM ca GROUP BY cat ORDER BY cat;

-- 后置处理
DROP TABLE ca;

exit;
