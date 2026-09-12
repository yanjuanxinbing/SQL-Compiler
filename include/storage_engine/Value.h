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

// 52_data_types: 把 double 格式化为短精度十进制字符串，用于 DECIMAL/NUMERIC
// 列写入（"%.12g" 风格）。保留足够有效数字覆盖 DECIMAL(10, 2) 等常见精度，
// 又不会把 IEEE-754 的二进制噪声（如 99999999.98999999...）原样写入。
std::string FormatDecimal(double v);

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

    // 谓词真值判定：用于 WHERE / HAVING / IF 等布尔上下文。
    //  - NULL → false（SQL 三值逻辑在 WHERE 中按"不通过"处理，与标准一致）
    //  - INTEGER: 非 0 即 true
    //  - FLOAT: 非 0.0 即 true（NaN 视为 false）
    //  - VARCHAR: "true"/"1" 等视为 true，"false"/"0"/"" 视为 false，
    //             其他非空字符串视为 true
    // 旧实现 FilterExecutor 直接 `AsInt() != 0`，对 VARCHAR/FLOAT 结果
    // 会读到无关 int_val_ 字段——是潜在 bug。
    bool IsTruthy() const;

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
