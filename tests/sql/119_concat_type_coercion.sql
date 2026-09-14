-- 113_concat_type_coercion.sql
-- 测试目标：验证 || 连接运算符的跨类型隐式转换与优先级
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. INT || VARCHAR：1 || 'a' → '1a'
--   2. FLOAT || VARCHAR：1.5 || 'x' → '1.5x'
--   3. NULL || VARCHAR → NULL（SQL 三值逻辑）
--   4. VARCHAR || INT：'n' || 100 → 'n100'
--   5. 优先级：1 + 1 || 'b' —— 算术优先于连接 → '2b'
-- 预期结果：
--   - '1a' / '1.5x' / NULL / 'n100' / '2b'
-- 后置处理：无表（仅 SELECT）

SELECT 1 || 'a' AS c1;
SELECT 1.5 || 'x' AS c2;
SELECT NULL || 'a' AS c3;
SELECT 'n' || 100 AS c4;
SELECT 1 + 1 || 'b' AS c5;

exit;
