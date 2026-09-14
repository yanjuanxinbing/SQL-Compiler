-- 123_cross_join_cartesian.sql
-- 测试目标：验证笛卡尔积（逗号多表 / CROSS JOIN）与 WHERE 过滤组合
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. FROM x1, x2：2×2 = 4 行笛卡尔积
--   2. FROM x1 CROSS JOIN x2：显式关键字，同样的 4 行
--   3. 笛卡尔积 + WHERE 过滤：x1.id = 1 → 2 行
--   4. 笛卡尔积 + WHERE 等值连接（隐式 inner join）
-- 预期结果：
--   - COUNT(*) = 4
--   - CROSS JOIN：(1,10)(1,20)(2,10)(2,20)
--   - 过滤后 COUNT = 2
--   - 隐式连接：id 相等对 (1,1)(2,2)
-- 后置处理：DROP 测试表

CREATE TABLE x1(id INT PRIMARY KEY);
CREATE TABLE x2(id INT PRIMARY KEY);

INSERT INTO x1 VALUES (1), (2);
INSERT INTO x2 VALUES (10), (20);

-- 1) 逗号笛卡尔积
SELECT COUNT(*) AS cartesian FROM x1, x2;

-- 2) CROSS JOIN 关键字
SELECT x1.id AS a, x2.id AS b FROM x1 CROSS JOIN x2 ORDER BY a, b;

-- 3) 笛卡尔积 + WHERE 过滤
SELECT COUNT(*) AS filtered FROM x1, x2 WHERE x1.id = 1;

-- 4) 隐式等值连接
SELECT x1.id AS a, x2.id AS b FROM x1, x2 WHERE x1.id = x2.id ORDER BY a, b;

-- 后置处理
DROP TABLE x2;
DROP TABLE x1;

exit;
