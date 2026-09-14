#include "storage_engine/Value.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>

namespace sqlcompiler {

namespace {

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

// 52_data_types: DECIMAL 类型输出。DECIMAL 按文本持久化，运行时仍然作为
// VARCHAR 在 Value 层流转，但写入列时需要把 FLOAT 字面量精确转换为十进制
// 字符串（如 99999999.99），不能走 FormatDouble 那种"修剪尾零"的表示，
// 否则会得到 "99999999.989999..." 这种带 IEEE-754 噪声的形式。
//
// 选用 %g 风格 + 12 位有效数字：足以覆盖 DECIMAL(10, 2) 之类的常见精度，
// 又能在尾随零可被省略时不产生多余小数位。
//
// 此函数被声明在 storage_engine/Value.h 并被 InsertExecutor.cpp 等调用，
// 因此必须放在匿名命名空间外以保留外部链接性。
std::string FormatDecimal(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    return std::string(buf);
}

ValueType ValueTypeFromString(const std::string& type_name) {
    std::string u = ToUpper(type_name);
    if (u == "INT" || u == "INTEGER" || u == "BIGINT") return ValueType::INTEGER;
    // 52_data_types: SMALLINT/TINYINT 沿用 INT 运行期表示。
    if (u == "SMALLINT" || u == "TINYINT") return ValueType::INTEGER;
    // BOOLEAN / BOOL 运行期表示为 INTEGER（0/1）。
    if (u == "BOOLEAN" || u == "BOOL") return ValueType::INTEGER;
    if (u == "FLOAT" || u == "DOUBLE" || u == "REAL") return ValueType::FLOAT;
    // 52_data_types: DECIMAL/NUMERIC 按精确文本持久化，运行时按 VARCHAR 流转，
    // 与 DATE / TIMESTAMP 一致——写入路径由 CoerceToColumnType 负责把 FLOAT
    // 字面量格式化为十进制字符串。
    if (u == "VARCHAR" || u == "STRING" || u == "TEXT" || u == "CHAR" ||
        u == "DATE" || u == "TIMESTAMP" || u == "TIME" ||
        u == "DECIMAL" || u == "NUMERIC" ||
        u == "JSON" || u == "UUID") return ValueType::VARCHAR;
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

bool Value::IsTruthy() const {
    // SQL 三值逻辑：NULL 在 WHERE 中按"不通过"处理。
    if (IsNull()) return false;
    switch (type_) {
        case ValueType::INTEGER:
            return int_val_ != 0;
        case ValueType::FLOAT:
            // NaN 视为 false；其他非 0 即 true。
            return float_val_ != 0.0 && !std::isnan(float_val_);
        case ValueType::VARCHAR: {
            // 空串视为 false；其余大小写不敏感匹配 "false"/"0"。
            const std::string& s = str_val_;
            if (s.empty()) return false;
            if (s.size() == 1 && s[0] == '0') return false;
            // 长度上限：长字符串几乎不可能是布尔字面量，直接 true
            if (s.size() > 5) return true;
            // 小写化后比对 "false"/"0"
            char buf[8] = {};
            for (size_t i = 0; i < s.size() && i < sizeof(buf) - 1; ++i) {
                buf[i] = static_cast<char>(std::tolower(
                    static_cast<unsigned char>(s[i])));
            }
            buf[s.size()] = '\0';
            return !(std::strcmp(buf, "false") == 0 || std::strcmp(buf, "0") == 0);
        }
        case ValueType::NULL_TYPE:
            return false;
    }
    return false;
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
            // BUG-5 (round 2): 不再用 in-band marker 表示 NULL。
            // 旧实现写一个特殊 sentinel（如 -1 / INT_MIN）让 reader 检测，但
            // sentinel 总会与某个合法 INT / FLOAT 值碰撞（最严重的就是 -1 与
            // 自身冲突）。新做法：slot 写「默认值」（int=0 / float=0.0 / varchar 长度=0），
            // NULL 信息由 Tuple 级别的 NULL bitmap 承载，reader 按位查 bitmap
            // 强制把该列设为 NULL_TYPE。
            //
            // 这样 -2147483648 / -1 / 任何罕见 int / 任何 double 都能正常存储；
            // 同时也彻底消除了 FLOAT slot「前 4 字节模式巧合等于 marker」的
            // ~1/2^32 碰撞。
            switch (column_type) {
                case ValueType::INTEGER:
                    WriteInt32(buf, 0);
                    return kIntBytes;
                case ValueType::FLOAT:
                    WriteDouble(buf, 0.0);
                    return static_cast<size_t>(kFloatBytes);
                case ValueType::VARCHAR:
                    WriteInt32(buf, 0);
                    return kIntBytes;
            }
            return 0;
        }
    }
    return 0;
}

size_t Value::DeserializeFrom(const char* buf, ValueType type, Value* out) {
    if (!out) return 0;
    out->type_ = type;
    switch (type) {
        case ValueType::INTEGER: {
            // BUG-5 (round 2): 不再检测 in-band NULL marker。NULL 由 Tuple
            // 级 bitmap 标识，Deserialize 只负责把字节读出来当作合法 INT 值。
            out->int_val_ = ReadInt32(buf);
            out->float_val_ = 0.0;
            out->str_val_.clear();
            return kIntBytes;
        }
        case ValueType::FLOAT: {
            out->float_val_ = ReadDouble(buf);
            out->int_val_ = 0;
            out->str_val_.clear();
            return static_cast<size_t>(kFloatBytes);
        }
        case ValueType::VARCHAR: {
            int32_t len = ReadInt32(buf);
            // 防御性：负长度（旧格式下是 NULL marker）。新格式下不会发生，
            // 但读老 DB 时仍可能撞见；把负长度的内容当作 0 长度处理。
            if (len < 0) len = 0;
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

namespace {

// 跨类型 VARCHAR ↔ INT/FLOAT 时，把 VARCHAR 按十进制文本解析为 double。
// 用于 DECIMAL/NUMERIC 等按文本持久化的数值列与数值字面量比较。
// 失败时返回 false（空串 / 非数字字符 / 浮点溢出均视为不可解析）。
bool ParseVarcharAsDouble(const Value& v, double* out) {
    try {
        size_t pos = 0;
        std::string s = v.AsVarchar();
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) return false;
        double d = std::stod(s, &pos);
        *out = d;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

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
        // 52_data_types: VARCHAR 与数值类型（INT/FLOAT）的跨类型比较。
        // DECIMAL 等按文本持久化的数值列与数值字面量比较时，尝试把 VARCHAR
        // 解析为 double 再比较；解析失败（非数字字符串）则视为不相等（返回非零，
        // 避免被误判为"相等"）。两次跨类型方向都要处理，避免 DECIMAL 出现在
        // 比较两侧时的非对称。
        if (a.type_ == ValueType::VARCHAR && (b.type_ == ValueType::FLOAT || b.type_ == ValueType::INTEGER)) {
            double av = 0.0, bv = (b.type_ == ValueType::INTEGER) ? static_cast<double>(b.AsInt()) : b.AsFloat();
            if (!ParseVarcharAsDouble(a, &av)) {
                // 不可解析：返回非零，使调用方识别为"不相等"。
                return 1;
            }
            if (av < bv) return -1;
            if (av > bv) return 1;
            return 0;
        }
        if (b.type_ == ValueType::VARCHAR && (a.type_ == ValueType::FLOAT || a.type_ == ValueType::INTEGER)) {
            double bv = 0.0, av = (a.type_ == ValueType::INTEGER) ? static_cast<double>(a.AsInt()) : a.AsFloat();
            if (!ParseVarcharAsDouble(b, &bv)) {
                return 1;
            }
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

bool Value::CanCompare(const Value& a, const Value& b) {
    // NULL 参与比较（结果为 NULL），不算"不可比"。
    if (a.IsNull() || b.IsNull()) return true;
    // 同类型可直接比较。
    if (a.type_ == b.type_) return true;
    // 两端都为数值（INT / FLOAT）→ 数值提升可比。
    auto is_numeric = [](ValueType t) {
        return t == ValueType::INTEGER || t == ValueType::FLOAT;
    };
    if (is_numeric(a.type_) && is_numeric(b.type_)) return true;
    // VARCHAR ↔ 数值：要求 VARCHAR 能解析为 double。
    double tmp = 0.0;
    if (a.type_ == ValueType::VARCHAR && is_numeric(b.type_)) {
        return ParseVarcharAsDouble(a, &tmp);
    }
    if (b.type_ == ValueType::VARCHAR && is_numeric(a.type_)) {
        return ParseVarcharAsDouble(b, &tmp);
    }
    // 其他跨类型组合（如 VARCHAR ↔ VARCHAR 已在同类型分支处理，
    // NULL ↔ 任意已在 NULL 分支处理）一律视为不可比。
    return false;
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