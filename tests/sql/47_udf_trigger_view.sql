-- 47_udf_trigger_view.sql
-- Part A / B / C：UDF 真正执行、BEFORE INSERT 触发器改写 NEW、视图可查询。

-- 简单 UDF：RETURN expr; 形态
CREATE FUNCTION add_one(x INT) RETURNS INT
BEGIN
    RETURN x + 1;
END;

SELECT add_one(5);

-- DECLARE / SET / RETURN 形态
CREATE FUNCTION double_it(x INT) RETURNS INT
BEGIN
    DECLARE y INT;
    SET y = x * 2;
    RETURN y;
END;

SELECT double_it(7);

-- IF / ELSEIF / ELSE / RETURN 形态（ELSEIF 是单关键字）
CREATE FUNCTION classify(n INT) RETURNS VARCHAR
BEGIN
    IF n > 0 THEN RETURN 'positive';
    ELSEIF n = 0 THEN RETURN 'zero';
    ELSE RETURN 'negative';
    END IF;
END;

SELECT classify(5);
SELECT classify(0);
SELECT classify(-3);

-- Part B：BEFORE INSERT 触发器改写 NEW 行
CREATE TABLE acct(id INT PRIMARY KEY, bal INT);

CREATE TRIGGER trg_acct
BEFORE INSERT ON acct
FOR EACH ROW
SET NEW.bal = NEW.bal + 1;

INSERT INTO acct VALUES (1, 100);
SELECT * FROM acct;

-- Part C：视图查询
CREATE VIEW v_acct AS SELECT id, bal FROM acct WHERE bal > 50;
SELECT * FROM v_acct;

DROP VIEW v_acct;
DROP TRIGGER trg_acct;
DROP FUNCTION add_one;
DROP FUNCTION double_it;
DROP FUNCTION classify;

exit;