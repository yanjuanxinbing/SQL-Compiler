#pragma once

#include <vector>

#include "catalog/SystemCatalog.h"
#include "execution/Executor.h"
#include "semantic/SymbolTable.h"
#include "storage_engine/TableHeap.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// 列约束校验（NOT NULL / VARCHAR(N) 长度 / PRIMARY KEY 唯一性 / 列级 CHECK）。
//
// 设计说明：
//   - 约束在写入路径上校验，而不是在语义分析阶段——因为唯一性依赖运行时数据。
//   - INSERT 与 UPDATE 共用此入口，避免两条路径的约束语义漂移。
//   - 违约时抛出 CompilerException(ErrorStage::SEMANTIC)，由 Database::ExecuteSQL
//     统一转成 ExecutionResult 的错误信息，语句整体不生效。
//
// 参数 exclude_rid：UPDATE 场景下指向被改写的那一行，唯一性扫描需跳过它自身，
// 否则「原地更新且主键不变」会被误判为冲突。INSERT 传 nullptr。
//
// 唯一性校验优先走主键 B+Tree 索引点查（O(log N)）。仅当该表没有可用的主键
// 索引时（旧库、或建表时索引创建失败）才退回全表顺序扫描——保留这条兜底路径
// 是为了让索引子系统即使缺席，约束语义也不会静默失效。
//
// 参数 ctx 为可空指针：CHECK 表达式求值时需要 ExecutionContext 来支持子查询
// （虽然 DEFAULT 表达式不接受子查询，CHECK 仍可能包含）。传 nullptr 时退化为
// 不可求子查询的常规表达式求值。
void ValidateRowConstraints(SystemCatalog* catalog, const TableInfo& table_info,
                            TableHeap* heap, const std::vector<Value>& row,
                            const RID* exclude_rid,
                            ExecutionContext* ctx = nullptr);

// 由列声明类型推导出运行时类型向量（序列化/反序列化用）。
std::vector<ValueType> BuildColumnTypes(const TableInfo& table_info);

}  // namespace sqlcompiler
