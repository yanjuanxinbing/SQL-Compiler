#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include <cctype>
#include <fstream>
#include <sstream>

#include "ast/AST.h"
#include "db/Database.h"
#include "lexer/Token.h"
#include "plan/Plan.h"

namespace {

// ============ 调试输出模式（Phase 1.5）============
//
// REPL 的三条元命令 \.tokens / \.ast / \.plan 把"最近一次成功通过对应阶段的
// 编译产物"打印到 stdout。这里的三个 Print* 函数只负责格式化输出；底层数据
// （tokens / AST / plan）来自 Database 的 LastTokens / LastAst / LastPlan
// 访问器，由 ExecuteSQL 在各阶段成功后写入。
//
// 输出格式：
//   - PrintTokens：每行一个 token，格式 "[TYPE] 'lexeme' @line:col"。
//                  超过 200 个时截断并追加 "... (N more tokens)" 行。
//   - PrintAst：直接复用 ast::Node::ToString()，再按行输出带首行标题。
//   - PrintPlan：复用 plan::PlanNode::ToString()（与 EXPLAIN 输出同源）；
//                若计划为空（如纯 BEGIN/COMMIT 的 NoOpNode），打印 "(empty plan)"。

// 把任意 ASCII 控制字符（如换行）转义成可见形式，便于在 REPL 中阅读 token
// 的 lexeme。原 \t / \n 等可能让 "sqlcompiler> Error:" 的 prompt 解析器
// 抓不到 prompt 边界。
std::string EscapeForDisplay(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void PrintTokens(const std::vector<sqlcompiler::Token>& tokens) {
    constexpr size_t kMaxPrint = 200;
    std::cout << "[tokens] " << tokens.size() << " token(s):" << std::endl;
    size_t n = std::min(tokens.size(), kMaxPrint);
    for (size_t i = 0; i < n; ++i) {
        const auto& t = tokens[i];
        std::cout << "  [" << sqlcompiler::TokenTypeToString(t.type)
                  << "] '" << EscapeForDisplay(t.lexeme)
                  << "' @line=" << t.line
                  << ":col=" << t.column << std::endl;
    }
    if (tokens.size() > kMaxPrint) {
        std::cout << "  ... (" << (tokens.size() - kMaxPrint)
                  << " more tokens)" << std::endl;
    }
}

void PrintAst(const sqlcompiler::Statement* ast) {
    if (ast == nullptr) {
        std::cout << "[ast] (no statement; the last SQL did not parse)"
                  << std::endl;
        return;
    }
    std::cout << "[ast]" << std::endl;
    // ast::Node::ToString() 已自带换行（SelectStatement 等多行表达式排版），
    // 直接整段输出即可。为视觉对齐加一个缩进。
    std::string s = ast->ToString();
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            std::cout << "  " << s.substr(pos) << std::endl;
            break;
        }
        std::cout << "  " << s.substr(pos, nl - pos) << std::endl;
        pos = nl + 1;
    }
}

void PrintPlan(const sqlcompiler::PlanNode* plan) {
    if (plan == nullptr) {
        std::cout << "[plan] (no plan; the last SQL did not produce an "
                     "optimized plan)"
                  << std::endl;
        return;
    }
    std::cout << "[plan]" << std::endl;
    // plan::PlanNode::ToString() 与 EXPLAIN 同源：每行一个节点（顶层节点无缩进，
    // 每深一层缩进 +2 空格）。直接整段输出即可。
    std::string s = plan->ToString();
    size_t pos = 0;
    while (pos < s.size()) {
        size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            std::cout << "  " << s.substr(pos) << std::endl;
            break;
        }
        std::cout << "  " << s.substr(pos, nl - pos) << std::endl;
        pos = nl + 1;
    }
}

// 判断文本是否仅由空白与 SQL 行注释（-- ...）组成。
// 用于 REPL 跳过完全由注释构成的输入（典型场景：用户以 `-- xxx;` 行注释一段语句），
// 避免底层 parser 返回 nullptr 后误报 "no statement"。
bool IsOnlyCommentsOrWhitespace(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        // 跳过水平空白与换行
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' ||
                                s[i] == '\r' || s[i] == '\n')) {
            ++i;
        }
        if (i >= s.size()) return true;
        // 行注释：吞到下一个换行
        if (i + 1 < s.size() && s[i] == '-' && s[i + 1] == '-') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        return false;
    }
    return true;
}

// 判断缓冲区中是否已出现语句结束符 ';'。
// 必须跳过字符串字面量与行注释，否则 `INSERT ... VALUES ('a;b')` 会被提前截断，
// 而 `-- 注释里的分号;` 会被误认为语句已完整。
// 额外规则：在 CREATE FUNCTION/PROCEDURE 类语句的 BEGIN ... END 块内的 ';'
// 不算语句结束；通过简单扫描文本中的 BEGIN / END 大写关键字计数实现。
bool HasCompleteStatement(const std::string& s) {
    bool in_string = false;
    auto extract_word_upper = [](const std::string& s, size_t i) -> std::string {
        std::string w;
        while (i < s.size() &&
               (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_')) {
            w.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
            ++i;
        }
        return w;
    };
    int begin_depth = 0;
    // 仅当显式看到 CREATE FUNCTION/PROCEDURE 时才启用 BEGIN...END 计数。
    // 这样独立的 BEGIN; 事务语句不会被误识别为函数体起点。
    bool create_fn_seen = false;
    auto upper_contains = [](const std::string& s, const std::string& needle) -> bool {
        if (needle.size() > s.size()) return false;
        for (size_t k = 0; k + needle.size() <= s.size(); ++k) {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j) {
                char a = static_cast<char>(std::toupper(
                    static_cast<unsigned char>(s[k + j])));
                char b = needle[j];
                if (a != b) { match = false; break; }
            }
            if (match) return true;
        }
        return false;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_string) {
            if (c == '\'') {
                if (i + 1 < s.size() && s[i + 1] == '\'') { ++i; continue; }
                in_string = false;
            }
            continue;
        }
        if (c == '\'') { in_string = true; continue; }
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) {
            std::string w = extract_word_upper(s, i);
            // 检测 "CREATE FUNCTION/PROCEDURE" 触发 BEGIN/END 计数。
            if (w == "CREATE" && create_fn_seen == false) {
                // 看后续 token：FUNCTION / PROCEDURE / TRIGGER。
                size_t j = i + w.size();
                while (j < s.size() &&
                       (std::isspace(static_cast<unsigned char>(s[j])))) ++j;
                std::string next = extract_word_upper(s, j);
                if (next == "FUNCTION" || next == "PROCEDURE") {
                    create_fn_seen = true;
                }
            }
            if (create_fn_seen && w == "BEGIN") {
                ++begin_depth;
            } else if (create_fn_seen && w == "END") {
                // END IF / END WHILE / END LOOP / END REPEAT / END CASE 都是
                // 子块的结束，不是函数/过程体的 END。看到 END 后看下一个非空白
                // 关键字：若是上述关键字之一，则跳过。
                size_t j = i + w.size();
                while (j < s.size() &&
                       (std::isspace(static_cast<unsigned char>(s[j])))) ++j;
                std::string next = extract_word_upper(s, j);
                if (next == "IF" || next == "WHILE" || next == "LOOP" ||
                    next == "REPEAT" || next == "CASE") {
                    // 跳过整个 next 词，避免下一次循环再处理它
                    i += w.size() + (j - (i + w.size())) + next.size() - 1;
                    continue;
                }
                if (begin_depth > 0) --begin_depth;
            }
            i += w.size() - 1;
            continue;
        }
        if (c == ';' && begin_depth == 0) {
            // 行尾单独的 ';' 之前的整段若包含 CREATE FUNCTION + END（depth 已
            // 归零），也认作语句完成。否则只信任 depth==0 时退出。
            (void)upper_contains;
            return true;
        }
    }
    return false;
}

// 终端显示宽度（近似）：ASCII 记 1 列，CJK 等宽字符记 2 列，
// UTF-8 续字节不计。用于把结果集对齐成表格。
size_t DisplayWidth(const std::string& s) {
    size_t width = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        // 3 字节及以上的 UTF-8 大多落在 CJK / 全角区间，按 2 列计
        width += (len >= 3) ? 2 : 1;
        i += len;
    }
    return width;
}

void PadTo(const std::string& s, size_t width) {
    std::cout << s;
    for (size_t w = DisplayWidth(s); w < width; ++w) std::cout << ' ';
}

void PrintResult(const sqlcompiler::ExecutionResult& result) {
    if (!result.success) {
        // 52_data_types: 先 flush stdout 确保 prompt 落地，再让 cerr 单独写
        // 到新的一行。否则当 stdout 缓冲 + stderr 无缓冲时，错误行可能拼接到
        // "sqlcompiler> " 之后，触发测试驱动里的 "^sqlcompiler> Error:" 模
        // 式误判。这里额外在 cerr 行首加 '\n' 是最后兜底：即使 OS 层把两个
        // write() 合并成一行，也会把错误切到独立行。
        std::cout.flush();
        std::cerr << "\nError: " << result.message << std::endl;
        return;
    }
    if (result.column_names.empty()) {
        std::cout << result.message << std::endl;
        return;
    }

    const size_t ncols = result.column_names.size();
    // 先扫一遍算出每列的显示宽度，再统一输出表头/分隔线/数据行，
    // 否则分隔线宽度与数据宽度对不上（原实现固定输出 3 个 '-'）。
    std::vector<size_t> widths(ncols);
    for (size_t i = 0; i < ncols; ++i) {
        widths[i] = DisplayWidth(result.column_names[i]);
    }
    std::vector<std::vector<std::string>> cells;
    cells.reserve(result.rows.size());
    for (const auto& row : result.rows) {
        std::vector<std::string> line;
        line.reserve(ncols);
        for (size_t i = 0; i < ncols; ++i) {
            std::string text = (i < row.ColumnCount()) ? row.GetValue(i).ToString() : "";
            widths[i] = std::max(widths[i], DisplayWidth(text));
            line.push_back(std::move(text));
        }
        cells.push_back(std::move(line));
    }

    for (size_t i = 0; i < ncols; ++i) {
        if (i) std::cout << " | ";
        PadTo(result.column_names[i], widths[i]);
    }
    std::cout << std::endl;
    for (size_t i = 0; i < ncols; ++i) {
        if (i) std::cout << "-+-";
        std::cout << std::string(widths[i], '-');
    }
    std::cout << std::endl;
    for (const auto& line : cells) {
        for (size_t i = 0; i < ncols; ++i) {
            if (i) std::cout << " | ";
            PadTo(line[i], widths[i]);
        }
        std::cout << std::endl;
    }
    std::cout << "(" << result.rows.size()
              << (result.rows.size() == 1 ? " row)" : " rows)") << std::endl;
}

// 把 .sql 脚本里的全部语句按 ';' 切分，逐条交给 Database 执行。
// 失败时打印错误并继续（与 REPL 行为一致），不中断后续语句。
// 返回 true 表示文件被成功打开（即使里面所有语句都失败）。
//
// 切分策略：与 REPL 复用 HasCompleteStatement() —— 逐字符累积到 buffer，
// 当 buffer 出现"语句已完整"信号时整段 trim 后执行、清空 buffer 继续。
// 文本末尾若没有 ';'，会作为最后一条语句兜底执行。
//
// 复杂度：HasCompleteStatement 自身是 O(|buffer|)，整体 O(N²)。对教学用的
// 脚本（KB 级）足够快；工业级可换成单趟扫描的 statement splitter。
bool RunScriptFile(sqlcompiler::Database* database, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "Error: cannot open script file '" << path << "'" << std::endl;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();

    auto run_one = [&](const std::string& buf) -> bool {
        size_t a = buf.find_first_not_of(" \t\r\n");
        size_t b = buf.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return false;
        std::string trimmed = buf.substr(a, b - a + 1);
        if (trimmed.empty() || IsOnlyCommentsOrWhitespace(trimmed)) return false;
        auto result = database->ExecuteSQL(trimmed);
        PrintResult(result);
        return true;
    };

    std::string buffer;
    size_t statements_run = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        buffer.push_back(content[i]);
        if (HasCompleteStatement(buffer)) {
            if (run_one(buffer)) ++statements_run;
            buffer.clear();
        }
    }
    // 兜底：文件末尾可能没有 ';'，把残留 buffer 也跑掉。
    if (!buffer.empty()) {
        if (run_one(buffer)) ++statements_run;
    }
    std::cout << "[script] " << path << ": ran " << statements_run
              << " statement(s)" << std::endl;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // ---- 参数解析 ----
    // 优先级：
    //   1) `-f <file>` / `--file <file>` / `--source <file>` / `-f=<file>`
    //      → 批量执行该脚本文件后退出（不进入 REPL）。可与 DB 文件混用。
    //   2) 否则第一个非 flag 参数视为数据库文件路径（向后兼容旧行为）。
    //   3) 都没有就用默认 "sqlcompiler.db"。
    std::string db_file = "sqlcompiler.db";
    std::vector<std::string> script_files;  // 多个 -f 依次执行
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto take_next = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << flag << " requires a file argument"
                          << std::endl;
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-f" || a == "--file" || a == "--source") {
            script_files.push_back(take_next(a));
        } else if (a.rfind("-f=", 0) == 0) {
            script_files.push_back(a.substr(3));
        } else if (a == "-h" || a == "--help") {
            std::cout << "Usage: sqlcompiler [db_file] [-f script.sql ...]\n"
                      << "  db_file         Path to the database file "
                      << "(default: sqlcompiler.db)\n"
                      << "  -f <file>       Run <file> as a SQL script, then "
                      << "exit (repeatable)\n"
                      << "  --file, --source   Aliases for -f\n"
                      << "Inside the REPL you can also run: .source <file> "
                      << "(or .read <file>)\n";
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "Error: unknown option '" << a << "'" << std::endl;
            return 2;
        } else {
            // 第一个非 flag 参数 → 数据库文件
            db_file = a;
        }
    }

    sqlcompiler::Database* database = nullptr;
    try {
        database = new sqlcompiler::Database(db_file);
    } catch (const std::exception& e) {
        std::cerr << "Failed to open database '" << db_file << "': " << e.what()
                  << std::endl;
        std::cerr << "Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    // ---- 批处理模式：跑完脚本直接退出 ----
    // REPL 帮助信息在 REPL 内打印；批处理模式只输出脚本自身的执行结果。
    if (!script_files.empty()) {
        int rc = 0;
        for (const auto& f : script_files) {
            if (!RunScriptFile(database, f)) rc = 1;
        }
        database->Shutdown();
        delete database;
        return rc;
    }

    std::string line;
    std::string sql;
    std::cout << "sqlcompiler> " << std::flush;
    // Phase 1.5：首次启动时打印一行帮助，让用户知道有调试元命令可用。
    // 启动 banner 之前已在外层输出"sqlcompiler> "，这里再追加一行避免覆盖 prompt。
    std::cout << "Meta-commands: \\.tokens, \\.ast, \\.plan "
              << " (show last statement's debug info)" << std::endl;
    std::cout << "Meta-commands: .source <file>  (or .read <file>) - "
              << "load and execute SQL script" << std::endl;
    std::cout << "sqlcompiler> " << std::flush;
    while (std::getline(std::cin, line)) {
        // ---- Phase 1.5: 元命令就地处理 ----
        // 调试元命令不写库、不需要 ';'，也不需要拼到 sql 累加器里。
        // 当 sql 累加器为空（即上一条语句已完整消化）且本行就是元命令时，
        // 直接处理并跳过常规 ExecuteSQL 路径。
        // 注意：累加器非空时不处理，避免用户在多行 SQL 中误打 "\.tokens"
        // 整段作为字面字符串被截断解析。
        if (sql.empty()) {
            // 复制行做 trim（不去改原 line，下一轮循环还要用）。
            std::string cmd = line;
            size_t ca = cmd.find_first_not_of(" \t\r\n");
            size_t cb = cmd.find_last_not_of(" \t\r\n");
            std::string trimmed_cmd =
                (ca == std::string::npos) ? std::string()
                                          : cmd.substr(ca, cb - ca + 1);
            if (trimmed_cmd == R"(\.tokens)") {
                PrintTokens(database->LastTokens());
                std::cout << "sqlcompiler> " << std::flush;
                continue;
            }
            if (trimmed_cmd == R"(\.ast)") {
                PrintAst(database->LastAst());
                std::cout << "sqlcompiler> " << std::flush;
                continue;
            }
            if (trimmed_cmd == R"(\.plan)") {
                PrintPlan(database->LastPlan());
                std::cout << "sqlcompiler> " << std::flush;
                continue;
            }
            // 脚本加载元命令：`.source <file>` / `.read <file>` / 同名带反斜杠。
            // 接受两种风格（带或不带前导 `\`）以贴合 MySQL/PostgreSQL 习惯。
            auto try_source = [&](const std::string& trigger) -> bool {
                if (trimmed_cmd.rfind(trigger, 0) != 0) return false;
                std::string rest = trimmed_cmd.substr(trigger.size());
                size_t ra = rest.find_first_not_of(" \t\r\n");
                size_t rb = rest.find_last_not_of(" \t\r\n");
                if (ra == std::string::npos) {
                    std::cerr << "Error: " << trigger
                              << " requires a file argument" << std::endl;
                    std::cout << "sqlcompiler> " << std::flush;
                    return true;
                }
                std::string path = rest.substr(ra, rb - ra + 1);
                RunScriptFile(database, path);
                std::cout << "sqlcompiler> " << std::flush;
                return true;
            };
            if (try_source(".source ") || try_source(".read ") ||
                try_source("\\.source ") || try_source("\\.read ")) {
                continue;
            }
        }

        sql += line;
        sql += "\n";
        if (!HasCompleteStatement(sql)) {
            // 纯注释/空行不应进入"续行"状态，否则脚本开头的注释会刷出一串
            // "       -> " 提示，干扰输出。
            if (IsOnlyCommentsOrWhitespace(sql)) {
                sql.clear();
                std::cout << "sqlcompiler> " << std::flush;
            } else {
                std::cout << "       -> " << std::flush;
            }
            continue;
        }
        // Trim
        std::string trimmed = sql;
        size_t a = trimmed.find_first_not_of(" \t\r\n");
        size_t b = trimmed.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) {
            trimmed = trimmed.substr(a, b - a + 1);
        }
        if (trimmed == "exit;" || trimmed == "quit;" || trimmed == "exit" || trimmed == "quit") {
            break;
        }
        // 跳过纯注释输入：避免 parser 因 lexer 把整行都吃掉后返回 nullptr
        // 而误报 "no statement"（典型例子：测试文件中以 `-- INSERT ...;` 形式
        // 注释掉的语句）。
        if (IsOnlyCommentsOrWhitespace(trimmed)) {
            sql.clear();
            std::cout << "sqlcompiler> " << std::flush;
            continue;
        }
        auto result = database->ExecuteSQL(trimmed);
        PrintResult(result);
        sql.clear();
        std::cout << "sqlcompiler> " << std::flush;
    }
    database->Shutdown();
    delete database;
    return 0;
}