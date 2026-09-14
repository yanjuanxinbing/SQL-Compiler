-- 75_string_funcs_edge.sql
-- 测试目标：验证字符串函数在边界条件下的行为
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. UPPER/LOWER 对空串和纯空白串的行为
--   2. SUBSTR 各种 start/length 组合（正数、超出、越界）
--   3. TRIM 对全空白串
--   4. REPLACE 无匹配 → 原样返回
--   5. LENGTH 对多字节字符（UTF-8 中文 = 3 字节）
--   6. || 连接运算符（含 NULL 语义）
-- 预期结果：
--   - UPPER('') = ''，LOWER('') = ''
--   - SUBSTR('abcdef',2,3) = 'bcd'
--   - SUBSTR('abcdef',1,100) = 'abcdef'（超长截取到末尾）
--   - SUBSTR('abcdef',10,3) = ''（起始越界 → 空串）
--   - TRIM('  x  ') = 'x'
--   - REPLACE('aaa','b','c') = 'aaa'（无匹配）
--   - LENGTH('中') = 3
--   - 'a' || 'b' = 'ab'，NULL || 'b' = NULL
-- 后置处理：DROP 测试表

CREATE TABLE sf(id INT, s VARCHAR);

INSERT INTO sf VALUES (1, 'hello'), (2, ''), (3, '  x  '), (4, NULL);

-- UPPER / LOWER
SELECT id, UPPER(s) AS u, LOWER(s) AS l FROM sf ORDER BY id;

-- LENGTH（NULL → NULL，空串 → 0）
SELECT id, LENGTH(s) AS len FROM sf ORDER BY id;

-- SUBSTR：正常截取
SELECT SUBSTR('abcdef', 2, 3) AS sub1;

-- SUBSTR：超长 length 截取到末尾
SELECT SUBSTR('abcdef', 1, 100) AS sub2;

-- SUBSTR：起始越界 → 空串
SELECT SUBSTR('abcdef', 10, 3) AS sub3;

-- TRIM
SELECT TRIM('  x  ') AS trim1;
SELECT TRIM('') AS trim2;

-- REPLACE 无匹配 → 原样
SELECT REPLACE('aaa', 'b', 'c') AS rep1;
SELECT REPLACE('hello', 'l', 'L') AS rep2;

-- LENGTH 对多字节字符
SELECT LENGTH('中') AS cjk_len;
SELECT LENGTH('abc中def') AS mixed_len;

-- || 连接运算符
SELECT 'a' || 'b' AS concat1;
SELECT s || '!' AS concat2 FROM sf ORDER BY id;

-- 后置处理
DROP TABLE sf;

exit;
