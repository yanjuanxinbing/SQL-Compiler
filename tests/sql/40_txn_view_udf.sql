-- 40_txn_view_udf.sql
-- 元功能扩展：事务 / 视图 / 触发器 / 用户自定义函数

CREATE TABLE acct(id INT PRIMARY KEY, bal INT);
INSERT INTO acct VALUES (1, 100), (2, 50);

BEGIN;
UPDATE acct SET bal = bal - 10 WHERE id = 1;
COMMIT;

ROLLBACK;

SAVEPOINT sp1;
RELEASE SAVEPOINT sp1;

CREATE VIEW v_acct AS SELECT id, bal FROM acct WHERE bal > 0;

SELECT * FROM v_acct;

DROP VIEW v_acct;

CREATE TRIGGER trg_acct
BEFORE INSERT ON acct
FOR EACH ROW
SET NEW.bal = NEW.bal + 1;

DROP TRIGGER trg_acct;

CREATE FUNCTION add_one(x INT) RETURNS INT
BEGIN
    RETURN x + 1;
END;

SELECT add_one(5);

DROP FUNCTION add_one;

SELECT * FROM acct ORDER BY id;

exit;
