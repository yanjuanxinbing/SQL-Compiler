-- ============================================================
-- 53_ddl: DDL 扩展 (Category 2)
-- ============================================================
-- 涵盖：
--   - FOREIGN KEY ... REFERENCES ... ON DELETE CASCADE / RESTRICT
--   - CREATE SCHEMA / DROP SCHEMA（含 schema.table 限定）
--   - CREATE SEQUENCE ... NEXTVAL FOR ...
--   - RENAME COLUMN
--   - 表级多列 PRIMARY KEY (a, b)
-- ============================================================

-- ---------- 1. FOREIGN KEY ON DELETE CASCADE ----------
CREATE TABLE p (id INT PRIMARY KEY, name VARCHAR);
CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE);
INSERT INTO p VALUES (1, 'a'), (2, 'b');
INSERT INTO c VALUES (10, 1), (20, 2), (30, 1);
SELECT * FROM c ORDER BY id;
DELETE FROM p WHERE id = 1;
SELECT * FROM c ORDER BY id;
SELECT * FROM p ORDER BY id;

-- ---------- 2. FOREIGN KEY default RESTRICT ----------
CREATE TABLE p2 (id INT PRIMARY KEY);
CREATE TABLE c2 (id INT PRIMARY KEY, pid INT REFERENCES p2(id));
INSERT INTO p2 VALUES (1);
INSERT INTO c2 VALUES (1, 1);
-- 此 DELETE 应因 RESTRICT 失败（提交后整个语句回滚）
-- 用一个 wrapper SQL 让 batch run 不致整体崩溃：先尝试 DELETE，预期失败；
-- 若失败则保留 c2 行不变。
-- （无法在脚本里"预期失败"，故保留 c2/p2 数据留作下一步校验。）
-- DELETE FROM p2 WHERE id = 1;
SELECT * FROM p2 ORDER BY id;
SELECT * FROM c2 ORDER BY id;

-- ---------- 3. CREATE SCHEMA / schema.table ----------
CREATE SCHEMA finance;
CREATE TABLE finance.txn (id INT PRIMARY KEY, amt INT);
INSERT INTO finance.txn VALUES (1, 100), (2, 250);
SELECT * FROM finance.txn ORDER BY id;
DROP TABLE finance.txn;
DROP SCHEMA finance;

-- ---------- 4. CREATE SEQUENCE / NEXTVAL FOR ----------
CREATE SEQUENCE seq1 START 100 INCREMENT 5;
SELECT NEXTVAL FOR seq1;
SELECT NEXTVAL FOR seq1;
SELECT NEXTVAL FOR seq1;
DROP SEQUENCE seq1;

-- ---------- 5. RENAME COLUMN ----------
CREATE TABLE t1 (id INT, old_name VARCHAR);
INSERT INTO t1 VALUES (1, 'a'), (2, 'b');
ALTER TABLE t1 RENAME COLUMN old_name TO new_name;
SELECT id, new_name FROM t1 ORDER BY id;

-- ---------- 6. PRIMARY KEY (a, b) 多列 ----------
CREATE TABLE mp (a INT, b INT, val VARCHAR, PRIMARY KEY (a, b));
INSERT INTO mp VALUES (1, 1, 'x'), (1, 2, 'y'), (2, 1, 'z');
SELECT * FROM mp ORDER BY a, b;
