-- 82_decimal_arithmetic.sql
-- Bug 4: DECIMAL type arithmetic completely broken
--   DECIMAL 按文本持久化（运行时为 VARCHAR），但 ADD/SUB/MUL/DIV/MOD
--   路径只识别 INTEGER / FLOAT —— 当一侧是 VARCHAR 时直接掉到 INT 分支，
--   用 AsInt()（对 VARCHAR 返回 0）参与运算，结果不正确。

CREATE TABLE t (val DECIMAL(10,2));
INSERT INTO t VALUES (100.00), (200.50);
SELECT val + 10 FROM t;
SELECT val * 2 FROM t;
SELECT SUM(val) FROM t;
SELECT AVG(val) FROM t;

-- 附加 sanity checks
SELECT val - 5 FROM t;
SELECT val / 4 FROM t;
SELECT val + val FROM t;
SELECT val + 0.50 FROM t;
SELECT val * 1.10 FROM t;
SELECT -val FROM t;

-- 比较运算符
SELECT val > 150 FROM t;
SELECT val = 100.00 FROM t;

-- 字面量 DECIMAL 算术
SELECT 5.50 + 2.25;
SELECT 10.00 - 3.5;
SELECT 4.00 * 2.5;
SELECT 9.00 / 3.0;

-- NULL 语义
INSERT INTO t VALUES (NULL);
SELECT val + 10 FROM t;
SELECT SUM(val) FROM t;

exit;
