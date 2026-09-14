-- 42_check_default.sql
-- 验证 CHECK 与 DEFAULT 在 INSERT/UPDATE 路径上的执行期语义：
--   - CHECK (expr) 失败时抛 "check constraint violated: <table>.<col> (<expr>)"
--   - DEFAULT 仅对「INSERT 未列出该列」生效；显式 NULL 不被覆盖
--   - SQL 标准：CHECK 在三值逻辑下 NULL 不视为违反约束

CREATE TABLE t(
    id INT PRIMARY KEY,
    score INT CHECK (score >= 0 AND score <= 100),
    status VARCHAR DEFAULT 'active'
);

-- 1) DEFAULT 触发：列出 (id, score)，status 走默认
INSERT INTO t(id, score) VALUES (1, 50);
SELECT * FROM t WHERE id = 1;

-- 2) CHECK 拒绝：score 超界
INSERT INTO t(id, score) VALUES (2, 150);

-- 3) CHECK 拒绝：score 负值
INSERT INTO t VALUES (3, -5, 'bad');

-- 4) UPDATE 路径同样校验
UPDATE t SET score = 200 WHERE id = 1;

-- 5) 显式 NULL 保留：用户列出了 status 并给 NULL
INSERT INTO t(id, score, status) VALUES (4, 80, NULL);
SELECT id, score, status FROM t WHERE id IN (1, 4) ORDER BY id;

-- 6) CHECK 与 NULL：score = NULL 不视为违反 CHECK（SQL 标准）
INSERT INTO t(id, score) VALUES (5, NULL);
SELECT id, score, status FROM t WHERE id = 5;

exit;
