-- 80_tokens_complete.sql
-- 回归：TokenTypeToString 必须为每个 TokenType 枚举成员返回正确的字符串名。
-- 修复前：KeywordTable 已注册 KEYWORD_DESC 等 130+ 关键字，但
-- TokenTypeToString 漏写了大部分 case，未匹配成员回退到 "UNKNOWN"。
-- 结果是 .tokens 把 [KEYWORD_DESC] 印成 [UNKNOWN]，但 lexer 本身正确、
-- parser 也正常解析（ORDER BY ... DESC 排序结果正确），所以语义不受影响，
-- 仅 debug / 报错定位被误导。
-- 修复后：switch 覆盖整个枚举，下面的 \.tokens 不应再出现 "UNKNOWN"。

CREATE TABLE t(
    id INT,
    gpa FLOAT,
    name VARCHAR,
    created_at TIMESTAMP
);

-- DESC 与 ASC：用户报告的原 bug。
SELECT * FROM t ORDER BY gpa DESC;
\.tokens
exit;
