#include "execution/ExpressionEvaluator.h"

#include <cstdlib>

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

int CompareToInt(const Value& a, const Value& b) {
    int c = Value::Compare(a, b);
    if (c < 0) return 1;
    if (c > 0) return 0;
    return 1;  // equal => true
}

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

}  // namespace

ExpressionEvaluator::ExpressionEvaluator(
    const std::unordered_map<std::string, size_t>& column_index_map)
    : column_index_map_(column_index_map) {
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
    }
    return Value::MakeNull();
}

Value ExpressionEvaluator::EvaluateColumnRef(const ColumnRefExpr& expr,
                                              const Tuple& tuple) const {
    auto it = column_index_map_.find(expr.column_name);
    if (it == column_index_map_.end()) {
        // case-insensitive fallback
        std::string lc;
        for (char c : expr.column_name) lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        for (auto& kv : column_index_map_) {
            std::string kc;
            for (char c : kv.first) kc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (kc == lc) { it = column_index_map_.find(kv.first); break; }
        }
        if (it == column_index_map_.end()) {
            // fprintf(stderr, "[DBG] NOT FOUND %s\n", expr.column_name.c_str());
            return Value::MakeNull();
        }
    }
    if (it->second >= tuple.ColumnCount()) {
        return Value::MakeNull();
    }
    return tuple.GetValue(it->second);
}

Value ExpressionEvaluator::EvaluateBinary(const BinaryExpr& expr, const Tuple& tuple) const {
    Value l = Evaluate(expr.left, tuple);
    Value r = Evaluate(expr.right, tuple);
    switch (expr.op) {
        case BinaryOperator::ADD: {
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                return Value::MakeFloat(l.AsFloat() + r.AsFloat());
            }
            return Value::MakeInt(l.AsInt() + r.AsInt());
        }
        case BinaryOperator::SUB: {
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                return Value::MakeFloat(l.AsFloat() - r.AsFloat());
            }
            return Value::MakeInt(l.AsInt() - r.AsInt());
        }
        case BinaryOperator::MUL: {
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                return Value::MakeFloat(l.AsFloat() * r.AsFloat());
            }
            return Value::MakeInt(l.AsInt() * r.AsInt());
        }
        case BinaryOperator::DIV: {
            if (l.GetType() == ValueType::FLOAT || r.GetType() == ValueType::FLOAT) {
                double rv = r.AsFloat();
                if (rv == 0.0) return Value::MakeNull();
                return Value::MakeFloat(l.AsFloat() / rv);
            }
            int32_t rv = r.AsInt();
            if (rv == 0) return Value::MakeNull();
            return Value::MakeInt(l.AsInt() / rv);
        }
        case BinaryOperator::EQUAL: {
            int c = Value::Compare(l, r);
            return MakeBool(c == 0);
        }
        case BinaryOperator::NOT_EQUAL: {
            int c = Value::Compare(l, r);
            return MakeBool(c != 0);
        }
        case BinaryOperator::LESS: {
            int c = Value::Compare(l, r);
            return MakeBool(c < 0);
        }
        case BinaryOperator::LESS_EQUAL: {
            int c = Value::Compare(l, r);
            return MakeBool(c <= 0);
        }
        case BinaryOperator::GREATER: {
            int c = Value::Compare(l, r);
            return MakeBool(c > 0);
        }
        case BinaryOperator::GREATER_EQUAL: {
            int c = Value::Compare(l, r);
            return MakeBool(c >= 0);
        }
        case BinaryOperator::AND:
            return MakeBool(IsTruthy(l) && IsTruthy(r));
        case BinaryOperator::OR:
            return MakeBool(IsTruthy(l) || IsTruthy(r));
        case BinaryOperator::IS_NULL:
            return MakeBool(l.IsNull());
        case BinaryOperator::IS_NOT_NULL:
            return MakeBool(!l.IsNull());
        case BinaryOperator::LIKE: {
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
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr.right);
            for (const auto& v : fc->arguments) {
                Value vv = Evaluate(v, tuple);
                if (Value::Compare(l, vv) == 0) return MakeBool(true);
            }
            return MakeBool(false);
        }
        case BinaryOperator::BETWEEN: {
            if (!expr.right || expr.right->GetType() != NodeType::FUNCTION_CALL_EXPR) {
                return MakeBool(false);
            }
            auto fc = std::static_pointer_cast<FunctionCallExpr>(expr.right);
            if (fc->arguments.size() != 2) return MakeBool(false);
            Value low = Evaluate(fc->arguments[0], tuple);
            Value high = Evaluate(fc->arguments[1], tuple);
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
            return MakeBool(!IsTruthy(v));
        case UnaryOperator::NEGATE:
            if (v.GetType() == ValueType::FLOAT) return Value::MakeFloat(-v.AsFloat());
            return Value::MakeInt(-v.AsInt());
    }
    return Value::MakeNull();
}

Value ExpressionEvaluator::EvaluateFunctionCall(const FunctionCallExpr& expr,
                                                  const Tuple& tuple) const {
    if (expr.function_name == "*" || expr.function_name == "STAR") {
        return Value::MakeInt(1);
    }
    return Value::MakeNull();
}

}  // namespace sqlcompiler