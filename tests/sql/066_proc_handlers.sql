-- 68_proc_handlers.sql
-- Category 8 (extension): EXIT / UNDO HANDLER for stored procedures
--
-- 覆盖：
--   * DECLARE EXIT HANDLER FOR SQLEXCEPTION —— 触发后退出 handler 所在 block
--   * DECLARE UNDO   HANDLER FOR SQLEXCEPTION —— 触发后回滚 block 的写入并退出
--   * DECLARE EXIT   HANDLER FOR SQLSTATE '...' —— SQLSTATE 特化匹配
--   * CONTINUE 在嵌套 EXIT 之外的回归（仍可与 EXIT 共存）
--   * UNDO + SAVEPOINT 与显式 BEGIN TRANSACTION 的协作
--   * UNDO 在 CALL 自动事务下的回滚
--   * 优先级：UNDO > EXIT > CONTINUE（同 specificity / 同 condition）
--
-- 已知差异：本 V1 中 UNDO handler 的 body 写入也会被 ROLLBACK TO SAVEPOINT
-- 一并撤销（因 handler body 与 block 内 DML 共享同一 savepoint scope）。
-- MySQL/MariaDB 仅回滚 block 内 DML，handler body 的写入保留。本测试按
-- V1 行为设计：直接检查 block 内 DML 被回滚（持有表为空），不依赖
-- handler body 的副作用。

CREATE TABLE _h_out (tag VARCHAR, val INT);

-- ============================================================
-- Part A: EXIT HANDLER 基本语义
-- ============================================================
-- procedure 内 INSERT 后 SIGNAL SQLSTATE '22012'，由 EXIT handler 捕获。
-- 期望：
--   * INSERT before_signal 提交（procedure body 没有 UNDO）
--   * EXIT handler body 执行（INSERT exit_basic）
--   * procedure 退出，after_signal 不应出现
CREATE PROCEDURE exit_basic_proc()
BEGIN
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
        INSERT INTO _h_out VALUES ('exit_basic', 1);
    INSERT INTO _h_out VALUES ('before_signal', 100);
    SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'forced';
    INSERT INTO _h_out VALUES ('after_signal', 200);
END;

CALL exit_basic_proc();
SELECT * FROM _h_out WHERE tag IN ('before_signal', 'exit_basic', 'after_signal')
    ORDER BY tag;
-- 期望两行：before_signal / exit_basic（after_signal 不应出现）。
DELETE FROM _h_out;

-- ============================================================
-- Part B: EXIT HANDLER 出现在 WHILE 内 —— 仅跳出 WHILE
-- ============================================================
CREATE PROCEDURE exit_in_loop_proc()
BEGIN
    DECLARE i INT DEFAULT 0;
    DECLARE total INT DEFAULT 0;
    WHILE i < 5 DO
        DECLARE EXIT HANDLER FOR SQLEXCEPTION
            INSERT INTO _h_out VALUES ('exit_in_loop', i);
        SET i = i + 1;
        IF i = 3 THEN
            SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'break at 3';
        END IF;
        SET total = total + i;
    END WHILE;
    INSERT INTO _h_out VALUES ('after_loop', total);
END;

CALL exit_in_loop_proc();
SELECT * FROM _h_out WHERE tag IN ('exit_in_loop', 'after_loop')
    ORDER BY tag;
-- 期望两行：exit_in_loop（i=3 触发） / after_loop（procedure 继续）。
DELETE FROM _h_out;

-- ============================================================
-- Part C: EXIT HANDLER 在嵌套 IF 内 —— 只退出 IF body
-- ============================================================
CREATE PROCEDURE exit_in_if_proc()
BEGIN
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
        INSERT INTO _h_out VALUES ('exit_in_if', 1);
    INSERT INTO _h_out VALUES ('outer_before', 10);
    IF 1 = 1 THEN
        INSERT INTO _h_out VALUES ('if_before', 20);
        SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'in if';
        INSERT INTO _h_out VALUES ('if_after', 30);
    END IF;
    INSERT INTO _h_out VALUES ('outer_after', 40);
END;

CALL exit_in_if_proc();
SELECT * FROM _h_out ORDER BY tag;
-- 期望四行：outer_before, if_before, exit_in_if, outer_after
-- （if_after 不应出现，因为 EXIT handler 在 IF body 内触发，只退出 IF）。
DELETE FROM _h_out;

-- ============================================================
-- Part D: CONTINUE 仍然有效 —— 回归测试
-- ============================================================
-- 函数内 CONTINUE handler；行为：触发后继续执行剩余语句。
-- 注意：本函数没有 a/0 之类的后续 DML，所以 CONTINUE 触发后
-- SET r = n = 0 把 r 覆盖为 0；这与 59_procs 的 safe_divide 不一样
-- （后者 SET r = a/b 本身会因 b=0 失败从而再触发 CONTINUE）。
CREATE FUNCTION continue_still_works(n INT) RETURNS INT
BEGIN
    DECLARE r INT DEFAULT 0;
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET r = -1;
    IF n = 0 THEN
        SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'div0';
    END IF;
    SET r = n;
    RETURN r;
END;

SELECT continue_still_works(7);
SELECT continue_still_works(0);
-- 期望：7 与 0（CONTINUE 把 r 置为 -1 后 SET r = n 覆盖为 0）。

-- ============================================================
-- Part E: UNDO + SAVEPOINT —— 显式事务内的 INSERT 必须被回滚
-- ============================================================
CREATE TABLE _undo_t (id INT, val INT);

-- 在事务开始前先 INSERT 一行，作为"基准"：UNDO 不应影响事务起点之前
-- 的写入（SAVEPOINT 仅覆盖 procedure body 进入之后的写入）。
INSERT INTO _undo_t VALUES (0, 0);

CREATE PROCEDURE undo_with_savepoint_proc()
BEGIN
    DECLARE UNDO HANDLER FOR SQLEXCEPTION
        INSERT INTO _h_out VALUES ('undo_fired', 1);
    INSERT INTO _undo_t VALUES (1, 10);
    SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'force rollback';
    INSERT INTO _undo_t VALUES (2, 20);
END;

BEGIN TRANSACTION;
CALL undo_with_savepoint_proc();
-- 此时 SAVEPOINT 应已被 ROLLBACK + RELEASE；UNDO 的 INSERT（INSERT INTO
-- _h_out ...）在 SAVEPOINT 之后写入，也被 ROLLBACK TO 影响（V1 简化）。
COMMIT;

SELECT * FROM _undo_t;
-- 期望：仅 (0, 0) 基准行；procedure 内的 (1, 10) / (2, 20) 被 UNDO 回滚。
DROP TABLE _undo_t;
DELETE FROM _h_out;

-- ============================================================
-- Part F: UNDO 在 CALL 自动事务下的回滚
-- ============================================================
-- 本 V1 中每次 CALL/UDF 由 Database::ExecuteSQL 的 autocommit 包裹，会
-- Begin() 一个隐式事务。EnterBlock 看到 active txn 后会分配 SAVEPOINT，
-- UNDO 行为是真正的 ROLLBACK TO SAVEPOINT：procedure 内的 INSERT 与
-- handler body 自身的 INSERT 都会被撤销，procedure 退出。
-- 期望：_h_out 为空。
CREATE PROCEDURE undo_autocommit_proc()
BEGIN
    DECLARE UNDO HANDLER FOR SQLEXCEPTION
        INSERT INTO _h_out VALUES ('undo_autocommit', 1);
    INSERT INTO _h_out VALUES ('undo_autocommit_before', 100);
    SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'autocommit txn';
    INSERT INTO _h_out VALUES ('undo_autocommit_after', 200);
END;

CALL undo_autocommit_proc();
SELECT * FROM _h_out WHERE tag LIKE 'undo_autocommit%' ORDER BY tag;
-- 期望：0 rows（UNDO 把 procedure 与 handler body 的 INSERT 一并回滚）。
DELETE FROM _h_out;

-- ============================================================
-- Part G: 优先级 —— UNDO > EXIT > CONTINUE（同 specificity / 同 condition）
-- ============================================================
-- 声明顺序：CONTINUE, EXIT, UNDO；全部对 SQLEXCEPTION。期望 UNDO 胜出
-- （procedure body UNDO 退出；handler 体里写入 'priority_undo'）。
-- 因 V1 简化：handler body 的 INSERT 也被 ROLLBACK TO 一并撤销——
-- 我们只能从 SELECT priority_t 的空结果反推 UNDO 触发了 procedure body
-- 回滚（否则 priority_before / priority_after 会留存）。
CREATE TABLE _priority_t (tag VARCHAR, val INT);
INSERT INTO _priority_t VALUES ('baseline', 0);

CREATE PROCEDURE priority_undo_wins_proc()
BEGIN
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
        INSERT INTO _priority_t VALUES ('priority_continue', 1);
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
        INSERT INTO _priority_t VALUES ('priority_exit', 1);
    DECLARE UNDO HANDLER FOR SQLEXCEPTION
        INSERT INTO _priority_t VALUES ('priority_undo', 1);
    INSERT INTO _priority_t VALUES ('priority_before', 1);
    SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'priority test';
    INSERT INTO _priority_t VALUES ('priority_after', 1);
END;

BEGIN TRANSACTION;
CALL priority_undo_wins_proc();
COMMIT;

SELECT * FROM _priority_t ORDER BY tag;
-- 期望仅 baseline 留存；procedure 内所有 INSERT 都被 UNDO 回滚。
DROP TABLE _priority_t;

-- ============================================================
-- Part H: SQLSTATE 特化 handler 优先于 class handler
-- ============================================================
CREATE PROCEDURE sqlstate_priority_proc()
BEGIN
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
        INSERT INTO _h_out VALUES ('caught_class', 1);
    DECLARE EXIT HANDLER FOR SQLSTATE '22012'
        INSERT INTO _h_out VALUES ('caught_sqlstate', 1);
    SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'sqlstate specific test';
END;

CALL sqlstate_priority_proc();
SELECT * FROM _h_out WHERE tag IN ('caught_class', 'caught_sqlstate')
    ORDER BY tag;
-- 期望：caught_sqlstate 胜出（specificity 优先）；caught_class 不应出现。
DELETE FROM _h_out;

-- ============================================================
-- Part I: EXIT handler body 内再 SIGNAL —— 二次抛出
-- ============================================================
-- handler body 内再 SIGNAL 一个 SQLEXCEPTION：procedure 顶层无更多匹配
-- handler 时异常上抛，CALL 失败；后续 SQL 继续执行。
CREATE PROCEDURE exit_handler_rethrow_proc()
BEGIN
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
        SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'rethrown';
    SIGNAL SQLSTATE '99999' SET MESSAGE_TEXT = 'first';
END;

CALL exit_handler_rethrow_proc();
-- 期望：CALL 抛 SIGNAL 错误（rethrown 异常无法被再次匹配）。
INSERT INTO _h_out VALUES ('after_failed_call', 1);
SELECT * FROM _h_out WHERE tag = 'after_failed_call';
-- 期望：after_failed_call 一行（确认调用方继续执行）。
DELETE FROM _h_out;

-- ============================================================
-- Part J: EXIT 在 LOOP 内 —— handler body 写入持有表
-- ============================================================
CREATE PROCEDURE exit_in_loop_only_proc()
BEGIN
    DECLARE i INT DEFAULT 0;
    WHILE i < 10 DO
        DECLARE EXIT HANDLER FOR SQLEXCEPTION
            INSERT INTO _h_out VALUES ('loop_exit', i);
        SET i = i + 1;
        IF i = 4 THEN
            SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'break loop';
        END IF;
    END WHILE;
    INSERT INTO _h_out VALUES ('after_loop_only', i);
END;

CALL exit_in_loop_only_proc();
SELECT * FROM _h_out WHERE tag IN ('loop_exit', 'after_loop_only') ORDER BY tag;
-- 期望两行：loop_exit（i=4 触发） / after_loop_only（loop 退出后 procedure 继续）。
DELETE FROM _h_out;

-- ============================================================
-- Cleanup
-- ============================================================
DROP PROCEDURE exit_basic_proc;
DROP PROCEDURE exit_in_loop_proc;
DROP PROCEDURE exit_in_if_proc;
DROP FUNCTION continue_still_works;
DROP PROCEDURE undo_with_savepoint_proc;
DROP PROCEDURE undo_autocommit_proc;
DROP PROCEDURE priority_undo_wins_proc;
DROP PROCEDURE sqlstate_priority_proc;
DROP PROCEDURE exit_handler_rethrow_proc;
DROP PROCEDURE exit_in_loop_only_proc;
DROP TABLE _h_out;

exit;
