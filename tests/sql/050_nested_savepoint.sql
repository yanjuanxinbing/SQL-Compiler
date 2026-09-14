-- 51_nested_savepoint.sql
-- Phase D：嵌套 SAVEPOINT 的三种语义场景。
--
-- 场景 1：ROLLBACK TO 内层 savepoint —— 只撤销内层 savepoint 之后的写。
--          BEGIN; INSERT(1); SAVEPOINT s1; INSERT(2); SAVEPOINT s2;
--          INSERT(3); ROLLBACK TO s2; COMMIT;
--          期望 (1), (2) —— (3) 被撤销。
--
-- 场景 2：RELEASE SAVEPOINT —— 仅移除保存点标记，不撤销写入。
--          BEGIN; SAVEPOINT s1; INSERT(10); SAVEPOINT s2; INSERT(11);
--          RELEASE SAVEPOINT s2; INSERT(12); COMMIT;
--          期望 (1), (2), (10), (11), (12) 共 5 行。
--
-- 场景 3：ROLLBACK TO 外层 savepoint —— 撤销外层 savepoint 之后的所有写
--         （包括内层 savepoint 区间内的写）。
--          BEGIN; SAVEPOINT s1; INSERT(20); SAVEPOINT s2; INSERT(21);
--          ROLLBACK TO s1; INSERT(22); COMMIT;
--          期望只有 (22) —— (20), (21) 都被撤销。

CREATE TABLE acct(id INT PRIMARY KEY, bal INT);

-- 1) ROLLBACK TO 内层 savepoint
BEGIN;
INSERT INTO acct VALUES (1, 100);

SAVEPOINT s1;
INSERT INTO acct VALUES (2, 200);

SAVEPOINT s2;
INSERT INTO acct VALUES (3, 300);

ROLLBACK TO s2;
SELECT * FROM acct ORDER BY id;

COMMIT;
SELECT * FROM acct ORDER BY id;

-- 2) RELEASE SAVEPOINT 仅移除标记，保留写入
BEGIN;
SAVEPOINT s1;
INSERT INTO acct VALUES (10, 1000);

SAVEPOINT s2;
INSERT INTO acct VALUES (11, 1100);

RELEASE SAVEPOINT s2;
INSERT INTO acct VALUES (12, 1200);

COMMIT;
SELECT count(*) AS cnt FROM acct;

-- 3) ROLLBACK TO 外层 savepoint 撤销所有之后写入
BEGIN;
SAVEPOINT s1;
INSERT INTO acct VALUES (20, 2000);

SAVEPOINT s2;
INSERT INTO acct VALUES (21, 2100);

ROLLBACK TO s1;
INSERT INTO acct VALUES (22, 2200);

COMMIT;
SELECT * FROM acct WHERE id >= 20 ORDER BY id;

exit;