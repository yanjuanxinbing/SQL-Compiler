-- 110_scalar_subquery_empty.sql
-- 测试目标：验证 SELECT 列表中标量子查询（空结果 → NULL、参与算术、双子查询）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 子查询命中 / 未命中并列：WHERE id=1 命中 → 100；WHERE id=99 未命中 → NULL
--   2. 子查询无别名：列名按表达式文本
--   3. 标量子查询参与算术：(... ) + 5
--   4. 别名使用关键字 found / missing（BUG-17 回归：FOUND 曾不可作别名）
-- 预期结果：
--   - found=100, missing=NULL
--   - computed = 100 + 5 = 105
-- 后置处理：DROP 测试表

CREATE TABLE ss(id INT PRIMARY KEY, v INT);

INSERT INTO ss VALUES (1, 100);

-- 1) + 4) 命中与未命中（别名用上下文关键字 found/missing，回归 BUG-17）
SELECT (SELECT v FROM ss WHERE id = 1) AS found,
       (SELECT v FROM ss WHERE id = 99) AS missing;

-- 2) 无别名的标量子查询
SELECT (SELECT v FROM ss WHERE id = 1);

-- 3) 参与算术
SELECT (SELECT v FROM ss WHERE id = 1) + 5 AS computed;

-- 外层行 × 标量子查询
SELECT id, (SELECT MAX(v) FROM ss) AS mx FROM ss;

-- 后置处理
DROP TABLE ss;

exit;
