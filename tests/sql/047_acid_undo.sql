-- 48_acid_undo.sql
-- Phase A：事务的 undo 语义 + 隐式 auto-commit。
--
-- 覆盖 5 个场景：
--   1) BEGIN; UPDATE; COMMIT;   —— 改动保留
--   2) BEGIN; UPDATE; ROLLBACK; —— 改动撤销
--   3) BEGIN; INSERT; SAVEPOINT sp; INSERT; ROLLBACK TO sp; COMMIT;
--                              —— sp 之后的 INSERT 撤销，sp 之前的保留
--   4) standalone UPDATE       —— 隐式 auto-commit，改动保留
--   5) standalone ROLLBACK / SAVEPOINT / RELEASE SAVEPOINT
--                              —— 静默 no-op，不报错

CREATE TABLE acct(id INT PRIMARY KEY, bal INT);
INSERT INTO acct VALUES (1, 100), (2, 50);

-- 1) COMMIT keeps
BEGIN;
UPDATE acct SET bal = bal - 10 WHERE id = 1;
COMMIT;

SELECT * FROM acct WHERE id = 1 ORDER BY id;

-- 2) ROLLBACK reverts
BEGIN;
UPDATE acct SET bal = bal + 100 WHERE id = 2;
ROLLBACK;

SELECT * FROM acct WHERE id = 2;

-- 3) ROLLBACK TO undoes only post-savepoint writes
BEGIN;
INSERT INTO acct VALUES (3, 30);
SAVEPOINT sp;
INSERT INTO acct VALUES (4, 40);
ROLLBACK TO sp;
COMMIT;

SELECT * FROM acct ORDER BY id;

-- 4) implicit auto-commit (no BEGIN)
UPDATE acct SET bal = bal + 1 WHERE id = 1;

SELECT * FROM acct WHERE id = 1;

-- 5) silent no-ops outside any txn
ROLLBACK;
SAVEPOINT anyname;
RELEASE SAVEPOINT anyname;

exit;
