#include "storage_engine/Value.h"

#include <cctype>
#include <cstring>

namespace sqlcompiler {

namespace {

const int32_t kNullMarker = -1;
const int32_t kIntBytes = 4;
const int32_t kFloatBytes = 8;
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
    if (u == "INT" || u == "INTEGER") return ValueType::INTEGER;
    if (u == "FLOAT" || u == "DOUBLE") return ValueType::FLOAT;
    if (u == "VARCHAR" || u == "STRING" || u == "TEXT" || u == "CHAR") return ValueType::VARCHAR;
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

size_t Value::SerializeTo(char* buf) const {
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
            WriteInt32(buf, kNullMarker);
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

size_t Value::SerializedSize() const {
    switch (type_) {
        case ValueType::INTEGER:    return static_cast<size_t>(kIntBytes);
        case ValueType::FLOAT:      return static_cast<size_t>(kFloatBytes);
        case ValueType::VARCHAR:    return static_cast<size_t>(kVarcharLenBytes + str_val_.size());
        case ValueType::NULL_TYPE:  return static_cast<size_t>(kIntBytes);
    }
    return 0;
}

int Value::Compare(const Value& a, const Value& b) {
    if (a.type_ != b.type_) {
        if (a.IsNull()) return b.IsNull() ? 0 : -1;
        if (b.IsNull()) return 1;
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
            return std::to_string(float_val_);
        case ValueType::VARCHAR:
            return str_val_;
        case ValueType::NULL_TYPE:
            return "NULL";
    }
    return "";
}

}  // namespace sqlcompiler