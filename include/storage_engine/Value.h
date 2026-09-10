#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sqlcompiler {

// 运行时数据类型（区别于AST中LiteralExpr的文本形式，这里是求值后的实际值）
enum class ValueType { INTEGER, FLOAT, VARCHAR, NULL_TYPE };

// 根据CREATE TABLE中声明的类型名（"INT" / "VARCHAR" / "FLOAT"）
// 映射到运行时的ValueType
ValueType ValueTypeFromString(const std::string& type_name);

// 运行时的值，供执行引擎在各算子之间传递数据（Tuple的每一列即为一个Value）
class Value {
public:
    Value();  // 构造一个NULL值

    static Value MakeInt(int32_t v);
    static Value MakeFloat(double v);
    static Value MakeVarchar(const std::string& v);
    static Value MakeNull();

    ValueType GetType() const;
    bool IsNull() const;

    int32_t AsInt() const;
    double AsFloat() const;
    const std::string& AsVarchar() const;

    // 序列化到buf（调用方需保证buf足够大），返回写入的字节数
    // column_type 用于 NULL 值：NULL 在磁盘上占用的字节数应与该列一个非空值的
    // 序列化字节数一致，避免反序列化时错位读取后续列。
    size_t SerializeTo(char* buf, ValueType column_type) const;

    // 从buf中按给定的目标类型反序列化出一个Value，返回读取的字节数
    static size_t DeserializeFrom(const char* buf, ValueType type, Value* out);

    // 计算该类型的值在序列化后固定/最大占用的字节数（VARCHAR为变长，需在实现中约定编码方式，
    // 如"4字节长度前缀 + 内容"）。column_type 用于 NULL 值。
    size_t SerializedSize(ValueType column_type) const;

    // 比较两个同类型Value的大小，返回负数/0/正数表示小于/等于/大于
    static int Compare(const Value& a, const Value& b);

    std::string ToString() const;

private:
    ValueType type_;
    int32_t int_val_;
    double float_val_;
    std::string str_val_;
};

}  // namespace sqlcompiler
