-- 106_nested_func_calls.sql
-- 测试目标：验证函数嵌套调用（内层结果作为外层参数）的正确求值顺序
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. UPPER(SUBSTR(...))：先截取后大写
--   2. SUBSTR(UPPER(...), ...)：先大写后截取
--   3. LENGTH(TRIM(...))：先去空白后计长
--   4. ABS(-ROUND(...))：函数包裹表达式、表达式包裹函数
--   5. REPLACE(UPPER(...), ...)：三层参数嵌套
-- 预期结果：
--   - UPPER(SUBSTR('hello world',1,5)) = 'HELLO'
--   - SUBSTR(UPPER('hello'),2,3) = 'ELL'
--   - LENGTH(TRIM('  ab  ')) = 2
--   - ABS(-ROUND(2.7)) = 3
--   - REPLACE(UPPER('abcabc'),'B','X') = 'AXCAXC'
-- 后置处理：无表（仅 SELECT）

-- 1) 先截取后大写
SELECT UPPER(SUBSTR('hello world', 1, 5)) AS n1;

-- 2) 先大写后截取
SELECT SUBSTR(UPPER('hello'), 2, 3) AS n2;

-- 3) 先去空白后计长
SELECT LENGTH(TRIM('  ab  ')) AS n3;

-- 4) 函数与表达式互相嵌套
SELECT ABS(-ROUND(2.7)) AS n4;

-- 5) REPLACE 参数中嵌套函数
SELECT REPLACE(UPPER('abcabc'), 'B', 'X') AS n5;

exit;
