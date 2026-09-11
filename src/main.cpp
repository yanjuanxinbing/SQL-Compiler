#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include <cctype>

#include "db/Database.h"

namespace {

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
                // END IF / END WHILE 是 IF / WHILE 块的结束，不是函数体的 END。
                // 看到 END 后看下一个非空白关键字：若是 IF / WHILE，则跳过。
                size_t j = i + w.size();
                while (j < s.size() &&
                       (std::isspace(static_cast<unsigned char>(s[j])))) ++j;
                std::string next = extract_word_upper(s, j);
                if (next == "IF" || next == "WHILE") {
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
        std::cerr << "Error: " << result.message << std::endl;
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

}  // namespace

int main(int argc, char** argv) {
    std::string db_file = (argc > 1) ? argv[1] : "sqlcompiler.db";

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

    std::string line;
    std::string sql;
    std::cout << "sqlcompiler> " << std::flush;
    while (std::getline(std::cin, line)) {
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