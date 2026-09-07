#include "lexer/Token.h"

namespace sqlcompiler {

std::string TokenTypeToString(TokenType type) {
    // TODO: 使用switch-case将每个TokenType映射为对应字符串，便于调试输出
    return "";
}

Token::Token() : type(TokenType::UNKNOWN), lexeme(""), line(0), column(0) {
    // TODO: 如有需要可补充默认构造逻辑
}

Token::Token(TokenType type, const std::string& lexeme, int line, int column)
    : type(type), lexeme(lexeme), line(line), column(column) {
    // TODO: 如有需要可补充构造逻辑
}

std::string Token::ToString() const {
    // TODO: 返回形如 "[SELECT, 'select', line=1, col=1]" 的调试字符串
    return "";
}

}  // namespace sqlcompiler
