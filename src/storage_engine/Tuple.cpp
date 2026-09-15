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
    size_t n = std::min(values_.size(), column_types.size());

    // BUG-5 (round 2): NULL bitmap —— 每列 1 bit，置于 tuple 字节流的最前面。
    // 旧实现依赖 in-band marker（特殊字节模式），总会与合法值碰撞。
    // 新实现把 NULL 信息外置到 bitmap，per-column slot 只存值本身，
    // 任何 INT（包括 -2147483648 / -1）/ FLOAT / VARCHAR 都可正常存储。
    const size_t bitmap_bytes = (n + 7) / 8;

    // 第一遍：算总长度，避开逐列 resize 的 O(log n) 重分配链。
    // vector::resize 在容量不够时触发整体重分配，n 列就触发 O(log n) 次。
    // 改成两遍后只一次 resize(total)，分配次数从 O(log n) 降到 O(1)。
    size_t total = bitmap_bytes;
    for (size_t i = 0; i < n; ++i) {
        total += values_[i].SerializedSize(column_types[i]);
    }
    // Trailing values without column metadata fall back to INTEGER width.
    // 这些"无 schema 尾段"的 NULL bitmap 信息已由 n 列的 bitmap 覆盖；
    // 尾段值不会被 Deserialize 读回（Deserialize 只看 column_types.size()）。
    for (size_t i = n; i < values_.size(); ++i) {
        total += values_[i].SerializedSize(ValueType::INTEGER);
    }

    // 一次性分配目标缓冲区。
    std::vector<char> out(total);
    char* dst = out.data();

    // NULL bitmap 清零并按列填位。
    std::memset(dst, 0, bitmap_bytes);
    for (size_t i = 0; i < n; ++i) {
        if (values_[i].IsNull()) {
            dst[i / 8] |= static_cast<char>(1 << (i % 8));
        }
    }

    // 第二遍：把每列值直接 memcpy 到 out 的指定偏移。
    size_t at = bitmap_bytes;
    for (size_t i = 0; i < n; ++i) {
        at += values_[i].SerializeTo(dst + at, column_types[i]);
    }
    for (size_t i = n; i < values_.size(); ++i) {
        at += values_[i].SerializeTo(dst + at, ValueType::INTEGER);
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