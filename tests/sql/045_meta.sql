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
-- 注：EXPLAIN ANALYZE 行为由 69_explain_analyze.sql 专项覆盖；本测试仅
-- 确认 ANALYZE 子句不破坏主元命令测试上下文（不会抛错、不会崩溃）。

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

-- EXPLAIN ANALYZE：实际驱动并打印带统计注释的计划树。
EXPLAIN ANALYZE SELECT * FROM t;

-- ============ 谓词下推演示 ============
-- 64_pushdown.sql 对应的 EXPLAIN 形态参考。
-- name 列无索引 → PushDownPredicates 会把 Filter(name='x') 吸收进 SeqScan.predicate，
-- EXPLAIN 输出 SeqScan(t, [(name = 'x')]) 而不是 Filter -> SeqScan。
EXPLAIN SELECT * FROM t WHERE name = 'x';

-- score 列有索引 → ChooseAccessPaths 先把它改写成 IndexScan，
-- PushDownPredicates 不再处理该路径。
EXPLAIN SELECT * FROM t WHERE score > 50;

-- 混合：score 走索引，name 作为 residual predicate 留在 IndexScan 上。
-- （注：当前 IndexScanNode::ToString 不打印 residual_predicate，
--  所以 EXPLAIN 输出与单纯 score > 50 看起来一样，但语义相同。）
EXPLAIN SELECT * FROM t WHERE name = 'x' AND score > 50;

-- JOIN 两侧都有可下推的单表谓词：拆开后两侧各下沉一份。
CREATE TABLE u (uid INT, uname VARCHAR);
INSERT INTO u VALUES (1, 'a'), (2, 'b');
EXPLAIN SELECT t.id, u.uname FROM t JOIN u ON t.id = u.uid
  WHERE t.name = 'a' AND u.uname = 'a';

-- ============ EXPLAIN FORMAT JSON / SEXPR ============
-- 结构化输出：JSON 是合法 JSON 对象（含 type/fields/children 三段），
-- SEXPR 是 Lisp 风格 (Name :key "val" ... child ...)。
EXPLAIN FORMAT JSON SELECT * FROM t WHERE name = 'x';
EXPLAIN FORMAT SEXPR SELECT * FROM t WHERE name = 'x';
EXPLAIN FORMAT JSON SELECT * FROM t WHERE score > 50;

exit;
