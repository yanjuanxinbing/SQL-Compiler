-- 112_non_equi_join.sql
-- 测试目标：验证非等值 JOIN（ON 使用 < 比较而非等值）的笛卡尔过滤语义
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. ON n1.v < n2.w：所有满足不等式的行对组合
--   2. ON 使用两列运算（n1.v + n2.w < 10）
--   3. 复合条件：等值与不等值混合（ON n1.id = n2.id AND n1.v < n2.w）
-- 预期结果（n1: (1,1),(2,3)；n2: (1,2),(2,4)）：
--   - v < w：(1,1)(1,2)(2,2) —— 3 行
--   - v+w<10：全部 4 对（1+2=3, 1+4=5, 3+2=5, 3+4=7）
--   - 混合：(2,2) —— id 相等且 3 < 4
-- 后置处理：DROP 测试表

CREATE TABLE n1(id INT PRIMARY KEY, v INT);
CREATE TABLE n2(id INT PRIMARY KEY, w INT);

INSERT INTO n1 VALUES (1, 1), (2, 3);
INSERT INTO n2 VALUES (1, 2), (2, 4);

-- 1) 纯不等值条件
SELECT n1.id AS a, n2.id AS b FROM n1 JOIN n2 ON n1.v < n2.w ORDER BY a, b;

-- 2) 列运算条件
SELECT n1.id AS a, n2.id AS b FROM n1 JOIN n2 ON n1.v + n2.w < 10 ORDER BY a, b;

-- 3) 等值 + 不等值复合
SELECT n1.id AS a, n2.id AS b FROM n1 JOIN n2 ON n1.id = n2.id AND n1.v < n2.w ORDER BY a, b;

-- 后置处理
DROP TABLE n2;
DROP TABLE n1;

exit;
