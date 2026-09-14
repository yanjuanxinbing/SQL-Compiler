-- 80_numeric_overflow.sql
-- 测试目标：验证数值溢出与特殊浮点值的行为
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. INT 最大/最小边界值（2147483647 / -2147483648）
--   2. INT 算术溢出（MAX + 1 → 静默回绕，C++ 未定义行为）
--   3. FLOAT 极大值（1e308）、极小值（1e-308）
--   4. FLOAT 除零：0.0/0.0 → NaN（存为 NULL），1.0/0.0 → Inf（存为 NULL）
--   5. 整数除零：应被拒绝或返回 NULL
--   6. 负数运算
-- 预期结果：
--   - INT 极值正确存储和读取
--   - INT 溢出：回绕到负值（C++ 有符号整数溢出行为）
--   - FLOAT 极大/极小值正确存储
--   - NaN/Inf 存为 NULL（数据库不原生支持非有限浮点）
--   - 整数除零：返回 NULL 或报错（取决于实现）
-- 后置处理：DROP 所有测试表

CREATE TABLE ni(id INT PRIMARY KEY, n INT);
CREATE TABLE nf(id INT PRIMARY KEY, f FLOAT);

-- INT 极值
INSERT INTO ni VALUES (1, 2147483647), (2, -2147483648);
SELECT id, n FROM ni ORDER BY id;

-- INT 算术溢出：MAX + 1 回绕
SELECT 2147483647 + 1 AS int_overflow;

-- INT 算术：MIN - 1 回绕
SELECT -2147483648 - 1 AS int_underflow;

-- 负数运算
SELECT -10 + 5 AS neg1;
SELECT -10 * -3 AS neg2;
SELECT 10 / -3 AS neg3;

-- 整数除零（返回 NULL）
SELECT 10 / 0 AS int_div_zero;

-- FLOAT 极大值
INSERT INTO nf VALUES (1, 1.0e308);
-- FLOAT 极小值（下溢接近 0）
INSERT INTO nf VALUES (2, 1.0e-308);
-- FLOAT 除零：0.0/0.0 → NaN（存为 NULL）
INSERT INTO nf VALUES (3, 0.0 / 0.0);
-- FLOAT 除零：1.0/0.0 → Inf（存为 NULL）
INSERT INTO nf VALUES (4, 1.0 / 0.0);

SELECT id, f FROM nf ORDER BY id;

-- FLOAT 算术
SELECT 3.14 + 2.86 AS float_add;
SELECT 3.14 * 0.0 AS float_mul_zero;
SELECT 1.5 / 0.0 AS float_div_zero;

-- FLOAT 与 INT 混合运算
SELECT 10 + 1.5 AS mixed1;
SELECT 10 / 3 AS int_div;
SELECT 10.0 / 3.0 AS float_div;

-- 后置处理
DROP TABLE ni;
DROP TABLE nf;

exit;
