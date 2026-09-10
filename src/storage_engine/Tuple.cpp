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
    for (size_t i = 0; i < n; ++i) {
        const Value& v = values_[i];
        ValueType col_ty = column_types[i];
        size_t sz = v.SerializedSize(col_ty);
        size_t cur = out.size();
        out.resize(cur + sz);
        v.SerializeTo(out.data() + cur, col_ty);
    }
    // Trailing values without column metadata fall back to the previous size.
    for (size_t i = n; i < values_.size(); ++i) {
        const Value& v = values_[i];
        size_t sz = v.SerializedSize(ValueType::INTEGER);
        size_t cur = out.size();
        out.resize(cur + sz);
        v.SerializeTo(out.data() + cur, ValueType::INTEGER);
    }
    return out;
}

Tuple Tuple::Deserialize(const char* data, const std::vector<ValueType>& column_types) {
    Tuple t;
    t.values_.reserve(column_types.size());
    const char* cur = data;
    for (ValueType ty : column_types) {
        Value v;
        size_t used = Value::DeserializeFrom(cur, ty, &v);
        cur += used;
        t.values_.push_back(std::move(v));
    }
    return t;
}

}  // namespace sqlcompiler