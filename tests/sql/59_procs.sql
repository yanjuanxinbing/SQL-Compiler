-- 59_procs.sql
-- Category 8: Procedural Language Extensions
--
-- 覆盖：
--   * LOOP / LEAVE                （无限循环 + LEAVE 退出）
--   * REPEAT ... UNTIL           （do-while 语义）
--   * 体内 CASE WHEN ... THEN ... ELSE ... END CASE
--   * SIGNAL SQLSTATE 'XXXXX' SET MESSAGE_TEXT = '...'
--   * DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
--   * DECLARE name CURSOR FOR select; OPEN / FETCH / CLOSE
--   * CREATE PROCEDURE name(args) BEGIN body END
--   * CALL proc(args)
--
-- OUT 参数通过"持有表"机制实现：procedure 内 INSERT INTO proc_results，
-- 调用方 SELECT 该表读取返回值。这是 V1 文档化的简化策略。

CREATE TABLE _proc_results (tag VARCHAR, val INT);

-- ============================================================
-- Part A: LOOP / LEAVE
-- ============================================================
-- V1 简化：不用 label；LEAVE 默认跳出最近循环。
-- procedure loop_sum(IN n INT) —— 把 1+2+...+n 累加后写入 _proc_results。
CREATE PROCEDURE loop_sum(IN n INT)
BEGIN
    DECLARE i INT DEFAULT 0;
    DECLARE total INT DEFAULT 0;
    LOOP
        SET i = i + 1;
        SET total = total + i;
        IF i >= n THEN LEAVE; END IF;
    END LOOP;
    INSERT INTO _proc_results VALUES ('loop_sum', total);
END;

CALL loop_sum(5);
SELECT * FROM _proc_results WHERE tag = 'loop_sum';

-- ============================================================
-- Part B: REPEAT ... UNTIL
-- ============================================================
CREATE FUNCTION repeat_test(n INT) RETURNS INT
BEGIN
    DECLARE i INT DEFAULT 0;
    REPEAT
        SET i = i + 1;
    UNTIL i >= n END REPEAT;
    RETURN i;
END;

SELECT repeat_test(5);
SELECT repeat_test(0);
SELECT repeat_test(1);

-- ============================================================
-- Part C: 体内 CASE WHEN ... THEN ... ELSE ... END CASE
-- ============================================================
CREATE FUNCTION case_body(n INT) RETURNS VARCHAR
BEGIN
    DECLARE r VARCHAR;
    CASE
        WHEN n > 0 THEN SET r = 'positive';
        WHEN n = 0 THEN SET r = 'zero';
        ELSE SET r = 'negative';
    END CASE;
    RETURN r;
END;

SELECT case_body(5);
SELECT case_body(0);
SELECT case_body(-3);

-- 简单 CASE (有 subject)
CREATE FUNCTION case_body_simple(n INT) RETURNS VARCHAR
BEGIN
    DECLARE r VARCHAR;
    CASE n
        WHEN 1 THEN SET r = 'one';
        WHEN 2 THEN SET r = 'two';
        ELSE SET r = 'other';
    END CASE;
    RETURN r;
END;

SELECT case_body_simple(1);
SELECT case_body_simple(2);
SELECT case_body_simple(99);

-- ============================================================
-- Part D: SIGNAL SQLSTATE '...' SET MESSAGE_TEXT = '...'
-- ============================================================
CREATE FUNCTION divide(a INT, b INT) RETURNS INT
BEGIN
    IF b = 0 THEN
        SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'divide by zero';
    END IF;
    RETURN a / b;
END;

SELECT divide(10, 2);

-- ============================================================
-- Part E: DECLARE CONTINUE HANDLER FOR SQLEXCEPTION
-- ============================================================
CREATE FUNCTION safe_divide(a INT, b INT) RETURNS INT
BEGIN
    DECLARE r INT DEFAULT 0;
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET r = -1;
    IF b = 0 THEN
        SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'div0';
    END IF;
    SET r = a / b;
    RETURN r;
END;

SELECT safe_divide(10, 2);
SELECT safe_divide(10, 0);

-- 按特定 SQLSTATE 匹配的 HANDLER
CREATE FUNCTION safe_divide_typed(a INT, b INT) RETURNS INT
BEGIN
    DECLARE r INT DEFAULT 0;
    DECLARE CONTINUE HANDLER FOR SQLSTATE '22012' SET r = -42;
    DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET r = -1;
    IF b = 0 THEN
        SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'div0';
    END IF;
    SET r = a / b;
    RETURN r;
END;

SELECT safe_divide_typed(10, 2);
SELECT safe_divide_typed(10, 0);

-- ============================================================
-- Part F: CURSOR / OPEN / FETCH / CLOSE
-- ============================================================
CREATE TABLE cur_src (id INT, val INT);
INSERT INTO cur_src VALUES (1, 10), (2, 20), (3, 30);

CREATE FUNCTION cursor_sum() RETURNS INT
BEGIN
    DECLARE total INT DEFAULT 0;
    DECLARE v INT;
    DECLARE CURSOR cur FOR SELECT val FROM cur_src ORDER BY id;
    OPEN cur;
    FETCH cur INTO v;
    WHILE v IS NOT NULL DO
        SET total = total + v;
        FETCH cur INTO v;
    END WHILE;
    CLOSE cur;
    RETURN total;
END;

SELECT cursor_sum();

-- ============================================================
-- Part G: PROCEDURE / CALL（综合）
-- ============================================================
-- procedure 通过 INSERT 持有表返回结果
CREATE PROCEDURE compute(IN a INT, IN b INT)
BEGIN
    DECLARE s INT;
    SET s = a + b;
    INSERT INTO _proc_results VALUES ('sum', s);
    SET s = a * b;
    INSERT INTO _proc_results VALUES ('prod', s);
END;

CALL compute(3, 4);
SELECT * FROM _proc_results WHERE tag IN ('sum', 'prod') ORDER BY tag;

-- 清理
DROP PROCEDURE compute;
DROP FUNCTION cursor_sum;
DROP FUNCTION safe_divide_typed;
DROP FUNCTION safe_divide;
DROP FUNCTION divide;
DROP FUNCTION case_body_simple;
DROP FUNCTION case_body;
DROP FUNCTION repeat_test;
DROP PROCEDURE loop_sum;
DROP TABLE _proc_results;
DROP TABLE cur_src;

exit;
