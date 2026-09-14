# 行级锁升级：批量写事务的行锁→表锁收敛

## Context（为什么做）

行级并发下，写算子（`InsertExecutor`/`UpdateExecutor`/`DeleteExecutor`/`UpsertExecutor`）对每行取 X 锁并持有到提交。大批量 DML（如 `UPDATE t SET ... WHERE` 全表、批量 INSERT）会为每一行各建一个锁条目——锁表、`UnlockAll` 释放、死锁检测等待图全部按行数线性膨胀。

### 优化前的技术瓶颈（t2 之前的缺陷）

1. **锁表条目 O(行数)**：万行批量写 = 万条行锁条目。锁管理器的 `locks_` map、每事务持有集合、死锁检测等待图边数全部线性放大；长事务的 `UnlockAll` 也要逐个释放。
2. **无多粒度收敛通道**：事务对同一表持有海量行 X 锁后，语义上等价于「独占该表」，但没有任何机制把这一组行锁折叠成单条表级 X 锁——锁条目无法随持有规模收敛，内存与检测开销被白白放大。

### 优化实施方案与技术细节

在既有 LockManager 多粒度冲突矩阵（表 X 与任意行锁冲突、表 S 与行 X 冲突、与行 S 兼容）之上，新增**行级锁升级**链路：

- **归属登记**：`ExecutionContext::AcquireRowWriteLock` 每取得一行 X 锁即 `RegisterRowGroup(row_res, table_res)`，维护 行→表 与 表→行集合 两张索引。
- **阈值触发**：`CountRowLocks(txn, table_res) >= kRowLockEscalationThreshold(=128)` 时调 `TryEscalateTable(txn, table, kExclusive)`。
- **升级判定**（`LockManager::TryEscalateTable`）：
  - 本事务已持表锁 / 已升级 → 幂等成功；
  - 他人持冲突表锁，或本表下任一他人行锁与目标模式冲突（`HierarchyConflicts`）→ `kWouldBlock`，**回退为继续逐行持锁**（正确性不受影响，只是不享受收敛收益）；
  - 通过 → 授予表锁，**释放本事务在本表上的全部行锁**（含被阻塞等待者的重新授予评估），并标记 `IsTableEscalated`。
- **升级后放行**：`AcquireRowWriteLock` 对已升级表直接返回 `kOk`（表锁已覆盖该表行访问），后续行写 O(1)，不再逐行加锁。
- **清理**：`UnlockAll` 撤销升级标记并释放表锁。

```cpp
// ExecutionContext::AcquireRowWriteLock（节选）
if (table_res >= 0 && lm->IsTableEscalated(txn->GetTxnId(), table_res)) {
    return RowLockResult::kOk;                       // 表锁覆盖，O(1) 放行
}
...
lm->RegisterRowGroup(row_res, table_res);
if (lm->CountRowLocks(txn->GetTxnId(), table_res) >= kRowLockEscalationThreshold) {
    LockResult er = lm->TryEscalateTable(txn->GetTxnId(), table_res, LockMode::kExclusive);
    if (er == LockResult::kDeadlock) return RowLockResult::kDeadlock;
}
```

### 优化后取得的进步数据

| 指标 | 优化前 | 优化后 | 度量方式 |
|---|---|---|---|
| 批量写 200 行的锁条目 | 200 条行锁 | **1 条表级 X 锁**（升级后 `CountRowLocks==0`） | `TestRowLockEscalation` |
| 升级后单行访问成本 | O(1) 行锁 + 归属登记 | **O(1) 直接放行**（`IsTableEscalated`） | 结构分析 |
| 长事务 `UnlockAll` | O(行数) | **O(表数)**（升级后仅表锁） | 结构分析 |
| 死锁检测等待图 | O(行数) 边 | O(表数) 边 | 结构分析 |
| 升级失败回退 | —（无此机制） | 保留逐行锁，正确性不变（`kWouldBlock` → 行锁仍持有） | `TestRowLockEscalation`(3) |
| 幂等重入 | — | 重复升级直接成功，不重复加锁/释放 | `TestRowLockEscalation`(1) |
| 多粒度互斥 | — | 表X vs 行S/表S 冲突、表S vs 行S 兼容 | `TestRowLockEscalation`(2)(4) |

**稳定性/安全性**：升级仅在无他人冲突时进行，失败自动回退逐行持锁，隔离语义（同行使行锁互斥、不同行不互斥）完全不变；升级过程中被释放行锁上的等待者会被重新评估授予，不产生锁丢失。存储 UT **6982 checks / 0 fails**，SQL 回归 **55 passed / 0 failed**。

## 关键文件
- `src/execution/Executor.cpp`：`AcquireRowWriteLock` 登记 + 阈值触发（`kRowLockEscalationThreshold = 128`）。
- `src/storage/LockManager.cpp`：`RegisterRowGroup`/`CountRowLocks`/`TryEscalateTable`/`IsTableEscalated`/多粒度冲突矩阵。
- `tests/storage/storage_ut.cpp`：`TestRowLockEscalation`。
