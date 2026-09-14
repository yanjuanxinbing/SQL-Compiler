-- 101_insert_expr_values.sql
-- 测试目标：验证 INSERT ... VALUES 使用计算表达式、函数、标量子查询求值
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. VALUES 中使用算术表达式（2 + 3）
--   2. VALUES 中使用字符串函数（UPPER('ab')）
--   3. VALUES 中使用标量子查询（SELECT MAX(id) FROM 其它表）
--   4. VALUES 中使用连接表达式（'x' || 'y'）
-- 预期结果：
--   - (1, 5, 'AB')
--   - (2, 3, 'xy') —— MAX(id) 取自 u1 的 3 行数据
-- 后置处理：DROP 测试表

CREATE TABLE u1(id INT PRIMARY KEY, v INT);
INSERT INTO u1 VALUES (1, 10), (2, -5), (3, NULL);

CREATE TABLE i1(id INT PRIMARY KEY, a INT, b VARCHAR);

-- 1) + 2) 算术表达式与函数
INSERT INTO i1 VALUES (1, 2 + 3, UPPER('ab'));

-- 3) + 4) 标量子查询与 || 连接
INSERT INTO i1 VALUES (2, (SELECT MAX(id) FROM u1), 'x' || 'y');

SELECT id, a, b FROM i1 ORDER BY id;

-- 后置处理
DROP TABLE i1;
DROP TABLE u1;

exit;
