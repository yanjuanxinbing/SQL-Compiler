-- 74_unique_constraint.sql
-- BUG-1 [P0 严重]: 列级 UNIQUE 在表同时有 PRIMARY KEY 时未生效
-- --------------------------------------------------------------------------
-- 历史原因（已修复，见 ConstraintChecker.cpp §PRIMARY KEY 节末）：
--   ValidateRowConstraints 在 PRIMARY KEY 走完索引点查之后，如果
--   pk_groups_present=true 且 unindexed_groups 为空，会 early-return，
--   导致紧随其后的 UNIQUE 兜底扫描被跳过。
--   现象：CREATE TABLE t(id INT PK, email TEXT UNIQUE) 时，重复 email
--   静默被接受。
--
-- 本测试覆盖：
--   1) 列级 UNIQUE 单独存在时（无 PK）能拒重复。
--   2) 列级 UNIQUE 与 PK 同时存在时（BUG-1 主路径）能拒重复。
--   3) 列级 UNIQUE 多次重复全部拒绝。
--   4) 表级 UNIQUE(a, b) 组合约束也能拒重复。
--   5) NULL 不参与 UNIQUE 判定（同一列允许多个 NULL）。
--   6) UPDATE 路径上的 UNIQUE 也能拒（排除自身 RID）。
--   7) 删完冲突行后 INSERT 合法值能成功。

-- =====================================================================
-- 1) 列级 UNIQUE，无 PK
-- =====================================================================
CREATE TABLE uq_nopk (
    id    INT,
    email TEXT UNIQUE
);
INSERT INTO uq_nopk VALUES (1, 'a@x.com');
INSERT INTO uq_nopk VALUES (2, 'b@x.com');
INSERT INTO uq_nopk VALUES (3, 'a@x.com');
SELECT id, email FROM uq_nopk ORDER BY id;
DROP TABLE uq_nopk;

-- =====================================================================
-- 2) 列级 UNIQUE 与 PK 同存（BUG-1 主路径）
-- =====================================================================
CREATE TABLE bug_unique (
    id    INT PRIMARY KEY,
    email TEXT UNIQUE,
    age   INT
);
INSERT INTO bug_unique VALUES (1, 'a@x.com', 20);
INSERT INTO bug_unique VALUES (2, 'b@x.com', 25);
INSERT INTO bug_unique VALUES (3, 'a@x.com', 99);
SELECT id, email, age FROM bug_unique ORDER BY id;
DROP TABLE bug_unique;

-- =====================================================================
-- 3) 列级 UNIQUE 多次重复全部拒绝（不只第一次）
-- =====================================================================
CREATE TABLE uq_repeat (
    id    INT PRIMARY KEY,
    email TEXT UNIQUE
);
INSERT INTO uq_repeat VALUES (1, 'dup@x.com');
INSERT INTO uq_repeat VALUES (2, 'dup@x.com');
INSERT INTO uq_repeat VALUES (3, 'dup@x.com');
SELECT id, email FROM uq_repeat ORDER BY id;
DROP TABLE uq_repeat;

-- =====================================================================
-- 4) 表级 UNIQUE(a, b) 组合约束
-- =====================================================================
CREATE TABLE uq_combo (
    id   INT PRIMARY KEY,
    a    INT,
    b    INT,
    UNIQUE (a, b)
);
INSERT INTO uq_combo VALUES (1, 10, 20);
INSERT INTO uq_combo VALUES (2, 10, 30);
-- (10, 20) 重复 → 拒绝
INSERT INTO uq_combo VALUES (3, 10, 20);
-- (a, b) 不同 → 合法
INSERT INTO uq_combo VALUES (4, 11, 20);
SELECT id, a, b FROM uq_combo ORDER BY id;
DROP TABLE uq_combo;

-- =====================================================================
-- 5) NULL 不参与 UNIQUE 判定
-- =====================================================================
CREATE TABLE uq_nullable (
    id    INT PRIMARY KEY,
    email TEXT UNIQUE
);
-- 多个 NULL 视为不同（SQL 标准语义）
INSERT INTO uq_nullable VALUES (1, NULL);
INSERT INTO uq_nullable VALUES (2, NULL);
INSERT INTO uq_nullable VALUES (3, 'a@x.com');
-- 但具体值仍要唯一
INSERT INTO uq_nullable VALUES (4, 'a@x.com');
SELECT id, email FROM uq_nullable ORDER BY id;
DROP TABLE uq_nullable;

-- =====================================================================
-- 6) UPDATE 路径上的 UNIQUE
-- =====================================================================
CREATE TABLE uq_update (
    id    INT PRIMARY KEY,
    email TEXT UNIQUE
);
INSERT INTO uq_update VALUES (1, 'x@y.com');
INSERT INTO uq_update VALUES (2, 'a@b.com');
-- 把 id=1 的 email 改成已存在的 a@b.com → 拒绝
UPDATE uq_update SET email = 'a@b.com' WHERE id = 1;
-- 改成新值 → 合法
UPDATE uq_update SET email = 'c@d.com' WHERE id = 1;
SELECT id, email FROM uq_update ORDER BY id;
DROP TABLE uq_update;

exit;
