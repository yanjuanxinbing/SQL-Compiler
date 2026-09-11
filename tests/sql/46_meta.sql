-- 46_meta.sql
-- 元命令测试：EXPLAIN / SHOW TABLES / SHOW COLUMNS / DESCRIBE / SHOW INDEX /
-- SHOW CREATE TABLE。
--
-- 覆盖范围：
--   - EXPLAIN SELECT 应打印出含 Project / Filter / 扫描节点的计划树。
--   - SHOW TABLES 列出所有用户表。
--   - SHOW COLUMNS FROM t 与 DESCRIBE t / DESC t 等价。
--   - SHOW INDEX FROM t 同时包含主键索引和二级索引。
--   - SHOW CREATE TABLE t 重建 CREATE TABLE 文本。
--
-- 注：EXPLAIN ANALYZE 在本实现中仅打印 "EXPLAIN ANALYZE not supported"
-- 占位提示（任务文档允许的 scope-cut）。

CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR, score INT);
CREATE INDEX idx_score ON t(score);

-- EXPLAIN SELECT：项目文档要求至少出现 Project + Filter；底层是 SeqScan
-- （无二级索引谓词命中条件下，optimizer 选择 SeqScan）。
EXPLAIN SELECT * FROM t WHERE score > 50;

-- SHOW TABLES：应列出 t。
SHOW TABLES;

-- SHOW COLUMNS FROM t：列出 3 列。
SHOW COLUMNS FROM t;
-- DESCRIBE / DESC 是 SHOW COLUMNS 的 MySQL 别名。
DESCRIBE t;
DESC t;

-- SHOW INDEX FROM t：包含主键索引 + idx_score。
SHOW INDEX FROM t;

-- SHOW CREATE TABLE t：复现 CREATE TABLE 文本。
SHOW CREATE TABLE t;

-- EXPLAIN ANALYZE：scope-cut 占位。
EXPLAIN ANALYZE SELECT * FROM t;

exit;
