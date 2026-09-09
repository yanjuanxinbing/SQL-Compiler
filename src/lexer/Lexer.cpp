#include "lexer/Lexer.h"

#include "common/Error.h"

#include <cctype>
#include <unordered_map>
#include <utility>

namespace sqlcompiler {

namespace {

const std::unordered_map<std::string, TokenType>& KeywordTable() {
    static const std::unordered_map<std::string, TokenType> kKeywords = {
        {"SELECT",   TokenType::KEYWORD_SELECT},
        {"FROM",     TokenType::KEYWORD_FROM},
        {"WHERE",    TokenType::KEYWORD_WHERE},
        {"INSERT",   TokenType::KEYWORD_INSERT},
        {"INTO",     TokenType::KEYWORD_INTO},
        {"VALUES",   TokenType::KEYWORD_VALUES},
        {"UPDATE",   TokenType::KEYWORD_UPDATE},
        {"SET",      TokenType::KEYWORD_SET},
        {"DELETE",   TokenType::KEYWORD_DELETE},
        {"CREATE",   TokenType::KEYWORD_CREATE},
        {"TABLE",    TokenType::KEYWORD_TABLE},
        {"DROP",     TokenType::KEYWORD_DROP},
        {"AND",      TokenType::KEYWORD_AND},
        {"OR",       TokenType::KEYWORD_OR},
        {"NOT",      TokenType::KEYWORD_NOT},
        {"NULL",     TokenType::KEYWORD_NULL},
        {"ORDER",    TokenType::KEYWORD_ORDER},
        {"BY",       TokenType::KEYWORD_BY},
        {"GROUP",    TokenType::KEYWORD_GROUP},
        {"HAVING",   TokenType::KEYWORD_HAVING},
        {"JOIN",     TokenType::KEYWORD_JOIN},
        {"INNER",    TokenType::KEYWORD_INNER},
        {"LEFT",     TokenType::KEYWORD_LEFT},
        {"RIGHT",    TokenType::KEYWORD_RIGHT},
        {"ON",       TokenType::KEYWORD_ON},
        {"AS",       TokenType::KEYWORD_AS},
        {"DISTINCT", TokenType::KEYWORD_DISTINCT},
        {"LIMIT",    TokenType::KEYWORD_LIMIT},
        {"INT",      TokenType::KEYWORD_INT},
        {"VARCHAR",  TokenType::KEYWORD_VARCHAR},
        {"FLOAT",    TokenType::KEYWORD_FLOAT},
        {"PRIMARY",  TokenType::KEYWORD_PRIMARY},
        {"KEY",      TokenType::KEYWORD_KEY},
    };
    return kKeywords;
}

std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

bool IsIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool IsIdentPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c));
}

}  // namespace

Lexer::Lexer(const std::string& source) : source_(source), pos_(0), line_(1), column_(1) {
}

std::vector<Token> Lexer::Tokenize() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = NextToken();
        tokens.push_back(tok);
        if (tok.type == TokenType::END_OF_FILE) {
            break;
        }
    }
    return tokens;
}

bool Lexer::IsAtEnd() const {
    return pos_ >= source_.size();
}

char Lexer::CurrentChar() const {
    if (pos_ >= source_.size()) return '\0';
    return source_[pos_];
}

char Lexer::PeekChar(int offset) const {
    size_t idx = pos_ + static_cast<size_t>(offset);
    if (idx >= source_.size()) return '\0';
    return source_[idx];
}

void Lexer::Advance() {
    if (pos_ >= source_.size()) return;
    if (source_[pos_] == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    ++pos_;
}

void Lexer::SkipWhitespaceAndComments() {
    while (!IsAtEnd()) {
        char c = CurrentChar();
        if (std::isspace(static_cast<unsigned char>(c))) {
            Advance();
            continue;
        }
        if (c == '-' && PeekChar() == '-') {
            while (!IsAtEnd() && CurrentChar() != '\n') {
                Advance();
            }
            continue;
        }
        if (c == '/' && PeekChar() == '*') {
            Advance(); // '/'
            Advance(); // '*'
            while (!IsAtEnd()) {
                if (CurrentChar() == '*' && PeekChar() == '/') {
                    Advance();
                    Advance();
                    break;
                }
                Advance();
            }
            continue;
        }
        break;
    }
}

TokenType Lexer::LookupKeyword(const std::string& text) const {
    std::string upper = ToUpper(text);
    auto& table = KeywordTable();
    auto it = table.find(upper);
    if (it != table.end()) {
        return it->second;
    }
    return TokenType::IDENTIFIER;
}

Token Lexer::ScanIdentifierOrKeyword() {
    int start_line = line_;
    int start_col = column_;
    size_t start_pos = pos_;
    while (!IsAtEnd() && IsIdentPart(CurrentChar())) {
        Advance();
    }
    std::string text = source_.substr(start_pos, pos_ - start_pos);
    TokenType type = LookupKeyword(text);
    return Token(type, text, start_line, start_col);
}

Token Lexer::ScanNumber() {
    int start_line = line_;
    int start_col = column_;
    size_t start_pos = pos_;
    while (!IsAtEnd() && IsDigit(CurrentChar())) {
        Advance();
    }
    bool is_float = false;
    if (!IsAtEnd() && CurrentChar() == '.' && IsDigit(PeekChar())) {
        is_float = true;
        Advance();
        while (!IsAtEnd() && IsDigit(CurrentChar())) {
            Advance();
        }
    }
    std::string text = source_.substr(start_pos, pos_ - start_pos);
    TokenType type = is_float ? TokenType::FLOAT_LITERAL : TokenType::INTEGER_LITERAL;
    return Token(type, text, start_line, start_col);
}

Token Lexer::ScanString() {
    int start_line = line_;
    int start_col = column_;
    Advance(); // consume opening '
    std::string buf;
    while (!IsAtEnd() && CurrentChar() != '\'') {
        char c = CurrentChar();
        if (c == '\\' && PeekChar() != '\0') {
            Advance();
            char esc = CurrentChar();
            switch (esc) {
                case 'n': buf.push_back('\n'); break;
                case 't': buf.push_back('\t'); break;
                case 'r': buf.push_back('\r'); break;
                case '\\': buf.push_back('\\'); break;
                case '\'': buf.push_back('\''); break;
                case '"': buf.push_back('"'); break;
                case '0': buf.push_back('\0'); break;
                default: buf.push_back(esc); break;
            }
            Advance();
            continue;
        }
        buf.push_back(c);
        Advance();
    }
    if (IsAtEnd()) {
        throw CompilerException(ErrorStage::LEXICAL,
                                "unterminated string literal", start_line, start_col);
    }
    Advance(); // consume closing '
    return Token(TokenType::STRING_LITERAL, buf, start_line, start_col);
}

Token Lexer::ScanOperatorOrSymbol() {
    int start_line = line_;
    int start_col = column_;
    char c = CurrentChar();
    char n = PeekChar();
    // Two-character operators first
    if (c == '!' && n == '=') {
        Advance(); Advance();
        return Token(TokenType::OP_NOT_EQUAL, "!=", start_line, start_col);
    }
    if (c == '<' && n == '=') {
        Advance(); Advance();
        return Token(TokenType::OP_LESS_EQUAL, "<=", start_line, start_col);
    }
    if (c == '>' && n == '=') {
        Advance(); Advance();
        return Token(TokenType::OP_GREATER_EQUAL, ">=", start_line, start_col);
    }
    if (c == '<' && n == '>') {
        Advance(); Advance();
        return Token(TokenType::OP_NOT_EQUAL, "<>", start_line, start_col);
    }
    Advance();
    switch (c) {
        case '=': return Token(TokenType::OP_EQUAL, "=", start_line, start_col);
        case '<': return Token(TokenType::OP_LESS, "<", start_line, start_col);
        case '>': return Token(TokenType::OP_GREATER, ">", start_line, start_col);
        case '+': return Token(TokenType::OP_PLUS, "+", start_line, start_col);
        case '-': return Token(TokenType::OP_MINUS, "-", start_line, start_col);
        case '*': return Token(TokenType::OP_STAR, "*", start_line, start_col);
        case '/': return Token(TokenType::OP_SLASH, "/", start_line, start_col);
        case '(': return Token(TokenType::LEFT_PAREN, "(", start_line, start_col);
        case ')': return Token(TokenType::RIGHT_PAREN, ")", start_line, start_col);
        case ',': return Token(TokenType::COMMA, ",", start_line, start_col);
        case ';': return Token(TokenType::SEMICOLON, ";", start_line, start_col);
        case '.': return Token(TokenType::DOT, ".", start_line, start_col);
        default: {
            std::string bad(1, c);
            throw CompilerException(ErrorStage::LEXICAL,
                "unexpected character: '" + bad + "'", start_line, start_col);
        }
    }
}

Token Lexer::NextToken() {
    SkipWhitespaceAndComments();
    if (IsAtEnd()) {
        return Token(TokenType::END_OF_FILE, "", line_, column_);
    }
    char c = CurrentChar();
    if (IsIdentStart(c)) {
        return ScanIdentifierOrKeyword();
    }
    if (IsDigit(c)) {
        return ScanNumber();
    }
    if (c == '\'') {
        return ScanString();
    }
    return ScanOperatorOrSymbol();
}

Token Lexer::PeekToken() {
    size_t saved_pos = pos_;
    int saved_line = line_;
    int saved_col = column_;
    Token tok = NextToken();
    pos_ = saved_pos;
    line_ = saved_line;
    column_ = saved_col;
    return tok;
}

}  // namespace sqlcompiler