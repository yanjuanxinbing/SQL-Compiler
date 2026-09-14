-- 124_insert_select_width.sql
-- 测试目标：验证 INSERT ... SELECT 按源 SELECT 的声明列数消费（BUG-19 回归测试）
--
-- 旧实现：ProjectExecutor 为了支持 ORDER BY 引用未投影列（隐藏列设计），
-- 会把底层元组追加在 SELECT 值之后（输出 = [select_values ++ underlying]）。
-- INSERT ... SELECT 按 GetValue(i) 逐列消费时把这些隐藏列当成了源列：
--   INSERT INTO dst(x, y) SELECT a FROM src;      -- 错误插入 (1,1),(2,2)
--   INSERT INTO dst SELECT a FROM src UNION SELECT b FROM src;
--                                                  -- 错误插入 (1,1),(2,2),(10,1),(20,2)
-- 且 src.ColumnCount() < N 的列数校验因隐藏列而失效。
-- 修复：InsertExecutor 记录源计划的声明输出列数（DeclaredOutputWidth），
-- 只消费声明宽度内的列；合法宽度不足目标列数时按 SQL 标准报错。
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 1 列源 → 2 列目标（UNION 源）：应报错，不插入任何行
--   2. 1 列源 → 1 列目标：正常插入
--   3. 1 列源 → 3 列目标：应报错，不插入任何行
--   4. 2 列源 → 2 列目标（显式列名）：正常插入
--   5. 2 列源 → 2 列目标（省略列名）：正常插入
--   6. 源带表达式 / WHERE：正常插入
-- 预期结果：
--   - 1、3 报 "INSERT ... SELECT column count mismatch"，目标表保持 0 行
--   - 2 → (1),(2)；4、5 → (1,10),(2,20)；6 → (101, 200)
-- 后置处理：DROP 所有测试表

CREATE TABLE src(a INT PRIMARY KEY, b INT);
CREATE TABLE d1(x INT PRIMARY KEY, y INT);
CREATE TABLE d2(x INT PRIMARY KEY);
CREATE TABLE d3(x INT PRIMARY KEY, y INT, z INT);
CREATE TABLE d4(x INT PRIMARY KEY, y INT);

INSERT INTO src VALUES (1, 10), (2, 20);

-- 1) 1 列源（UNION）→ 2 列目标：报错（旧行为错误插入 4 行错位列）
INSERT INTO d1 SELECT a FROM src UNION SELECT b FROM src;
SELECT COUNT(*) AS d1_rows FROM d1;

-- 2) 1 列源 → 1 列目标
INSERT INTO d2 SELECT a FROM src;
SELECT * FROM d2 ORDER BY x;

-- 3) 1 列源 → 3 列目标：报错
INSERT INTO d3 SELECT a FROM src;
SELECT COUNT(*) AS d3_rows FROM d3;

-- 4) 2 列源 → 2 列目标（显式列名）
INSERT INTO d1(x, y) SELECT a, b FROM src;
SELECT * FROM d1 ORDER BY x;

-- 5) 2 列源 → 2 列目标（省略列名）
INSERT INTO d4 SELECT a, b FROM src;
SELECT * FROM d4 ORDER BY x;

-- 6) 源带表达式 / WHERE
INSERT INTO d4(x, y) SELECT a + 100, b * 2 FROM src WHERE a = 1;
SELECT * FROM d4 ORDER BY x;

-- 后置处理
DROP TABLE d4;
DROP TABLE d3;
DROP TABLE d2;
DROP TABLE d1;
DROP TABLE src;

exit;
