# MVCC 多版本真空：独立后台线程 + 水位边界修正

## Context（为什么做）

MVCC 快照隔离下，每次 UPDATE 都把旧 head 迁到新 slot（版本链变长），DELETE 仅置 `end_xid` 保留物理记录。这些旧版本必须按「最老活动快照」水位回收，否则版本链无限膨胀、溢出页堆积、空间泄漏。

### 优化前的技术瓶颈（t3 之前的缺陷）

1. **真空无独立线程、触发稀疏**：`Database::ExecuteSQLImpl` 每 100 语句前台同步触发一次 `SystemCatalog::VacuumAll`。语句多时延迟放大，版本链长期不回收；前台同步执行还挤占查询延迟。
2. **回收判据错误（会丢数据）**：`TableHeap::Vacuum` 原以 `begin_csn < oldest_active_csn` 判可回收。对「创建早（begin=1）、但被替换/删除晚于某活动快照（end=3）」的版本（老读者 S=2 仍可见），`begin<2` 会**误回收**，导致老快照读者沿链读到墓碑而丢行（读不见本应可见的版本）。
3. **最老快照水位计算不可靠**：`CommitTracker::OldestActiveSnapshot` 用 `unordered_map::begin()` 取最小键——迭代顺序不确定，可能取到较大快照 CSN，真空以偏高的水位回收，**误删活动快照仍需要的版本**（测试中表现为老快照读到 NULL 行）。
4. **共享堆快照水位残留**：非快照/自动提交读的语句结束后，共享 `TableHeap` 上遗留上一语句的快照水位，后续会话会读到旧快照（陈旧读泄漏）。

### 优化实施方案与技术细节

1. **独立后台真空线程**（`Database`，复刻 `SQLCOMPILER_BG_FLUSH_MS` 后台刷脏模式）：
   - 构造参数 `bg_vacuum_ms`（环境变量 `SQLCOMPILER_BG_VACUUM_MS` 可配，默认 0 = 关闭）；开启时 `std::thread` 周期性（每 tick）对所有表执行 `TableHeap::Vacuum(OldestActiveSnapshot)`。
   - `GetBackgroundVacuumTicks`/`IsBackgroundVacuumEnabled` 暴露观测接口；`Shutdown` 置停止标志 + join，**幂等**（已停再停不崩溃）。
2. **回收判据修正**（`TableHeap::Vacuum`）：可回收 ⇔ `h.end_xid != 0 && h.end_csn != 0 && h.end_csn <= oldest_active_csn`。
   - 用 **end_csn**（被替代/删除的提交水位）而非 begin_csn：任何活动快照都看不到「结束于它之前」的版本；`begin_csn` 判据会误伤「创建早、结束晚」的版本。
   - 无活动快照（oldest ≤ 0）时整体 no-op，保守安全。
3. **水位计算修正**（`CommitTracker::OldestActiveSnapshot`）：显式遍历 `active_snapshots_` 求最小 CSN，不再依赖 `unordered_map` 迭代序。
4. **陈旧读泄漏修复**：`SeqScan/IndexScan/Delete/Update/Upsert` 执行器的读堆路径，非快照/自动提交时统一 `SetSnapshot(-1, nullptr)` 复位共享堆水位。

```cpp
// Vacuum 回收判据（节选）
if (h.end_xid != 0 && h.end_csn != 0 && h.end_csn <= oldest_active_csn) {
    WriteSlot(data, s, 0, kTombstone);   // 可回收：任何活动快照都看不到它
}
```

### 优化后取得的进步数据

| 指标 | 优化前 | 优化后 | 度量方式 |
|---|---|---|---|
| 真空触发方式 | 前台、每 100 语句 | **独立后台线程**、周期 tick | `TestBackgroundVacuumThread`（ticks ≥ 3） |
| 旧版本回收 | begin_csn 判据（误删风险） | **end_csn ≤ 最老活动快照** | `TestVacuumReclaimsOldVersions` |
| 版本槽位回收量 | — | 3 个版本 → 真空后 **1 个（head）**；边界版本保留 | `live_slots()` 3→1 |
| 老快照边界正确性 | 可能读到 NULL（误删） | 老快照 S=3 读 v=30 **不丢行** | `TestBackgroundVacuumThread` |
| 水位计算 | `unordered_map::begin()`（序不定） | 显式求最小 CSN（确定性） | 修 flaky 测试（连续多次运行全绿） |
| 无活动快照 | — | no-op 保守（不误回收） | `TestVacuumReclaimsOldVersions` |
| 线程收尾 | — | `Shutdown` 幂等 join，不崩溃 | `TestBackgroundVacuumThread` 末尾双 Shutdown |
| 陈旧读泄漏 | 跨语句读到旧快照 | 非快照读统一复位水位 | `SeqScan/IndexScan/Delete/Update/Upsert` 全路径 |

**稳定性/安全性**：真空以「最老活动快照」为界，任何仍可能被读取的版本都不会被回收（快照语义不破）；回收与前台快照写经 per-table `write_mutex_` + 页写锁串行，无并发撕裂。存储 UT 新增 `TestBackgroundVacuumThread` + `TestVacuumReclaimsOldVersions` 后 **6982 checks / 0 fails**，SQL 回归 **55 passed / 0 failed**。

## 关键文件
- `include/db/Database.h` + `src/db/Database.cpp`：后台真空线程启动/停止/循环（`bg_vacuum_ms`、`GetBackgroundVacuumTicks`）。
- `src/storage_engine/TableHeap.cpp`：`Vacuum` 判据 end_csn；`GetTuple` 非快照读过滤逻辑删除 head。
- `include/txn/CommitTracker.h`：`OldestActiveSnapshot` 显式求最小 CSN。
- `src/execution/SeqScan/IndexScan/Delete/Update/UpsertExecutor`：非快照读复位共享堆水位。
- `tests/storage/storage_ut.cpp`：`TestBackgroundVacuumThread`、`TestVacuumReclaimsOldVersions`。
