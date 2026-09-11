#include "lexer/Token.h"

#include <sstream>

namespace sqlcompiler {

std::string TokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::KEYWORD_SELECT:    return "KEYWORD_SELECT";
        case TokenType::KEYWORD_FROM:      return "KEYWORD_FROM";
        case TokenType::KEYWORD_WHERE:     return "KEYWORD_WHERE";
        case TokenType::KEYWORD_INSERT:    return "KEYWORD_INSERT";
        case TokenType::KEYWORD_INTO:      return "KEYWORD_INTO";
        case TokenType::KEYWORD_VALUES:    return "KEYWORD_VALUES";
        case TokenType::KEYWORD_UPDATE:    return "KEYWORD_UPDATE";
        case TokenType::KEYWORD_SET:       return "KEYWORD_SET";
        case TokenType::KEYWORD_DELETE:    return "KEYWORD_DELETE";
        case TokenType::KEYWORD_CREATE:    return "KEYWORD_CREATE";
        case TokenType::KEYWORD_TABLE:     return "KEYWORD_TABLE";
        case TokenType::KEYWORD_DROP:      return "KEYWORD_DROP";
        case TokenType::KEYWORD_AND:       return "KEYWORD_AND";
        case TokenType::KEYWORD_OR:        return "KEYWORD_OR";
        case TokenType::KEYWORD_NOT:       return "KEYWORD_NOT";
        case TokenType::KEYWORD_NULL:      return "KEYWORD_NULL";
        case TokenType::KEYWORD_ORDER:     return "KEYWORD_ORDER";
        case TokenType::KEYWORD_BY:        return "KEYWORD_BY";
        case TokenType::KEYWORD_GROUP:     return "KEYWORD_GROUP";
        case TokenType::KEYWORD_HAVING:    return "KEYWORD_HAVING";
        case TokenType::KEYWORD_JOIN:      return "KEYWORD_JOIN";
        case TokenType::KEYWORD_INNER:     return "KEYWORD_INNER";
        case TokenType::KEYWORD_LEFT:      return "KEYWORD_LEFT";
        case TokenType::KEYWORD_RIGHT:     return "KEYWORD_RIGHT";
        case TokenType::KEYWORD_ON:        return "KEYWORD_ON";
        case TokenType::KEYWORD_AS:        return "KEYWORD_AS";
        case TokenType::KEYWORD_DISTINCT:  return "KEYWORD_DISTINCT";
        case TokenType::KEYWORD_LIMIT:     return "KEYWORD_LIMIT";
        case TokenType::KEYWORD_INT:       return "KEYWORD_INT";
        case TokenType::KEYWORD_VARCHAR:   return "KEYWORD_VARCHAR";
        case TokenType::KEYWORD_FLOAT:     return "KEYWORD_FLOAT";
        case TokenType::KEYWORD_PRIMARY:   return "KEYWORD_PRIMARY";
        case TokenType::KEYWORD_KEY:       return "KEYWORD_KEY";
        case TokenType::KEYWORD_IS:        return "KEYWORD_IS";
        case TokenType::KEYWORD_LIKE:      return "KEYWORD_LIKE";
        case TokenType::KEYWORD_IN:        return "KEYWORD_IN";
        case TokenType::KEYWORD_BETWEEN:   return "KEYWORD_BETWEEN";
        case TokenType::KEYWORD_ASC:       return "KEYWORD_ASC";
        case TokenType::KEYWORD_BEGIN:     return "KEYWORD_BEGIN";
        case TokenType::KEYWORD_TRANSACTION: return "KEYWORD_TRANSACTION";
        case TokenType::KEYWORD_COMMIT:    return "KEYWORD_COMMIT";
        case TokenType::KEYWORD_ROLLBACK:  return "KEYWORD_ROLLBACK";
        case TokenType::KEYWORD_SAVEPOINT: return "KEYWORD_SAVEPOINT";
        case TokenType::KEYWORD_RELEASE:   return "KEYWORD_RELEASE";
        case TokenType::KEYWORD_VIEW:      return "KEYWORD_VIEW";
        case TokenType::KEYWORD_TRIGGER:   return "KEYWORD_TRIGGER";
        case TokenType::KEYWORD_FUNCTION:  return "KEYWORD_FUNCTION";
        case TokenType::KEYWORD_BEFORE:    return "KEYWORD_BEFORE";
        case TokenType::KEYWORD_AFTER:     return "KEYWORD_AFTER";
        case TokenType::KEYWORD_FOR:       return "KEYWORD_FOR";
        case TokenType::KEYWORD_EACH:      return "KEYWORD_EACH";
        case TokenType::KEYWORD_NEW:       return "KEYWORD_NEW";
        case TokenType::KEYWORD_OLD:       return "KEYWORD_OLD";
        case TokenType::KEYWORD_RETURN:    return "KEYWORD_RETURN";
        case TokenType::KEYWORD_RETURNS:   return "KEYWORD_RETURNS";
        case TokenType::IDENTIFIER:        return "IDENTIFIER";
        case TokenType::INTEGER_LITERAL:   return "INTEGER_LITERAL";
        case TokenType::FLOAT_LITERAL:     return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL:    return "STRING_LITERAL";
        case TokenType::OP_EQUAL:          return "OP_EQUAL";
        case TokenType::OP_NOT_EQUAL:      return "OP_NOT_EQUAL";
        case TokenType::OP_LESS:           return "OP_LESS";
        case TokenType::OP_LESS_EQUAL:     return "OP_LESS_EQUAL";
        case TokenType::OP_GREATER:        return "OP_GREATER";
        case TokenType::OP_GREATER_EQUAL:  return "OP_GREATER_EQUAL";
        case TokenType::OP_PLUS:           return "OP_PLUS";
        case TokenType::OP_MINUS:          return "OP_MINUS";
        case TokenType::OP_STAR:           return "OP_STAR";
        case TokenType::OP_SLASH:          return "OP_SLASH";
        case TokenType::OP_CONCAT:         return "OP_CONCAT";
        case TokenType::LEFT_PAREN:        return "LEFT_PAREN";
        case TokenType::RIGHT_PAREN:       return "RIGHT_PAREN";
        case TokenType::COMMA:             return "COMMA";
        case TokenType::SEMICOLON:         return "SEMICOLON";
        case TokenType::DOT:               return "DOT";
        case TokenType::END_OF_FILE:       return "END_OF_FILE";
        case TokenType::UNKNOWN:           return "UNKNOWN";
    }
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