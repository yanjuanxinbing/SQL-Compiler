#include "lexer/Lexer.h"

#include "common/Error.h"

namespace sqlcompiler {

Lexer::Lexer(const std::string& source) : source_(source), pos_(0), line_(1), column_(1) {
    // TODO: 如有需要可补充初始化逻辑（如预处理源码）
}

std::vector<Token> Lexer::Tokenize() {
    // TODO: 循环调用NextToken()直到遇到END_OF_FILE，收集所有Token并返回
    return {};
}

Token Lexer::NextToken() {
    // TODO:
    // 1. 调用SkipWhitespaceAndComments()跳过空白与注释
    // 2. 判断IsAtEnd()，若结束返回END_OF_FILE的Token
    // 3. 根据CurrentChar()的类型分派给
    //    ScanIdentifierOrKeyword / ScanNumber / ScanString / ScanOperatorOrSymbol
    return Token();
}

Token Lexer::PeekToken() {
    // TODO: 在不移动pos_/line_/column_的前提下预读下一个Token
    // 可以通过保存/恢复内部状态，或调用NextToken()后回退实现
    return Token();
}

bool Lexer::IsAtEnd() const {
    // TODO: 判断pos_是否超出source_长度
    return true;
}

char Lexer::CurrentChar() const {
    // TODO: 返回source_[pos_]，注意越界处理
    return '\0';
}

char Lexer::PeekChar(int offset) const {
    // TODO: 返回source_[pos_ + offset]，注意越界处理
    return '\0';
}

void Lexer::Advance() {
    // TODO: pos_自增，同时维护line_/column_（遇到换行符时line_++, column_=1）
}

void Lexer::SkipWhitespaceAndComments() {
    // TODO:
    // 1. 跳过空格/制表符/换行符
    // 2. 跳过 "--" 开头的单行注释
    // 3. 跳过 "/* ... */" 多行注释
}

Token Lexer::ScanIdentifierOrKeyword() {
    // TODO:
    // 1. 从当前位置开始，读取连续的字母/数字/下划线
    // 2. 调用LookupKeyword()判断是否为关键字
    // 3. 构造并返回对应的Token（KEYWORD_* 或 IDENTIFIER）
    return Token();
}

Token Lexer::ScanNumber() {
    // TODO:
    // 1. 读取连续数字
    // 2. 若遇到'.'则继续读取小数部分，标记为FLOAT_LITERAL，否则为INTEGER_LITERAL
    return Token();
}

Token Lexer::ScanString() {
    // TODO: 读取单引号包裹的字符串字面量，处理转义字符，返回STRING_LITERAL类型Token
    return Token();
}

Token Lexer::ScanOperatorOrSymbol() {
    // TODO: 根据当前字符（及下一个字符，用于识别 <=, >=, !=, <> 等双字符运算符）
    // 构造对应的运算符/符号Token
    return Token();
}

TokenType Lexer::LookupKeyword(const std::string& text) const {
    // TODO: 将text转为大写后与关键字表比对，命中则返回对应KEYWORD_*类型，
    // 否则返回TokenType::IDENTIFIER
    return TokenType::IDENTIFIER;
}

}  // namespace sqlcompiler
