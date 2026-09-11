#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ast/AST.h"
#include "catalog/SystemCatalog.h"
#include "execution/Executor.h"
#include "storage_engine/Tuple.h"
#include "storage_engine/Value.h"

namespace sqlcompiler {

// =============================================================================
// 触发器执行（47_udf_trigger_view）
//
// 触发器执行模型
// --------------
// 我们采用 SQL 标准"伪记录 NEW/OLD"语义：
//
//   - INSERT：BEFORE INSERT 时 OLD 各列为 NULL，NEW 是本次候选行。
//             AFTER  INSERT 时 OLD 仍为 NULL，NEW 是最终落盘的行。
//   - UPDATE：BEFORE UPDATE 时 OLD 是当前行（修改前），NEW 是用户 SET 子句
//             算出的新行。AFTER UPDATE 时 NEW 是写入后的最终行。
//   - DELETE：BEFORE/AFTER DELETE 时 OLD 是即将被删的行，NEW 视为 OLD 的
//             副本（标准 SQL 在 DELETE 上不允许给 NEW 赋值；本实现仍暴露
//             NEW.col 读取，写回被忽略）。
//
// 触发器体目前只接受 `SET target = expr[, ...]` 形式的语句（与现有 parser 输
// 出的 assignments 列表一致）。执行顺序：
//
//   1. 构造 frame：map<key, Value>
//        - key = "OLD.<col>"      → 旧行同名列值（DELETE 上为待删行）
//        - key = "NEW.<col>"      → 候选行同名列值（可写）
//        - key = "<col>"          → 同 NEW.<col>（仅 UDF 体使用，触发器内不
//                                   暴露为可读别名，避免歧义）
//   2. 对每条 `target = expr`：
//        - 若 target 是 NEW.col：求值 expr（frame 作 outer_bind），写回 frame。
//        - 若 target 是 OLD.col：求值 expr（frame 作 outer_bind），仅写回
//          frame["OLD.col"]，不传回行（标准 SQL 禁止修改 OLD）。
//        - 其它形式：忽略。
//   3. 把 frame["NEW.<col>"] 写回 row_values（适用于 BEFORE INSERT / UPDATE）；
//      AFTER 路径仅用于"已写盘"的日志记录，不修改行。
//
// 当前实现只覆盖 BEFORE INSERT 和 BEFORE UPDATE；BEFORE DELETE 与 AFTER *
// 视为"已接受但 no-op"，按文档约定保留。
// =============================================================================

class TriggerExecutor {
public:
    // 评估 BEFORE 触发器，并返回"经过触发器改写后"的候选行（用于 INSERT / UPDATE）。
    //
    //   - column_index_map / column_types 描述 row_values 的语义。
    //   - timing / event 决定调用 LookupTriggers 的过滤条件。
    //   - old_row（DELETE 上为待删行；UPDATE 上为当前行；INSERT 上可空）用于
    //     构造 OLD.*；INSERT 上省略 old_row 时 OLD 各列视为 NULL。
    //   - 调用方传入的 row_values 是 NEW 的初值。函数内部会就地修改它。
    //
    // 不抛错：触发器体内的运行错误通过 CompilerException 上抛，由调用方捕获。
    static void FireBefore(SystemCatalog* catalog,
                           ExecutionContext* context,
                           const std::string& table_name,
                           TriggerTiming timing,
                           TriggerEvent event,
                           const std::unordered_map<std::string, size_t>& column_index_map,
                           const std::vector<Value>* old_row,  // 可空
                           std::vector<Value>& row_values);    // 就地修改

    // AFTER 触发器：当前实现仅打印日志（按任务文档约定保留为 no-op）。
    static void FireAfter(SystemCatalog* catalog,
                          const std::string& table_name,
                          TriggerEvent event);
};

}  // namespace sqlcompiler