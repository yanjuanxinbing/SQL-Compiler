# 隔离级别落地：SET TRANSACTION + 行级锁（表锁+行锁并存）

## Context（背景与目标）

事务隔离级别（READ UNCOMMITTED / READ COMMITTED / SERIALIZABLE）与事务级 S/X 锁管理器已在执行边界接入（表级锁，资源 id = 表堆首页页号），`TestIsolationLevels` 已验证 RC 语句末释放读锁 / SERIALIZABLE 持有到提交。但有两处未落地：

1. **隔离级别无法在 SQL 层配置**——目前只能通过 C++ API `SetIsolationLevel()` 设置，CLI/脚本无法切换。缺一条 `SET TRANSACTION ISOLATION LEVEL ...` 语句。
2. **仍是表级粒度**——计划里"行级锁 / MVCC 快照"为待续项。本次推进行级锁，按用户选定采用**表锁+行锁并存（低风险）**策略：保留现有语句边界表锁作安全网，在执行算子内追加行级 S/X 锁，证明行锁基础设施链路可用，为后续放宽表锁打底。

本次目标（用户选定"两者都做"）：
- **A**：`SET TRANSACTION ISOLATION LEVEL {READ COMMITTED | READ UNCOMMITTED | SERIALIZABLE}` 全链路实现 + 测试。
- **B**：执行算子内行级 S/X 锁（读表逐行 S、写表逐行 X），含行锁语句末释放（仅 RC 读锁）+ 行资源 id 编码 + 测试。

**如实说明并存模式的边界**：在"表锁+行锁并存"下，语句边界表锁已先于执行器独占写表 / 共享读表，行锁多数时候被表锁覆盖，行为上不可观察；其价值是打通行锁链路、确立 RID 编码与释放契约，为后续"仅行锁/放宽表读锁"的并发收益铺路。因此测试聚焦：行资源 id 编码正确性、LockManager 行级层冲突/死锁单元验证、全量不回归；并行的 SQL 设置隔离级别则是本次真正用户可观测的交付。

---

## Part A — `SET TRANSACTION ISOLATION LEVEL`（从词法到执行）

### A0 既有事实
- `SetIsolationLevel(IsolationLevel)` 已存在（`include/txn/TransactionManager.h` L107），写 `default_isolation_`，`BEGIN` 时采样进新事务（`src/txn/TransactionManager.cpp` L52）。
- `IsolationLevel` 三值恰与三档对应（`include/txn/Transaction.h` L23-27）。
- Lexer 已有 `SET` / `TRANSACTION` 关键词（`src/lexer/Lexer.cpp` L21、L133），**无需新增**。

### A1 Lexer（`include/lexer/Token.h` + `src/lexer/Lexer.cpp` + `src/lexer/Token.cpp`）
- `TokenType` 枚举（`KEYWORD_TRANSACTION` 附近）新增 6 个：`KEYWORD_ISOLATION / KEYWORD_LEVEL / KEYWORD_READ / KEYWORD_COMMITTED / KEYWORD_UNCOMMITTED / KEYWORD_SERIALIZABLE`。
- `KeywordTable()` 的"40_txn_view_udf"块追加对应 `{"ISOLATION", ...}` 等 6 行。
- `TokenTypeToString` 追加 6 个 case（便于报错/调试）。
- 注意代价：`READ`/`LEVEL` 等成保留字；parser 只在特定上下文消费，不影响既有查询。

### A2 AST（`include/ast/AST.h` + `src/ast/AST.cpp`）
- `NodeType` 在 `RELEASE_SAVEPOINT_STMT` 后新增 `SET_ISOLATION_STMT`。
- 新增 `SetIsolationStatement : Statement`，字段 `int isolation_level`（存 `IsolationLevel` 整值；AST.h 不引入 `txn/Transaction.h` 以避耦合），实现 `GetType()` 与 `ToString()`（风格对齐 `BeginStatement`，`src/ast/AST.cpp` L675-678）。

### A3 Parser（`include/parser/Parser.h` + `src/parser/Parser.cpp`）
- `ParseStatement()` switch（约 L213 的 `KEYWORD_RELEASE` 后）加 `case KEYWORD_SET: return ParseSetIsolationStatement();`。
- 新函数：`SET`→`TRANSACTION`→`ISOLATION`→`LEVEL` 依次 `Expect`，然后按当前 token 分派：`READ COMMITTED / READ UNCOMMITTED / SERIALIZABLE`，映射到 `IsolationLevel` 整值；其它值抛 `CompilerException(ErrorStage::SYNTAX, ...)`。Parser.cpp 顶部需 `#include "txn/Transaction.h"`（唯一承担 keyword→枚举映射的 TU）。

### A4 Plan（`include/plan/Plan.h` + `include/plan/Planner.h` + `src/plan/Plan.cpp` + `src/plan/Planner.cpp`）
- `Plan.h`：`#include "txn/Transaction.h"`；`PlanNodeType` 在 `RELEASE_SP` 后加 `SET_ISOLATION`；新增 `SetIsolationNode : PlanNode`（持 `IsolationLevel isolation_level` + `GetType()`/`ToString()`）。
- `Planner.h`：声明 `PlanNodePtr PlanSetIsolation(const SetIsolationStatement& stmt);`。
- `Planner.cpp`：`CreatePlan` switch（约 L175）加 `case SET_ISOLATION_STMT:` 转发；实现 `PlanSetIsolation`（构造 `SetIsolationNode`，风格对齐 `PlanSavepoint`，约 L764-770）。
- `Plan.cpp`：实现 `SetIsolationNode` 构造/`GetType`/`ToString`。

### A5 Executor（`include/execution/TransactionExecutor.h` + `src/execution/TransactionExecutor.cpp` + `src/execution/ExecutionEngine.cpp`）
- 新增 `SetIsolationExecutor`：`Init()` 里 `context_->GetTransactionManager()->SetIsolationLevel(level_)`，`Next()` 恒 `false`（完全对齐 `SavepointExecutor` 模板，`src/execution/TransactionExecutor.cpp` L70-82）。
- `ExecutionEngine::BuildExecutor`（约 L936-952 区）加 `case PlanNodeType::SET_ISOLATION:`。
- 无需改 `is_query`（白名单式，新节点自动走 `message="OK"`，与 BEGIN/COMMIT 一致）。

### A6 测试
- **行为测试**（主要）：`tests/storage/storage_ut.cpp` 新增 `TestSetIsolationStatement()`，注册到 `main()`（`TestIsolationLevels` 之后）：
  - `SET ... READ COMMITTED` 成功 → `mg->GetIsolationLevel()==kReadCommitted`；
  - `BEGIN` 后 `GetCurrentTransaction()->GetIsolationLevel()==kReadCommitted`（采样生效）；
  - `SET ... WRONG`（非法级别）→ `success==false`；
  - 反复 `SET ... SERIALIZABLE` 成功。
- **回归**：`tests/sql/52_set_isolation.sql`（`run_all_tests.bat` 全量自动跑）仅放**必然成功**的语句（见 Plan 代理方案末尾的说明），非法级别的报错断言放 storage_ut（避免 `.sql` 触发 run_all_tests 的失败判定）。

---

## Part B — 行级 S/X 锁（表锁+行锁并存）

### B0 新增行资源 id 编码（`include/storage/LockManager.h`）
在 LockManager.h 追加 free 函数，符号位标记"行锁"命名空间，保证不与表锁 id 重叠：
```cpp
// 行级资源 id：页号高 32 位、槽位低 32 位，置符号位以示「行锁」命名空间。
// 表锁 id = 首页页号（非负），行锁 id 恒为负 → 两类 id 数值永不相交，
// 避免「某行 (page=0,slot=7) 与某表首页 7」这类数值撞车。
inline int64_t RowResourceId(int64_t page_id, int slot_num) {
    return ((int64_t)page_id << 32) | (int64_t)(uint32_t)slot_num | (1LL << 63);
}
```
（实现期需复核 LockManager.cpp 无 `res_id < 0` 守卫；仅在 `Acquire` 查 `txn_id<0`，res_id 仅作 map 键，安全。）

### B1 行级读锁注册表（`include/execution/Executor.h` 的 `ExecutionContext`）
- 新增成员 `std::vector<int64_t> statement_row_read_locks_`（本语句已取得、需按 RC 语句末释放的行 S 锁）。
- 加方法：`void RecordRowReadLock(int64_t rid)`、`const std::vector<int64_t>& GetRowReadLocks() const`、`void ClearRowReadLocks()`。
- 说明：SERIALIZABLE 的行读锁无需记录（持有到提交，由 `Commit/Rollback` 的 `UnlockAll` 释放）；仅 RC 需要本语句内记录并回收。

### B2 执行算子内加行锁（读表 S / 写表 X）
统一辅助函数（放 `ExecutionEngine.cpp` 匿名命名空间或 Executor 侧）：
```cpp
// 仅当「活动显式事务 + 注入 LockManager」时取行锁（与表锁 lock_enabled 同一门控）。
// 返回是否已加锁（false = 自动提交/未启用，调用方无需处理）。
```
各 Executor 接入点（均先经 `context_->GetTransactionManager()->GetLockManager()` + `context_->GetTransaction()` 判定 lock_enabled）：
- **读表 S**：`SeqScanExecutor::Next`（`src/execution/SeqScanExecutor.cpp` L28 产出行后）、`IndexScanExecutor::Next`（`GetTuple` 回表后）。当 `iso != kReadUncommitted` 且 RID 有效时 `LockShared(txn, RowResourceId(...))`；RC 时 `RecordRowReadLock`，SERIALIZABLE 不记录。死锁/超时抛 `CompilerException`。
- **写表 X**：`UpdateExecutor::Next`（`src/execution/UpdateExecutor.cpp` L98 `UpdateTuple` 前）、`DeleteExecutor::Next`（`DeleteExecutor.cpp` L59 `DeleteTuple` 前）、`InsertExecutor`（`InsertTuple` 成功后对新 RID）。所有隔离级别都取，**持有到提交**（`UnlockAll` 释放），不记录。
- DDL / TRUNCATE / ALTER / 纯 SELECT 无 `FROM` 不加行锁。

> 约束：行 S/X 锁与同事务自身的任何锁不冲突（`Conflicts` 跳过 `txn_id` 自身）；行锁所在表已有表锁，锁序在语句内自洽，不引入新死锁环。

### B3 行级读锁语句末释放（RC，`src/execution/ExecutionEngine.cpp` 的 `Execute`）
- `Execute()`（外层，每语句一次）在 `ExecuteSubplan` 返回后：若 `lock_enabled && iso==kReadCommitted`，对 `ctx.GetRowReadLocks()` 逐个 `lm->Unlock(txn, rid)`，然后 `ClearRowReadLocks()`。
- 用 `try/catch`（或 RAII）包住 `ExecuteSubplan` 调用，确保成功与异常路径都回收 RC 读锁（对齐现有表级 RC 读锁的回收语义）。
- SERIALIZABLE 不在此回收（提交时 `UnlockAll`）；READ UNCOMMITTED 本就不取行读锁。

### B4 测试（`tests/storage/storage_ut.cpp`）
- 新增 `TestRowLockEncoding()`：断言 `RowResourceId` 的位布局、非负性（表 id ≥0 vs 行 id <0）、`(page1,slot)` 与 `(page1,slot')` 及跨 page 的互异性。
- 新增 `TestRowLockTier()`（直接操作 LockManager，用 RID 编码的负 id）：S-S 兼容 / S-X・X-X 冲突 / 死锁成环 victim 回收 / UnlockAll 释放——证明行级锁层与锁管理器的集成正确。
- 复用现有 `TestIsolationLevels` / `TestConcurrentSessions` / `TestLockManager` 断言**不回归**（并存模式下行锁不应破坏既有并发与隔离行为）。
- `main()` 注册新测试。

---

## 验证方式（Verification）

1. **编译**：`cmake --build build --target storage_ut sqlcompiler` 全绿。
2. **存储 UT**：`build/storage_ut.exe` → 期望 PASS（在既有 6541 checks 基础上新增 SET + 行锁测试项，fails=0）。
3. **SQL 回归**：`run_all_tests.bat`（或直接跑 `tests/sql/` 关键文件）全绿，含新 `52_set_isolation.sql`；`40/46/48` 等既有无回归。
4. **SET 生效**（A6）：storage_ut 断言 `SET → BEGIN 采样` 命中新隔离级别；非法级别 `success==false`。
5. **行锁链路**（B4）：编码 + LockManager 行级层冲突/死锁断言 + 既有并发/隔离测试不回归。

## 关键待改文件清单
- `include/lexer/Token.h`、`src/lexer/Lexer.cpp`、`src/lexer/Token.cpp`
- `include/ast/AST.h`、`src/ast/AST.cpp`
- `include/parser/Parser.h`、`src/parser/Parser.cpp`
- `include/plan/Plan.h`、`include/plan/Planner.h`、`src/plan/Plan.cpp`、`src/plan/Planner.cpp`
- `include/execution/TransactionExecutor.h`、`src/execution/TransactionExecutor.cpp`
- `include/execution/Executor.h`、`src/execution/ExecutionEngine.cpp`
- `src/execution/SeqScanExecutor.cpp`、`src/execution/IndexScanExecutor.cpp`、`src/execution/UpdateExecutor.cpp`、`src/execution/DeleteExecutor.cpp`、`src/execution/InsertExecutor.cpp`
- `include/storage/LockManager.h`
- `tests/storage/storage_ut.cpp`、`tests/sql/52_set_isolation.sql`