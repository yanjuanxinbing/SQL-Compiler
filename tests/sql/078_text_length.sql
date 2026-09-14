-- 76_text_length.sql
-- BUG-3 [P0 严重]: TEXT(n) 长度限制未生效
-- --------------------------------------------------------------------------
-- 历史原因（已修复，见 ConstraintChecker.cpp §VARCHAR(N) 长度校验）：
--   has_char_limit lambda 只列举 VARCHAR / CHAR / STRING，没把 TEXT 包进去。
--   现象：CREATE TABLE t(s TEXT(5)) 后插入 6 字符串被静默接受，
--   数据完整性被破坏（CREATE INDEX 路径已拒绝 → DDL/DML 行为不一致）。
--
-- 本测试覆盖：
--   1) TEXT(N) 边界：N 字符允许，N+1 拒绝。
--   2) TEXT(N) 与 VARCHAR(N) 行为对齐（同样长度限制、同样的报错文案）。
--   3) TEXT 无长度限制时（裸 TEXT / TEXT() / 不写 (N)）不限制。
--   4) UPDATE 路径上的 TEXT(N) 也强制。
--   5) 多字节 UTF-8 字符按字符数计（"你好" = 2 字符，不是 6 字节）。
--   6) NULL 不参与长度校验。
--   7) TEXT 与其他长度类型（VARCHAR(N)、CHAR(N)）混合在一张表里各自生效。

-- =====================================================================
-- 1) TEXT(N) 边界
-- =====================================================================
CREATE TABLE t_textlen (
    id INT PRIMARY KEY,
    s  TEXT(5)
);
INSERT INTO t_textlen VALUES (1, 'abc');      -- OK, 3 字符
INSERT INTO t_textlen VALUES (2, 'abcde');    -- OK, 5 字符（边界）
INSERT INTO t_textlen VALUES (3, '');         -- OK, 空串
-- 6 字符：拒绝
INSERT INTO t_textlen VALUES (4, 'abcdef');
-- 远超：拒绝
INSERT INTO t_textlen VALUES (5, 'this_string_is_way_too_long_for_text_5');
SELECT id, s, LENGTH(s) AS len FROM t_textlen ORDER BY id;
DROP TABLE t_textlen;

-- =====================================================================
-- 2) TEXT(N) 与 VARCHAR(N) 行为对齐
-- =====================================================================
CREATE TABLE t_vc (
    id INT PRIMARY KEY,
    a  VARCHAR(5),
    b  TEXT(5),
    c  CHAR(5)
);
INSERT INTO t_vc VALUES (1, 'abcde', 'abcde', 'abcde');  -- 边界 OK
INSERT INTO t_vc VALUES (2, 'abcdef', 'abcdef', 'abcdef'); -- 6 字符 → 三列全拒
SELECT id FROM t_vc ORDER BY id;
DROP TABLE t_vc;

-- =====================================================================
-- 3) TEXT 无长度限制
-- =====================================================================
CREATE TABLE t_text_free (
    id INT PRIMARY KEY,
    s  TEXT
);
-- 任意长度都允许
INSERT INTO t_text_free VALUES (1, REPEAT('x', 1000));
INSERT INTO t_text_free VALUES (2, REPEAT('y', 10000));
SELECT id, LENGTH(s) AS len FROM t_text_free ORDER BY id;
DROP TABLE t_text_free;

-- =====================================================================
-- 4) UPDATE 路径上的 TEXT(N)
-- =====================================================================
CREATE TABLE t_text_upd (
    id INT PRIMARY KEY,
    s  TEXT(5)
);
INSERT INTO t_text_upd VALUES (1, 'abc');
-- 改成 6 字符：拒绝
UPDATE t_text_upd SET s = 'abcdef' WHERE id = 1;
-- 改成 5 字符：合法
UPDATE t_text_upd SET s = 'abcde' WHERE id = 1;
SELECT id, s FROM t_text_upd ORDER BY id;
DROP TABLE t_text_upd;

-- =====================================================================
-- 5) 多字节 UTF-8 字符按字符数计
-- =====================================================================
CREATE TABLE t_utf8 (
    id INT PRIMARY KEY,
    s  TEXT(3)
);
-- "你好" = 2 字符：合法
INSERT INTO t_utf8 VALUES (1, '你好');
-- "你好啊" = 3 字符（边界）：合法
INSERT INTO t_utf8 VALUES (2, '你好啊');
-- "你好啊!" = 4 字符：拒绝
INSERT INTO t_utf8 VALUES (3, '你好啊!');
SELECT id, s FROM t_utf8 ORDER BY id;
DROP TABLE t_utf8;

-- =====================================================================
-- 6) NULL 不参与长度校验
-- =====================================================================
CREATE TABLE t_text_null (
    id INT PRIMARY KEY,
    s  TEXT(5)
);
INSERT INTO t_text_null VALUES (1, NULL);
INSERT INTO t_text_null VALUES (2, NULL);
SELECT id, s FROM t_text_null ORDER BY id;
DROP TABLE t_text_null;

-- =====================================================================
-- 7) TEXT 与 VARCHAR / CHAR 在同一张表里各自生效
-- =====================================================================
CREATE TABLE t_mix (
    id  INT PRIMARY KEY,
    a   TEXT(5),
    b   VARCHAR(10),
    c   CHAR(3)
);
INSERT INTO t_mix VALUES (1, 'abc', 'abcdefg', 'xyz');
-- a 超 TEXT(5)：拒绝
INSERT INTO t_mix VALUES (2, 'abcdef', 'abc', 'x');
-- b 超 VARCHAR(10)：拒绝
INSERT INTO t_mix VALUES (3, 'ab', 'abcdefghijk', 'x');
-- c 超 CHAR(3)：拒绝
INSERT INTO t_mix VALUES (4, 'ab', 'abc', 'wxyz');
SELECT id FROM t_mix ORDER BY id;
DROP TABLE t_mix;

exit;
