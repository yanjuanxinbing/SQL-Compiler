-- @expect_error: semantic error: column not found: wrong_col
-- @category: Semantic
--
-- 语义错误: 列名拼写错误
-- 表 t 的列是 a; 查询写成 wrong_col, SemanticAnalyzer 报 column not found。
-- 期望 .out 中出现: Error: semantic error: column not found: wrong_col
--
-- 注: 该错误的阶段标识是 Database.cpp 在收集 SemanticError 后拼出的
--     `semantic error: ...`(小写), 而非 [Semantic] 前缀。

CREATE TABLE t(a INT);
INSERT INTO t VALUES (1);
SELECT wrong_col FROM t;
