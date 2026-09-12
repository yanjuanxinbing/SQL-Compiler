-- ============================================================
-- 56_pattern: 字符串 / 模式匹配扩展（Category 5）
-- ============================================================
-- 涵盖：
--   - SIMILAR TO 'pat'                —— SQL:1999 风格正则（%/ _ 通配符 +
--                                        ERE 元字符 . * + ? [] () | ^ $ {}）
--   - SIMILAR TO 'pat' ESCAPE '\\'    —— 自定义 SQL 转义字符
--   - REGEXP 兼容 ERE 单词字符类
--       \d \D \s \S \w \W            —— std::regex ECMAScript 自身支持，
--                                        本任务通过预翻译显式化为 [0-9] 等
--   - REGEXP 仍按 POSIX ERE 子串匹配（回归检查）
--
-- 说明：项目 lexer 把字符串里的 '\\' 解析为单个 '\'；因此 SQL 模式中
--       显式 '\\' 转义字符在测试文件里必须写成 '\\\\'（即 lexer 看到 '\\'）。
--       这一点与 44_pattern_match.sql 中 `LIKE '\%' ESCAPE '\\'` 的写法一致。
-- ============================================================

-- ---------- 1. SIMILAR TO 基础匹配 ----------
SELECT 'hello' SIMILAR TO 'h.*o';
SELECT 'hello' SIMILAR TO 'h_llo';
SELECT 'hello' SIMILAR TO 'h%o';
SELECT 'abc'   SIMILAR TO 'a_c';
SELECT 'abc'   SIMILAR TO 'a_c$';
SELECT 'abc'   SIMILAR TO 'xyz';

-- ---------- 2. SIMILAR TO 含 ERE 元字符（| + * ?） ----------
SELECT 'abc' SIMILAR TO '(a|b)c';
SELECT 'cat' SIMILAR TO 'ca+t';
SELECT 'ct'  SIMILAR TO 'ca*t';
SELECT 'cat' SIMILAR TO 'ca?t';

-- ---------- 3. SIMILAR TO + ESCAPE ----------
SELECT 'a%b' SIMILAR TO 'a\\%b' ESCAPE '\\';
SELECT 'a_b' SIMILAR TO 'a\\_b' ESCAPE '\\';

-- ---------- 4. ERE 单词字符类（REGEXP） ----------
SELECT 'abc123'    REGEXP '\d+';
SELECT 'abc'       REGEXP '\d+';
SELECT 'hello world' REGEXP '\s+';
SELECT 'hello_world' REGEXP '\w+';
SELECT '   '       REGEXP '\w+';
SELECT 'Hello'     REGEXP '[A-Z][a-z]+';

-- ---------- 5. REGEXP 既有语义回归 ----------
SELECT 'abc' REGEXP '^a';

exit;