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

// ============================================================================
// 53_ddl: FOREIGN KEY 强制执行（外键约束）
// ----------------------------------------------------------------------------
// 语义（在最小可用实现下）：
//   - 子表 INSERT / UPDATE 写入前：对每条 FK，验证 (child_cols) 在 parent 上
//     存在匹配行。若不存在 → 抛 "foreign key violation: ..."。
//   - 父表 DELETE：对每条引用本表的 FK，若声明 RESTRICT（默认）且存在任一
//     子行 → 抛错拒绝 DELETE；若声明 CASCADE → 删除对应子行；若声明 SET NULL
//     → 把子行 FK 列置 NULL（要求 FK 列可空）。
//   - 父表 UPDATE：当前实现对 FK 列更新按 CASCADE/RESTRICT 类同 DELETE 处理。
//
// 备注：
//   - "匹配行" 的判定：依次取 (child_cols[i] 值) 在 parent 上对应 PK 索引中
//     找是否存在 RID。parent 的 PK 索引不存在时退回全表扫描；扫描时对
//     parent_cols 的每一列做精确等值比较。
//   - 对非空列（FK 列含 NULL）：若 child 行该列值为 NULL，按 SQL 语义跳过
//     校验（NULL 不参与 FK 匹配）。
// ============================================================================

// 在 child_table 上的 INSERT / UPDATE 路径上调用：检查每一行即将写入的
// (child_cols) 值都能在 parent_table 上找到匹配行。无匹配抛 SEMANTIC 错误。
// child_table 与 table_info.table_name 必须一致；row 是即将写入的列值列表。
void EnforceChildForeignKeys(SystemCatalog* catalog, const std::string& child_table,
                             const std::vector<Value>& row);

// 在 parent_table 的 DELETE / UPDATE 路径上调用：
//   - 对每条引用本表的 FK 按其 on_delete_action 分支：
//     RESTRICT：扫描子表，若有任何 child_cols 与 parent 即将删除/已删除行
//               的 (parent_cols 值) 相等的行 → 抛错拒绝。
//     CASCADE：扫描子表，删除 child_cols 与 parent 行匹配的子行。
//     SET NULL：扫描子表，把 child_cols 置 NULL（要求这些列可空，否则报错）。
//   - 当前实现一次仅处理单行（典型 DELETE WHERE pk = X 调用一次），因此
//     passed_parent_values 应仅含一行（被删/被改的父行）即将消失的
//     parent_cols 值集合。
//   - exclude_child_rid 可选：当 CASCADE / SET NULL 修改子行时跳过该 RID
//     （例如 UPDATE 同表父子关系时的子行正在被改写）。
void EnforceParentForeignKeys(SystemCatalog* catalog,
                              const std::string& parent_table,
                              const std::vector<Value>& parent_row,
                              const RID* exclude_child_rid = nullptr);

}  // namespace sqlcompiler
