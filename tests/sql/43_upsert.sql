-- 43_upsert.sql
-- 验证 MySQL 风格 ON DUPLICATE KEY UPDATE：
--   - 主键冲突时按 assignments 改写已有行（而非报错）；
--   - VALUES(col) 引用本次候选行，而不是已有行；
--   - 改写后必须再次通过 CHECK / NOT NULL 约束；
--   - 未冲突时退化为常规 INSERT。
--
-- 当前实现仅覆盖 PRIMARY KEY 冲突路径；UNIQUE INDEX（非主键）的
-- upsert 改写尚未接入（详见 include/execution/UpsertExecutor.h）。

CREATE TABLE t(
    id INT PRIMARY KEY,
    name VARCHAR,
    score INT CHECK (score >= 0 AND score <= 100)
);

-- 初始两行
INSERT INTO t VALUES (1, 'Alice', 50);
INSERT INTO t VALUES (2, 'Bob',   75);

-- 1) 主键冲突：score 加 10 → Alice 改写为 60
INSERT INTO t VALUES (1, 'Alice', 50) ON DUPLICATE KEY UPDATE score = score + 10;
SELECT * FROM t ORDER BY id;

-- 2) 主键未冲突：插入新行 Carol，score 直接取自 VALUES(80)。
--    VALUES(score) 在无冲突路径上不被求值（仅冲突路径触发 assignments）。
INSERT INTO t VALUES (3, 'Carol', 80) ON DUPLICATE KEY UPDATE score = VALUES(score) + 5;
SELECT * FROM t ORDER BY id;

-- 3) 主键冲突但 score = 200 违反 CHECK：整条 INSERT 回滚（CHECK 抛错）
INSERT INTO t VALUES (1, 'Alice', 200) ON DUPLICATE KEY UPDATE score = VALUES(score);
SELECT * FROM t ORDER BY id;

-- 4) 主键未冲突，CHECK 通过：插入 Dave
INSERT INTO t VALUES (4, 'Dave', 60) ON DUPLICATE KEY UPDATE score = VALUES(score);
SELECT * FROM t ORDER BY id;

exit;