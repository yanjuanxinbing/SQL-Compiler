-- 119_derived_agg_nested.sql
-- 测试目标：验证外层对派生表的聚合与分组（BUG-18 回归测试）
--
-- 旧实现：派生表 / 集合运算源走 PlanSelect 早退分支，只构建
-- Project/Sort/Limit，从不构造 AggregateNode —— 外层聚合按行求值恒为
-- NULL 且不折叠行数，外层 GROUP BY 每行自成一组。
-- 修复：派生表源落入与普通 FROM 共享的尾部（Aggregate/HAVING/...）。
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 派生表投影（原有能力，回归确认）
--   2. 外层 COUNT(*) + SUM(派生聚合列) —— 应折叠为 1 行
--   3. 外层 MAX(派生聚合列)
--   4. 外层 GROUP BY 派生表列
--   5. UNION ALL 派生表 + 外层 GROUP BY（集合运算源）
-- 预期结果（dg: a=1+2, b=10+20）：
--   - 派生投影：a=3, b=30
--   - groups=2, grand_total=33
--   - biggest=30
--   - GROUP BY t.grp：a=2 行、b=2 行
--   - UNION ALL 源 GROUP BY cat：x 组 MIN='apple'、y 组 MIN='cherry'
-- 后置处理：DROP 测试表

CREATE TABLE dg(id INT PRIMARY KEY, grp VARCHAR, v INT);

INSERT INTO dg VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 10), (4, 'b', 20);

-- 1) 派生表投影（回归确认）
SELECT t.grp, t.s
FROM (SELECT grp, SUM(v) AS s FROM dg GROUP BY grp) AS t
ORDER BY t.grp;

-- 2) 外层聚合：折叠为 1 行
SELECT COUNT(*) AS groups, SUM(t.s) AS grand_total
FROM (SELECT grp, SUM(v) AS s FROM dg GROUP BY grp) AS t;

-- 3) 外层 MAX
SELECT MAX(t.s) AS biggest
FROM (SELECT grp, SUM(v) AS s FROM dg GROUP BY grp) AS t;

-- 4) 外层 GROUP BY 派生表列
SELECT t.grp, COUNT(*) AS c
FROM (SELECT grp, v FROM dg) AS t
GROUP BY t.grp ORDER BY t.grp;

-- 5) 集合运算源 + 外层 GROUP BY
SELECT cat, MIN(w) AS mn
FROM (SELECT 'x' AS cat, 'banana' AS w
      UNION ALL
      SELECT 'x', 'apple'
      UNION ALL
      SELECT 'y', 'cherry') t
GROUP BY cat ORDER BY cat;

-- 后置处理
DROP TABLE dg;

exit;
