-- 71_proc_out_params.sql
-- Category 8 (extension): OUT / INOUT 参数通过 session variable (@var) 回传
--
-- 覆盖：
--   * 基本 OUT：procedure 把结果写到 OUT 形参；调用方在 CALL 后立即
--     SELECT @var 即可读到（不需要持有表）。
--   * INOUT：传入的 @var 既作为输入也作为输出，procedure 修改后
--     写入同一 @var。
--   * 多个 OUT 参数：一次 CALL 把多个值回传。
--   * OUT 与空结果集：procedure 体里跑一个空表聚合，OUT 形参应为 0
--     （而非 NULL）。
--   * IN 形参与现有 INSERT-持有表方案共存：继续兼容 59_procs 的用法。
--
-- 测试用表
CREATE TABLE _out_src (id INT, val INT);
INSERT INTO _out_src VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);
CREATE TABLE _out_empty (id INT);

-- ============================================================
-- Part A：基本 OUT —— single column aggregate
-- ============================================================
-- 71_proc_out_params 替代 V1 的"持有表"机制：OUT 形参最终值通过
-- ExecutionContext::session_vars_ 回传给 @var；调用方立即 SELECT 即可。
CREATE PROCEDURE get_count(IN threshold INT, OUT total INT)
BEGIN
    SET total = (SELECT COUNT(*) FROM _out_src WHERE val > threshold);
END;

CALL get_count(20, @result);
SELECT @result;
-- 期望：3（val > 20 的行：30, 40, 50）

CALL get_count(100, @result);
SELECT @result;
-- 期望：0（没有 val > 100 的行；空结果子查询返回 COUNT(*) = 0）

CALL get_count(0, @result);
SELECT @result;
-- 期望：5（所有行 val > 0）

-- ============================================================
-- Part B：INOUT —— 既输入又输出
-- ============================================================
-- procedure 在原值基础上加 delta；OUT 形参已绑定到 @var，INOUT 起点
-- 是调用方传入的 @var 当前值。
CREATE PROCEDURE add_delta(IN delta INT, INOUT x INT)
BEGIN
    SET x = x + delta;
END;

-- 71_proc_out_params 不要求顶层 SET @var = expr；下面用一个简单 procedure
-- 设置 @counter，再 add_delta 修改它。
CREATE PROCEDURE set_counter(IN v INT, OUT out_v INT)
BEGIN
    SET out_v = v;
END;
CALL set_counter(100, @counter);
SELECT @counter;
-- 期望：100

CALL add_delta(7, @counter);
SELECT @counter;
-- 期望：107（100 + 7）

CALL add_delta(50, @counter);
SELECT @counter;
-- 期望：157（107 + 50）

-- INOUT 起点：调用方从未设置过 @counter2 时为 NULL；x + delta 走 SQL
-- 三值逻辑（NULL + INT = NULL），@counter2 应仍为 NULL。
CALL add_delta(3, @counter2);
SELECT @counter2;
-- 期望：NULL

-- ============================================================
-- Part C：多个 OUT 参数
-- ============================================================
CREATE PROCEDURE stats(IN t INT, OUT cnt INT, OUT sumv INT)
BEGIN
    SET cnt  = (SELECT COUNT(*) FROM _out_src WHERE val > t);
    SET sumv = (SELECT COALESCE(SUM(val), 0) FROM _out_src WHERE val > t);
END;

CALL stats(20, @c, @s);
SELECT @c, @s;
-- 期望：3, 120（val > 20 的行有 30,40,50；sum=120）

CALL stats(100, @c, @s);
SELECT @c, @s;
-- 期望：0, 0（空集路径）

-- ============================================================
-- Part D：OUT 配合 procedure 内 SELECT —— 标量子查询
-- ============================================================
-- 71_proc_out_params：标量子查询（SELECT MIN(val) ...）能直接在 SET rhs
-- 里给 OUT 形参赋值。这条路径覆盖了"非聚合查询"作为 OUT 值的场景。
CREATE PROCEDURE first_row(IN t INT, OUT first_val INT)
BEGIN
    SET first_val = (SELECT MIN(val) FROM _out_src WHERE val > t);
END;

CALL first_row(15, @fv);
SELECT @fv;
-- 期望：20（val > 15 的最小值）

CALL first_row(100, @fv);
SELECT @fv;
-- 期望：NULL（空集时标量子查询返回 NULL）

-- D-2: 同一个 procedure 内多次 SET 覆盖 OUT 形参 —— 验证最后一次写入生效。
CREATE PROCEDURE last_wins(IN a INT, IN b INT, OUT outv INT)
BEGIN
    SET outv = a;
    SET outv = b;
END;
CALL last_wins(1, 9, @v);
SELECT @v;
-- 期望：9（最后一次 SET 覆盖）

-- ============================================================
-- Part E：回归 —— 59_procs 持有表方式依然有效
-- ============================================================
-- 旧用法 procedure 把 OUT 值 INSERT 到一张表，调用方 SELECT 该表。
-- 71_proc_out_params 不破坏这条路径。
CREATE PROCEDURE legacy_sum(IN a INT, IN b INT)
BEGIN
    DECLARE s INT;
    SET s = a + b;
    INSERT INTO _out_src VALUES (100, s);
END;

CALL legacy_sum(11, 22);
SELECT val FROM _out_src WHERE id = 100;
-- 期望：33（持有表形式继续工作）

-- ============================================================
-- Part F：语义校验 —— 非 @var 实参到 OUT 形参应报错
-- ============================================================
-- 调用方传字面量给 OUT 形参应给出明确错误，而不是默默写 NULL。
CALL get_count(20, 999);
-- 期望：runtime error，提示 OUT 必须传 @var

-- ============================================================
-- Cleanup
-- ============================================================
DELETE FROM _out_src WHERE id = 100;
DROP PROCEDURE legacy_sum;
DROP PROCEDURE first_row;
DROP PROCEDURE last_wins;
DROP PROCEDURE stats;
DROP PROCEDURE add_delta;
DROP PROCEDURE set_counter;
DROP PROCEDURE get_count;
DROP TABLE _out_src;
DROP TABLE _out_empty;

exit;