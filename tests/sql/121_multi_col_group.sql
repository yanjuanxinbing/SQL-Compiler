-- 115_multi_col_group.sql
-- 测试目标：验证 GROUP BY 多列组合分组与聚合、多列排序输出
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. GROUP BY dept, role 双列：每对 (dept, role) 一个分组
--   2. 双列分组 + ORDER BY 双键（聚合值 DESC + dept ASC）
--   3. 双列分组 + HAVING 过滤
-- 预期结果（e: sales/dev 3 人、sales/qa 1 人、hr/dev 1 人）：
--   - 3 个分组：sales-dev=2、sales-qa=1、hr-dev=1
--   - 按 cnt DESC, dept ASC：sales-dev(2) → hr-dev(1) → sales-qa(1)
--   - HAVING cnt >= 2 → 仅 sales-dev
-- 后置处理：DROP 测试表

CREATE TABLE e(id INT PRIMARY KEY, dept VARCHAR, role VARCHAR, salary INT);

INSERT INTO e VALUES
    (1, 'sales', 'dev', 100),
    (2, 'sales', 'dev', 120),
    (3, 'sales', 'qa',  90),
    (4, 'hr',    'dev', 110);

-- 1) 双列分组
SELECT dept, role, COUNT(*) AS cnt, SUM(salary) AS total
FROM e GROUP BY dept, role;

-- 2) 双列分组 + 双键排序
SELECT dept, role, COUNT(*) AS cnt
FROM e GROUP BY dept, role ORDER BY cnt DESC, dept ASC;

-- 3) 双列分组 + HAVING
SELECT dept, role, COUNT(*) AS cnt
FROM e GROUP BY dept, role HAVING COUNT(*) >= 2 ORDER BY dept, role;

-- 后置处理
DROP TABLE e;

exit;
