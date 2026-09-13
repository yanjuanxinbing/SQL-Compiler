# SERIALIZABLE 谓词锁：父子区间继承与合并

## Context（为什么做）

SERIALIZABLE 隔离级别通过谓词锁防幻读：语句执行前，`ExecutionEngine::CollectScanPredicates` 收集每个被扫描表的读谓词（主键区间或全表），写入 `LockManager` 的谓词锁表；写算子（INSERT/UPDATE/DELETE/UPSERT）写前经 `CheckWritePredicate` 检查冲突。本机制解决的是**谓词锁集合的膨胀与重复覆盖**问题。

### 优化前的技术瓶颈（t1 之前的缺陷）

1. **区间谓词只增不减**：一个长 SERIALIZABLE 事务内多条 SELECT 语句对同一表注册不同区间（如先扫 `[10,20]`、再扫 `[15,25]`、再扫 `[30,40]`），谓词锁表按「每语句一条」累积，互不归并。锁表条目数随语句数线性增长，长事务退化为 O(谓词数) 的锁表 + O(谓词数) 的死锁检测等待边 + O(P) 的每次写前冲突检查。
2. **父谓词（全表）不覆盖子谓词（区间）**：事务已注册全表谓词（`is_full=true`）后，后续区间的注册不产生任何额外覆盖能力（全表已包含所有键），却仍新增条目；反之先有若干区间、后注册全表，区间条目也不被清理——覆盖语义重复、条目冗余。
3. **重叠区间无法合并**：`[10,20]` 与 `[15,25]` 并集是 `[10,25]`，但旧实现保留两条，`CheckWritePredicate` 对键 `15` 需要遍历两条谓词判断覆盖，检查开销随冗余条目放大。

### 优化实施方案与技术细节

`LockManager::AcquireReadPredicate`（`src/storage/LockManager.cpp`）改为**集合规约式注册**：先整体摘除本事务本表的既有谓词，对新旧谓词做如下归约后重插：

- **父谓词继承**：本事务本表已持全表谓词 → 新谓词一律被父谓词覆盖，直接丢弃（条目数不变）。子区间无需注册：全表谓词已为防幻读兜底。
- **全表收敛**：新谓词为全表 → 删除本表全部区间谓词，仅保留全表谓词。
- **区间合并**：既有区间 + 新区间按 `lo` 升序排序，仅合并**真重叠**区间（`last.lo <= cur.hi && cur.lo <= last.hi`），输出为「不重叠的最小区间覆盖」；被完整覆盖的子区间在合并中自然消失。
  - 合并输出使用**独立容器** `merged`：`kept` 中还保留其他事务/其他表的谓词，直接复用 `kept.back()` 会与最后一条被保留的谓词误判重叠，导致新区间被误丢弃（跨表注册时覆盖能力丢失）。
- 覆盖能力不变：归约只做「冗余条目消除」，任何被合并/丢弃的键仍被并集或全表谓词覆盖。

```cpp
// 核心归约逻辑（节选）
if (has_full) { /* 仅保留全表谓词 */ }
else if (is_full) { /* 删除全部区间谓词，只保留全表 */ }
else {
    // 区间合并：sort by lo → 合并真重叠 → 不重叠最小区间覆盖
}
```

### 优化后取得的进步数据

| 指标 | 优化前 | 优化后 | 度量方式 |
|---|---|---|---|
| 重叠区间条目数 | 2（`[10,20]`+`[15,25]`） | **1**（并集 `[10,25]`） | `CountPredicateLocks` |
| 全表谓词后的条目数 | 不收敛（区间残留） | **1**（全表覆盖一切子区间） | `CountPredicateLocks` |
| 不相交区间 | 2（各自独立） | 2（正确保持不合并） | `CountPredicateLocks` |
| 桥接区间 `[22,28]` | 3（多一条冗余） | **2**（并入 `[10,28]`，无伪合并） | `CountPredicateLocks` |
| 覆盖能力 | — | **不变**：被合并/覆盖的键仍被写前检查拦截 | `CheckWritePredicate`（`k15`→Timeout、`k35`→Timeout） |
| 每写键的谓词扫描 | O(条目数) | O(不重叠区间数) ≤ 优化前 | 结构分析 |

**稳定性/安全性**：长 SERIALIZABLE 事务的谓词锁表从「随语句数膨胀」收敛为「不重叠区间数（≤键空间分区数）」，死锁检测等待图与 `CheckWritePredicate` 扫描规模同步下降；合并逻辑经 `TestPredicateLockMerge` 8 组断言（含跨事务独立、跨表独立、父谓词继承、合并后覆盖能力）全绿。存储 UT 新增断言后 **6982 checks / 0 fails**，SQL 回归 **55 passed / 0 failed**（含 40/46/48/49/50/51/52 与崩溃注入）。

## 关键文件
- `src/storage/LockManager.cpp`：`AcquireReadPredicate` 集合规约（父子继承 + 区间合并）。
- `src/execution/ExecutionEngine.cpp`：`CollectScanPredicates`（全表/区间谓词收集，is_full 判定）。
- `tests/storage/storage_ut.cpp`：`TestPredicateLockMerge`。
