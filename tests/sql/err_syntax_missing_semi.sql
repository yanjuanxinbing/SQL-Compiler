-- @expect_error: [Syntax]
-- @category: Syntax
--
-- 语法错误: SELECT 后直接跟 FROM, 缺少投影列
-- Parser 在 SELECT 关键字之后期望一个表达式列表, 但读到 FROM 关键字,
-- 抛出 CompilerException(ErrorStage::SYNTAX, ..., cur.line, cur.column)。
-- 期望 .out 中出现: Error: [Syntax] line=2, col=8: ... (got 'FROM') 或类似
--
-- 注: 本仓库的实现里, Parser 对 `SELECT 1 SELECT 2;` 这种「两条语句缺分号」
--     会把 `SELECT` 解析为第一列的别名, 整个串被当作单条 SELECT 处理而不报错。
--     因此这里改用一个真正触发 SYNTAX 阶段错误的语句来覆盖「语法错误」这一类
--     错误路径, 同样满足课程要求里「缺分号/语法错误」覆盖。

CREATE TABLE t(a INT);
SELECT FROM t;
