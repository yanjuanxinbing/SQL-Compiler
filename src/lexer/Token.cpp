#include "lexer/Token.h"

namespace sqlcompiler
{
    std::string TokenTypeToString(TokenType type)
    {
        switch (type)
        {
        // ---- 关键字 ----
        case TokenType::KEYWORD_SELECT:
            return "SELECT";
        case TokenType::KEYWORD_FROM:
            return "FROM";
        case TokenType::KEYWORD_WHERE:
            return "WHERE";
        case TokenType::KEYWORD_INSERT:
            return "INSERT";
        case TokenType::KEYWORD_INTO:
            return "INTO";
        case TokenType::KEYWORD_VALUES:
            return "VALUES";
        case TokenType::KEYWORD_UPDATE:
            return "UPDATE";
        case TokenType::KEYWORD_SET:
            return "SET";
        case TokenType::KEYWORD_DELETE:
            return "DELETE";
        case TokenType::KEYWORD_CREATE:
            return "CREATE";
        case TokenType::KEYWORD_TABLE:
            return "TABLE";
        case TokenType::KEYWORD_DROP:
            return "DROP";
        case TokenType::KEYWORD_AND:
            return "AND";
        case TokenType::KEYWORD_OR:
            return "OR";
        case TokenType::KEYWORD_NOT:
            return "NOT";
        case TokenType::KEYWORD_NULL:
            return "NULL";
        case TokenType::KEYWORD_ORDER:
            return "ORDER";
        case TokenType::KEYWORD_BY:
            return "BY";
        case TokenType::KEYWORD_GROUP:
            return "GROUP";
        case TokenType::KEYWORD_HAVING:
            return "HAVING";
        case TokenType::KEYWORD_JOIN:
            return "JOIN";
        case TokenType::KEYWORD_INNER:
            return "INNER";
        case TokenType::KEYWORD_LEFT:
            return "LEFT";
        case TokenType::KEYWORD_RIGHT:
            return "RIGHT";
        case TokenType::KEYWORD_ON:
            return "ON";
        case TokenType::KEYWORD_AS:
            return "AS";
        case TokenType::KEYWORD_DISTINCT:
            return "DISTINCT";
        case TokenType::KEYWORD_LIMIT:
            return "LIMIT";
        case TokenType::KEYWORD_INT:
            return "INT";
        case TokenType::KEYWORD_VARCHAR:
            return "VARCHAR";
        case TokenType::KEYWORD_FLOAT:
            return "FLOAT";
        case TokenType::KEYWORD_PRIMARY:
            return "PRIMARY";
        case TokenType::KEYWORD_KEY:
            return "KEY";

        // ---- 标识符与字面量 ----
        case TokenType::IDENTIFIER:
            return "IDENTIFIER";
        case TokenType::INTEGER_LITERAL:
            return "INTEGER_LITERAL";
        case TokenType::FLOAT_LITERAL:
            return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL:
            return "STRING_LITERAL";

        // ---- 运算符与符号 ----
        case TokenType::OP_EQUAL:
            return "=";
        case TokenType::OP_NOT_EQUAL:
            return "!=";
        case TokenType::OP_LESS:
            return "<";
        case TokenType::OP_LESS_EQUAL:
            return "<=";
        case TokenType::OP_GREATER:
            return ">";
        case TokenType::OP_GREATER_EQUAL:
            return ">=";
        case TokenType::OP_PLUS:
            return "+";
        case TokenType::OP_MINUS:
            return "-";
        case TokenType::OP_STAR:
            return "*";
        case TokenType::OP_SLASH:
            return "/";
        case TokenType::LEFT_PAREN:
            return "(";
        case TokenType::RIGHT_PAREN:
            return ")";
        case TokenType::COMMA:
            return ",";
        case TokenType::SEMICOLON:
            return ";";
        case TokenType::DOT:
            return ".";

        case TokenType::END_OF_FILE:
            return "EOF";
        case TokenType::UNKNOWN:
            return "UNKNOWN";
        }
        // 防御:枚举新增值但忘记在此补 case 时,不会静默返回空串
        return "<UNKNOWN:" + std::to_string(static_cast<int>(type)) + ">";
    }

    Token::Token()
        : type(TokenType::UNKNOWN), lexeme(""), line(0), column(0) {}

    Token::Token(TokenType type, const std::string &lexeme, int line, int column)
        : type(type), lexeme(lexeme), line(line), column(column) {}

    std::string Token::ToString() const
    {
        // TODO: 返回形如 "[SELECT, 'select', line=1, col=1]" 的调试字符串
        return "[" + TokenTypeToString(type) + ", '" + lexeme + "', line=" + std::to_string(line) + ", col=" + std::to_string(column) + "]";
    }

} // namespace sqlcompiler
