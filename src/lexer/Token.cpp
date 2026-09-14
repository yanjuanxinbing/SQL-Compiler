#include "lexer/Token.h"

#include <sstream>

namespace sqlcompiler {

std::string TokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::KEYWORD_SELECT:        return "KEYWORD_SELECT";
        case TokenType::KEYWORD_FROM:          return "KEYWORD_FROM";
        case TokenType::KEYWORD_WHERE:         return "KEYWORD_WHERE";
        case TokenType::KEYWORD_INSERT:        return "KEYWORD_INSERT";
        case TokenType::KEYWORD_INTO:          return "KEYWORD_INTO";
        case TokenType::KEYWORD_VALUES:        return "KEYWORD_VALUES";
        case TokenType::KEYWORD_UPDATE:        return "KEYWORD_UPDATE";
        case TokenType::KEYWORD_SET:           return "KEYWORD_SET";
        case TokenType::KEYWORD_DELETE:        return "KEYWORD_DELETE";
        case TokenType::KEYWORD_CREATE:        return "KEYWORD_CREATE";
        case TokenType::KEYWORD_TABLE:         return "KEYWORD_TABLE";
        case TokenType::KEYWORD_INDEX:         return "KEYWORD_INDEX";
        case TokenType::KEYWORD_UNIQUE:        return "KEYWORD_UNIQUE";
        case TokenType::KEYWORD_DROP:          return "KEYWORD_DROP";
        case TokenType::KEYWORD_AND:           return "KEYWORD_AND";
        case TokenType::KEYWORD_OR:            return "KEYWORD_OR";
        case TokenType::KEYWORD_NOT:           return "KEYWORD_NOT";
        case TokenType::KEYWORD_NULL:          return "KEYWORD_NULL";
        case TokenType::KEYWORD_ORDER:         return "KEYWORD_ORDER";
        case TokenType::KEYWORD_BY:            return "KEYWORD_BY";
        case TokenType::KEYWORD_GROUP:         return "KEYWORD_GROUP";
        case TokenType::KEYWORD_HAVING:        return "KEYWORD_HAVING";
        case TokenType::KEYWORD_JOIN:          return "KEYWORD_JOIN";
        case TokenType::KEYWORD_INNER:         return "KEYWORD_INNER";
        case TokenType::KEYWORD_LEFT:          return "KEYWORD_LEFT";
        case TokenType::KEYWORD_RIGHT:         return "KEYWORD_RIGHT";
        case TokenType::KEYWORD_FULL:          return "KEYWORD_FULL";
        case TokenType::KEYWORD_OUTER:         return "KEYWORD_OUTER";
        case TokenType::KEYWORD_CROSS:         return "KEYWORD_CROSS";
        case TokenType::KEYWORD_NATURAL:       return "KEYWORD_NATURAL";
        case TokenType::KEYWORD_USING:         return "KEYWORD_USING";
        case TokenType::KEYWORD_ON:            return "KEYWORD_ON";
        case TokenType::KEYWORD_AS:            return "KEYWORD_AS";
        case TokenType::KEYWORD_DISTINCT:      return "KEYWORD_DISTINCT";
        case TokenType::KEYWORD_LIMIT:         return "KEYWORD_LIMIT";
        case TokenType::KEYWORD_INT:           return "KEYWORD_INT";
        case TokenType::KEYWORD_VARCHAR:       return "KEYWORD_VARCHAR";
        case TokenType::KEYWORD_FLOAT:         return "KEYWORD_FLOAT";
        case TokenType::KEYWORD_PRIMARY:       return "KEYWORD_PRIMARY";
        case TokenType::KEYWORD_KEY:           return "KEYWORD_KEY";
        case TokenType::KEYWORD_IS:            return "KEYWORD_IS";
        case TokenType::KEYWORD_LIKE:          return "KEYWORD_LIKE";
        case TokenType::KEYWORD_IN:            return "KEYWORD_IN";
        case TokenType::KEYWORD_BETWEEN:       return "KEYWORD_BETWEEN";
        case TokenType::KEYWORD_ASC:           return "KEYWORD_ASC";
        case TokenType::KEYWORD_DESC:          return "KEYWORD_DESC";
        case TokenType::KEYWORD_IF:            return "KEYWORD_IF";
        case TokenType::KEYWORD_DUPLICATE:     return "KEYWORD_DUPLICATE";
        case TokenType::KEYWORD_TRUNCATE:      return "KEYWORD_TRUNCATE";
        case TokenType::KEYWORD_ALTER:         return "KEYWORD_ALTER";
        case TokenType::KEYWORD_ADD:           return "KEYWORD_ADD";
        case TokenType::KEYWORD_RENAME:        return "KEYWORD_RENAME";
        case TokenType::KEYWORD_MODIFY:        return "KEYWORD_MODIFY";
        case TokenType::KEYWORD_CHECK:         return "KEYWORD_CHECK";
        case TokenType::KEYWORD_DEFAULT:       return "KEYWORD_DEFAULT";
        case TokenType::KEYWORD_TO:            return "KEYWORD_TO";
        case TokenType::KEYWORD_COLUMN:        return "KEYWORD_COLUMN";
        case TokenType::KEYWORD_CASE:          return "KEYWORD_CASE";
        case TokenType::KEYWORD_WHEN:          return "KEYWORD_WHEN";
        case TokenType::KEYWORD_THEN:          return "KEYWORD_THEN";
        case TokenType::KEYWORD_ELSE:          return "KEYWORD_ELSE";
        case TokenType::KEYWORD_END:           return "KEYWORD_END";
        case TokenType::KEYWORD_CAST:          return "KEYWORD_CAST";
        case TokenType::KEYWORD_COALESCE:      return "KEYWORD_COALESCE";
        case TokenType::KEYWORD_NULLIF:        return "KEYWORD_NULLIF";
        case TokenType::KEYWORD_WITH:          return "KEYWORD_WITH";
        case TokenType::KEYWORD_RECURSIVE:     return "KEYWORD_RECURSIVE";
        case TokenType::KEYWORD_OVER:          return "KEYWORD_OVER";
        case TokenType::KEYWORD_PARTITION:     return "KEYWORD_PARTITION";
        case TokenType::KEYWORD_ROWS:          return "KEYWORD_ROWS";
        case TokenType::KEYWORD_RANGE:         return "KEYWORD_RANGE";
        case TokenType::KEYWORD_BETWEEN_KW:    return "KEYWORD_BETWEEN_KW";
        case TokenType::KEYWORD_UNBOUNDED:     return "KEYWORD_UNBOUNDED";
        case TokenType::KEYWORD_PRECEDING:     return "KEYWORD_PRECEDING";
        case TokenType::KEYWORD_FOLLOWING:     return "KEYWORD_FOLLOWING";
        case TokenType::KEYWORD_CURRENT:       return "KEYWORD_CURRENT";
        case TokenType::KEYWORD_ROW:           return "KEYWORD_ROW";
        case TokenType::KEYWORD_WINDOW:        return "KEYWORD_WINDOW";
        case TokenType::KEYWORD_ROW_NUMBER:    return "KEYWORD_ROW_NUMBER";
        case TokenType::KEYWORD_RANK:          return "KEYWORD_RANK";
        case TokenType::KEYWORD_DENSE_RANK:    return "KEYWORD_DENSE_RANK";
        case TokenType::KEYWORD_NTILE:         return "KEYWORD_NTILE";
        case TokenType::KEYWORD_LAG:           return "KEYWORD_LAG";
        case TokenType::KEYWORD_LEAD:          return "KEYWORD_LEAD";
        case TokenType::KEYWORD_FIRST_VALUE:   return "KEYWORD_FIRST_VALUE";
        case TokenType::KEYWORD_LAST_VALUE:    return "KEYWORD_LAST_VALUE";
        case TokenType::KEYWORD_PERCENT_RANK:  return "KEYWORD_PERCENT_RANK";
        case TokenType::KEYWORD_CUME_DIST:     return "KEYWORD_CUME_DIST";
        case TokenType::KEYWORD_UNION:         return "KEYWORD_UNION";
        case TokenType::KEYWORD_INTERSECT:     return "KEYWORD_INTERSECT";
        case TokenType::KEYWORD_EXCEPT:        return "KEYWORD_EXCEPT";
        case TokenType::KEYWORD_ANY:           return "KEYWORD_ANY";
        case TokenType::KEYWORD_ALL:           return "KEYWORD_ALL";
        case TokenType::KEYWORD_EXISTS:        return "KEYWORD_EXISTS";
        case TokenType::KEYWORD_UPPER:         return "KEYWORD_UPPER";
        case TokenType::KEYWORD_LOWER:         return "KEYWORD_LOWER";
        case TokenType::KEYWORD_LENGTH:        return "KEYWORD_LENGTH";
        case TokenType::KEYWORD_SUBSTR:        return "KEYWORD_SUBSTR";
        case TokenType::KEYWORD_TRIM:          return "KEYWORD_TRIM";
        case TokenType::KEYWORD_REPLACE:       return "KEYWORD_REPLACE";
        case TokenType::KEYWORD_ROUND:         return "KEYWORD_ROUND";
        case TokenType::KEYWORD_CEIL:          return "KEYWORD_CEIL";
        case TokenType::KEYWORD_FLOOR:         return "KEYWORD_FLOOR";
        case TokenType::KEYWORD_ABS:           return "KEYWORD_ABS";
        case TokenType::KEYWORD_POWER:         return "KEYWORD_POWER";
        case TokenType::KEYWORD_MOD:           return "KEYWORD_MOD";
        case TokenType::KEYWORD_YEAR:          return "KEYWORD_YEAR";
        case TokenType::KEYWORD_MONTH:         return "KEYWORD_MONTH";
        case TokenType::KEYWORD_DAY:           return "KEYWORD_DAY";
        case TokenType::KEYWORD_NOW:           return "KEYWORD_NOW";
        case TokenType::KEYWORD_IFNULL:        return "KEYWORD_IFNULL";
        case TokenType::KEYWORD_HOUR:          return "KEYWORD_HOUR";
        case TokenType::KEYWORD_MINUTE:        return "KEYWORD_MINUTE";
        case TokenType::KEYWORD_SECOND:        return "KEYWORD_SECOND";

        case TokenType::KEYWORD_BEGIN:         return "KEYWORD_BEGIN";
        case TokenType::KEYWORD_TRANSACTION:   return "KEYWORD_TRANSACTION";
        case TokenType::KEYWORD_COMMIT:        return "KEYWORD_COMMIT";
        case TokenType::KEYWORD_ROLLBACK:      return "KEYWORD_ROLLBACK";
        case TokenType::KEYWORD_SAVEPOINT:     return "KEYWORD_SAVEPOINT";
        case TokenType::KEYWORD_RELEASE:       return "KEYWORD_RELEASE";
        case TokenType::KEYWORD_VIEW:          return "KEYWORD_VIEW";
        case TokenType::KEYWORD_TRIGGER:       return "KEYWORD_TRIGGER";
        case TokenType::KEYWORD_FUNCTION:      return "KEYWORD_FUNCTION";
        case TokenType::KEYWORD_BEFORE:        return "KEYWORD_BEFORE";
        case TokenType::KEYWORD_AFTER:         return "KEYWORD_AFTER";
        case TokenType::KEYWORD_FOR:           return "KEYWORD_FOR";
        case TokenType::KEYWORD_EACH:          return "KEYWORD_EACH";
        case TokenType::KEYWORD_NEW:           return "KEYWORD_NEW";
        case TokenType::KEYWORD_OLD:           return "KEYWORD_OLD";
        case TokenType::KEYWORD_RETURN:        return "KEYWORD_RETURN";
        case TokenType::KEYWORD_RETURNS:       return "KEYWORD_RETURNS";
        case TokenType::KEYWORD_DECLARE:       return "KEYWORD_DECLARE";
        case TokenType::KEYWORD_WHILE:         return "KEYWORD_WHILE";
        case TokenType::KEYWORD_DO:            return "KEYWORD_DO";
        case TokenType::KEYWORD_ELSEIF:        return "KEYWORD_ELSEIF";

        case TokenType::KEYWORD_ILIKE:         return "KEYWORD_ILIKE";
        case TokenType::KEYWORD_REGEXP:        return "KEYWORD_REGEXP";
        case TokenType::KEYWORD_RLIKE:         return "KEYWORD_RLIKE";
        case TokenType::KEYWORD_ESCAPE:        return "KEYWORD_ESCAPE";
        case TokenType::KEYWORD_SIMILAR:       return "KEYWORD_SIMILAR";
        case TokenType::KEYWORD_DATE:          return "KEYWORD_DATE";
        case TokenType::KEYWORD_TIMESTAMP:     return "KEYWORD_TIMESTAMP";
        case TokenType::KEYWORD_INTERVAL:      return "KEYWORD_INTERVAL";
        case TokenType::KEYWORD_EXTRACT:       return "KEYWORD_EXTRACT";
        case TokenType::KEYWORD_EXPLAIN:       return "KEYWORD_EXPLAIN";
        case TokenType::KEYWORD_SHOW:          return "KEYWORD_SHOW";
        case TokenType::KEYWORD_DESCRIBE:      return "KEYWORD_DESCRIBE";
        case TokenType::KEYWORD_TABLES:        return "KEYWORD_TABLES";
        case TokenType::KEYWORD_COLUMNS:       return "KEYWORD_COLUMNS";
        case TokenType::KEYWORD_BOOLEAN:       return "KEYWORD_BOOLEAN";
        case TokenType::KEYWORD_BOOL:          return "KEYWORD_BOOL";
        case TokenType::KEYWORD_CHAR:          return "KEYWORD_CHAR";
        case TokenType::KEYWORD_TEXT:          return "KEYWORD_TEXT";
        case TokenType::KEYWORD_DECIMAL:       return "KEYWORD_DECIMAL";
        case TokenType::KEYWORD_NUMERIC:       return "KEYWORD_NUMERIC";
        case TokenType::KEYWORD_DOUBLE:        return "KEYWORD_DOUBLE";
        case TokenType::KEYWORD_REAL:          return "KEYWORD_REAL";
        case TokenType::KEYWORD_SMALLINT:      return "KEYWORD_SMALLINT";
        case TokenType::KEYWORD_TINYINT:       return "KEYWORD_TINYINT";
        case TokenType::KEYWORD_TIME:          return "KEYWORD_TIME";
        case TokenType::KEYWORD_JSON:          return "KEYWORD_JSON";
        case TokenType::KEYWORD_UUID:          return "KEYWORD_UUID";
        case TokenType::KEYWORD_AUTO_INCREMENT: return "KEYWORD_AUTO_INCREMENT";
        case TokenType::KEYWORD_SERIAL:        return "KEYWORD_SERIAL";
        case TokenType::KEYWORD_IDENTITY:      return "KEYWORD_IDENTITY";
        case TokenType::KEYWORD_TRUE:          return "KEYWORD_TRUE";
        case TokenType::KEYWORD_FALSE:         return "KEYWORD_FALSE";
        case TokenType::KEYWORD_SCHEMA:        return "KEYWORD_SCHEMA";
        case TokenType::KEYWORD_SEQUENCE:      return "KEYWORD_SEQUENCE";
        case TokenType::KEYWORD_NEXTVAL:       return "KEYWORD_NEXTVAL";
        case TokenType::KEYWORD_FOREIGN:       return "KEYWORD_FOREIGN";
        case TokenType::KEYWORD_REFERENCES:    return "KEYWORD_REFERENCES";
        case TokenType::KEYWORD_CASCADE:       return "KEYWORD_CASCADE";
        case TokenType::KEYWORD_RESTRICT:      return "KEYWORD_RESTRICT";
        case TokenType::KEYWORD_ACTION:        return "KEYWORD_ACTION";
        case TokenType::KEYWORD_CONSTRAINT:    return "KEYWORD_CONSTRAINT";
        case TokenType::KEYWORD_MERGE:         return "KEYWORD_MERGE";
        case TokenType::KEYWORD_MATCHED:       return "KEYWORD_MATCHED";
        case TokenType::KEYWORD_RETURNING:     return "KEYWORD_RETURNING";
        case TokenType::KEYWORD_LATERAL:       return "KEYWORD_LATERAL";
        case TokenType::KEYWORD_FETCH:         return "KEYWORD_FETCH";
        case TokenType::KEYWORD_OFFSET:        return "KEYWORD_OFFSET";
        case TokenType::KEYWORD_FIRST:         return "KEYWORD_FIRST";
        case TokenType::KEYWORD_NEXT:          return "KEYWORD_NEXT";
        case TokenType::KEYWORD_ONLY:          return "KEYWORD_ONLY";
        case TokenType::KEYWORD_TIES:          return "KEYWORD_TIES";
        case TokenType::KEYWORD_SHARE:         return "KEYWORD_SHARE";
        case TokenType::KEYWORD_NO:            return "KEYWORD_NO";
        case TokenType::KEYWORD_STDDEV:        return "KEYWORD_STDDEV";
        case TokenType::KEYWORD_VARIANCE:      return "KEYWORD_VARIANCE";
        case TokenType::KEYWORD_MEDIAN:        return "KEYWORD_MEDIAN";
        case TokenType::KEYWORD_STRING_AGG:    return "KEYWORD_STRING_AGG";
        case TokenType::KEYWORD_GROUP_CONCAT:  return "KEYWORD_GROUP_CONCAT";
        case TokenType::KEYWORD_PERCENTILE_CONT: return "KEYWORD_PERCENTILE_CONT";
        case TokenType::KEYWORD_PERCENTILE_DISC: return "KEYWORD_PERCENTILE_DISC";
        case TokenType::KEYWORD_GREATEST:      return "KEYWORD_GREATEST";
        case TokenType::KEYWORD_LEAST:         return "KEYWORD_LEAST";
        case TokenType::KEYWORD_RAND:          return "KEYWORD_RAND";
        case TokenType::KEYWORD_RANDOM:        return "KEYWORD_RANDOM";
        case TokenType::KEYWORD_FILTER:        return "KEYWORD_FILTER";
        case TokenType::KEYWORD_SETS:          return "KEYWORD_SETS";
        case TokenType::KEYWORD_ROLLUP:        return "KEYWORD_ROLLUP";
        case TokenType::KEYWORD_CUBE:          return "KEYWORD_CUBE";
        case TokenType::KEYWORD_GROUPING:      return "KEYWORD_GROUPING";
        case TokenType::KEYWORD_IGNORE:        return "KEYWORD_IGNORE";
        case TokenType::KEYWORD_RESPECT:       return "KEYWORD_RESPECT";
        case TokenType::KEYWORD_NULLS:         return "KEYWORD_NULLS";
        case TokenType::KEYWORD_WITHIN:        return "KEYWORD_WITHIN";
        case TokenType::KEYWORD_LOOP:          return "KEYWORD_LOOP";
        case TokenType::KEYWORD_REPEAT:        return "KEYWORD_REPEAT";
        case TokenType::KEYWORD_UNTIL:         return "KEYWORD_UNTIL";
        case TokenType::KEYWORD_OPEN:          return "KEYWORD_OPEN";
        case TokenType::KEYWORD_CLOSE:         return "KEYWORD_CLOSE";
        case TokenType::KEYWORD_LEAVE:         return "KEYWORD_LEAVE";
        case TokenType::KEYWORD_ITERATE:       return "KEYWORD_ITERATE";
        case TokenType::KEYWORD_SIGNAL:        return "KEYWORD_SIGNAL";
        case TokenType::KEYWORD_SQLSTATE:      return "KEYWORD_SQLSTATE";
        case TokenType::KEYWORD_MESSAGE_TEXT:  return "KEYWORD_MESSAGE_TEXT";
        case TokenType::KEYWORD_HANDLER:       return "KEYWORD_HANDLER";
        case TokenType::KEYWORD_CONTINUE:      return "KEYWORD_CONTINUE";
        case TokenType::KEYWORD_SQLEXCEPTION:  return "KEYWORD_SQLEXCEPTION";
        case TokenType::KEYWORD_SQLWARNING:    return "KEYWORD_SQLWARNING";
        case TokenType::KEYWORD_FOUND:         return "KEYWORD_FOUND";
        case TokenType::KEYWORD_CURSOR:        return "KEYWORD_CURSOR";
        case TokenType::KEYWORD_PROCEDURE:     return "KEYWORD_PROCEDURE";
        case TokenType::KEYWORD_CALL:          return "KEYWORD_CALL";
        case TokenType::KEYWORD_OUT:           return "KEYWORD_OUT";
        case TokenType::KEYWORD_INOUT:         return "KEYWORD_INOUT";
        case TokenType::KEYWORD_EXIT:          return "KEYWORD_EXIT";
        case TokenType::KEYWORD_UNDO:          return "KEYWORD_UNDO";
        case TokenType::KEYWORD_CONDITION:     return "KEYWORD_CONDITION";
        case TokenType::KEYWORD_MATERIALIZED:  return "KEYWORD_MATERIALIZED";
        case TokenType::KEYWORD_REFRESH:       return "KEYWORD_REFRESH";
        case TokenType::KEYWORD_STATEMENT:     return "KEYWORD_STATEMENT";
        case TokenType::KEYWORD_OPTION:        return "KEYWORD_OPTION";
        case TokenType::KEYWORD_CASCADED:      return "KEYWORD_CASCADED";
        case TokenType::KEYWORD_LOCAL:         return "KEYWORD_LOCAL";

        case TokenType::IDENTIFIER:            return "IDENTIFIER";
        case TokenType::INTEGER_LITERAL:       return "INTEGER_LITERAL";
        case TokenType::FLOAT_LITERAL:         return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL:        return "STRING_LITERAL";
        case TokenType::OP_EQUAL:              return "OP_EQUAL";
        case TokenType::OP_NOT_EQUAL:          return "OP_NOT_EQUAL";
        case TokenType::OP_LESS:               return "OP_LESS";
        case TokenType::OP_LESS_EQUAL:         return "OP_LESS_EQUAL";
        case TokenType::OP_GREATER:            return "OP_GREATER";
        case TokenType::OP_GREATER_EQUAL:      return "OP_GREATER_EQUAL";
        case TokenType::OP_PLUS:               return "OP_PLUS";
        case TokenType::OP_MINUS:              return "OP_MINUS";
        case TokenType::OP_STAR:               return "OP_STAR";
        case TokenType::OP_SLASH:              return "OP_SLASH";
        case TokenType::OP_MODULO:             return "OP_MODULO";
        case TokenType::OP_CONCAT:             return "OP_CONCAT";
        case TokenType::LEFT_PAREN:            return "LEFT_PAREN";
        case TokenType::RIGHT_PAREN:           return "RIGHT_PAREN";
        case TokenType::COMMA:                 return "COMMA";
        case TokenType::SEMICOLON:             return "SEMICOLON";
        case TokenType::DOT:                   return "DOT";
        case TokenType::BACKTICK:              return "BACKTICK";
        case TokenType::END_OF_FILE:           return "END_OF_FILE";
        case TokenType::UNKNOWN:               return "UNKNOWN";
    }
    // 80_tokens_complete: 上述 switch 已覆盖 TokenType 枚举的全部成员；
    // 任何新增枚举值都必须补一条 case 并以 std::string 返回，否则 GCC -Werror
    // 下的 -Wswitch 会失败。
    return "UNKNOWN";
}

Token::Token() : type(TokenType::UNKNOWN), lexeme(""), line(0), column(0) {
}

Token::Token(TokenType type, const std::string& lexeme, int line, int column)
    : type(type), lexeme(lexeme), line(line), column(column) {
}

std::string Token::ToString() const {
    std::ostringstream oss;
    oss << "[" << TokenTypeToString(type) << ", '" << lexeme
        << "', line=" << line << ", col=" << column << "]";
    return oss.str();
}

}  // namespace sqlcompiler
