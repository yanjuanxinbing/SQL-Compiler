#pragma once

#include <string>
#include <vector>
#include <cctype>
#include <unordered_map>

#include "lexer/Token.h"

namespace sqlcompiler {

// 词法分析器：将SQL源字符串转换为Token序列
class Lexer {
public:
    explicit Lexer(const std::string& source);

    // 对外主接口：一次性将整个source转换为Token序列（以END_OF_FILE结尾）
    std::vector<Token> Tokenize();

    // 获取下一个Token，并移动内部指针
    Token NextToken();

    // 查看下一个Token但不移动指针
    Token PeekToken();

    // 是否已经到达源码末尾
    bool IsAtEnd() const;

private:
    std::string source_;
    size_t pos_;
    int line_;
    int column_;

    char CurrentChar() const;
    char PeekChar(int offset = 1) const;
    void Advance();

    // 跳过空白字符与注释（-- 单行注释 / * 多行注释 */）
    void SkipWhitespaceAndComments();

    // 各类子扫描函数
    Token ScanIdentifierOrKeyword();
    Token ScanNumber();
    Token ScanString();
    Token ScanOperatorOrSymbol();

    // 根据标识符文本判断是否为关键字，返回对应TokenType
    TokenType LookupKeyword(const std::string& text) const;
};

}  // namespace sqlcompiler
