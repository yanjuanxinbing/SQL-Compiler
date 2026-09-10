#pragma once

#include <memory>
#include <string>

#include "catalog/SystemCatalog.h"
#include "plan/Plan.h"

namespace sqlcompiler {

// 访问路径选择：尝试把 Filter(谓词) -> SeqScan(表) 改写成 IndexScan。
//
// 改写策略刻意保守：只识别能百分之百确定语义的形态，其余一律返回 nullptr 让
// 计划保持原样。理由是访问路径改写一旦出错，症状是「查询静默少返回几行」，
// 比崩溃难查得多；而放弃改写的代价仅仅是慢一点。
//
// 当前识别的谓词形态（列必须是某个索引的最左前缀，且另一侧是常量字面量）：
//   col = const
//   col > / >= / < / <= const
//   col BETWEEN a AND b
//   上述形态用 AND 连接（其余合取项转为 residual_predicate 回表后再判）
//
// 返回 nullptr 表示没有可用索引或谓词形态不支持。
PlanNodePtr TryRewriteWithIndex(SystemCatalog* catalog,
                                const std::string& table_name,
                                const std::string& table_alias,
                                const ExprPtr& predicate);

}  // namespace sqlcompiler
