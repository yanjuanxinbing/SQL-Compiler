#include "execution/ExpressionEvaluator.h"

#include "catalog/SystemCatalog.h"
#include "common/DateTime.h"
#include "common/Error.h"
#include "execution/ExecutionEngine.h"
#include "execution/Executor.h"
#include "execution/UdfExecutor.h"
#include "plan/Plan.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <functional>
#include <memory>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

namespace sqlcompiler {

namespace {

Value MakeBool(bool b) {
    return Value::MakeInt(b ? 1 : 0);
}

bool IsTruthy(const Value& v) {
    if (v.IsNull()) return false;
    if (v.GetType() == ValueType::INTEGER) return v.AsInt() != 0;
    if (v.GetType() == ValueType::FLOAT) return v.AsFloat() != 0.0;
    if (v.GetType() == ValueType::VARCHAR) return !v.AsVarchar().empty();
    return false;
}

// SQL 三值逻辑：v 是否确定为 TRUE。NULL 不视为 true。
bool IsTrue(const Value& v) {
    if (v.IsNull()) return false;
    return IsTruthy(v);
}

// SQL 三值逻辑：v 是否确定为 FALSE。NULL 不视为 false。
bool IsFalse(const Value& v) {
    if (v.IsNull()) return false;
    return !IsTruthy(v);
}

// =============================================================================
// 模式匹配支持面（44_pattern_match / 56_pattern）
//
//   - SIMILAR TO       : SQL:1999 模式语法（see TranslateSqlLikeToEre）
//   - LIKE             : SQL 标准 LIKE，'%' 任意序列、'_' 单字符；默认以 '\\'
//                       作为转义字符，也可通过 LikeExprNode 的 has_escape/
//                       escape_char 自定义。
//   - ILIKE            : 同 LIKE，但先对输入和模式做 ASCII 大小写折叠再匹配。
//   - REGEXP / RLIKE   : POSIX ERE 风格的正则子串匹配（默认不锚定 ^/$）；
//                       模式按 <regex> 在每行即时编译（pattern 表达式非字面量
//                       时无法预编译，统一在运行时编译一次以保持简单）。
//                       ERE 单词字符类 `\d \D \s \S \w \W` 走 TranslateEreWordClasses
//                       转译为对应字符类（std::regex 的 ECMAScript 也支持这些，
//                       但我们的转译在两种语法下结果一致，并提供后备保证）。
//
//   - 错误处理        : REGEXP/SIMILAR TO 模式非法时抛出 RuntimeError，
//                       文案为 "Error: invalid regex pattern: <reason>"。
//
// 旧 `BinaryOperator::LIKE` 路径复用 MatchLikePattern（保留 '\\' 默认转义），
// 行为对 00–55 测试零变更。新运算符和 ESCAPE 子句全部走 LikeExprNode。
// =============================================================================

// SQL LIKE pattern matching: '%' matches any sequence, '_' matches one char.
// Other characters are matched literally. '\' escapes the next character.
bool MatchLikePattern(const std::string& s, const std::string& p) {
    size_t i = 0, j = 0;
    size_t star_i = std::string::npos, star_j = 0;
    while (i < s.size()) {
        if (j < p.size() && (p[j] == '_' ||
            (p[j] == '\\' && j + 1 < p.size() && (p[j + 1] == '_' || p[j + 1] == '%')))) {
            if (p[j] == '\\') ++j;
            ++i;
            ++j;
        } else if (j < p.size() && p[j] == '%') {
            star_i = i;
            star_j = j;
            ++j;
        } else if (j < p.size() && p[j] == '\\' && j + 1 < p.size()) {
            if (p[j + 1] == s[i]) { ++i; j += 2; }
            else if (star_i != std::string::npos) {
                i = ++star_i;
                j = star_j + 1;
            } else {
                return false;
            }
        } else if (j < p.size() && p[j] == s[i]) {
            ++i; ++j;
        } else if (star_i != std::string::npos) {
            i = ++star_i;
            j = star_j + 1;
        } else {
            return false;
        }
    }
    while (j < p.size() && p[j] == '%') ++j;
    return j == p.size();
}

// 可配置转义字符的 LIKE 匹配：'esc' 取消紧随字符的特殊含义（包括其自身）。
// 算法与 MatchLikePattern 等价的二维回溯，但把 '\\' 换成 esc，便于 ILIKE
// 在大小写折叠后直接复用。
bool MatchLikePatternEscaped(const std::string& s, const std::string& p, char esc) {
    size_t i = 0, j = 0;
    size_t star_i = std::string::npos, star_j = 0;
    while (i < s.size()) {
        if (j < p.size() && p[j] == '_') {
            ++i; ++j;
        } else if (j < p.size() && p[j] == '%') {
            star_i = i;
            star_j = j;
            ++j;
        } else if (j < p.size() && p[j] == esc && j + 1 < p.size()) {
            // 转义：把后一个字符当字面量匹配。
            if (p[j + 1] == s[i]) { ++i; j += 2; }
            else if (star_i != std::string::npos) {
                i = ++star_i;
                j = star_j + 1;
            } else {
                return false;
            }
        } else if (j < p.size() && p[j] == s[i]) {
            ++i; ++j;
        } else if (star_i != std::string::npos) {
            i = ++star_i;
            j = star_j + 1;
        } else {
            return false;
        }
    }
    while (j < p.size() && p[j] == '%') ++j;
    return j == p.size();
}

// 56_pattern: SQL:1999 SIMILAR TO 模式 → POSIX ERE。
//
// 输入: SQL pattern（保留 LIKE 通配符 % / _ 与 ERE 元字符 . * + ? [] () | ^ $ {}）。
// 输出: 可直接喂给 std::regex（ECMAScript 语法与 ERE 在这些元字符上语义相近）的
//       ERE 等价字符串。
//
// 算法：单次扫描 pattern，遇到：
//   - escape + 任意字符   : 把后一个字符视为字面量；若它同时是 ERE 元字符，
//                            在前面再加 '\' 以保证 ERE 把它当字面量读。
//                            例（esc='\\'）: `\\.` → ERE 里的 `\\.`（字面量 '.'）；
//                                            `\\%` → ERE 里的 `%`（字面量 '%'）。
//   - '%'                  : 输出 '.*'
//   - '_'                  : 输出 '.'
//   - 其他（ERE 元字符 / 普通字符）: 原样输出 —— SQL:1999 让 . * + ? | () []
//                                    ^ $ {} 在 SIMILAR TO 中按 ERE 元字符
//                                    解释；SQL 标准本身不把它们视为字面量。
//
// 注：SQL 标准的 SIMILAR TO 不要求锚定 ^/$（与 REGEXP/RLIKE 一致），因此
//     翻译结果不做任何锚定处理。
std::string TranslateSqlLikeToEre(const std::string& p, char esc) {
    std::string out;
    out.reserve(p.size() * 2);
    auto is_ere_metachar = [](char c) -> bool {
        switch (c) {
            case '.': case '*': case '+': case '?':
            case '|': case '(': case ')':
            case '[': case ']': case '{': case '}':
            case '^': case '$': case '\\':
                return true;
            default:
                return false;
        }
    };
    auto emit_literal = [&](char c) {
        if (is_ere_metachar(c)) out.push_back('\\');
        out.push_back(c);
    };
    for (size_t i = 0; i < p.size(); ++i) {
        char c = p[i];
        if (c == esc) {
            // SQL 转义：后一个字符视为字面量。若已是模式末尾，则保留 esc 自身。
            if (i + 1 < p.size()) {
                emit_literal(p[i + 1]);
                ++i;
            } else {
                emit_literal(c);
            }
            continue;
        }
        if (c == '%') { out += ".*"; continue; }
        if (c == '_') { out += ".";  continue; }
        // ERE 元字符或普通字符：原样输出。SQL 标准让 ERE 元字符在 SIMILAR
        // TO 模式中按 ERE 元字符解释，因此不做额外转义。
        out.push_back(c);
    }
    return out;
}

// 56_pattern: ERE 单词字符类翻译（保守后援）。
//
// std::regex 的 ECMAScript 语法本来就支持 \d \D \s \S \w \W（与 POSIX ERE
// 行为一致）。我们仍对输入 pattern 做一次扫描：当遇到 \<wordchar> 形式时，
// 替换为显式字符类（\d → [0-9] 等），目的：
//   1) 与 POSIX ERE 语义对齐（即便 ECMAScript 后端被换成其他引擎也能正常工作）；
//   2) 保持语义文档化。
// 注意只翻译 \d \D \s \S \w \W 这六个单词类；其他 \<x>（如 \. \* \( 等）不动，
// 以免破坏已有的转义语义。
std::string TranslateEreWordClasses(const std::string& p) {
    std::string out;
    out.reserve(p.size() * 2);
    for (size_t i = 0; i < p.size(); ++i) {
        if (p[i] == '\\' && i + 1 < p.size()) {
            char nx = p[i + 1];
            switch (nx) {
                case 'd': out += "[0-9]";              ++i; continue;
                case 'D': out += "[^0-9]";             ++i; continue;
                case 's': out += "[ \t\n\r\f\v]";      ++i; continue;
                case 'S': out += "[^ \t\n\r\f\v]";     ++i; continue;
                case 'w': out += "[A-Za-z0-9_]";       ++i; continue;
                case 'W': out += "[^A-Za-z0-9_]";      ++i; continue;
                default:
                    // 其他 \<x> 原样保留（含 \\ \. \* \( 等）。
                    out.push_back(p[i]);
                    out.push_back(nx);
                    ++i;
                    continue;
            }
        }
        out.push_back(p[i]);
    }
    return out;
}

// ASCII 小写折叠（仅 A-Z）。
std::string AsciiLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}

// 函数名规范化：大小写不敏感
std::string UpperName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

// 把 Value 规范化为 double（INT 提升为 FLOAT 的实际值）
double ValueToDouble(const Value& v) {
    if (v.GetType() == ValueType::INTEGER) return static_cast<double>(v.AsInt());
    if (v.GetType() == ValueType::FLOAT) return v.AsFloat();
    return 0.0;
}

// 把 Value 取整（截断向零）
int32_t ValueToIntTruncate(const Value& v) {
    if (v.GetType() == ValueType::INTEGER) return v.AsInt();
    if (v.GetType() == ValueType::FLOAT) {
        double d = v.AsFloat();
        if (d >= 0) return static_cast<int32_t>(d);
        return static_cast<int32_t>(std::ceil(d));
    }
    if (v.GetType() == ValueType::VARCHAR) {
        // 字符串转整数（参考 std::atoi）
        try {
            size_t pos = 0;
            std::string s = v.AsVarchar();
            // 去除首尾空白
            while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
            int sign = 1;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
                if (s[pos] == '-') sign = -1;
                ++pos;
            }
            int64_t acc = 0;
            bool any = false;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
                acc = acc * 10 + (s[pos] - '0');
                ++pos;
                any = true;
            }
            if (!any) return 0;
            int64_t r = acc * sign;
            if (r > 2147483647LL) return 2147483647;
            if (r < -2147483648LL) return -2147483648;
            return static_cast<int32_t>(r);
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

double ValueToFloat(const Value& v) {
    if (v.GetType() == ValueType::FLOAT) return v.AsFloat();
    if (v.GetType() == ValueType::INTEGER) return static_cast<double>(v.AsInt());
    if (v.GetType() == ValueType::VARCHAR) {
        try {
            return std::stod(v.AsVarchar());
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

std::string TrimWhitespace(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 解析形如 "YYYY-MM-DD" / "YYYY-MM-DD HH:MM:SS" / "YYYY/MM/DD" 等格式，
// 返回是否成功以及 year/month/day
// 纯 C++ 实现，避免 MSVC 的 sscanf 弃用警告
bool ParseDateString(const std::string& s, int* year, int* month, int* day) {
    if (s.empty()) return false;
    auto read_int = [&](size_t& pos) -> int {
        int sign = 1;
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
            if (s[pos] == '-') sign = -1;
            ++pos;
        }
        int64_t acc = 0;
        bool any = false;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            acc = acc * 10 + (s[pos] - '0');
            ++pos;
            any = true;
        }
        if (!any) return 0;
        return static_cast<int>(acc * sign);
    };
    auto expect = [&](size_t& pos, char ch) -> bool {
        if (pos < s.size() && s[pos] == ch) { ++pos; return true; }
        return false;
    };
    size_t pos = 0;
    int y = read_int(pos);
    if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos - 1 >= 0 ? s.size() - 1 : 0]))) {
        // first read_int failed
        return false;
    }
    // 必须紧跟分隔符
    bool dash = (pos < s.size() && s[pos] == '-');
    bool slash = (pos < s.size() && s[pos] == '/');
    if (!dash && !slash) return false;
    ++pos;
    int mo = read_int(pos);
    if (pos >= s.size() || (s[pos] != '-' && s[pos] != '/')) return false;
    ++pos;
    int d = read_int(pos);
    *year = y; *month = mo; *day = d;
    return true;
}

}  // namespace

ExpressionEvaluator::ExpressionEvaluator(
    const std::unordered_map<std::string, size_t>& column_index_map)
    : column_index_map_(column_index_map), ctx_(nullptr) {
    // 1-arg constructor：ctx/outer_bind/proc_locals 均为 nullptr。
}

ExpressionEvaluator::ExpressionEvaluator(
    const std::unordered_map<std::string, size_t>& column_index_map,
    ExecutionContext* ctx,
    const std::unordered_map<std::string, Value>* outer_bind)
    : column_index_map_(column_index_map), ctx_(ctx), outer_bind_(outer_bind) {
    // 调用方未显式提供 outer_bind 时，回退到 ExecutionContext 上的绑定（相关子查询传播）。
    if (!outer_bind_ && ctx_) outer_bind_ = ctx_->GetOuterBind();
    // 59_procs (Category 8): 同样回退到 procedure 当前局部变量绑定。
    // proc_locals 优先级低于 outer_bind：当 outer_bind 为空时，列引用优先
    // 解析为 procedure 局部变量；否则按 outer_bind 解析。
    if (!proc_locals_ && ctx_) proc_locals_ = ctx_->GetProcLocals();
}

Value ExpressionEvaluator::Evaluate(const ExprPtr& expr, const Tuple& tuple) const {
    if (!expr) return Value::MakeNull();
    switch (expr->GetType()) {
        case NodeType::LITERAL_EXPR:
            return EvaluateLiteral(*static_cast<const LiteralExpr*>(expr.get()));
        case NodeType::COLUMN_REF_EXPR:
            return EvaluateColumnRef(*static_cast<const ColumnRefExpr*>(expr.get()), tuple);
        case NodeType::BINARY_EXPR:
            return EvaluateBinary(*static_cast<const BinaryExpr*>(expr.get()), tuple);
        case NodeType::UNARY_EXPR:
            return EvaluateUnary(*static_cast<const UnaryExpr*>(expr.get()), tuple);
        case NodeType::FUNCTION_CALL_EXPR:
            return EvaluateFunctionCall(*static_cast<const FunctionCallExpr*>(expr.get()), tuple);
        case NodeType::CASE_EXPR:
            return EvaluateCase(*static_cast<const CaseExprNode*>(expr.get()), tuple);
        case NodeType::CAST_EXPR:
            return EvaluateCast(*static_cast<const CastExprNode*>(expr.get()), tuple);
        case NodeType::SUBQUERY_EXPR:
            return EvaluateSubquery(*static_cast<const SubqueryExprNode*>(expr.get()), tuple);
        case NodeType::UPSERT_VALUES_REF_EXPR:
            // 43_upsert: ON DUPLICATE KEY UPDATE 的赋值右侧出现的 VALUES(col)。
            // 该值在 UpsertExecutor 里通过 ExecutionContext 推入，本 evaluator 仅
            // 负责按列名取出。不在该上下文中时返回 NULL（语义层会报错）。
            return EvaluateUpsertValuesRef(*static_cast<const UpsertValuesRefExpr*>(expr.get()),
                                           tuple);
        case NodeType::LIKE_EXPR:
            // 44_pattern_match: LIKE / ILIKE / REGEXP / RLIKE + 可选 ESCAPE。
            return EvaluateLike(*static_cast<const LikeExprNode*>(expr.get()), tuple);
        case NodeType::EXTRACT_EXPR:
            // 45_datetime: EXTRACT(field FROM source)
            return EvaluateExtract(*static_cast<const ExtractExprNode*>(expr.get()), tuple);
        case NodeType::INTERVAL_EXPR:
            // 45_datetime: INTERVAL <n> <unit> —— 单独出现没有意义；
            // 这里返回一个零值（NULL）。正常路径上 INTERVAL 总是作为
            // INTERVAL_ADD / INTERVAL_SUB 的右操作数被消费。
            return EvaluateInterval(*static_cast<const IntervalExprNode*>(expr.get()), tuple);
        case NodeType::NEXTVAL_EXPR:
            // 53_ddl: NEXTVAL FOR sequence_name —— 原子推进并返回当前值。
            return EvaluateNextval(*static_cast<const NextvalExpr*>(expr.get()));
        default:
            return Value::MakeNull();
    }
}

Value ExpressionEvaluator::EvaluateLiteral(const LiteralExpr& expr) const {
    switch (expr.literal_type) {
        case LiteralType::INTEGER:
            return Value::MakeInt(static_cast<int32_t>(std::atoi(expr.value.c_str())));
        case LiteralType::FLOAT:
            return Value::MakeFloat(std::atof(expr.value.c_str()));
        case LiteralType::STRING:
            return Value::MakeVarchar(expr.value);
        case LiteralType::NULL_VALUE:
            return Value::MakeNull();
        case LiteralType::BOOLEAN:
            return MakeBool(expr.value != "0" && expr.value != "false" && expr.value != "FALSE");
        // 45_datetime: DATE / TIMESTAMP 字面量按文本持久化（YYYY-MM-DD /
        // YYYY-MM-DD HH:MM:SS）；运行期仍然作为 VARCHAR 流转，ExtractField
        // / ApplyInterval 在需要时按字符串解释。
        case LiteralType::DATE:
        case LiteralType::TIMESTAMP:
            return Value::MakeVarchar(expr.value);
    }
    return Value::MakeNull();
}

Value ExpressionEvaluator::EvaluateColumnRef(const ColumnRefExpr& expr,
                                              const Tuple& tuple) const {
    // 限定列引用（如 u.id / users.id）优先按 "qualifier.column_name" 精确查找，
    // 找不到时回退到大小写不敏感扫描；仍未命中则退化为裸列名查找。
    auto find_ci = [&](const std::string& key)
        -> std::unordered_map<std::string, size_t>::const_iterator {
        auto it = column_index_map_.find(key);
        if (it != column_index_map_.end()) return it;
        std::string lc;
        lc.reserve(key.size());
        for (char c : key) {
            lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        for (auto it2 = column_index_map_.begin(); it2 != column_index_map_.end(); ++it2) {
            std::string kc;
            kc.reserve(it2->first.size());
            for (char c : it2->first) {
                kc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            if (kc == lc) return it2;
        }
        return column_index_map_.end();
    };
    // 子查询求值上下文：当 qualifier 非空且不在内层表集合中时，
    // 该限定列必定引用外层（相关子查询），应优先回退到 outer_bind 而非裸列名。
    bool qualifier_is_outer = false;
    if (!expr.table_name.empty() && ctx_) {
        const std::unordered_set<std::string>* inner_tables = ctx_->GetInnerTables();
        if (!inner_tables) {
            // 55_query: LATERAL 路径下 ApplyExecutor 会注册「lateral inner tables」，
            // 即使 EvaluateSubquery 没被调用，evaluator 也能识别哪些表名属于右子计划。
            inner_tables = ctx_->GetLateralInnerTables();
        }
        if (inner_tables && inner_tables->find(expr.table_name) == inner_tables->end()) {
            qualifier_is_outer = true;
        }
    }
    if (qualifier_is_outer && outer_bind_) {
        std::string qk = expr.table_name + "." + expr.column_name;
        auto ob = outer_bind_->find(qk);
        if (ob != outer_bind_->end()) return ob->second;
        // 大小写不敏感回退
        std::string lc;
        lc.reserve(qk.size());
        for (char c : qk) lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        for (auto& kv : *outer_bind_) {
            std::string kc;
            kc.reserve(kv.first.size());
            for (char c : kv.first) kc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (kc == lc) return kv.second;
        }
        // 55_query: LATERAL 派生表别名（如 `sub`）不属于外层表也不属于内层表，
        // 但在 outer_bind 里也没记录——它对应的是 Apply 右子计划的输出列。
        // 这种情况下应回退到 column_index_map_ 的常规 cmap 查找。
        // 若外层表别名（如 `t1` 在 LATERAL subquery 内的 from_table）也未在
        // outer_bind 中（极少见的"用户引用了 outer 的列但 subquery 不引用"情形），
        // 同样需要回退。
    }
    if (!expr.table_name.empty()) {
        std::string qkey = expr.table_name + "." + expr.column_name;
        auto itq = find_ci(qkey);
        if (itq != column_index_map_.end() && itq->second < tuple.ColumnCount()) {
            return tuple.GetValue(itq->second);
        }
        // 限定列未命中时仍尝试未限定列名查找（保持宽恕语义，避免 alias 拼错
        // 把整列静默 NULL 化）。某些执行路径（如 HAVING 中的聚合重写）传入
        // 空 qualifier 的 ColumnRefExpr，自然走下面的 fallback 路径。
    }
    auto it = find_ci(expr.column_name);
    if (it == column_index_map_.end()) {
        // 相关子查询回退：到外层行绑定中找同名列。
        if (outer_bind_) {
            auto ob = outer_bind_->find(expr.column_name);
            if (ob != outer_bind_->end()) return ob->second;
            // 也尝试限定形式 (qualifier.col)
            if (!expr.table_name.empty()) {
                std::string qk = expr.table_name + "." + expr.column_name;
                ob = outer_bind_->find(qk);
                if (ob != outer_bind_->end()) return ob->second;
            }
            // 大小写不敏感再试一次
            std::string lc;
            lc.reserve(expr.column_name.size());
            for (char c : expr.column_name) lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            for (auto& kv : *outer_bind_) {
                std::string kc;
                kc.reserve(kv.first.size());
                for (char c : kv.first) kc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                if (kc == lc) return kv.second;
            }
        }
        // 59_procs (Category 8): procedure 局部变量回退。在 outer_bind
        // 未命中时，若当前处于 CALL 上下文，ColumnRef 可解析为 procedure
        // 局部变量名（参数或 DECLARE 变量）。
        if (proc_locals_) {
            auto pl = proc_locals_->find(expr.column_name);
            if (pl != proc_locals_->end()) return pl->second;
        }
        return Value::MakeNull();
    }
    if (it->second >= tuple.ColumnCount()) {
        return Value::MakeNull();
    }
    return tuple.GetValue(it->second);
}

Value ExpressionEvaluator::EvaluateBinary(const BinaryExpr& expr, const Tuple& tuple) const {
    // 45_datetime: INTERVAL_ADD / INTERVAL_SUB 的右操作数是 IntervalExprNode，
    // 不需要对它求值（数据在 AST 节点上），而是直接用节点本身。
    Value l = Evaluate(expr.left, tuple);
    Value r = Value::MakeNull();
    bool right_is_interval = (expr.op == BinaryOperator::INTERVAL_ADD ||
                              expr.op == BinaryOperator::INTERVAL_SUB) &&
                              expr.right &&
                              expr.right->GetType() == NodeType::INTERVAL_EXPR;
    if (!right_is_interval) {
        r = Evaluate(expr.right, tuple);
    }
    // 混合运算时把INTEGER操作数提升为double：
    // AsFloat()对INTEGER值返回的是内部float_val_（恒为0），直接使用会得到错误结果
    auto ToDouble = [](const Value& v) -> double {
        if (v.GetType() == ValueType::INTEGER) return static_cast<double>(v.AsInt());
        return v.AsFloat();
    };
    switch (expr.op) {
        case BinaryOperator::ADD: {
            // SQL 三值逻辑：算术任一操作数为 NULL 则结果为 NULL。
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                return Value::MakeFloat(ToDouble(l) + ToDouble(r));
            }
            return Value::MakeInt(l.AsInt() + r.AsInt());
        }
        case BinaryOperator::SUB: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                return Value::MakeFloat(ToDouble(l) - ToDouble(r));
            }
            return Value::MakeInt(l.AsInt() - r.AsInt());
        }
        case BinaryOperator::MUL: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                return Value::MakeFloat(ToDouble(l) * ToDouble(r));
            }
            return Value::MakeInt(l.AsInt() * r.AsInt());
        }
        case BinaryOperator::DIV: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                double rv = ToDouble(r);
                if (rv == 0.0) return Value::MakeNull();
                return Value::MakeFloat(ToDouble(l) / rv);
            }
            int32_t rv = r.AsInt();
            if (rv == 0) return Value::MakeNull();
            return Value::MakeInt(l.AsInt() / rv);
        }
        case BinaryOperator::CONCAT: {
            // SQL标准：任一操作数为NULL则结果为NULL；非字符串操作数按其文本形式连接
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            return Value::MakeVarchar(l.ToString() + r.ToString());
        }
        case BinaryOperator::EQUAL: {
            // 任一为 NULL：UNKNOWN（用 NULL 表示）
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            int c = Value::Compare(l, r);
            return MakeBool(c == 0);
        }
        case BinaryOperator::NOT_EQUAL: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            int c = Value::Compare(l, r);
            return MakeBool(c != 0);
        }
        case BinaryOperator::LESS: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            int c = Value::Compare(l, r);
            return MakeBool(c < 0);
        }
        case BinaryOperator::LESS_EQUAL: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            int c = Value::Compare(l, r);
            return MakeBool(c <= 0);
        }
        case BinaryOperator::GREATER: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            int c = Value::Compare(l, r);
            return MakeBool(c > 0);
        }
        case BinaryOperator::GREATER_EQUAL: {
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            int c = Value::Compare(l, r);
            return MakeBool(c >= 0);
        }
        case BinaryOperator::AND: {
            // SQL 三值逻辑 AND：任一为 FALSE 则 FALSE；都为 TRUE 则 TRUE；否则 UNKNOWN。
            if (IsFalse(l) || IsFalse(r)) return MakeBool(false);
            if (IsTrue(l) && IsTrue(r)) return MakeBool(true);
            return Value::MakeNull();
        }
        case BinaryOperator::OR: {
            // SQL 三值逻辑 OR：任一为 TRUE 则 TRUE；都为 FALSE 则 FALSE；否则 UNKNOWN。
            if (IsTrue(l) || IsTrue(r)) return MakeBool(true);
            if (IsFalse(l) && IsFalse(r)) return MakeBool(false);
            return Value::MakeNull();
        }
        case BinaryOperator::IS_NULL:
            return MakeBool(l.IsNull());
        case BinaryOperator::IS_NOT_NULL:
            return MakeBool(!l.IsNull());
        // 45_datetime: <date_or_ts> ± INTERVAL <n> <unit>
        // 左侧求值为日期/时间字符串（DATE/TIMESTAMP 列或字面量），右侧是
        // IntervalExprNode 节点。语义见 include/common/DateTime.h 顶部注释。
        case BinaryOperator::INTERVAL_ADD:
        case BinaryOperator::INTERVAL_SUB: {
            if (l.IsNull()) return Value::MakeNull();
            if (!expr.right || expr.right->GetType() != NodeType::INTERVAL_EXPR) {
                return Value::MakeNull();
            }
            auto ie = std::static_pointer_cast<IntervalExprNode>(expr.right);
            int64_t base_epoch = 0;
            if (!ParseDateTime(l.ToString(), &base_epoch)) return Value::MakeNull();
            int sign = (expr.op == BinaryOperator::INTERVAL_ADD) ? 1 : -1;
            int64_t new_epoch = 0;
            if (!ApplyInterval(base_epoch, ie->count,
                               static_cast<IntervalUnit>(ie->unit),
                               sign, &new_epoch)) {
                return Value::MakeNull();
            }
            // 输出格式：若原值只到日（10 字符且无时间分量），输出 DATE 形式；
            // 否则输出 TIMESTAMP 形式。判断方式：原值长度 == 10 表示 YYYY-MM-DD。
            if (l.GetType() == ValueType::VARCHAR &&
                l.AsVarchar().size() == 10 &&
                l.AsVarchar().find(' ') == std::string::npos &&
                l.AsVarchar().find('T') == std::string::npos &&
                l.AsVarchar().find(':') == std::string::npos) {
                return Value::MakeVarchar(FormatDate(new_epoch));
            }
            return Value::MakeVarchar(FormatDateTime(new_epoch));
        }
        case BinaryOperator::LIKE: {
            // LIKE 任一边为 NULL 则 UNKNOWN
            if (l.IsNull() || r.IsNull()) return Value::MakeNull();
            if (l.GetType() != ValueType::VARCHAR || r.GetType() != ValueType::VARCHAR) {
                return MakeBool(false);
            }
            return MakeBool(MatchLikePattern(l.AsVarchar(), r.AsVarchar()));
        }
        case BinaryOperator::IN_LIST: {
            // right is a FunctionCallExpr("__IN_LIST__", [...values])
            if (!expr.right || expr.right->GetType() != NodeType::FUNCTION_CALL_EXPR) {
                return MakeBool(false);
            }
            // 左操作数为 NULL → UNKNOWN（永远不匹配）
            if (l.IsNull()) return Value::MakeNull();
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr.right);
            bool has_null = false;
            for (const auto& v : fc->arguments) {
                Value vv = Evaluate(v, tuple);
                if (vv.IsNull()) { has_null = true; continue; }
                if (Value::Compare(l, vv) == 0) return MakeBool(true);
            }
            // 没找到非 NULL 匹配：若有 NULL 在列表中则为 UNKNOWN，否则 FALSE。
            return has_null ? Value::MakeNull() : MakeBool(false);
        }
        case BinaryOperator::BETWEEN: {
            if (!expr.right || expr.right->GetType() != NodeType::FUNCTION_CALL_EXPR) {
                return MakeBool(false);
            }
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr.right);
            if (fc->arguments.size() != 2) return MakeBool(false);
            Value low = Evaluate(fc->arguments[0], tuple);
            Value high = Evaluate(fc->arguments[1], tuple);
            // 任一边为 NULL → UNKNOWN
            if (l.IsNull() || low.IsNull() || high.IsNull()) return Value::MakeNull();
            return MakeBool(Value::Compare(l, low) >= 0 &&
                            Value::Compare(l, high) <= 0);
        }
    }
    return Value::MakeNull();
}

Value ExpressionEvaluator::EvaluateUnary(const UnaryExpr& expr, const Tuple& tuple) const {
    Value v = Evaluate(expr.operand, tuple);
    switch (expr.op) {
        case UnaryOperator::NOT:
            // SQL 三值逻辑 NOT：NULL → NULL；其余按位取反。
            if (v.IsNull()) return Value::MakeNull();
            return MakeBool(!IsTruthy(v));
        case UnaryOperator::NEGATE:
            if (v.IsNull()) return Value::MakeNull();
            if (v.GetType() == ValueType::FLOAT) return Value::MakeFloat(-v.AsFloat());
            return Value::MakeInt(-v.AsInt());
    }
    return Value::MakeNull();
}

Value ExpressionEvaluator::EvaluateCase(const CaseExprNode& expr, const Tuple& tuple) const {
    // 搜索式 CASE（subject 为空）：逐 WHEN 求值其布尔谓词
    if (!expr.subject) {
        for (const auto& w : expr.whens) {
            Value cond = Evaluate(w.when_expr, tuple);
            if (!cond.IsNull() && IsTruthy(cond)) {
                return Evaluate(w.then_expr, tuple);
            }
        }
    } else {
        // 简单 CASE：subject 与每个 WHEN 值进行 NULL-safe equal 比较
        Value subj = Evaluate(expr.subject, tuple);
        if (!subj.IsNull()) {
            for (const auto& w : expr.whens) {
                Value v = Evaluate(w.when_expr, tuple);
                // SQL NULL 安全等值：任一为 NULL 视为不匹配（不命中则继续）
                if (!v.IsNull() && Value::Compare(subj, v) == 0) {
                    return Evaluate(w.then_expr, tuple);
                }
            }
        } else {
            // subject 为 NULL 时仍需要逐 WHEN 求值以保持副作用一致性，
            // 但都不匹配。若某 WHEN 值亦为 NULL 也算匹配（NULL = NULL）。
            // SQL 标准里 `CASE NULL WHEN NULL THEN ...` 会进入 THEN。
            for (const auto& w : expr.whens) {
                Value v = Evaluate(w.when_expr, tuple);
                if (v.IsNull()) {
                    return Evaluate(w.then_expr, tuple);
                }
            }
        }
    }
    if (expr.else_expr) return Evaluate(expr.else_expr, tuple);
    return Value::MakeNull();
}

Value ExpressionEvaluator::EvaluateCast(const CastExprNode& expr, const Tuple& tuple) const {
    Value v = Evaluate(expr.expr, tuple);
    if (v.IsNull()) return Value::MakeNull();
    std::string target = UpperName(expr.target_type);
    if (target == "INT" || target == "INTEGER" || target == "BIGINT") {
        if (v.GetType() == ValueType::INTEGER) return v;
        if (v.GetType() == ValueType::FLOAT) {
            double d = v.AsFloat();
            if (d >= 2147483648.0) return Value::MakeInt(2147483647);
            if (d <= -2147483649.0) return Value::MakeInt(-2147483648);
            // 截断向零（与 SQL 风格一致）
            return Value::MakeInt(static_cast<int32_t>(d));
        }
        if (v.GetType() == ValueType::VARCHAR) {
            return Value::MakeInt(ValueToIntTruncate(v));
        }
        return Value::MakeNull();
    }
    if (target == "FLOAT" || target == "DOUBLE" || target == "DECIMAL") {
        if (v.GetType() == ValueType::FLOAT) return v;
        if (v.GetType() == ValueType::INTEGER) {
            return Value::MakeFloat(static_cast<double>(v.AsInt()));
        }
        if (v.GetType() == ValueType::VARCHAR) {
            try {
                return Value::MakeFloat(std::stod(v.AsVarchar()));
            } catch (...) {
                return Value::MakeNull();
            }
        }
        return Value::MakeNull();
    }
    if (target == "VARCHAR" || target == "STRING" || target == "TEXT" || target == "CHAR") {
        return Value::MakeVarchar(v.ToString());
    }
    return Value::MakeNull();
}

Value ExpressionEvaluator::EvaluateFunctionCall(const FunctionCallExpr& expr,
                                                  const Tuple& tuple) const {
    // 函数名大小写不敏感：统一转大写比较
    std::string name = UpperName(expr.function_name);
    if (name == "*" || name == "STAR") {
        return Value::MakeInt(1);
    }
    // COALESCE(a, b, c, ...) — 返回首个非 NULL 参数
    if (name == "COALESCE" || name == "IFNULL") {
        for (const auto& a : expr.arguments) {
            Value v = Evaluate(a, tuple);
            if (!v.IsNull()) return v;
        }
        return Value::MakeNull();
    }
    // NULLIF(a, b) — a==b 返回 NULL，否则返回 a
    if (name == "NULLIF") {
        if (expr.arguments.size() != 2) return Value::MakeNull();
        Value a = Evaluate(expr.arguments[0], tuple);
        Value b = Evaluate(expr.arguments[1], tuple);
        if (a.IsNull() || b.IsNull()) return a;  // NULLIF(a, NULL) 返回 a；NULLIF(NULL, b) 返回 NULL
        return Value::Compare(a, b) == 0 ? Value::MakeNull() : a;
    }
    // ---- 字符串函数 ----
    if (name == "UPPER") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        std::string s = v.ToString();
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return Value::MakeVarchar(std::move(s));
    }
    if (name == "LOWER") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        std::string s = v.ToString();
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return Value::MakeVarchar(std::move(s));
    }
    if (name == "LENGTH" || name == "LEN" || name == "CHAR_LENGTH") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        return Value::MakeInt(static_cast<int32_t>(v.ToString().size()));
    }
    if (name == "SUBSTR" || name == "SUBSTRING") {
        if (expr.arguments.size() < 2 || expr.arguments.size() > 3) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        Value start = Evaluate(expr.arguments[1], tuple);
        if (start.IsNull()) return Value::MakeNull();
        int32_t start_idx = start.AsInt();
        std::string s = v.ToString();
        // SQL SUBSTR 是 1-based
        size_t begin = (start_idx > 0)
            ? static_cast<size_t>(start_idx - 1)
            : 0;
        if (begin > s.size()) return Value::MakeVarchar("");
        if (expr.arguments.size() == 3) {
            Value len = Evaluate(expr.arguments[2], tuple);
            if (len.IsNull()) return Value::MakeNull();
            int32_t L = len.AsInt();
            if (L < 0) L = 0;
            size_t take = std::min(static_cast<size_t>(L), s.size() - begin);
            return Value::MakeVarchar(s.substr(begin, take));
        }
        return Value::MakeVarchar(s.substr(begin));
    }
    if (name == "TRIM") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        return Value::MakeVarchar(TrimWhitespace(v.ToString()));
    }
    if (name == "REPLACE") {
        if (expr.arguments.size() != 3) return Value::MakeNull();
        Value sv = Evaluate(expr.arguments[0], tuple);
        Value fv = Evaluate(expr.arguments[1], tuple);
        Value tv = Evaluate(expr.arguments[2], tuple);
        if (sv.IsNull() || fv.IsNull() || tv.IsNull()) return Value::MakeNull();
        const std::string& s = sv.ToString();
        const std::string& from = fv.ToString();
        const std::string& to = tv.ToString();
        if (from.empty()) return Value::MakeVarchar(s);
        std::string out;
        out.reserve(s.size());
        size_t pos = 0;
        while (pos < s.size()) {
            size_t hit = s.find(from, pos);
            if (hit == std::string::npos) {
                out.append(s, pos, std::string::npos);
                break;
            }
            out.append(s, pos, hit - pos);
            out.append(to);
            pos = hit + from.size();
        }
        return Value::MakeVarchar(std::move(out));
    }
    // ---- 数学函数 ----
    if (name == "ROUND") {
        if (expr.arguments.empty() || expr.arguments.size() > 2) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        int n = 0;
        if (expr.arguments.size() == 2) {
            Value nv = Evaluate(expr.arguments[1], tuple);
            if (nv.IsNull()) return Value::MakeNull();
            n = nv.AsInt();
        }
        double x = ValueToDouble(v);
        double factor = std::pow(10.0, n);
        double r = std::round(x * factor) / factor;
        if (n == 0) return Value::MakeInt(static_cast<int32_t>(r));
        return Value::MakeFloat(r);
    }
    if (name == "CEIL" || name == "CEILING") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        double x = ValueToDouble(v);
        return Value::MakeInt(static_cast<int32_t>(std::ceil(x)));
    }
    if (name == "FLOOR") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        double x = ValueToDouble(v);
        return Value::MakeInt(static_cast<int32_t>(std::floor(x)));
    }
    if (name == "ABS") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        if (v.GetType() == ValueType::FLOAT) return Value::MakeFloat(std::fabs(v.AsFloat()));
        int32_t i = v.AsInt();
        if (i == INT32_MIN) {
            // |INT32_MIN| 溢出 INT32，提升为 FLOAT
            return Value::MakeFloat(static_cast<double>(i));
        }
        return Value::MakeInt(i < 0 ? -i : i);
    }
    if (name == "POWER" || name == "POW") {
        if (expr.arguments.size() != 2) return Value::MakeNull();
        Value a = Evaluate(expr.arguments[0], tuple);
        Value b = Evaluate(expr.arguments[1], tuple);
        if (a.IsNull() || b.IsNull()) return Value::MakeNull();
        double r = std::pow(ValueToDouble(a), ValueToDouble(b));
        return Value::MakeFloat(r);
    }
    if (name == "MOD") {
        if (expr.arguments.size() != 2) return Value::MakeNull();
        Value a = Evaluate(expr.arguments[0], tuple);
        Value b = Evaluate(expr.arguments[1], tuple);
        if (a.IsNull() || b.IsNull()) return Value::MakeNull();
        double av = ValueToDouble(a);
        double bv = ValueToDouble(b);
        if (bv == 0.0) return Value::MakeNull();
        double r = av - std::floor(av / bv) * bv;
        if (r < 0) r += bv;  // 与 SQL MOD 一致：返回非负余数
        return Value::MakeInt(static_cast<int32_t>(r));
    }
    // ---- 日期/时间函数 ----
    if (name == "YEAR" || name == "MONTH" || name == "DAY") {
        if (expr.arguments.size() != 1) return Value::MakeNull();
        Value v = Evaluate(expr.arguments[0], tuple);
        if (v.IsNull()) return Value::MakeNull();
        int y = 0, mo = 0, d = 0;
        if (!ParseDateString(v.ToString(), &y, &mo, &d)) return Value::MakeNull();
        if (name == "YEAR") return Value::MakeInt(y);
        if (name == "MONTH") return Value::MakeInt(mo);
        return Value::MakeInt(d);
    }
    if (name == "NOW" || name == "CURRENT_TIMESTAMP") {
        // 当前时间，格式：YYYY-MM-DD HH:MM:SS
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        return Value::MakeVarchar(buf);
    }
    // ---- 60_funcs: 标量函数 GREATEST / LEAST / RAND / RANDOM ----
    // GREATEST(a, b, ...) 与 LEAST(a, b, ...) 至少 1 个参数；
    // 任一参数为 NULL 时整体返回 NULL（PostgreSQL / 标准 SQL 语义）。
    if (name == "GREATEST") {
        if (expr.arguments.empty()) return Value::MakeNull();
        Value best = Evaluate(expr.arguments[0], tuple);
        if (best.IsNull()) return Value::MakeNull();
        for (size_t i = 1; i < expr.arguments.size(); ++i) {
            Value v = Evaluate(expr.arguments[i], tuple);
            if (v.IsNull()) return Value::MakeNull();
            if (Value::Compare(v, best) > 0) best = v;
        }
        return best;
    }
    if (name == "LEAST") {
        if (expr.arguments.empty()) return Value::MakeNull();
        Value best = Evaluate(expr.arguments[0], tuple);
        if (best.IsNull()) return Value::MakeNull();
        for (size_t i = 1; i < expr.arguments.size(); ++i) {
            Value v = Evaluate(expr.arguments[i], tuple);
            if (v.IsNull()) return Value::MakeNull();
            if (Value::Compare(v, best) < 0) best = v;
        }
        return best;
    }
    // RAND() / RANDOM() —— 返回 [0, 1) 区间 FLOAT。
    // 使用 thread_local mt19937_64，保证同一 SELECT 内多次调用得到的值不同。
    if (name == "RAND" || name == "RANDOM") {
        thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_real_distribution<double> dist(0.0, 1.0);
        return Value::MakeFloat(dist(rng));
    }
    // ---- 40_txn_view_udf：UDF 调用 ----
    // 当内置函数未命中且 ExecutionContext 中存在 catalog 时，尝试按 catalog
    // 注册的用户自定义函数求值。把实参值代入参数名（同名替换），然后调用
    // UdfExecutor 走 body_statements（顺序解释 DECLARE/SET/IF/WHILE/RETURN）。
    if (ctx_ != nullptr) {
        SystemCatalog* catalog = ctx_->GetCatalog();
        if (catalog != nullptr) {
            const SystemCatalog::FunctionDefinition* fn =
                catalog->LookupFunction(expr.function_name);
            if (fn != nullptr) {
                if (fn->parameters.size() != expr.arguments.size()) {
                    return Value::MakeNull();
                }
                // 构造实参绑定：每个形参名 → 对应实参表达式在当前行上的求值结果。
                std::unordered_map<std::string, Value> arg_bind;
                for (size_t i = 0; i < expr.arguments.size(); ++i) {
                    Value v = Evaluate(expr.arguments[i], tuple);
                    arg_bind[fn->parameters[i].name] = v;
                }
                UdfExecutor udf(catalog, ctx_, *fn, std::move(arg_bind));
                return udf.Run();
            }
        }
    }
    // 其它函数暂未实现
    return Value::MakeNull();
}

namespace {

// 在已构建好的子计划上跑一次，把全部结果收集到 vector<Tuple>。
// 子计划里可能嵌套 CTE_BIND/Subquery 节点，所以走 ExecutionEngine::Execute
// 是最稳的入口（它本身就会递归 BuildExecutor）。
// 注：每行都跑一次完整 Execute 对一个子查询来说代价不算太高（子查询的
// 结果集通常较小），换来代码量大幅缩减；非相关子查询可在更上层缓存，
// 但当前实现以正确性为先，不在每行重做缓存决策。
std::vector<Tuple> RunPlanToCompletion(ExecutionContext* ctx, const PlanNodePtr& plan) {
    std::vector<Tuple> rows;
    if (!ctx || !plan) return rows;
    ExecutionEngine engine(ctx->GetCatalog());
    ExecutionResult r = engine.ExecuteSubplan(plan, ctx);
    if (r.success) rows = std::move(r.rows);
    return rows;
}

// 比较两个 Value 与 SQL 比较运算符 op，返回比较结果（NULL 视为 UNKNOWN）
bool SqlCompare(const Value& l, const std::string& op, const Value& r) {
    if (l.IsNull() || r.IsNull()) return false;  // 留给调用方按 NULL 语义解释
    int c = Value::Compare(l, r);
    if (op == "=" || op == "==") return c == 0;
    if (op == "<>" || op == "!=") return c != 0;
    if (op == "<") return c < 0;
    if (op == "<=") return c <= 0;
    if (op == ">") return c > 0;
    if (op == ">=") return c >= 0;
    return c == 0;
}

// 把 SelectStatement 引用的表名（包括别名）收集到集合。包含 from_table /
// from_table_alias / 各 join 的 table_name + table_alias / 派生表 alias。
// 用一个 unordered_set 方便 O(1) 命中判断（限定列 ref 是否属于本层）。
void CollectInnerTableNames(const SelectStatement& sub,
                            std::unordered_set<std::string>& out) {
    // 55_query: LATERAL 子查询时 from_table 不参与 inner_tables：用户的内层
    // FROM 表名与外层同名时（例如 `FROM t1, LATERAL (SELECT ... FROM t1 t2 ...)`
    // 中 `t1.val` 期望引用外层），不应把内层 t1 视为屏蔽外层 t1 的标识符。
    // 仅 from_table_alias（以及 joins / derived_alias）作为内层有效表名。
    if (!sub.is_lateral && !sub.from_table.empty()) {
        out.insert(sub.from_table);
        if (!sub.from_table_alias.empty()) out.insert(sub.from_table_alias);
    } else if (sub.is_lateral) {
        // LATERAL：仍然把 from_table_alias 作为内部别名纳入（防止别名撞 outer 表名
        // 时反向解析），但 from_table 自身留给 outer_bind。
        if (!sub.from_table_alias.empty()) out.insert(sub.from_table_alias);
    }
    for (const auto& j : sub.joins) {
        if (!j.table_name.empty()) out.insert(j.table_name);
        if (!j.table_alias.empty()) out.insert(j.table_alias);
    }
    if (sub.derived_table && !sub.derived_alias.empty()) {
        out.insert(sub.derived_alias);
    }
}

// 在表达式树里查找任何 ColumnRefExpr，其 table_name 非空且不在 inner 表集合中
// —— 这表示它引用了外层 SELECT 的某列，是相关子查询标志。
bool WalkExprForOuterRefs(const ExprPtr& e,
                          const std::unordered_set<std::string>& inner_tables) {
    if (!e) return false;
    switch (e->GetType()) {
        case NodeType::COLUMN_REF_EXPR: {
            auto cr = static_cast<const ColumnRefExpr*>(e.get());
            // 限定列：table_name 不在 inner 集合 → 外层引用
            if (!cr->table_name.empty() &&
                inner_tables.find(cr->table_name) == inner_tables.end()) {
                return true;
            }
            // 未限定列：保守地视为可能的外层引用（无法在当前点做完整 schema 解析）。
            // 代价是少量非相关子查询被当成相关，每行重跑——可接受。
            if (cr->table_name.empty()) return true;
            return false;
        }
        case NodeType::BINARY_EXPR: {
            auto b = std::static_pointer_cast<BinaryExpr>(e);
            return WalkExprForOuterRefs(b->left, inner_tables) ||
                   WalkExprForOuterRefs(b->right, inner_tables);
        }
        case NodeType::UNARY_EXPR: {
            auto u = std::static_pointer_cast<UnaryExpr>(e);
            return WalkExprForOuterRefs(u->operand, inner_tables);
        }
        case NodeType::FUNCTION_CALL_EXPR: {
            auto f = std::static_pointer_cast<FunctionCallExpr>(e);
            for (auto& a : f->arguments) {
                if (WalkExprForOuterRefs(a, inner_tables)) return true;
            }
            return false;
        }
        case NodeType::CASE_EXPR: {
            auto c = std::static_pointer_cast<CaseExprNode>(e);
            if (WalkExprForOuterRefs(c->subject, inner_tables)) return true;
            for (auto& w : c->whens) {
                if (WalkExprForOuterRefs(w.when_expr, inner_tables)) return true;
                if (WalkExprForOuterRefs(w.then_expr, inner_tables)) return true;
            }
            if (WalkExprForOuterRefs(c->else_expr, inner_tables)) return true;
            return false;
        }
        case NodeType::CAST_EXPR: {
            auto c = std::static_pointer_cast<CastExprNode>(e);
            return WalkExprForOuterRefs(c->expr, inner_tables);
        }
        default:
            return false;
    }
}

// 判断一个子查询是否为相关子查询：扫描其 SELECT 列表 / WHERE / HAVING /
// ORDER BY / JOIN ON 中是否有引用外层列的 ColumnRefExpr。
bool IsSubqueryCorrelated(const SelectStatement& sub) {
    std::unordered_set<std::string> inner_tables;
    CollectInnerTableNames(sub, inner_tables);
    auto walk = [&](const ExprPtr& e) {
        return WalkExprForOuterRefs(e, inner_tables);
    };
    for (const auto& e : sub.select_list) {
        if (walk(e)) return true;
    }
    if (walk(sub.where_clause)) return true;
    if (walk(sub.having_clause)) return true;
    for (const auto& e : sub.group_by) {
        if (walk(e)) return true;
    }
    for (const auto& ob : sub.order_by) {
        if (walk(ob.expr)) return true;
    }
    for (const auto& j : sub.joins) {
        if (walk(j.on_condition)) return true;
    }
    return false;
}

// 把外层元组投影成一个 map：qualifier.col → Value、col → Value。
// 用本 evaluator 的 column_index_map_ 做位置查找（外层元组的列顺序）。
//   outer_cmap:    外层 evaluator 的 column_index_map
//   outer_tuple:   外层当前行的 Tuple
//   inner_tables:  子查询的 FROM/JOIN 表集合
// 返回 map。
std::unordered_map<std::string, Value> BuildOuterBind(
    const ExprPtr& expr,
    const Tuple& outer_tuple,
    const std::unordered_map<std::string, size_t>& outer_cmap) {
    std::unordered_map<std::string, Value> bind;
    std::function<void(const ExprPtr&)> collect = [&](const ExprPtr& e) {
        if (!e) return;
        switch (e->GetType()) {
            case NodeType::COLUMN_REF_EXPR: {
                auto cr = static_cast<const ColumnRefExpr*>(e.get());
                auto it = outer_cmap.find(cr->column_name);
                if (it == outer_cmap.end() || it->second >= outer_tuple.ColumnCount()) {
                    return;  // 外层也找不到，跳过
                }
                const Value& v = outer_tuple.GetValue(it->second);
                // 同时存限定 + 不限定形式，让子查询内部 evaluator 两种引用都能命中。
                if (!cr->table_name.empty()) {
                    bind[cr->table_name + "." + cr->column_name] = v;
                }
                bind[cr->column_name] = v;
                return;
            }
            case NodeType::BINARY_EXPR: {
                auto b = std::static_pointer_cast<BinaryExpr>(e);
                collect(b->left);
                collect(b->right);
                return;
            }
            case NodeType::UNARY_EXPR: {
                auto u = std::static_pointer_cast<UnaryExpr>(e);
                collect(u->operand);
                return;
            }
            case NodeType::FUNCTION_CALL_EXPR: {
                auto f = std::static_pointer_cast<FunctionCallExpr>(e);
                for (auto& a : f->arguments) collect(a);
                return;
            }
            case NodeType::CASE_EXPR: {
                auto c = std::static_pointer_cast<CaseExprNode>(e);
                collect(c->subject);
                for (auto& w : c->whens) {
                    collect(w.when_expr);
                    collect(w.then_expr);
                }
                collect(c->else_expr);
                return;
            }
            case NodeType::CAST_EXPR: {
                auto c = std::static_pointer_cast<CastExprNode>(e);
                collect(c->expr);
                return;
            }
            default:
                return;
        }
    };
    collect(expr);
    return bind;
}

}  // namespace

Value ExpressionEvaluator::EvaluateSubquery(const SubqueryExprNode& expr,
                                            const Tuple& tuple) const {
    if (!expr.subquery_plan) return Value::MakeNull();
    if (!ctx_) return Value::MakeNull();

    // === 相关子查询：把当前外层行的列值推到 ExecutionContext，再跑子计划 ===
    // 跑完后恢复旧的 outer_bind，避免影响同语句后续无关的 evaluator。
    const std::unordered_map<std::string, Value>* saved_bind = ctx_->GetOuterBind();
    std::unordered_map<std::string, Value> owned_bind;
    const std::unordered_map<std::string, Value>* use_bind = nullptr;
    const std::unordered_set<std::string>* saved_inner = ctx_->GetInnerTables();
    std::unique_ptr<std::unordered_set<std::string>> owned_inner_tables;
    const std::unordered_set<std::string>* use_inner = nullptr;
    if (expr.subquery && IsSubqueryCorrelated(*expr.subquery)) {
        // 合并子查询 AST 中所有相关位置的外层列引用：select_list / where /
        // having / order_by / join.on 都可能引用外层。把每处 expr 喂给 BuildOuterBind，
        // map 自动按 key 去重。
        std::vector<ExprPtr> rels;
        for (auto& e : expr.subquery->select_list) rels.push_back(e);
        if (expr.subquery->where_clause) rels.push_back(expr.subquery->where_clause);
        if (expr.subquery->having_clause) rels.push_back(expr.subquery->having_clause);
        for (auto& e : expr.subquery->group_by) rels.push_back(e);
        for (auto& ob : expr.subquery->order_by) rels.push_back(ob.expr);
        for (auto& j : expr.subquery->joins) rels.push_back(j.on_condition);
        // 关键步骤：把本 evaluator 的 column_index_map_ 当作「外层」列下标映射，
        // 因为调用方传进来的 tuple 就是外层元组，列下标语义一致。
        for (auto& r : rels) {
            auto sub_bind = BuildOuterBind(r, tuple, column_index_map_);
            for (auto& kv : sub_bind) owned_bind[kv.first] = kv.second;
        }
        if (!owned_bind.empty()) {
            use_bind = &owned_bind;
            ctx_->SetOuterBind(use_bind);
        }
        // 通知下游 evaluator：本上下文是子查询求值，给出内层表集合，
        // 让 EvaluateColumnRef 能在限定列引用外层时正确回退。
        owned_inner_tables = std::make_unique<std::unordered_set<std::string>>();
        CollectInnerTableNames(*expr.subquery, *owned_inner_tables);
        use_inner = owned_inner_tables.get();
        ctx_->SetInnerTables(use_inner);
    }

    auto rows = RunPlanToCompletion(ctx_, expr.subquery_plan);
    if (use_bind) ctx_->SetOuterBind(saved_bind);
    if (use_inner) ctx_->SetInnerTables(saved_inner);

    switch (expr.kind) {
        case SubqueryType::SCALAR: {
            if (rows.empty()) return Value::MakeNull();
            const auto& t = rows[0];
            if (t.ColumnCount() == 0) return Value::MakeNull();
            return t.GetValue(0);
        }
        case SubqueryType::EXISTS: {
            // EXISTS: 子查询至少返回一行 → TRUE；空集 → FALSE。
            return rows.empty() ? Value::MakeInt(0) : Value::MakeInt(1);
        }
        case SubqueryType::IN: {
            // 把外层表达式在当前行的值求出，然后判断是否在 rows 中。
            // IN-list 语义下空集合 → FALSE；含 NULL 且未命中 → UNKNOWN；
            // 命中 → TRUE。遵循 SQL 三值逻辑。
            if (!expr.outer_expr) return Value::MakeInt(0);
            Value outer = Evaluate(expr.outer_expr, tuple);
            if (outer.IsNull()) return Value::MakeNull();
            bool saw_null = false;
            for (const auto& r : rows) {
                if (r.ColumnCount() == 0) continue;
                const Value& v = r.GetValue(0);
                if (v.IsNull()) { saw_null = true; continue; }
                if (Value::Compare(outer, v) == 0) return Value::MakeInt(1);
            }
            return saw_null ? Value::MakeNull() : Value::MakeInt(0);
        }
        case SubqueryType::ANY: {
            // expr op ANY (SELECT ...)：行值匹配至少一个元素即 TRUE。
            // 子查询结果含 NULL 时按 NULL 语义：若全部为 NULL 或都不匹配，
            // 返回 UNKNOWN（NULL）。
            if (!expr.outer_expr) return Value::MakeInt(0);
            Value outer = Evaluate(expr.outer_expr, tuple);
            if (outer.IsNull()) return Value::MakeNull();
            const std::string& op = expr.comparison_op;
            bool saw_null = false;
            for (const auto& r : rows) {
                if (r.ColumnCount() == 0) continue;
                const Value& v = r.GetValue(0);
                if (v.IsNull()) { saw_null = true; continue; }
                if (SqlCompare(outer, op, v)) return Value::MakeInt(1);
            }
            // 全是 NULL 或全不匹配：含 NULL → UNKNOWN，否则 FALSE
            if (saw_null) return Value::MakeNull();
            return Value::MakeInt(0);
        }
    }
    return Value::MakeNull();
}

// 43_upsert: VALUES(col) —— 通过 ExecutionContext 上的 upsert_values_bind 取值。
// 不在 upsert 上下文时返回 NULL（语义层应保证 VALUES(col) 不出现在其它语境）。
Value ExpressionEvaluator::EvaluateUpsertValuesRef(const UpsertValuesRefExpr& expr,
                                                   const Tuple& tuple) const {
    if (!ctx_) return Value::MakeNull();
    const auto* bind = ctx_->GetUpsertValuesBind();
    if (!bind) return Value::MakeNull();
    auto it = bind->find(expr.column_name);
    if (it == bind->end()) return Value::MakeNull();
    return it->second;
}

// 44_pattern_match / 56_pattern: 统一处理 LIKE / ILIKE / REGEXP / RLIKE / SIMILAR TO
// （含可选 ESCAPE）。
//
// 设计取舍：
//   - LIKE / ILIKE：使用本文件的 MatchLikePatternEscaped（按 escape_char
//     转义）。ILIKE 在匹配前对两侧做 ASCII 小写折叠；REGEXP 不受 escape_char
//     影响（C++ <regex> 自身支持 '\' 转义）。
//   - REGEXP / RLIKE：pattern 通常是字面量，但在通用 AST 上无法保证；统一
//     在运行时编译一次 std::regex（regex::ECMAScript）。\d \D \s \S \w \W 这
//     六个 ERE 单词字符类在编译前由 TranslateEreWordClasses 转译为显式字符类
//     （std::regex ECMAScript 自身也支持，但显式化跨引擎更稳）。失败抛
//     RuntimeError，文案 `Error: invalid regex pattern: <what()>` 满足测试期望。
//   - SIMILAR TO：先把 SQL pattern 经 TranslateSqlLikeToEre 翻译成 ERE，再走
//     与 REGEXP 相同的编译路径。ESCAPE 子句在 SQL 通配符（%/ _）层面生效。
Value ExpressionEvaluator::EvaluateLike(const LikeExprNode& expr,
                                        const Tuple& tuple) const {
    Value l = Evaluate(expr.operand, tuple);
    Value r = Evaluate(expr.pattern, tuple);
    if (l.IsNull() || r.IsNull()) return Value::MakeNull();

    auto as_str = [](const Value& v) -> std::string {
        if (v.GetType() == ValueType::VARCHAR) return v.AsVarchar();
        // 非 VARCHAR 一律按其 ToString() 形式参与匹配，避免隐式 NULL。
        return v.ToString();
    };

    std::string s = as_str(l);
    std::string p = as_str(r);
    char esc = expr.has_escape ? expr.escape_char : '\\';

    switch (expr.kind) {
        case LikeExprNode::Kind::LIKE:
            return MakeBool(MatchLikePatternEscaped(s, p, esc));
        case LikeExprNode::Kind::ILIKE:
            return MakeBool(MatchLikePatternEscaped(AsciiLower(s),
                                                    AsciiLower(p),
                                                    esc));
        case LikeExprNode::Kind::REGEXP:
        case LikeExprNode::Kind::RLIKE: {
            // ECMAScript + 非显式 '^' 锚定 → 子串匹配；POSIX ERE 的 '^'/'$'
            // 在 ECMAScript 下同样按位置断言，因此 metacharacter 要求可达成。
            std::string ere = TranslateEreWordClasses(p);
            try {
                std::regex re(ere, std::regex::ECMAScript | std::regex::optimize);
                return MakeBool(std::regex_search(s, re));
            } catch (const std::regex_error& e) {
                // FormatError 对 RUNTIME 阶段跳过 "[Runtime]" 前缀，main.cpp
                // 再补 "Error: "。最终输出为 "Error: invalid regex pattern: <what()>"。
                throw CompilerException(ErrorStage::RUNTIME,
                    std::string("invalid regex pattern: ") + e.what());
            }
        }
        case LikeExprNode::Kind::SIMILAR_TO: {
            // 56_pattern: SQL pattern → ERE → std::regex。
            std::string ere = TranslateSqlLikeToEre(p, esc);
            std::string ere_with_wc = TranslateEreWordClasses(ere);
            try {
                std::regex re(ere_with_wc, std::regex::ECMAScript | std::regex::optimize);
                return MakeBool(std::regex_search(s, re));
            } catch (const std::regex_error& e) {
                throw CompilerException(ErrorStage::RUNTIME,
                    std::string("invalid regex pattern: ") + e.what());
            }
        }
    }
    return Value::MakeNull();
}

// 45_datetime: EXTRACT(field FROM source)
//
//   1) 求值 source → VARCHAR（按 YYYY-MM-DD / YYYY-MM-DD HH:MM:SS 解释）
//   2) ParseDateTime 把 source 转为 epoch 秒
//   3) ExtractDateTimeParts 取目标字段；YEAR/MONTH/DAY/HOUR/MINUTE/SECOND
//      均为 INT。SECOND 字段按规范可返回 FLOAT 保留小数秒；本实现简化
//      为 INT，仅返回秒（无小数部分）。
Value ExpressionEvaluator::EvaluateExtract(const ExtractExprNode& expr,
                                           const Tuple& tuple) const {
    if (!expr.source) return Value::MakeNull();
    Value v = Evaluate(expr.source, tuple);
    if (v.IsNull()) return Value::MakeNull();
    int64_t epoch = 0;
    if (!ParseDateTime(v.ToString(), &epoch)) return Value::MakeNull();
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    ExtractDateTimeParts(epoch, &y, &mo, &d, &h, &mi, &s);
    switch (static_cast<IntervalUnit>(expr.field)) {
        case IntervalUnit::YEAR:   return Value::MakeInt(y);
        case IntervalUnit::MONTH:  return Value::MakeInt(mo);
        case IntervalUnit::DAY:    return Value::MakeInt(d);
        case IntervalUnit::HOUR:   return Value::MakeInt(h);
        case IntervalUnit::MINUTE: return Value::MakeInt(mi);
        case IntervalUnit::SECOND: return Value::MakeInt(s);
    }
    return Value::MakeNull();
}

// 45_datetime: INTERVAL <n> <unit> 单独求值。
//
// 单独出现没有语义（用户应当用 `<date> + INTERVAL ...`），但执行器可能
// 在某些边界场景（如类型推断 / ShowPlan）调用它。这里返回 NULL。
Value ExpressionEvaluator::EvaluateInterval(const IntervalExprNode& expr,
                                            const Tuple& tuple) const {
    (void)tuple;
    (void)expr;
    return Value::MakeNull();
}

// 53_ddl: NEXTVAL FOR sequence_name —— 推进并返回当前值。
// 在 ctx_ 为空或 catalog 为空时退化为 NULL；序列不存在抛 SEMANTIC 错误。
Value ExpressionEvaluator::EvaluateNextval(const NextvalExpr& expr) const {
    if (ctx_ == nullptr) return Value::MakeNull();
    SystemCatalog* catalog = ctx_->GetCatalog();
    if (catalog == nullptr) return Value::MakeNull();
    int64_t v = 0;
    if (!catalog->NextSequence(expr.sequence_name, &v)) {
        throw CompilerException(
            ErrorStage::SEMANTIC,
            "sequence does not exist: " + expr.sequence_name);
    }
    return Value::MakeInt(static_cast<int32_t>(v));
}

}  // namespace sqlcompiler