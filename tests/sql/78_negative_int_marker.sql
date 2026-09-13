-- 78_negative_int_marker.sql
-- BUG-5 [P1 中等]: INSERT 中负数整数字面量解析边界
-- --------------------------------------------------------------------------
-- 历史：
--   第 1 轮：-1 被存为 NULL（与 -2/-10/-100 不同的现象）。
--           根因是 kNullMarker = -1 字节模式与合法 INT 值 -1 碰撞。
--           临时修：marker 改用 INT32_MIN，-1/-2/-10/-100 都正常，
--           但 INT_MIN（-2147483648）自己被解释为 NULL。
--   第 2 轮（本测试）：引入 NULL bitmap 后，in-band marker 整体废弃；
--           -1 / INT_MIN / 任何罕见 INT / 任何 FLOAT / 任何 VARCHAR
--           都能正常存储。NULL 信息由 tuple 级 bitmap 承载。
--
-- 本测试覆盖：
--   1) 全部负数（含 INT_MIN）正确存储。
--   2) NULL 路径仍然正常。
--   3) WHERE n < 0 命中所有真实负数行（NULL 不计入）。
--   4) NULL bitmap 不影响非 NULL 列的存储。
--   5) FLOAT / VARCHAR 列也能在各种边界值下正常工作。
--   6) 混合 NULL 的行按位正确读回（bitmap 顺序对齐列）。

-- =====================================================================
-- 1) 全部负数都能正确存储
-- =====================================================================
CREATE TABLE neg_test (id INT PRIMARY KEY, n INT);
INSERT INTO neg_test VALUES (1, -1);
INSERT INTO neg_test VALUES (2, -2);
INSERT INTO neg_test VALUES (3, -10);
INSERT INTO neg_test VALUES (4, -100);
INSERT INTO neg_test VALUES (5, -1000);
INSERT INTO neg_test VALUES (6, -2147483647);   -- INT_MIN+1
INSERT INTO neg_test VALUES (7, -2147483648);   -- INT_MIN（曾被 marker 占用）
SELECT id, n FROM neg_test ORDER BY id;
-- 期望：7 行，每行 n 与插入一致

-- =====================================================================
-- 2) NULL 路径仍然正常
-- =====================================================================
INSERT INTO neg_test VALUES (8, NULL);
INSERT INTO neg_test VALUES (9, NULL);
SELECT id, n FROM neg_test WHERE id IN (8, 9) ORDER BY id;
-- 期望：id=8,9, n 均为 NULL

-- =====================================================================
-- 3) WHERE n < 0 命中所有真实负数（NULL 不参与）
-- =====================================================================
SELECT COUNT(*) AS neg_count FROM neg_test WHERE n < 0;
-- 期望：7（id=1..7）

SELECT id, n FROM neg_test WHERE n < 0 ORDER BY id;
-- 期望：7 行

-- =====================================================================
-- 4) NULL bitmap 不影响非 NULL 列的存储
-- =====================================================================
-- 与 NULL 行交错存储后，非 NULL 行的 n 仍是原值。
SELECT id, n FROM neg_test WHERE id IN (1, 8, 2, 9, 7) ORDER BY id;
-- 期望：n 分别为 -1, NULL, -2, NULL, -2147483648

-- =====================================================================
-- 5) 表达式中的负数
-- =====================================================================
SELECT -1 AS expr_neg_one;
SELECT -2147483648 AS expr_min;
SELECT -2147483647 + 1 AS min_plus_one;   -- 期望：-2147483647
SELECT -1 * 5 AS five_neg;                -- 期望：-5

-- =====================================================================
-- 6) 多列混合 NULL 与边界值
-- =====================================================================
CREATE TABLE mixed_null (a INT, b INT, c INT);
INSERT INTO mixed_null VALUES (-2147483648, NULL, 0);
INSERT INTO mixed_null VALUES (NULL, -1, -2147483648);
INSERT INTO mixed_null VALUES (NULL, NULL, NULL);
SELECT a, b, c FROM mixed_null ORDER BY a, b, c;
-- 期望：3 行；NULL 由 bitmap 正确标识

-- =====================================================================
-- 7) FLOAT / VARCHAR 边界值
-- =====================================================================
CREATE TABLE fl_str (f FLOAT, s VARCHAR(20));
INSERT INTO fl_str VALUES (-1.0, 'hello');
INSERT INTO fl_str VALUES (-0.0, '');
INSERT INTO fl_str VALUES (NULL, NULL);
SELECT f, s FROM fl_str ORDER BY f, s;

DROP TABLE neg_test;
DROP TABLE mixed_null;
DROP TABLE fl_str;

exit;
