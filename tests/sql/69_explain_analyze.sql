-- 69_explain_analyze.sql
-- EXPLAIN ANALYZE 行为测试。
--
-- 覆盖范围：
--   1) 基本 SeqScan：EXPLAIN ANALYZE SELECT * 应输出含 rows / time 的注释。
--   2) 带 WHERE：Filter 节点（被优化器下推到 SeqScan 时显示为 SeqScan 的
--      predicate 字段）也应有 rows / time 注释。
--   3) FORMAT JSON：每节点带 "rows" 与 "time_ms" 字段。
--   4) FORMAT SEXPR：每节点带 :rows / :time_ms 键。
--   5) 执行失败：SELECT 1/0 应保留 partial stats 并把错误抛出。

CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR, score INT);
INSERT INTO t VALUES (1, 'a', 10), (2, 'b', 20), (3, 'c', 30), (4, 'd', 40);

-- 1) 基本 SeqScan：rows 应等于表行数（4），time_ms 应为正数。
EXPLAIN ANALYZE SELECT * FROM t;

-- 2) 带 WHERE：rows 应为匹配的行数（3：score > 10 的有 20/30/40）。
EXPLAIN ANALYZE SELECT * FROM t WHERE score > 10;

-- 3) FORMAT JSON：输出含 rows / time_ms 字段的合法 JSON。
EXPLAIN ANALYZE FORMAT JSON SELECT * FROM t WHERE score > 10;

-- 4) FORMAT SEXPR：每个 S-表达式末尾追加 :rows "N" :time_ms "X"。
EXPLAIN ANALYZE FORMAT SEXPR SELECT * FROM t WHERE score > 10;

-- 5) 执行失败：UPDATE 把主键列改成 NULL 触发 NOT NULL 约束违规。
--    partial stats 应保留（Update 节点的 Init/Next 时长），错误信息
--    作为 [error] 行追加到 plan 输出末尾，不被吞掉。
EXPLAIN ANALYZE UPDATE t SET id = NULL WHERE id = 1;

exit;
