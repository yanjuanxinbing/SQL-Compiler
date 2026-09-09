#pragma once

#include <cstdint>
#include <vector>

#include "storage/Page.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 记录标识符（Record ID）：定位一条记录在哪个页的哪个槽位（slot）
struct RID {
    page_id_t page_id = INVALID_PAGE_ID;
    int slot_num = -1;

    bool IsValid() const;
    bool operator==(const RID& other) const;
};

// 元组：一行记录在内存中的表示，是执行引擎各算子之间传递数据的基本单元
class Tuple {
public:
    Tuple();
    explicit Tuple(std::vector<Value> values);

    const std::vector<Value>& GetValues() const;
    const Value& GetValue(size_t column_index) const;
    size_t ColumnCount() const;

    const RID& GetRid() const;
    void SetRid(const RID& rid);

    // 序列化为字节数组，供TableHeap写入页面
    std::vector<char> Serialize() const;

    // 根据每一列的类型描述，从字节数组反序列化出一个Tuple
    static Tuple Deserialize(const char* data, const std::vector<ValueType>& column_types);

private:
    std::vector<Value> values_;
    RID rid_;
};

}  // namespace sqlcompiler
