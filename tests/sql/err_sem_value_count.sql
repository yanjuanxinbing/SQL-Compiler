-- @expect_error: semantic error: INSERT column count mismatch
-- @category: Semantic
--
-- 语义错误: INSERT 值的数量与列数不一致
-- 表 t 有两列 (a, b), 但 VALUES 只给了 1 个值。
-- SemanticAnalyzer::AnalyzeInsert 在 SemanticAnalyzer.cpp:438 报 column count mismatch。
-- 期望 .out 中出现: Error: semantic error: INSERT column count mismatch: got 1, expected 2

CREATE TABLE t(a INT, b INT);
INSERT INTO t(a, b) VALUES (1);
