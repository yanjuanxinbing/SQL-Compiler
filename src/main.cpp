#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "ast/AST.h"
#include "db/Database.h"
#include "lexer/Token.h"
#include "plan/Plan.h"

namespace {

// ============ 调试输出模式（Phase 1.5）============
//
// REPL 的四条元命令 \.tokens / \.ast / \.plan / \.optimized 把"最近一次成功
// 通过对应阶段的编译产物"打印到 stdout。这里的四个 Print* 函数只负责格式化
// 输出；底层数据（tokens / AST / plan / 优化前 plan 文本）来自 Database 的
// LastTokens / LastAst / LastPlan / LastPlanBeforeOptText 访问器，由 ExecuteSQL
// 在各阶段成功后写入。
//
// 输出格式：
//   - PrintTokens：每行一个 token，格式 "[TYPE] 'lexeme' @line:col"。
//                  超过 200 个时截断并追加 "... (N more tokens)" 行。
//   - PrintAst：直接复用 ast::Node::ToString()，再按行输出带首行标题。
//   - PrintPlanBeforeOpt：复用 Database 缓存的优化前 ToString() 文本；
//                         空文本时说明「最近一条 SQL 没有产生可优化计划」。
//   - PrintPlan：复用 plan::PlanNode::ToString()（与 EXPLAIN 输出同源），即
//                执行器真正跑的优化后版本；若指针为空（如纯 BEGIN/COMMIT 的
//                NoOpNode），打印 "(no plan)"。

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

// 打印一行带缩进的文本：用于 plan 树的格式化输出。空文本走 "(empty ...)"
// 兜底提示。label 在头部方括号里用作分类标签，方便用户区分 \.plan vs
// \.optimized；body 是已经预先渲染好的 ToString() 输出。
void PrintIndentedBlock(const std::string& label, const std::string& body,
                        const std::string& empty_msg) {
    if (body.empty()) {
        std::cout << "[" << label << "] " << empty_msg << std::endl;
        return;
    }
    std::cout << "[" << label << "]" << std::endl;
    // plan::PlanNode::ToString() 与 EXPLAIN 同源：每行一个节点（顶层节点无缩
    // 进，每深一层缩进 +2 空格）。直接整段逐行输出并加 2 空格视觉缩进。
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        if (nl == std::string::npos) {
            std::cout << "  " << body.substr(pos) << std::endl;
            break;
        }
        std::cout << "  " << body.substr(pos, nl - pos) << std::endl;
        pos = nl + 1;
    }
}

// \.plan：显示最近一次成功语句的「优化前」计划。文本快照由
// Database::ExecuteSQL 在调用 Optimizer::Optimize 之前一次性 ToString() 写
// 入，因此对 PredicatePushDown 的就地修改免疫。
void PrintPlanBeforeOpt(const std::string& text) {
    PrintIndentedBlock(
        "plan-before-opt", text,
        "(no plan; the last SQL did not produce a plan before optimization)");
}

// \.optimized：显示最近一次成功语句的「优化后」计划。指针为空时说明最近的
// SQL 不走计划-优化路径（典型如纯 BEGIN/COMMIT 的 NoOpNode，或 EXPLAIN 之类
// 在 EXPLAIN 节点内嵌子计划的语句）。
void PrintPlan(const sqlcompiler::PlanNode* plan) {
    if (plan == nullptr) {
        std::cout << "[plan-optimized] (no plan; the last SQL did not "
                     "produce an optimized plan)" << std::endl;
        return;
    }
    PrintIndentedBlock("plan-optimized", plan->ToString(), "");
}

// ============ 调试输出模式（Phase 1.5 + WebUI 可视化）============
//
// WebUI 的 `/api/query/debug` 端点用 `--debug-output` 调用引擎，并把
// `[DEBUG_JSON_START]…[DEBUG_JSON_END]` 信封解析成可视化数据
// （tokens / AST / plan / 存储统计 / 替换日志）。Python 端已经在
// `engine._assemble_multistatement_debug` 等位置按以下 JSON 契约
// 解析（见 webui/backend/schemas.py 的 DebugData / StorageStats /
// ReplacementEntry）：
//
//   {
//     "tokens":          [{"type":"…","lexeme":"…","line":N,"col":N}, …],
//     "ast_text":        "<ast::Node::ToString() 的多行文本>",
//     "plan_before_opt": "<优化前 PlanNode::ToString() 的多行文本>",
//     "plan_json":       "<优化后 PlanNode::ToString() 的多行文本>",
//     "storage_stats":   {"hit_count":N,"miss_count":N,
//                         "replacement_count":N,"hit_rate":0.0..1.0,
//                         "total_pages":N},
//     "replacement_log": [{"evicted":N,"loaded":N,"dirty":bool}, …]
//   }
//
// 信封必须落在**一行**内（Python 端 regex 在 `[DEBUG_JSON_START]` 与
// `[DEBUG_JSON_END]` 之间截取），因此所有字符串值都通过 JsonEscape 把
// 真正的换行转成 `\n`，信封之间只用单个 `\n` 分隔。引擎的
// `Database::ExecuteSQL` 在每次成功后已经把上面要拿的数据（LastTokens /
// LastAst / LastPlan / LastPlanBeforeOptText）缓存好；我们只需要
// 在这里统一格式化输出。
//
// 复用现有 API（不要重新发明）：
//   - Database::LastTokens / LastAst / LastPlan / LastPlanBeforeOptText
//   - BufferPoolManager::GetStats() → BufferPoolStats + HitRate()
//   - BufferPoolManager::GetReplacementLog() → vector<ReplacementLogEntry>
//   - DiskManager::GetNumPages()

// 把任意字符串按 JSON 字符串字面量的规则转义。我们手写而不引入
// nlohmann/json 是因为项目目前零第三方依赖；这里只覆盖会出现在
// tokens / AST / plan 文本里的字符（控制字符、"、\、换行）。
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char uc : s) {
        switch (uc) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (uc < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
                    out += buf;
                } else {
                    out += static_cast<char>(uc);
                }
                break;
        }
    }
    return out;
}

// 把数字格式化成有限精度的字符串。hit_rate 等浮点数在落 JSON 前
// 用此函数序列化，避免 ostringstream 默认格式带来的本地化 / 精度
// 不一致问题。
std::string JsonNumber(double d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", d);
    // 去掉无意义的尾随 0（保持紧凑，例如 0.5 而不是 0.500000）。
    std::string s(buf);
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last_nonzero = s.find_last_not_of('0');
        if (last_nonzero != std::string::npos && last_nonzero > dot) {
            s.erase(last_nonzero + 1);
        } else {
            // 全是 0（例如 1.000000）→ 保留一个小数点便于前端解析
            s.erase(dot + 2);
        }
    }
    return s;
}

// 把 page_id_t（int32_t）以 JSON 数字字面量写入。INVALID_PAGE_ID
// (-1) 在我们的语义里表示 "无页 / 自由帧"，直接序列化为 -1
// 比用 null 更便于前端做聚合统计。
void EmitJsonNumber(std::ostringstream& os, long n) {
    os << n;
}

// 在 stdout 上输出一个调试信封。调用方负责控制 `debug_output` 开关
// 与调用时机（仅在 --debug-output 模式下、且最近一条 ExecuteSQL
// 编译成功后调用）。
//
// 即使 LastAst / LastPlan 为空（典型场景：词法成功但语义失败、或
// 纯 DDL/DML 但优化器没改写），仍要发出 envelope —— 前端把
// "空 plan_json" 解读为 "该语句没有可优化计划"，且 storage_stats
// 仍然有价值。
void EmitDebugEnvelope(sqlcompiler::Database* db) {
    using namespace sqlcompiler;
    std::ostringstream os;

    os << "{";

    // --- tokens ---
    os << "\"tokens\":[";
    const auto& toks = db->LastTokens();
    for (size_t i = 0; i < toks.size(); ++i) {
        const auto& t = toks[i];
        if (i) os << ",";
        os << "{\"type\":\"" << JsonEscape(TokenTypeToString(t.type))
           << "\",\"lexeme\":\"" << JsonEscape(t.lexeme)
           << "\",\"line\":" << t.line
           << ",\"col\":" << t.column << "}";
    }
    os << "],";

    // --- ast_text ---
    const Statement* ast = db->LastAst();
    os << "\"ast_text\":\""
       << JsonEscape(ast ? ast->ToString() : std::string()) << "\",";

    // --- plan_before_opt（优化前的快照文本；Database 已缓存） ---
    os << "\"plan_before_opt\":\""
       << JsonEscape(db->LastPlanBeforeOptText()) << "\",";

    // --- plan_json（优化后的计划 = EXPLAIN 真正跑的那一份） ---
    const PlanNode* opt = db->LastPlan();
    os << "\"plan_json\":\""
       << JsonEscape(opt ? opt->ToString() : std::string()) << "\",";

    // --- storage_stats ---
    auto* bpm = db->GetBufferPoolManager();
    auto* dm  = db->GetDiskManager();
    const auto& st = bpm->GetStats();
    long total_pages = dm ? static_cast<long>(dm->GetNumPages()) : 0;
    os << "\"storage_stats\":{"
       << "\"hit_count\":"         << st.hit_count        << ","
       << "\"miss_count\":"        << st.miss_count       << ","
       << "\"replacement_count\":" << st.replacement_count<< ","
       << "\"hit_rate\":"          << JsonNumber(st.HitRate()) << ","
       << "\"total_pages\":"       << total_pages
       << "},";

    // --- replacement_log ---
    os << "\"replacement_log\":[";
    const auto& log = bpm->GetReplacementLog();
    for (size_t i = 0; i < log.size(); ++i) {
        const auto& e = log[i];
        if (i) os << ",";
        os << "{\"evicted\":" << static_cast<long>(e.evicted_page_id)
           << ",\"loaded\":"  << static_cast<long>(e.loaded_page_id)
           << ",\"dirty\":"   << (e.evicted_was_dirty ? "true" : "false")
           << "}";
    }
    os << "]";

    os << "}";

    // 单行输出：Python 端 regex 截取 [START]…[END]，要求信封内不含
    // 真换行。JsonEscape 已经把字符串值里的换行转成 \n，这里只写
    // 一个 `\n` 作为信封的结束符。
    std::cout << "[DEBUG_JSON_START]" << os.str() << "[DEBUG_JSON_END]\n";
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

// item #4/#6: 增量式语句边界检测。
//
// 原来 HasCompleteStatement 每次调用都重新扫描整个 buffer：N 行脚本累积
// buffer 长度 O(N²)，每次 HasCompleteStatement 又把 buffer 全扫一次 →
// O(N³) 总开销。这里把状态提取到 ReplLineParser：
//   * in_string / create_fn_seen / begin_depth 仅在新行上更新；
//   * FeedLine 单次调用 O(line.length())；
//   * RunScriptFile 与 REPL 各持有一个 ReplLineParser 实例；REPL 在
//     ExecuteSQL 后调用 Reset()，RunScriptFile 局部变量天然只活到函数返回。
//
// 语义与原 HasCompleteStatement 一致：跳过字符串字面量与行注释；
// CREATE FUNCTION/PROCEDURE 体内 BEGIN/END 嵌套深度内的 ';' 不算语句结束。
struct ReplLineParser {
    bool in_string = false;
    bool create_fn_seen = false;
    int begin_depth = 0;

    void Reset() {
        in_string = false;
        create_fn_seen = false;
        begin_depth = 0;
    }

    bool FeedLine(const std::string& line) {
        auto extract_word_upper = [](const std::string& s, size_t i) -> std::string {
            std::string w;
            while (i < s.size() &&
                   (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_')) {
                w.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(s[i]))));
                ++i;
            }
            return w;
        };
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (in_string) {
                if (c == '\'') {
                    if (i + 1 < line.size() && line[i + 1] == '\'') { ++i; continue; }
                    in_string = false;
                }
                continue;
            }
            if (c == '\'') { in_string = true; continue; }
            if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
                while (i < line.size() && line[i] != '\n') ++i;
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c))) {
                std::string w = extract_word_upper(line, i);
                // 检测 "CREATE FUNCTION/PROCEDURE" 触发 BEGIN/END 计数。
                if (!create_fn_seen && w == "CREATE") {
                    size_t j = i + w.size();
                    while (j < line.size() &&
                           std::isspace(static_cast<unsigned char>(line[j]))) ++j;
                    std::string next = extract_word_upper(line, j);
                    if (next == "FUNCTION" || next == "PROCEDURE") {
                        create_fn_seen = true;
                    }
                }
                if (create_fn_seen && w == "BEGIN") {
                    ++begin_depth;
                } else if (create_fn_seen && w == "END") {
                    // END IF / END WHILE / END LOOP / END REPEAT / END CASE 都是
                    // 子块的结束，不是函数/过程体的 END。看到 END 后看下一个非
                    // 空白关键字：若是上述关键字之一，则跳过。
                    size_t j = i + w.size();
                    while (j < line.size() &&
                           std::isspace(static_cast<unsigned char>(line[j]))) ++j;
                    std::string next = extract_word_upper(line, j);
                    if (next == "IF" || next == "WHILE" || next == "LOOP" ||
                        next == "REPEAT" || next == "CASE") {
                        i += w.size() + (j - (i + w.size())) + next.size() - 1;
                        continue;
                    }
                    if (begin_depth > 0) --begin_depth;
                }
                i += w.size() - 1;
                continue;
            }
            if (c == ';' && begin_depth == 0) {
                return true;
            }
        }
        return false;
    }
};

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
// 切分策略：按行扫描（行内仍走 ReplLineParser::FeedLine 检测 ';'）。这样能让
// `.tables` / `.exit` / `\.tokens` 等不带 ';' 的元命令/调试命令被识别为
// 行边界,而不是被错误拼到下一条 SQL 之后——之前的实现会把 `.tables\nINSERT`
// 整体交给解析器,因 `.` 报错而**静默丢弃**后续 INSERT 的数据(行数显示 OK
// 但表里少一条)。现在把元命令作为独立 ExecuteSQL 调用执行,失败仅影响该行,
// 不再污染相邻 SQL,与 REPL 的"累加器非空时不处理元命令"语义一致。
//
// 复杂度：ReplLineParser::FeedLine 单行 O(line.length())，buffer append 预
// reserve 后单次摊销 O(1)；整体 O(L)（L = 总字符数）。
//
// debug_output：true 时每条 ExecuteSQL 成功执行后，在 stdout 输出一个
// [DEBUG_JSON_START]…[DEBUG_JSON_END] 信封供 WebUI 解析（见 EmitDebugEnvelope
// 注释）。失败语句不输出信封 —— Python 端在 _assemble_multistatement_debug
// 里把它当作"空 plan_json"处理，前端的可视化面板对该 block 只渲染 Storage
// 这一块就够了。
bool RunScriptFile(sqlcompiler::Database* database, const std::string& path,
                   bool debug_output = false) {
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
        // 仅在编译/执行成功时输出信封。失败留给 stderr 的 "Error:" 提示，
        // 前端在 _split_blocks 里已经把空 stdout + stderr "Error:" 配对到
        // 正确的 block。
        if (debug_output && result.success) {
            EmitDebugEnvelope(database);
        }
        return true;
    };

    std::string buffer;
    ReplLineParser line_parser;  // item #4/#6: 局部状态，每条完整语句后 Reset。
    size_t statements_run = 0;
    auto flush_sql_buffer = [&]() {
        if (buffer.empty()) return false;
        bool ran = run_one(buffer);
        buffer.clear();
        line_parser.Reset();  // 与语句边界同步：state 从零开始累计下一条。
        return ran;
    };

    // 按行处理：把 `.tables` / `\.tokens` 等以 '.' 或 '\\' 开头的行识别为
    // 元命令/调试命令边界（仅当 SQL 累加器为空或只含注释/空白时——与 REPL
    // "累加器非空时不处理元命令"语义对齐,避免误把多行 SQL 中的同名行截断）。
    std::istringstream line_stream(content);
    std::string line;
    while (std::getline(line_stream, line)) {
        size_t a = line.find_first_not_of(" \t\r\n");
        size_t b = line.find_last_not_of(" \t\r\n");
        std::string trimmed_line =
            (a == std::string::npos) ? std::string()
                                       : line.substr(a, b - a + 1);
        const bool is_meta_command =
            !trimmed_line.empty() &&
            (trimmed_line.front() == '.' || trimmed_line.front() == '\\');
        const bool buffer_has_content =
            !buffer.empty() && !IsOnlyCommentsOrWhitespace(buffer);

        if (is_meta_command && !buffer_has_content) {
            // 元命令边界：先清空 SQL buffer（一般已是空），再独立执行这一行。
            // 即使 `.tables` 之类未识别而报错，也只影响本行,不会污染后续 SQL。
            // 注意：'\' 开头会被 Database::ExecuteSQL 当作 \crash 调试指令
            // (Database.cpp IsCrashDebugCommand),遇到未知 \xxx 直接 _Exit(1);
            // 这是 Phase B 调试注入的设计,保持原行为,不在 RunScriptFile 兜底。
            (void)flush_sql_buffer();
            if (run_one(line)) ++statements_run;
            continue;
        }

        // item #5: 预 reserve 一次性分配，避免 += 触发多次 realloc。
        buffer.reserve(buffer.size() + line.size() + 1);
        buffer += line;
        buffer += "\n";
        if (line_parser.FeedLine(line)) {
            if (flush_sql_buffer()) ++statements_run;
        }
    }
    // 兜底：文件末尾若残留 buffer（无 ';'），也跑掉。
    if (flush_sql_buffer()) ++statements_run;
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
    bool debug_output = false;              // --debug-output：每条语句后输出调试信封
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
        } else if (a == "--debug-output") {
            // WebUI 的可视化端点（POST /api/query/debug）依赖此 flag；
            // 见 EmitDebugEnvelope 注释了解信封格式。
            debug_output = true;
        } else if (a == "-h" || a == "--help") {
            std::cout << "Usage: sqlcompiler [db_file] [-f script.sql ...]\n"
                      << "  db_file         Path to the database file "
                      << "(default: sqlcompiler.db)\n"
                      << "  -f <file>       Run <file> as a SQL script, then "
                      << "exit (repeatable)\n"
                      << "  --file, --source   Aliases for -f\n"
                      << "  --debug-output  Emit a JSON envelope per "
                      << "statement (WebUI visualization)\n"
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
            if (!RunScriptFile(database, f, debug_output)) rc = 1;
        }
        database->Shutdown();
        delete database;
        return rc;
    }

    std::string line;
    std::string sql;
    ReplLineParser line_parser;  // item #4: REPL 持久化的解析状态。
    std::cout << "sqlcompiler> " << std::flush;
    // Phase 1.5：首次启动时打印一行帮助，让用户知道有调试元命令可用。
    // 启动 banner 之前已在外层输出"sqlcompiler> "，这里再追加一行避免覆盖 prompt。
    std::cout << "Meta-commands: \\.tokens, \\.ast, \\.plan, \\.optimized "
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
            // 2b: 未识别的以 '.' / '\\' 开头的行(典型如 .tables / .schema)
            // 也作为独立的 ExecuteSQL 调用执行 —— 仅触发当行的语法错误,
            // 不会污染 sql 累加器,让下一行 INSERT 等独立处理。否则
            // `.tables\nINSERT INTO nosuch ...;` 会被 Lexer 整体解析,
            // 在 '.' 处报 Syntax,而 INSERT 的 TableNotFound 被吞掉。
            // 已知 \xxx 会被 Database::IsCrashDebugCommand 当作 \crash
            // 调试指令 _Exit(1),保持原行为,不在 REPL 兜底。
            //
            // 顺序：先匹配已识别的元命令(\.tokens / \.ast / \.plan /
            // \.optimized / .source / .read 等),否则以 '.' / '\' 开头的行
            // 回退到单行 ExecuteSQL,避免误把已识别的调试命令当未知元命令
            // 丢给 parser。
            //
            // \.plan 与 \.optimized 的语义切分：
            //   \.plan        —— 显示 Optimizer 改写前的原始计划树（Planner 直
            //                   出），用来观察优化器到底改了什么。
            //   \.optimized   —— 显示 Optimizer 改写后的计划树，也是执行器真
            //                   正跑的那一份。
            // 两者由 Database 在 ExecuteSQL 中分别缓存（前者一次性 ToString
            // 成文本，规避 in-place 修改），调用方判空即可。
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
                PrintPlanBeforeOpt(database->LastPlanBeforeOptText());
                std::cout << "sqlcompiler> " << std::flush;
                continue;
            }
            if (trimmed_cmd == R"(\.optimized)") {
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
                RunScriptFile(database, path, debug_output);
                std::cout << "sqlcompiler> " << std::flush;
                return true;
            };
            if (try_source(".source ") || try_source(".read ") ||
                try_source("\\.source ") || try_source("\\.read ")) {
                continue;
            }
            // 兜底：未识别的以 '.' / '\\' 开头的行——独立 ExecuteSQL,失败仅影响当行。
            auto run_single_line = [&](const std::string& single_line) {
                size_t a = single_line.find_first_not_of(" \t\r\n");
                size_t b = single_line.find_last_not_of(" \t\r\n");
                if (a == std::string::npos) return;
                std::string trimmed = single_line.substr(a, b - a + 1);
                if (trimmed.empty() || IsOnlyCommentsOrWhitespace(trimmed)) return;
                auto result = database->ExecuteSQL(trimmed);
                PrintResult(result);
            };
            if (!trimmed_cmd.empty() &&
                (trimmed_cmd.front() == '.' || trimmed_cmd.front() == '\\')) {
                run_single_line(cmd);
                std::cout << "sqlcompiler> " << std::flush;
                continue;
            }
        }

        // item #5: 预 reserve 一次性分配，避免 += 触发多次 realloc。
        sql.reserve(sql.size() + line.size() + 1);
        sql += line;
        sql += "\n";
        if (!line_parser.FeedLine(line)) {
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
            line_parser.Reset();
            std::cout << "sqlcompiler> " << std::flush;
            continue;
        }
        auto result = database->ExecuteSQL(trimmed);
        PrintResult(result);
        sql.clear();
        line_parser.Reset();  // 与下一条语句对齐：从干净状态开始累计。
        std::cout << "sqlcompiler> " << std::flush;
    }
    database->Shutdown();
    delete database;
    return 0;
}