#include "storage_engine/Tuple.h"

#include <cstring>
#include <stdexcept>

namespace sqlcompiler {

bool RID::IsValid() const {
    return page_id != INVALID_PAGE_ID && slot_num >= 0;
}

bool RID::operator==(const RID& other) const {
    return page_id == other.page_id && slot_num == other.slot_num;
}

Tuple::Tuple() {
}

Tuple::Tuple(std::vector<Value> values) : values_(std::move(values)) {
}

const std::vector<Value>& Tuple::GetValues() const {
    return values_;
}

const Value& Tuple::GetValue(size_t column_index) const {
    if (column_index >= values_.size()) {
        throw std::out_of_range("Tuple::GetValue: column index out of range");
    }
    return values_[column_index];
}

size_t Tuple::ColumnCount() const {
    return values_.size();
}

const RID& Tuple::GetRid() const {
    return rid_;
}

void Tuple::SetRid(const RID& rid) {
    rid_ = rid;
}

std::vector<char> Tuple::Serialize(const std::vector<ValueType>& column_types) const {
    std::vector<char> out;
    size_t n = std::min(values_.size(), column_types.size());

    // BUG-5 (round 2): NULL bitmap —— 每列 1 bit，置于 tuple 字节流的最前面。
    // 旧实现依赖 in-band marker（特殊字节模式），总会与合法值碰撞。
    // 新实现把 NULL 信息外置到 bitmap，per-column slot 只存值本身，
    // 任何 INT（包括 -2147483648 / -1）/ FLOAT / VARCHAR 都可正常存储。
    size_t bitmap_bytes = (n + 7) / 8;
    size_t cur = out.size();
    out.resize(cur + bitmap_bytes);
    std::memset(out.data() + cur, 0, bitmap_bytes);
    for (size_t i = 0; i < n; ++i) {
        if (values_[i].IsNull()) {
            out[cur + i / 8] |= static_cast<char>(1 << (i % 8));
        }
    }

    for (size_t i = 0; i < n; ++i) {
        const Value& v = values_[i];
        ValueType col_ty = column_types[i];
        size_t sz = v.SerializedSize(col_ty);
        size_t at = out.size();
        out.resize(at + sz);
        v.SerializeTo(out.data() + at, col_ty);
    }
    // Trailing values without column metadata fall back to the previous size.
    // 这些"无 schema 尾段"的 NULL bitmap 信息已由 n 列的 bitmap 覆盖；
    // 尾段值不会被 Deserialize 读回（Deserialize 只看 column_types.size()）。
    for (size_t i = n; i < values_.size(); ++i) {
        const Value& v = values_[i];
        size_t sz = v.SerializedSize(ValueType::INTEGER);
        size_t at = out.size();
        out.resize(at + sz);
        v.SerializeTo(out.data() + at, ValueType::INTEGER);
    }
    return out;
}

Tuple Tuple::Deserialize(const char* data, const std::vector<ValueType>& column_types) {
    Tuple t;
    t.values_.reserve(column_types.size());

    // BUG-5 (round 2): 先读 NULL bitmap，再读各列值；bitmap bit i=1 时
    // 把第 i 列强制设为 NULL_TYPE（无视 slot 字节内容）。
    size_t n = column_types.size();
    size_t bitmap_bytes = (n + 7) / 8;
    const char* cur = data + bitmap_bytes;
    for (size_t i = 0; i < n; ++i) {
        Value v;
        size_t used = Value::DeserializeFrom(cur, column_types[i], &v);
        cur += used;
        if (data[i / 8] & static_cast<char>(1 << (i % 8))) {
            v = Value::MakeNull();
        }
        t.values_.push_back(std::move(v));
    }
    return t;
}

}  // namespace sqlcompiler