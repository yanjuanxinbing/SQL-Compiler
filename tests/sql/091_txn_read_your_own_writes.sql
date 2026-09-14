-- 85_txn_read_your_own_writes.sql
-- BUG-7 [P0 严重]: UPDATE inside a transaction does not change subsequent SELECTs
-- (read-your-own-writes broken when FK references exist).
--
-- 历史原因（已修复，见 UpdateExecutor.cpp + ConstraintChecker.cpp）：
--   UpdateExecutor::Next() 无条件调用 EnforceParentForeignKeys(cur.GetValues())，
--   即对每一次 UPDATE 都视同「删除 OLD 父行 + 插入 NEW 父行」。当 UPDATE 仅修改
--   非 FK 引用列（典型例子：仅改 credit 等非 PK 列）时，parent PK 未变，所有引用
--   该 PK 的子行（q.pid = 1 等）依然合法，无需任何 FK 处理。但旧实现会把旧行
--   当作「待删除」处理，发现有子行引用旧 PK 时抛
--   "foreign key violation: cannot delete parent row ..."，导致 UPDATE 失败。
--   即使在显式 BEGIN 包裹的事务里，失败的 UPDATE 也让 UPDATE 后的 SELECT 看不到
--   任何变化——因为 UPDATE 实际未生效，SELECT 读到的是已提交版本。
--
-- 修复要点：
--   - EnforceParentForeignKeys 增加 modified_parent_cols 参数；DELETE 调用路径
--     不传（即视为「全部 parent_cols 被移除」，与旧行为兼容）。
--   - UPDATE 调用路径收集 assignments_ 涉及到的列名，仅当至少一条 FK 的
--     parent_cols 被本次 UPDATE 实际修改时才触发 FK 强制执行；否则跳过
--     （parent PK 未变，FK 子行无需任何处理）。
--
-- 本测试覆盖：
--   1) 主复现：FK + 引用行 + UPDATE 非 PK 列 → 同事务内 SELECT 应看到新值。
--   2) 诊断变体 1：无 FK / q 表，UPDATE 非 PK 列 → 同事务 SELECT 应看到新值。
--   3) 诊断变体 2：COMMIT 后 SELECT（在连接内 RUN 不另起连接）→ 应看到
--      新值并持久。
--   4) 诊断变体 3：仅更新第 2 行 → 同事务 SELECT 应仅看到第 2 行变化。
--   5) 诊断变体 4：UPDATE 用 self-reference + 同事务 SELECT。
--   6) 诊断变体 5：ROLLBACK 后 SELECT → 应回到原始值（rollback 撤销 UPDATE）。
--   7) 兼容性 a：当 UPDATE 真正修改 parent PK 列时，RESTRICT FK 仍然拒绝
--      （用 stored procedure + EXIT HANDLER 捕获以避免污染 REPL 输出）。
--   8) 兼容性 b：ON DELETE CASCADE 仍能正确级联（与 bug 7 无关，回归验证）。

-- =====================================================================
-- 1) 主复现：FK + 引用行 + UPDATE 非 PK 列（read-your-own-writes）
-- =====================================================================
CREATE TABLE p(id INT PRIMARY KEY, country TEXT, credit REAL);
CREATE TABLE q(id INT PRIMARY KEY, pid INT REFERENCES p(id));
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000);
INSERT INTO q VALUES (10,1);
BEGIN;
UPDATE p SET credit = credit + 100 WHERE country='USA';
SELECT * FROM p WHERE country='USA' ORDER BY id;
ROLLBACK;

-- =====================================================================
-- 2) 诊断变体 1：无 FK / 无 q 表
-- =====================================================================
DROP TABLE q;
DROP TABLE p;
CREATE TABLE p(id INT PRIMARY KEY, country TEXT, credit REAL);
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000);
BEGIN;
UPDATE p SET credit = credit + 100 WHERE country='USA';
SELECT * FROM p WHERE country='USA' ORDER BY id;
ROLLBACK;

-- =====================================================================
-- 3) 诊断变体 2：COMMIT 后 SELECT（在同一连接内 RUN，不另起连接）
-- =====================================================================
DROP TABLE p;
CREATE TABLE p(id INT PRIMARY KEY, country TEXT, credit REAL);
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000);
BEGIN;
UPDATE p SET credit = credit + 100 WHERE country='USA';
COMMIT;
SELECT * FROM p ORDER BY id;

-- =====================================================================
-- 4) 诊断变体 3：仅更新第 2 行（不影响第 1 行）
-- =====================================================================
DROP TABLE p;
CREATE TABLE p(id INT PRIMARY KEY, country TEXT, credit REAL);
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000);
BEGIN;
UPDATE p SET credit = credit + 9999 WHERE id = 2;
SELECT * FROM p ORDER BY id;
ROLLBACK;
SELECT * FROM p ORDER BY id;

-- =====================================================================
-- 5) 诊断变体 4：UPDATE 用 self-reference + 同事务 SELECT
-- =====================================================================
DROP TABLE p;
CREATE TABLE p(id INT PRIMARY KEY, country TEXT, credit REAL);
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000);
BEGIN;
UPDATE p SET credit = credit * 2;
SELECT * FROM p ORDER BY id;
ROLLBACK;

-- =====================================================================
-- 6) 诊断变体 5：ROLLBACK 后 SELECT → 应回到原始值
-- =====================================================================
DROP TABLE p;
CREATE TABLE p(id INT PRIMARY KEY, country TEXT, credit REAL);
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000);
BEGIN;
UPDATE p SET credit = credit + 100 WHERE country='USA';
ROLLBACK;
SELECT * FROM p ORDER BY id;

-- =====================================================================
-- 7) 兼容性 a：当 UPDATE 真正修改 parent PK 列时，RESTRICT FK 仍然拒绝。
--    用 stored procedure + EXIT HANDLER 捕获异常，避免污染 REPL 输出。
--    验证点：
--      - 'fk_blocked' 应出现在 _fk_probe（说明 handler 被触发，即 FK 拒绝）。
--      - 'after_update' 不应出现（说明 procedure 在 UPDATE 失败后被 EXIT
--        handler 终止，没有执行 UPDATE 之后的 INSERT）。
--      - pr.id 仍为 1，cr.pid 仍为 1（说明 UPDATE 实际未生效）。
--      - 注：'pk_changed' 出现在 _fk_probe 是预期行为——它在 procedure 起始
--        处的 INSERT 就已经写入，与 UPDATE 是否成功无关。
-- =====================================================================
DROP TABLE p;
CREATE TABLE _fk_probe (tag VARCHAR);
INSERT INTO _fk_probe VALUES ('before');

CREATE TABLE pr(id INT PRIMARY KEY, name VARCHAR);
CREATE TABLE cr(id INT PRIMARY KEY, pid INT REFERENCES pr(id));
INSERT INTO pr VALUES (1,'a');
INSERT INTO cr VALUES (10,1);

CREATE PROCEDURE try_change_pk()
BEGIN
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
        INSERT INTO _fk_probe VALUES ('fk_blocked');
    INSERT INTO _fk_probe VALUES ('pk_changed');
    UPDATE pr SET id = 99 WHERE id = 1;
    INSERT INTO _fk_probe VALUES ('after_update');
END;

CALL try_change_pk();
SELECT * FROM _fk_probe ORDER BY tag;
SELECT * FROM pr ORDER BY id;
SELECT * FROM cr ORDER BY id;

DELETE FROM _fk_probe;
DROP PROCEDURE try_change_pk;
DROP TABLE cr;
DROP TABLE pr;

-- =====================================================================
-- 8) 兼容性 b：ON DELETE CASCADE 仍能正确级联（回归验证）
-- =====================================================================
CREATE TABLE pc2(id INT PRIMARY KEY, name VARCHAR);
CREATE TABLE cc2(id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES pc2(id) ON DELETE CASCADE);
INSERT INTO pc2 VALUES (1,'a'),(2,'b');
INSERT INTO cc2 VALUES (10,1),(20,2);
DELETE FROM pc2 WHERE id = 1;
SELECT * FROM pc2 ORDER BY id;
SELECT * FROM cc2 ORDER BY id;

DROP TABLE cc2;
DROP TABLE pc2;
DROP TABLE _fk_probe;

exit;
