#include "index/IndexKey.h"

#include <algorithm>
#include <cstring>

namespace sqlcompiler {

std::string IndexKey::ToString() const {
    std::string out = "(";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out += ", ";
        out += values[i].ToString();
    }
    out += ")";
    return out;
}

int CompareKeyOnly(const IndexKey& a, const IndexKey& b) {
    const size_t n = std::min(a.values.size(), b.values.size());
    for (size_t i = 0; i < n; ++i) {
        int c = Value::Compare(a.values[i], b.values[i]);
        if (c != 0) return c;
    }
    // 前缀相等时短者在前。这条规则让「用最左前缀做范围下界」能正确工作：
    // 例如复合键 (a) 作为下界时，会排在所有 (a, *) 之前。
    if (a.values.size() < b.values.size()) return -1;
    if (a.values.size() > b.values.size()) return 1;
    return 0;
}

int CompareKeyThenRid(const IndexKey& a, const RID& rid_a,
                      const IndexKey& b, const RID& rid_b) {
    int c = CompareKeyOnly(a, b);
    if (c != 0) return c;
    if (rid_a.page_id != rid_b.page_id) {
        return rid_a.page_id < rid_b.page_id ? -1 : 1;
    }
    if (rid_a.slot_num != rid_b.slot_num) {
        return rid_a.slot_num < rid_b.slot_num ? -1 : 1;
    }
    return 0;
}

size_t SerializedKeySize(const IndexKey& key,
                         const std::vector<ValueType>& schema) {
    size_t total = 0;
    const size_t n = std::min(key.values.size(), schema.size());
    for (size_t i = 0; i < n; ++i) {
        total += key.values[i].SerializedSize(schema[i]);
    }
    return total;
}

std::vector<char> SerializeKey(const IndexKey& key,
                               const std::vector<ValueType>& schema) {
    std::vector<char> buf(SerializedKeySize(key, schema));
    size_t offset = 0;
    const size_t n = std::min(key.values.size(), schema.size());
    for (size_t i = 0; i < n; ++i) {
        offset += key.values[i].SerializeTo(buf.data() + offset, schema[i]);
    }
    buf.resize(offset);
    return buf;
}

bool DeserializeKey(const char* buf, size_t len,
                    const std::vector<ValueType>& schema, IndexKey* out) {
    if (buf == nullptr || out == nullptr) return false;
    out->values.clear();
    out->values.reserve(schema.size());
    size_t offset = 0;
    for (ValueType t : schema) {
        // 必须在调用 Value::DeserializeFrom 之前完成边界校验：该函数自身不做
        // 越界检查，尤其 VARCHAR 会先读 4 字节长度前缀再按该长度 assign，
        // 页面损坏时足以读出页外内存。
        size_t need = 0;
        switch (t) {
            case ValueType::FLOAT:
                need = 8;
                break;
            case ValueType::VARCHAR: {
                if (offset + 4 > len) return false;
                int32_t str_len = 0;
                std::memcpy(&str_len, buf + offset, sizeof(int32_t));
                if (str_len < 0) {
                    need = 4;  // NULL 标记
                } else {
                    need = 4 + static_cast<size_t>(str_len);
                }
                break;
            }
            default:
                need = 4;
                break;
        }
        if (offset + need > len) return false;

        Value v;
        size_t consumed = Value::DeserializeFrom(buf + offset, t, &v);
        if (consumed == 0) return false;
        offset += consumed;
        out->values.push_back(std::move(v));
    }
    return true;
}

}  // namespace sqlcompiler
