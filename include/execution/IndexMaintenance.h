#pragma once

#include <string>
#include <vector>

#include "catalog/IndexInfo.h"
#include "catalog/SystemCatalog.h"
#include "index/IndexKey.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 索引维护：INSERT / UPDATE / DELETE 三条写路径共用的插桩入口。
//
// 之所以抽成独立单元，是因为「堆与索引必须同时更新」这条不变式一旦在三处各写
// 一遍，就必然会漂移——漏掉一处的后果是索引里存着指向已删除记录的 RID，查询
// 结果开始出现幽灵行，而且只在走索引时复现。
//
// 写顺序约定（调用方必须遵守）：
//   1. CheckUniqueIndexes()   —— 只读预检，此时堆尚未写入，冲突可直接抛错
//   2. heap->InsertTuple()    —— 写堆
//   3. InsertIntoIndexes()    —— 写索引
// 这个顺序保证「唯一性冲突」不会留下半写状态，无需回滚机制。

// 从一行数据中抽取某个索引的键。列缺失或值为 NULL 时返回 false
// （索引键不允许 NULL，见 ValidateIndexableColumns）。
bool BuildIndexKeyFromRow(const TableInfo& table_info, const IndexInfo& index_info,
                          const std::vector<Value>& row, IndexKey* out);

// 唯一索引预检。发现冲突抛 CompilerException(SEMANTIC)。
// exclude_rid：UPDATE 时指向被改写的行自身，避免「键未变的原地更新」自撞。
void CheckUniqueIndexes(SystemCatalog* catalog, const TableInfo& table_info,
                        const std::vector<Value>& row, const RID* exclude_rid);

// 把一行写入该表的全部索引。任一索引写入失败抛异常。
void InsertIntoIndexes(SystemCatalog* catalog, const TableInfo& table_info,
                       const std::vector<Value>& row, const RID& rid);

// 从该表的全部索引中删除一行对应的索引项。
// 索引项不存在不视为错误：删除路径要尽量幂等，否则一次失配会让后续 DELETE 全部
// 报错，反而把可恢复的局面变成不可用。
void DeleteFromIndexes(SystemCatalog* catalog, const TableInfo& table_info,
                       const std::vector<Value>& row, const RID& rid);

}  // namespace sqlcompiler
