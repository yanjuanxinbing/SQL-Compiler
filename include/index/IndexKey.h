#pragma once

#include <string>
#include <vector>

#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 索引键：一组 Value，支持复合键（PRIMARY KEY(a, b) 即两列）。
//
// schema（每列的 ValueType）不随键存盘，而是由 IndexInfo 统一持有——同一棵树里
// 所有键的列类型必然相同，逐键存一份是纯粹的空间浪费。
struct IndexKey {
    std::vector<Value> values;

    IndexKey() = default;
    explicit IndexKey(std::vector<Value> vals) : values(std::move(vals)) {}

    size_t ColumnCount() const { return values.size(); }
    std::string ToString() const;
};

// 仅比较键列，按字典序。用于唯一性判定与范围边界。
int CompareKeyOnly(const IndexKey& a, const IndexKey& b);

// 先比键列，相等时以 RID 作 tiebreaker。
//
// 这是支持非唯一索引的关键：重复键在叶子内按 (key, rid) 严格全序排列，因此
// 插入位置唯一、删除能精确定位到某一条，而无需在叶子里维护 RID 链表。
// 相比「把 RID 拼进键字节」的做法，这里 RID 已经存在 slot 头里，不必膨胀键长度。
int CompareKeyThenRid(const IndexKey& a, const RID& rid_a,
                      const IndexKey& b, const RID& rid_b);

// 序列化：逐列调用 Value::SerializeTo，复用堆表那套编码。
// 返回的字节序列可直接写入页面记录区。
std::vector<char> SerializeKey(const IndexKey& key,
                               const std::vector<ValueType>& schema);

// 反序列化：从 buf 按 schema 读出各列。len 用于边界校验，越界时返回 false。
bool DeserializeKey(const char* buf, size_t len,
                    const std::vector<ValueType>& schema, IndexKey* out);

// 序列化后的字节数（不实际写入），用于判断页内空间是否足够。
size_t SerializedKeySize(const IndexKey& key,
                         const std::vector<ValueType>& schema);

}  // namespace sqlcompiler
