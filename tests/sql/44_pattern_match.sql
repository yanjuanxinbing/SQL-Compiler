-- 44_pattern_match.sql
-- 验证 LIKE / ILIKE / REGEXP / RLIKE 的扩展语法：
--   - LIKE 'pat' ESCAPE 'x'  : 自定义转义字符；
--   - ILIKE 'pat'            : 大小写不敏感 LIKE；
--   - REGEXP / RLIKE 'pat'   : POSIX ERE 风格子串匹配；
--   - 非法的 REGEXP 模式     : 报错而非静默 false。

CREATE TABLE t(id INT, s VARCHAR);

INSERT INTO t VALUES (1, '100%');
INSERT INTO t VALUES (2, 'foo_bar');
INSERT INTO t VALUES (3, 'Apple');
INSERT INTO t VALUES (4, 'apple');
INSERT INTO t VALUES (5, 'Banana');
INSERT INTO t VALUES (6, 'aXbXc');
INSERT INTO t VALUES (7, 'plain');

-- 1) LIKE 'pat' ESCAPE '\\' : '%' 被转义视为字面量，仅匹配末尾含 '%' 的字符串。
SELECT s FROM t WHERE s LIKE '%\%' ESCAPE '\\' ORDER BY id;

-- 2) LIKE 'foo\_bar' ESCAPE '\\' : '_' 被转义视为字面量，仅匹配 'foo_bar'。
SELECT s FROM t WHERE s LIKE 'foo\_bar' ESCAPE '\\' ORDER BY id;

-- 3) ILIKE 'apple' : 大小写不敏感，应同时匹配 'Apple' 与 'apple'。
SELECT s FROM t WHERE s ILIKE 'apple' ORDER BY id;

-- 4) REGEXP '^a' : 锚定行首小写 a → 'Apple'、'apple'。
SELECT s FROM t WHERE s REGEXP '^a' ORDER BY id;

-- 5) REGEXP '^[0-9]+\%$' : 行首数字 + 字面量 % → '100%'。
SELECT s FROM t WHERE s REGEXP '^[0-9]+\%$' ORDER BY id;

-- 6) REGEXP 'X|Z' : 子串匹配 X 或 Z → 'aXbXc'，'Banana' 不命中。
SELECT s FROM t WHERE s REGEXP 'X|Z' ORDER BY id;

-- 7) RLIKE 是 REGEXP 的别名：'^B' → 'Banana'。
SELECT s FROM t WHERE s RLIKE '^B' ORDER BY id;

-- 8) ILIKE 与 ESCAPE 联用：模式 'a%' 仍按大小写不敏感匹配。
SELECT s FROM t WHERE s ILIKE 'A%' ESCAPE '\\' ORDER BY id;

-- 9) 非法的 REGEXP 模式：未闭合的字符类抛 Error。
SELECT s FROM t WHERE s REGEXP '[invalid(';

exit;