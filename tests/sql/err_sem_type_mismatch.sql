-- @expect_error: [Semantic] value too long for column 'a'
-- @category: Semantic
--
-- 语义错误: 插入值与列类型不匹配(用 VARCHAR 长度超限来暴露类型错误)
-- 列 a 是 VARCHAR(5), 但试图写入 20 字符的字符串。
-- ConstraintChecker 在 INSERT 阶段做长度校验并抛 CompilerException。
-- 期望 .out 中出现: Error: [Semantic] value too long for column 'a' (VARCHAR(5)): got 20 characters
--
-- 注: 本仓库 SQL 层对基本类型(INT/FLOAT/VARCHAR)做隐式 CoerceToColumnType,
--     因此纯数值/字符串类型不匹配不会显式报错; 此处改用 VARCHAR 长度上限
--     来覆盖「类型/取值不匹配」这一类语义错误。

CREATE TABLE t(a VARCHAR(5));
INSERT INTO t VALUES ('abcdefghijklmnopqrst');
