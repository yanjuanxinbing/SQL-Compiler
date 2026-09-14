-- 126_having_hidden_agg.sql
-- 测试目标：验证 HAVING / ORDER BY 引用不在 SELECT 列表中的聚合
--         （extended_aggs 机制：聚合表达式自动补入 AggregateNode 输出）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. HAVING SUM(v) > 20（SUM 不在 SELECT）
--   2. ORDER BY SUM(v) DESC（SUM 不在 SELECT）
--   3. ORDER BY AVG(v) DESC（AVG 不在 SELECT）
-- 预期结果（hg: a=10+30, b=5+15）：
--   - HAVING SUM>20 → 仅 a 组（SUM=40；b=20 不满足）
--   - ORDER BY SUM DESC → a 组在前
--   - ORDER BY AVG DESC → a(avg=20) 在 b(avg=10) 前
-- 后置处理：DROP 测试表

CREATE TABLE hg(id INT PRIMARY KEY, grp VARCHAR, v INT);

INSERT INTO hg VALUES (1, 'a', 10), (2, 'a', 30), (3, 'b', 5), (4, 'b', 15);

-- 1) HAVING 引用隐藏聚合
SELECT grp, COUNT(*) AS c FROM hg GROUP BY grp HAVING SUM(v) > 20 ORDER BY grp;

-- 2) ORDER BY 引用隐藏聚合 SUM
SELECT grp, COUNT(*) AS c FROM hg GROUP BY grp HAVING SUM(v) > 20 ORDER BY SUM(v) DESC;

-- 3) ORDER BY 引用隐藏聚合 AVG
SELECT grp, MAX(v) AS mx FROM hg GROUP BY grp ORDER BY AVG(v) DESC;

-- 后置处理
DROP TABLE hg;

exit;
