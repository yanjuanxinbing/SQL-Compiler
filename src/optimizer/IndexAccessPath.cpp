#include "optimizer/IndexAccessPath.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "index/IndexKey.h"

namespace sqlcompiler {

// 把合取谓词 (a AND b AND c) 拆成一组子句 [a, b, c]。
// 非 AND 节点原样返回单元素。PushDownPredicates 与 TryRewriteWithIndex
// 共用此 helper；放在 sqlcompiler 命名空间下以便其他 .cpp 直接调用。
void SplitConjuncts(const ExprPtr& expr, std::vector<ExprPtr>* out) {
    if (!expr) return;
    if (expr->GetType() == NodeType::BINARY_EXPR) {
        const auto* bin = static_cast<const BinaryExpr*>(expr.get());
        if (bin->op == BinaryOperator::AND) {
            SplitConjuncts(bin->left, out);
            SplitConjuncts(bin->right, out);
            return;
        }
    }
    out->push_back(expr);
}

namespace {

bool IsLiteral(const ExprPtr& e) {
    return e && e->GetType() == NodeType::LITERAL_EXPR;
}

// 字面量转 Value，并按目标列类型归一化。
//
// 类型必须与列声明一致：索引键是按列类型序列化的，若拿 INT 字面量去和 FLOAT
// 列的键比较，Value::Compare 的结果就没有意义，扫描区间会整个错位。
bool LiteralToValue(const ExprPtr& e, ValueType target, Value* out) {
    if (!IsLiteral(e) || out == nullptr) return false;
    const auto* lit = static_cast<const LiteralExpr*>(e.get());
    switch (lit->literal_type) {
        case LiteralType::INTEGER:
            if (target == ValueType::INTEGER) {
                *out = Value::MakeInt(static_cast<int32_t>(std::atoi(lit->value.c_str())));
                return true;
            }
            if (target == ValueType::FLOAT) {
                *out = Value::MakeFloat(std::atof(lit->value.c_str()));
                return true;
            }
            return false;
        case LiteralType::FLOAT:
            if (target != ValueType::FLOAT) return false;
            *out = Value::MakeFloat(std::atof(lit->value.c_str()));
            return true;
        case LiteralType::STRING:
            if (target != ValueType::VARCHAR) return false;
            *out = Value::MakeVarchar(lit->value);
            return true;
        default:
            return false;  // NULL / BOOLEAN 不参与索引区间
    }
}

// 若表达式是「<列> <比较运算符> <字面量>」，取出列名、运算符与字面量。
// 支持字面量写在左边的形态（会把运算符方向翻转）。
struct ColumnCompare {
    std::string column;
    BinaryOperator op;
    ExprPtr literal;
};

BinaryOperator FlipOperator(BinaryOperator op) {
    switch (op) {
        case BinaryOperator::LESS: return BinaryOperator::GREATER;
        case BinaryOperator::LESS_EQUAL: return BinaryOperator::GREATER_EQUAL;
        case BinaryOperator::GREATER: return BinaryOperator::LESS;
        case BinaryOperator::GREATER_EQUAL: return BinaryOperator::LESS_EQUAL;
        default: return op;
    }
}

bool MatchColumnCompare(const ExprPtr& expr, const std::string& table_name,
                        const std::string& table_alias, ColumnCompare* out) {
    if (!expr || expr->GetType() != NodeType::BINARY_EXPR) return false;
    const auto* bin = static_cast<const BinaryExpr*>(expr.get());
    switch (bin->op) {
        case BinaryOperator::EQUAL:
        case BinaryOperator::LESS:
        case BinaryOperator::LESS_EQUAL:
        case BinaryOperator::GREATER:
        case BinaryOperator::GREATER_EQUAL:
            break;
        default:
            return false;
    }

    auto column_of = [&](const ExprPtr& e, std::string* name) {
        if (!e || e->GetType() != NodeType::COLUMN_REF_EXPR) return false;
        const auto* col = static_cast<const ColumnRefExpr*>(e.get());
        // 带表限定名时必须与本次扫描的表（或别名）一致，否则可能是 JOIN 的对端
        if (!col->table_name.empty() && col->table_name != table_name &&
            col->table_name != table_alias) {
            return false;
        }
        *name = col->column_name;
        return true;
    };

    std::string name;
    if (column_of(bin->left, &name) && IsLiteral(bin->right)) {
        out->column = name;
        out->op = bin->op;
        out->literal = bin->right;
        return true;
    }
    if (column_of(bin->right, &name) && IsLiteral(bin->left)) {
        out->column = name;
        out->op = FlipOperator(bin->op);
        out->literal = bin->left;
        return true;
    }
    return false;
}

}  // namespace

namespace {

// 单个索引键列上的约束集合（来自合取谓词的合并/收紧）。
struct ColConstraint {
    bool has_eq = false;
    Value eq;
    bool has_lo = false;
    Value lo;
    bool lo_inc = true;
    bool has_hi = false;
    Value hi;
    bool hi_inc = true;
};

// 把一条比较应用（收紧）到列约束。等值最紧：一旦出现等值，忽略后续范围。
// 同向多个范围取更紧的那个（例如 x > 3 AND x > 5 取 5；x < 10 AND x < 8 取 8）。
void ApplyCompare(ColConstraint& c, BinaryOperator op, const Value& v) {
    const auto tighten_lo = [&](bool inc) {
        if (!c.has_lo || Value::Compare(v, c.lo) > 0) {
            c.lo = v;
            c.lo_inc = inc;
            c.has_lo = true;
        } else if (Value::Compare(v, c.lo) == 0 && !inc) {
            c.lo_inc = false;
        }
    };
    const auto tighten_hi = [&](bool inc) {
        if (!c.has_hi || Value::Compare(v, c.hi) < 0) {
            c.hi = v;
            c.hi_inc = inc;
            c.has_hi = true;
        } else if (Value::Compare(v, c.hi) == 0 && !inc) {
            c.hi_inc = false;
        }
    };
    switch (op) {
        case BinaryOperator::EQUAL:
            c.has_eq = true;
            c.eq = v;
            c.has_lo = c.has_hi = false;  // 等值替代任何范围
            break;
        case BinaryOperator::GREATER:
            if (!c.has_eq) tighten_lo(false);
            break;
        case BinaryOperator::GREATER_EQUAL:
            if (!c.has_eq) tighten_lo(true);
            break;
        case BinaryOperator::LESS:
            if (!c.has_eq) tighten_hi(false);
            break;
        case BinaryOperator::LESS_EQUAL:
            if (!c.has_eq) tighten_hi(true);
            break;
        default:
            break;
    }
}

}  // namespace

PlanNodePtr TryRewriteWithIndex(SystemCatalog* catalog,
                                const std::string& table_name,
                                const std::string& table_alias,
                                const ExprPtr& predicate) {
    if (catalog == nullptr || !predicate) return nullptr;
    const TableInfo* table = catalog->GetTable(table_name);
    if (table == nullptr) return nullptr;

    auto indexes = catalog->GetIndexesForTable(table_name);
    if (indexes.empty()) return nullptr;

    std::vector<ExprPtr> conjuncts;
    SplitConjuncts(predicate, &conjuncts);

    // Phase 5（周期 1）：多列键区间推导。对每个索引求「最左前缀区间」：
    //   * 前导列出现等值 → 该列收窄为单点，继续推进到下一列；
    //   * 遇到第一个范围列（lo/hi）→ 该列收窄为范围，其后的列在字典序下
    //     没有独立下界，停止推进（这些列回到残余谓词）；
    //   * 首个无约束列 → 其后列同样不可用，停止。
    // low_key/high_key 只含被收窄的列（前缀），配合 low_bound_cols/high_bound_cols
    // 让执行器做「前缀比较」，避免引入 MIN/MAX 哨兵值。
    // 选择得分最高的索引：唯一索引优先，其次命中的前导列数多者。
    struct Derived {
        const IndexInfo* info = nullptr;
        std::vector<Value> low, high;
        size_t low_cols = 0, high_cols = 0;
        bool low_inc = true, high_inc = true;
        int score = -1;
        std::unordered_set<std::string> bound_cols;  // 被索引区间消解的列
    };
    Derived best;
    for (const IndexInfo* info : indexes) {
        const size_t k = info->key_columns.size();
        if (k == 0 || info->key_types.size() != k) continue;
        std::vector<ColConstraint> cons(k);
        bool matched_any = false;
        for (const auto& c : conjuncts) {
            ColumnCompare cc;
            Value v;
            if (!MatchColumnCompare(c, table_name, table_alias, &cc)) continue;
            size_t j = 0;
            for (; j < k; ++j) {
                if (cc.column == info->key_columns[j]) break;
            }
            if (j == k) continue;  // 谓词列不在索引键中
            if (!LiteralToValue(cc.literal, info->key_types[j], &v)) continue;
            ApplyCompare(cons[j], cc.op, v);
            matched_any = true;
        }
        if (!matched_any) continue;

        Derived d;
        d.info = info;
        bool usable = false;
        for (size_t j = 0; j < k; ++j) {
            const ColConstraint& c = cons[j];
            if (c.has_eq) {
                d.low.push_back(c.eq);
                d.high.push_back(c.eq);
                ++d.low_cols;
                ++d.high_cols;
                d.bound_cols.insert(info->key_columns[j]);
                usable = true;
            } else if (c.has_lo || c.has_hi) {
                if (c.has_lo) {
                    d.low.push_back(c.lo);
                    d.low_inc = c.lo_inc;
                    ++d.low_cols;
                }
                if (c.has_hi) {
                    d.high.push_back(c.hi);
                    d.high_inc = c.hi_inc;
                    ++d.high_cols;
                }
                d.bound_cols.insert(info->key_columns[j]);
                usable = true;
                break;  // 范围列之后不可再收窄（字典序）
            } else {
                break;  // 该列无约束：其后列（含本列）不可用
            }
        }
        if (!usable) continue;
        d.score = (info->is_unique ? 100 : 0) +
                  static_cast<int>(d.low_cols + d.high_cols);
        if (d.score > best.score) best = std::move(d);
    }
    if (best.info == nullptr) return nullptr;

    auto node = std::make_shared<IndexScanNode>(table_name, best.info->index_name,
                                                table_alias);
    if (!best.low.empty()) {
        node->low_key = std::move(best.low);
        node->low_bound_cols = best.low_cols;
        node->low_inclusive = best.low_inc;
    }
    if (!best.high.empty()) {
        node->high_key = std::move(best.high);
        node->high_bound_cols = best.high_cols;
        node->high_inclusive = best.high_inc;
    }

    // 未被索引区间消解的合取项在回表后再判一次（非 bound 列的列比较 +
    // 非字面量/非比较表达式）。已消解列上的比较被区间覆盖，不再重复过滤。
    std::vector<ExprPtr> residual;
    for (const auto& c : conjuncts) {
        ColumnCompare cc;
        if (MatchColumnCompare(c, table_name, table_alias, &cc) &&
            best.bound_cols.count(cc.column) > 0) {
            continue;
        }
        residual.push_back(c);
    }
    if (!residual.empty()) {
        ExprPtr rest = residual.front();
        for (size_t i = 1; i < residual.size(); ++i) {
            rest = std::make_shared<BinaryExpr>(BinaryOperator::AND, rest,
                                                residual[i]);
        }
        node->residual_predicate = rest;
    }
    return node;
}

}  // namespace sqlcompiler
