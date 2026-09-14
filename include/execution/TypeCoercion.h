#pragma once

// =============================================================================
// TypeCoercion —— 把运行时 Value 按列声明类型转换。
//
// DECIMAL 在本引擎中运行时是 VARCHAR（无独立 TypeId），但写入路径如果直接
// 把一个 FLOAT/INTEGER 的 Evaluate 结果塞进 VARCHAR 列槽，序列化层
// (Value::SerializeTo) 按 Value 自身的运行时类型写入二进制（IEEE-754 或
// int32），反序列化时按 VARCHAR 读就得到乱码——典型症状：UPDATE DECIMAL
// SET x = 3.95 返回 OK 但 SELECT 看到 0/空串。
//
// 该 helper 把所有 DML 写入路径（INSERT/UPDATE/UPDATE FROM/UPSERT/MERGE）
// 统一成"按列声明类型对齐"的语义：DECIMAL → VARCHAR 字符串表示；INT/FLOAT
// → 兼容转换；NULL 透传。
// =============================================================================

#include "storage_engine/Value.h"

#include <cctype>
#include <string>

namespace sqlcompiler {

// 把 v 强制转换为与列声明类型 col_type 匹配的 Value。
// col_type 接受 "DECIMAL"/"NUMERIC"、"INT"/"INTEGER"/"BIGINT"/"SMALLINT"/
// "TINYINT"/"BOOLEAN"/"BOOL"、"FLOAT"/"DOUBLE"/"REAL"、"VARCHAR"/"CHAR"/
// "TEXT"/"STRING"/"DATE"/"TIMESTAMP"/"TIME"/"JSON"/"UUID"，以及其他未识别
// 类型——最后一类保持原样（让存储层继续按原类型序列化）。
//
// 返回的 Value 与 col_type 对应的运行时类型一致：DECIMAL → VARCHAR，
// INT 类 → INTEGER，FLOAT 类 → FLOAT，VARCHAR/CHAR/TEXT/DATE/TIMESTAMP 等
// 字符串列 → VARCHAR（数值类型也强制转字符串以避免序列化错位）。
inline Value CoerceToColumnType(const Value& v, const std::string& col_type) {
    std::string up;
    up.reserve(col_type.size());
    for (char c : col_type) {
        up.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))));
    }
    if (v.IsNull()) return v;
    if (up == "INT" || up == "INTEGER" || up == "BIGINT" ||
        up == "SMALLINT" || up == "TINYINT" || up == "BOOLEAN" ||
        up == "BOOL") {
        if (v.GetType() == ValueType::INTEGER) return v;
        if (v.GetType() == ValueType::FLOAT) {
            return Value::MakeInt(static_cast<int32_t>(v.AsFloat()));
        }
        if (v.GetType() == ValueType::VARCHAR) {
            try {
                return Value::MakeInt(
                    static_cast<int32_t>(std::stoi(v.AsVarchar())));
            } catch (...) {
                return Value::MakeInt(0);
            }
        }
    } else if (up == "FLOAT" || up == "DOUBLE" || up == "REAL") {
        if (v.GetType() == ValueType::FLOAT) return v;
        if (v.GetType() == ValueType::INTEGER) {
            return Value::MakeFloat(static_cast<double>(v.AsInt()));
        }
        if (v.GetType() == ValueType::VARCHAR) {
            try {
                return Value::MakeFloat(std::stod(v.AsVarchar()));
            } catch (...) {
                return Value::MakeFloat(0.0);
            }
        }
    } else if (up == "DECIMAL" || up == "NUMERIC") {
        if (v.GetType() == ValueType::VARCHAR) return v;
        if (v.GetType() == ValueType::INTEGER) {
            return Value::MakeVarchar(std::to_string(v.AsInt()));
        }
        if (v.GetType() == ValueType::FLOAT) {
            return Value::MakeVarchar(FormatDecimal(v.AsFloat()));
        }
        return v;
    } else if (up == "VARCHAR" || up == "STRING" || up == "TEXT" ||
               up == "CHAR" || up == "DATE" || up == "TIMESTAMP" ||
               up == "TIME" || up == "JSON" || up == "UUID") {
        // DEFAULT(col) 跨列场景可能把 INT/FLOAT 默认值塞进 VARCHAR 列；
        // 数值 → VARCHAR 必须显式字符串化，否则序列化层按 INTEGER 二进制
        // 写入后反序列化按 VARCHAR 读就得到空串/乱码。
        if (v.GetType() == ValueType::VARCHAR) return v;
        if (v.GetType() == ValueType::INTEGER) {
            return Value::MakeVarchar(std::to_string(v.AsInt()));
        }
        if (v.GetType() == ValueType::FLOAT) {
            return Value::MakeVarchar(FormatDecimal(v.AsFloat()));
        }
        return v;
    }
    return v;
}

}  // namespace sqlcompiler
