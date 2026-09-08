#include "lexer/Lexer.h"
#include "common/Error.h"

namespace sqlcompiler
{
    Lexer::Lexer(const std::string &source) : source_(source), pos_(0), line_(1), column_(1) {}

    std::vector<Token> Lexer::Tokenize()
    {
        std::vector<Token> res;
        Token cur = NextToken();

        while (cur.type != TokenType::END_OF_FILE)
        {
            // Scan* 函数把无法识别的字符标成 UNKNOWN(例如 '!'、未闭合字符串、EOF 后的 junk),
            // 这里统一抛错,不让错误 token 静默流入结果
            if (cur.type == TokenType::UNKNOWN)
            {
                throw CompilerException(
                    ErrorStage::LEXICAL,
                    "Unexpected character(s): '" + cur.lexeme + "'",
                    cur.line,
                    cur.column);
            }
            res.push_back(cur);
            cur = NextToken();
        }

        return res;
    }

    Token Lexer::NextToken()
    {
        SkipWhitespaceAndComments();

        if (IsAtEnd())
        {
            return Token(TokenType::END_OF_FILE, "", line_, column_);
        }

        char c = CurrentChar();

        if (c == '\'')
        {
            return ScanString();
        }
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            return ScanNumber();
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            return ScanIdentifierOrKeyword();
        }

        return ScanOperatorOrSymbol();
    }

    Token Lexer::PeekToken()
    {
        size_t tpos = pos_;
        int tline = line_;
        int tcolumn = column_;

        Token res = NextToken();

        pos_ = tpos;
        line_ = tline;
        column_ = tcolumn;

        return res;
    }

    bool Lexer::IsAtEnd() const
    {
        return pos_ >= source_.length();
    }

    char Lexer::CurrentChar() const
    {
        if (IsAtEnd())
        {
            return '\0';
        }
        return source_[pos_];
    }

    char Lexer::PeekChar(int offset) const
    {
        // 显式分两个方向,逻辑不依赖 size_t + int 的隐式转换语义,可读性更好
        if (offset >= 0)
        {
            // 前看:offset 不能超过剩余字符数
            // 防御:即便 offset 大到溢出, pos_ + static_cast<size_t>(offset) 也会在 size_t 域内回绕
            if (pos_ + static_cast<size_t>(offset) >= source_.size())
            {
                return '\0';
            }
            return source_[pos_ + static_cast<size_t>(offset)];
        }
        // 后看:offset < 0,|offset| 不能超过 pos_
        size_t back = static_cast<size_t>(-offset);
        if (back > pos_)
        {
            return '\0';
        }
        return source_[pos_ - back];
    }

    void Lexer::Advance()
    {
        // 防御:已到末尾时再调 Advance 是空操作,避免 pos_ 自增越界(size_t 上溢是 UB)
        if (IsAtEnd())
        {
            return;
        }
        // 先看当前字符再前进,这样能根据"刚被消费的字符"决定行/列怎么变
        if (CurrentChar() == '\n')
        {
            line_++;
            column_ = 1;
        }
        else
        {
            column_++;
        }
        pos_++;
    }

    void Lexer::SkipWhitespaceAndComments()
    {
        while (!IsAtEnd())
        {
            char c = CurrentChar();

            // 1) 普通空白:空格 / Tab / 回车 / 换行
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                Advance();
                continue;
            }

            // 2) 单行注释:从 "--" 开始,直到行尾
            //    PeekChar() 在 EOF 时返回 '\0',不等于 '-',自然不会误判
            if (c == '-' && PeekChar() == '-')
            {
                Advance(); // 吃掉第一个 '-'
                Advance(); // 吃掉第二个 '-'
                while (!IsAtEnd() && CurrentChar() != '\n')
                {
                    Advance();
                }
                continue;
            }

            // 3) 多行注释:从 "/*" 开始,直到 "*/"
            if (c == '/' && PeekChar() == '*')
            {
                Advance(); // 吃掉 '/'
                Advance(); // 吃掉 '*'
                while (!IsAtEnd())
                {
                    if (CurrentChar() == '*' && PeekChar() == '/')
                    {
                        Advance(); // 吃掉 '*'
                        Advance(); // 吃掉 '/'
                        break;
                    }
                    Advance();
                }
                continue;
            }

            // 既不是空白也不是注释,说明遇到了真正的 token 起点,退出循环
            break;
        }
    }

    Token Lexer::ScanIdentifierOrKeyword()
    {
        int startLine = line_;
        int startColumn = column_;
        std::string lexeme;

        // 标识符 = [字母|下划线][字母|数字|下划线]*
        while (!IsAtEnd())
        {
            char c = CurrentChar();
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) || c == '_')
            {
                lexeme.push_back(c);
                Advance();
            }
            else
            {
                break;
            }
        }

        // LookupKeyword 已经处理大小写不敏感:小写 select、大写 SELECT、混合 Select
        // 都会被正确归类为 KEYWORD_SELECT;否则按 IDENTIFIER 返回
        TokenType type = LookupKeyword(lexeme);
        return Token(type, lexeme, startLine, startColumn);
    }

    Token Lexer::ScanNumber()
    {
        int startLine = line_;
        int startColumn = column_;
        std::string lexeme;
        TokenType t = TokenType::INTEGER_LITERAL;

        while (!IsAtEnd())
        {
            char c = CurrentChar();
            if (std::isdigit(static_cast<unsigned char>(c)))
            {
                lexeme.push_back(c);
                Advance();
            }
            else if (c == '.')
            {
                if (!std::isdigit(static_cast<unsigned char>(PeekChar())))
                {
                    break;
                }
                t = TokenType::FLOAT_LITERAL;
                lexeme.push_back(c);
                Advance();
            }
            else
            {
                break;
            }
        }

        return Token(t, lexeme, startLine, startColumn);
    }

    Token Lexer::ScanString()
    {
        int startLine = line_;
        int startColumn = column_;

        std::string lexeme = "'";
        Advance();

        while (!IsAtEnd())
        {
            char c = CurrentChar();

            if (c == '\'')
            {
                if (PeekChar() == '\'')
                {
                    lexeme += "''";
                    Advance();
                    Advance();
                    continue;
                }
                break;
            }

            lexeme.push_back(c);
            Advance();
        }

        // Bug 3:字符串未闭合就遇到 EOF
        if (IsAtEnd())
        {
            return Token(TokenType::UNKNOWN, lexeme, startLine, startColumn);
        }

        lexeme.push_back('\'');
        Advance();

        return Token(TokenType::STRING_LITERAL, lexeme, startLine, startColumn);
    }

    Token Lexer::ScanOperatorOrSymbol()
    {
        // 在消费字符之前先记录位置,这样报错/调试时能指到正确的行列
        int startLine = line_;
        int startColumn = column_;
        char c = CurrentChar();

        switch (c)
        {
            // ---- 双字符运算符:先吃掉首字符,再看第二个字符 ----
        case '<':
            Advance();
            if (CurrentChar() == '=')
            {
                Advance();
                return Token(TokenType::OP_LESS_EQUAL, "<=", startLine, startColumn);
            }
            else if (CurrentChar() == '>')
            {
                Advance();
                return Token(TokenType::OP_NOT_EQUAL, "<>", startLine, startColumn);
            }
            return Token(TokenType::OP_LESS, "<", startLine, startColumn);

        case '>':
            Advance();
            if (CurrentChar() == '=')
            {
                Advance();
                return Token(TokenType::OP_GREATER_EQUAL, ">=", startLine, startColumn);
            }
            return Token(TokenType::OP_GREATER, ">", startLine, startColumn);

        case '=':
            Advance();
            return Token(TokenType::OP_EQUAL, "=", startLine, startColumn);

        case '!':
            Advance();
            if (CurrentChar() == '=')
            {
                Advance();
                return Token(TokenType::OP_NOT_EQUAL, "!=", startLine, startColumn);
            }
            // 单独的 '!' 不合法,但仍要前进避免死循环
            return Token(TokenType::UNKNOWN, "!", startLine, startColumn);

            // ---- 单字符算术运算符 ----
        case '+':
            Advance();
            return Token(TokenType::OP_PLUS, "+", startLine, startColumn);

        case '-':
            // '--' 单行注释应当由 SkipWhitespaceAndComments() 在 dispatch 前吃掉,
            // 这里只会遇到独立的减号
            Advance();
            return Token(TokenType::OP_MINUS, "-", startLine, startColumn);

        case '*':
            Advance();
            return Token(TokenType::OP_STAR, "*", startLine, startColumn);

        case '/':
            Advance();
            return Token(TokenType::OP_SLASH, "/", startLine, startColumn);

            // ---- 分隔符号 ----
        case '(':
            Advance();
            return Token(TokenType::LEFT_PAREN, "(", startLine, startColumn);

        case ')':
            Advance();
            return Token(TokenType::RIGHT_PAREN, ")", startLine, startColumn);

        case ',':
            Advance();
            return Token(TokenType::COMMA, ",", startLine, startColumn);

        case ';':
            Advance();
            return Token(TokenType::SEMICOLON, ";", startLine, startColumn);

        case '.':
            Advance();
            return Token(TokenType::DOT, ".", startLine, startColumn);
        }

        // 理论上不该到这里:NextToken 只在识别到操作符/符号字符时才会调用本函数
        // 万一进来,吃掉当前字符避免死循环,并标记为 UNKNOWN 留给上层报错
        Advance();
        return Token(TokenType::UNKNOWN, std::string(1, c), startLine, startColumn);
    }

    TokenType Lexer::LookupKeyword(const std::string &text) const
    {
        // 函数局部 static:只在首次调用时构造,后续复用同一张表,C++11 起线程安全
        static const std::unordered_map<std::string, TokenType> kKeywords = {
            {"SELECT", TokenType::KEYWORD_SELECT},
            {"FROM", TokenType::KEYWORD_FROM},
            {"WHERE", TokenType::KEYWORD_WHERE},
            {"INSERT", TokenType::KEYWORD_INSERT},
            {"INTO", TokenType::KEYWORD_INTO},
            {"VALUES", TokenType::KEYWORD_VALUES},
            {"UPDATE", TokenType::KEYWORD_UPDATE},
            {"SET", TokenType::KEYWORD_SET},
            {"DELETE", TokenType::KEYWORD_DELETE},
            {"CREATE", TokenType::KEYWORD_CREATE},
            {"TABLE", TokenType::KEYWORD_TABLE},
            {"DROP", TokenType::KEYWORD_DROP},
            {"AND", TokenType::KEYWORD_AND},
            {"OR", TokenType::KEYWORD_OR},
            {"NOT", TokenType::KEYWORD_NOT},
            {"NULL", TokenType::KEYWORD_NULL},
            {"ORDER", TokenType::KEYWORD_ORDER},
            {"BY", TokenType::KEYWORD_BY},
            {"GROUP", TokenType::KEYWORD_GROUP},
            {"HAVING", TokenType::KEYWORD_HAVING},
            {"JOIN", TokenType::KEYWORD_JOIN},
            {"INNER", TokenType::KEYWORD_INNER},
            {"LEFT", TokenType::KEYWORD_LEFT},
            {"RIGHT", TokenType::KEYWORD_RIGHT},
            {"ON", TokenType::KEYWORD_ON},
            {"AS", TokenType::KEYWORD_AS},
            {"DISTINCT", TokenType::KEYWORD_DISTINCT},
            {"LIMIT", TokenType::KEYWORD_LIMIT},
            {"INT", TokenType::KEYWORD_INT},
            {"VARCHAR", TokenType::KEYWORD_VARCHAR},
            {"FLOAT", TokenType::KEYWORD_FLOAT},
            {"PRIMARY", TokenType::KEYWORD_PRIMARY},
            {"KEY", TokenType::KEYWORD_KEY},
        };

        // SQL 关键字大小写不敏感:统一转大写后再查
        // unsigned char 中转 + static_cast 消除 char 为负值时的 UB,并消除 MSVC C4244 警告
        std::string upper(text);
        for (char &c : upper)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        if (auto it = kKeywords.find(upper); it != kKeywords.end())
        {
            return it->second;
        }
        return TokenType::IDENTIFIER;
    }

} // namespace sqlcompiler
