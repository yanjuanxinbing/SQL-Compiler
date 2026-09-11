#include "storage_engine/Value.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace sqlcompiler {

namespace {

const int32_t kNullMarker = -1;
const int32_t kIntBytes = 4;
const int32_t kFloatBytes = 8;

// 以固定 6 位小数 + 修剪尾零的形式输出浮点数，避免
// `6000 * 1.1 = 6600.000000000001` 这类 IEEE-754 噪声被原样打印。
// 大值/小值仍保留 fixed 表示，不会出现科学计数法。NaN / Inf 走通用格式。
std::string FormatDouble(double v) {
    if (std::isnan(v)) {
        return "nan";
    }
    if (std::isinf(v)) {
        return v < 0 ? "-inf" : "inf";
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << v;
    std::string s = oss.str();
    // 修剪小数部分的尾零和孤立的小数点，例如
    //   "6600.000000" -> "6600"
    //   "5500.500000" -> "5500.5"
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last == dot) {
            // 小数点后全是 0：去掉小数点
            s.erase(dot);
        } else if (last != std::string::npos) {
            s.erase(last + 1);
        }
    }
    return s;
}
const int32_t kVarcharLenBytes = 4;

std::string ToUpper(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return out;
}

void WriteInt32(char* buf, int32_t v) {
    std::memcpy(buf, &v, sizeof(int32_t));
}

int32_t ReadInt32(const char* buf) {
    int32_t v;
    std::memcpy(&v, buf, sizeof(int32_t));
    return v;
}

void WriteDouble(char* buf, double v) {
    std::memcpy(buf, &v, sizeof(double));
}

double ReadDouble(const char* buf) {
    double v;
    std::memcpy(&v, buf, sizeof(double));
    return v;
}

}  // namespace

ValueType ValueTypeFromString(const std::string& type_name) {
    std::string u = ToUpper(type_name);
    if (u == "INT" || u == "INTEGER" || u == "BIGINT") return ValueType::INTEGER;
    if (u == "FLOAT" || u == "DOUBLE" || u == "DECIMAL") return ValueType::FLOAT;
    // 45_datetime: DATE / TIMESTAMP 持久化为 VARCHAR（按 YYYY-MM-DD 或
    // YYYY-MM-DD HH:MM:SS 文本），运行时按 VARCHAR 处理，比较 / 算术在
    // ExpressionEvaluator 的 ExtractField / ApplyInterval 路径上完成。
    if (u == "VARCHAR" || u == "STRING" || u == "TEXT" || u == "CHAR" ||
        u == "DATE" || u == "TIMESTAMP") return ValueType::VARCHAR;
    return ValueType::NULL_TYPE;
}

Value::Value() : type_(ValueType::NULL_TYPE), int_val_(0), float_val_(0.0) {
}

Value Value::MakeInt(int32_t v) {
    Value val;
    val.type_ = ValueType::INTEGER;
    val.int_val_ = v;
    return val;
}

Value Value::MakeFloat(double v) {
    Value val;
    val.type_ = ValueType::FLOAT;
    val.float_val_ = v;
    return val;
}

Value Value::MakeVarchar(const std::string& v) {
    Value val;
    val.type_ = ValueType::VARCHAR;
    val.str_val_ = v;
    return val;
}

Value Value::MakeNull() {
    Value val;
    val.type_ = ValueType::NULL_TYPE;
    return val;
}

ValueType Value::GetType() const {
    return type_;
}

bool Value::IsNull() const {
    return type_ == ValueType::NULL_TYPE;
}

int32_t Value::AsInt() const {
    return int_val_;
}

double Value::AsFloat() const {
    return float_val_;
}

const std::string& Value::AsVarchar() const {
    return str_val_;
}

size_t Value::SerializeTo(char* buf, ValueType column_type) const {
    switch (type_) {
        case ValueType::INTEGER: {
            WriteInt32(buf, int_val_);
            return kIntBytes;
        }
        case ValueType::FLOAT: {
            WriteDouble(buf, float_val_);
            return static_cast<size_t>(kFloatBytes);
        }
        case ValueType::VARCHAR: {
            int32_t len = static_cast<int32_t>(str_val_.size());
            WriteInt32(buf, len);
            if (len > 0) {
                std::memcpy(buf + kVarcharLenBytes, str_val_.data(), static_cast<size_t>(len));
            }
            return static_cast<size_t>(kVarcharLenBytes + len);
        }
        case ValueType::NULL_TYPE: {
            // Write a NULL marker whose width matches the column's declared
            // type so subsequent columns are read at the correct offset.
            WriteInt32(buf, kNullMarker);
            if (column_type == ValueType::FLOAT) {
                // Pad with 4 zero bytes so total width is kFloatBytes (8).
                std::memset(buf + kIntBytes, 0, kIntBytes);
                return static_cast<size_t>(kFloatBytes);
            }
            return kIntBytes;
        }
    }
    return 0;
}

size_t Value::DeserializeFrom(const char* buf, ValueType type, Value* out) {
    if (!out) return 0;
    out->type_ = type;
    switch (type) {
        case ValueType::INTEGER: {
            int32_t v = ReadInt32(buf);
            if (v == kNullMarker) {
                // Magic marker: stored as -1 means NULL
                out->type_ = ValueType::NULL_TYPE;
                out->int_val_ = 0;
            } else {
                out->int_val_ = v;
            }
            out->float_val_ = 0.0;
            out->str_val_.clear();
            return kIntBytes;
        }
        case ValueType::FLOAT: {
            // Detect NULL marker (-1) at the start of an 8-byte slot.
            int32_t marker = ReadInt32(buf);
            if (marker == kNullMarker) {
                out->type_ = ValueType::NULL_TYPE;
                out->int_val_ = 0;
                out->float_val_ = 0.0;
                out->str_val_.clear();
                return static_cast<size_t>(kFloatBytes);
            }
            out->float_val_ = ReadDouble(buf);
            out->int_val_ = 0;
            out->str_val_.clear();
            return static_cast<size_t>(kFloatBytes);
        }
        case ValueType::VARCHAR: {
            int32_t len = ReadInt32(buf);
            if (len < 0) {
                out->type_ = ValueType::NULL_TYPE;
                out->int_val_ = 0;
                out->float_val_ = 0.0;
                out->str_val_.clear();
                return kIntBytes;
            }
            out->str_val_.assign(buf + kVarcharLenBytes, static_cast<size_t>(len));
            out->int_val_ = 0;
            out->float_val_ = 0.0;
            return static_cast<size_t>(kVarcharLenBytes + len);
        }
        case ValueType::NULL_TYPE: {
            out->int_val_ = 0;
            out->float_val_ = 0.0;
            out->str_val_.clear();
            return kIntBytes;
        }
    }
    return 0;
}

size_t Value::SerializedSize(ValueType column_type) const {
    switch (type_) {
        case ValueType::INTEGER:    return static_cast<size_t>(kIntBytes);
        case ValueType::FLOAT:      return static_cast<size_t>(kFloatBytes);
        case ValueType::VARCHAR:    return static_cast<size_t>(kVarcharLenBytes + str_val_.size());
        case ValueType::NULL_TYPE:
            // Match the column's declared width so the slot stays aligned.
            if (column_type == ValueType::FLOAT) return static_cast<size_t>(kFloatBytes);
            return static_cast<size_t>(kIntBytes);
    }
    return 0;
}

int Value::Compare(const Value& a, const Value& b) {
    if (a.IsNull() || b.IsNull()) return 0;
    // 跨类型数值比较：把 INT 提升为 FLOAT
    if (a.type_ != b.type_) {
        if ((a.type_ == ValueType::INTEGER || a.type_ == ValueType::FLOAT) &&
            (b.type_ == ValueType::INTEGER || b.type_ == ValueType::FLOAT)) {
            double av = (a.type_ == ValueType::INTEGER) ? a.AsInt() : a.AsFloat();
            double bv = (b.type_ == ValueType::INTEGER) ? b.AsInt() : b.AsFloat();
            if (av < bv) return -1;
            if (av > bv) return 1;
            return 0;
        }
        return 0;
    }
    switch (a.type_) {
        case ValueType::INTEGER:
            if (a.int_val_ < b.int_val_) return -1;
            if (a.int_val_ > b.int_val_) return 1;
            return 0;
        case ValueType::FLOAT:
            if (a.float_val_ < b.float_val_) return -1;
            if (a.float_val_ > b.float_val_) return 1;
            return 0;
        case ValueType::VARCHAR:
            if (a.str_val_ < b.str_val_) return -1;
            if (a.str_val_ > b.str_val_) return 1;
            return 0;
        case ValueType::NULL_TYPE:
            return 0;
    }
    return 0;
}

std::string Value::ToString() const {
    switch (type_) {
        case ValueType::INTEGER:
            return std::to_string(int_val_);
        case ValueType::FLOAT:
            return FormatDouble(float_val_);
        case ValueType::VARCHAR:
            return str_val_;
        case ValueType::NULL_TYPE:
            return "NULL";
    }
    return "";
}

}  // namespace sqlcompiler