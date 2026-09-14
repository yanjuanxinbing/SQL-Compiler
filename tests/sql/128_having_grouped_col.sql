-- 122_having_grouped_col.sql
-- 测试目标：验证 HAVING 引用 GROUP BY 列（非聚合分组键过滤）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. GROUP BY grp HAVING grp = 'a'：按分组键等值过滤
--   2. GROUP BY grp HAVING grp IN ('a', 'b')：分组键 IN 过滤
--   3. GROUP BY grp HAVING grp LIKE 'a%'：分组键模式过滤
-- 预期结果（dg: a=1+2, b=10+20, c=30）：
--   - 1 → 仅 a 组（s=3）
--   - 2 → a 组与 b 组
--   - 3 → 仅 a 组
-- 后置处理：DROP 测试表

CREATE TABLE dg(id INT PRIMARY KEY, grp VARCHAR, v INT);

INSERT INTO dg VALUES (1, 'a', 1), (2, 'a', 2), (3, 'b', 10), (4, 'b', 20), (5, 'c', 30);

-- 1) 分组键等值
SELECT grp, SUM(v) AS s FROM dg GROUP BY grp HAVING grp = 'a';

-- 2) 分组键 IN
SELECT grp, SUM(v) AS s FROM dg GROUP BY grp HAVING grp IN ('a', 'b') ORDER BY grp;

-- 3) 分组键 LIKE
SELECT grp, SUM(v) AS s FROM dg GROUP BY grp HAVING grp LIKE 'a%';

-- 后置处理
DROP TABLE dg;

exit;
