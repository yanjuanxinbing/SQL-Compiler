-- 104_order_by_hidden_col.sql
-- 测试目标：验证 ORDER BY / GROUP BY 引用不在 SELECT 列表中的列
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. GROUP BY grp（在 SELECT 中）+ ORDER BY grp
--   2. ORDER BY val —— val 不在 SELECT 列表中（隐藏列排序）
--   3. GROUP BY grp + ORDER BY 聚合别名 mx DESC
-- 预期结果（h1: x/x/y 三行，val 30/10/20）：
--   - GROUP BY grp → x:2, y:1（按 grp 升序）
--   - ORDER BY val DESC → id 1(30), 3(20), 2(10)
--   - ORDER BY mx DESC → x:30, y:20
-- 后置处理：DROP 测试表

CREATE TABLE h1(id INT PRIMARY KEY, grp VARCHAR, val INT);

INSERT INTO h1 VALUES (1, 'x', 30), (2, 'x', 10), (3, 'y', 20);

-- 1) GROUP BY + ORDER BY 分组列
SELECT grp, COUNT(*) AS c FROM h1 GROUP BY grp ORDER BY grp;

-- 2) ORDER BY 引用不在 SELECT 中的列
SELECT id FROM h1 ORDER BY val DESC;

-- 3) GROUP BY + ORDER BY 聚合别名
SELECT grp, MAX(val) AS mx FROM h1 GROUP BY grp ORDER BY mx DESC;

-- 后置处理
DROP TABLE h1;

exit;
