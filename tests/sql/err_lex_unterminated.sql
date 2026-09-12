-- @expect_error: [Lexical] line=1, col=10: unexpected character: '@'
-- @category: Lexical
--
-- 词法错误: 非法字符 '@'
-- Lexer 在识别单字符 operator/symbol 时, '@' 不在已知集合内, 触发
-- `throw CompilerException(ErrorStage::LEXICAL, "unexpected character: '@'", ...)`。
-- 期望 .out 中出现: Error: [Lexical] line=1, col=10: unexpected character: '@'
--
-- 注: 本仓库的 CLI (HasCompleteStatement) 跟踪字符串状态, 未闭合的字符串
--     字面量永远不会触发执行 (EOF 时直接丢弃缓冲), 因此无法通过 stdin 触发
--     "unterminated string literal"。这里用同属 LEXICAL 阶段的 "unexpected
--     character" 错误覆盖词法层面的非法输入检测。

SELECT 1 @ 2;
