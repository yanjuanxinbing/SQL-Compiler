#include "optimizer/IndexAccessPath.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "index/IndexKey.h"

namespace sqlcompiler {

namespace {

// 把合取谓词拆成一组 AND 连接的子句。非 AND 节点原样返回单元素。
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

    // 只考虑单列索引的最左列。多列索引的区间推导需要处理「前缀等值 + 末列范围」
    // 的组合，收益有限而出错空间很大，留待后续。
    const IndexInfo* best = nullptr;
    for (const IndexInfo* info : indexes) {
        if (info->key_columns.size() != 1) continue;
        for (const auto& c : conjuncts) {
            ColumnCompare cc;
            if (!MatchColumnCompare(c, table_name, table_alias, &cc)) continue;
            if (cc.column != info->key_columns[0]) continue;
            // 唯一索引优先：等值查找最多命中一行，选择性最好
            if (best == nullptr || (!best->is_unique && info->is_unique)) {
                best = info;
            }
            break;
        }
    }
    if (best == nullptr || best->key_types.size() != 1) return nullptr;

    const ValueType key_type = best->key_types[0];
    const std::string& key_col = best->key_columns[0];

    bool has_low = false, has_high = false;
    bool low_inclusive = true, high_inclusive = true;
    Value low, high;
    std::vector<ExprPtr> residual;

    for (const auto& c : conjuncts) {
        ColumnCompare cc;
        Value v;
        if (!MatchColumnCompare(c, table_name, table_alias, &cc) ||
            cc.column != key_col || !LiteralToValue(cc.literal, key_type, &v)) {
            residual.push_back(c);
            continue;
        }
        // 多个同向边界取更紧的那个（例如 x > 3 AND x > 5 取 5）
        auto tighten_low = [&](const Value& val, bool inclusive) {
            if (!has_low || Value::Compare(val, low) > 0) {
                low = val;
                low_inclusive = inclusive;
                has_low = true;
            } else if (Value::Compare(val, low) == 0 && !inclusive) {
                low_inclusive = false;
            }
        };
        auto tighten_high = [&](const Value& val, bool inclusive) {
            if (!has_high || Value::Compare(val, high) < 0) {
                high = val;
                high_inclusive = inclusive;
                has_high = true;
            } else if (Value::Compare(val, high) == 0 && !inclusive) {
                high_inclusive = false;
            }
        };

        switch (cc.op) {
            case BinaryOperator::EQUAL:
                tighten_low(v, true);
                tighten_high(v, true);
                break;
            case BinaryOperator::GREATER:
                tighten_low(v, false);
                break;
            case BinaryOperator::GREATER_EQUAL:
                tighten_low(v, true);
                break;
            case BinaryOperator::LESS:
                tighten_high(v, false);
                break;
            case BinaryOperator::LESS_EQUAL:
                tighten_high(v, true);
                break;
            default:
                residual.push_back(c);
                break;
        }
    }

    // 一个边界都没定住就没必要走索引：那等价于全索引扫描再回表，
    // 比顺序扫描更慢（多了一次随机 I/O）。
    if (!has_low && !has_high) return nullptr;

    auto node = std::make_shared<IndexScanNode>(table_name, best->index_name,
                                                table_alias);
    if (has_low) node->low_key = {low};
    if (has_high) node->high_key = {high};
    node->low_inclusive = low_inclusive;
    node->high_inclusive = high_inclusive;

    // 未被索引消解的合取项在回表后再判一次
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
