# O(1) 快照水位 + 无后台真空（OS 优化 Phase 2，创新特性 B）

## Context（为什么做）

MVCC 快照隔离下，版本回收（真空）需要知道「最老活动快照」水位，且回收动作此前**依赖后台线程**（`bg_vacuum_ms` 默认 0 = 关闭）或每 100 语句的前台低频触发。两个瓶颈：

1. **水位计算 O(N)**：`CommitTracker::OldestActiveSnapshot` 每次调用都遍历活动快照表求最小 CSN。1 万并发快照读者/写者时，每次真空/水位读取都要扫 1 万项——高频路径上不可扩展。
2. **真空依赖后台/低频通道**：后台真空默认关闭、前台每 100 语句触发，版本链在两次触发之间无限堆积，空间归还没有即时性保证。

Phase 2 的目标（建议书原文验收指标）：**1 万活跃快照下 `OldestActiveSnapshot` 耗时恒定（O(1)）**；**真空滞后（未回收版本数）下降 ≥ 60%**；**长事务下无版本误删**（复用现有真空边界测试）。

## 优化前（t4/v3 基线）

| 项 | 旧实现 | 代价 |
|---|---|---|
| 最老活动快照 | 每次调用 O(N) 遍历 `active_snapshots_` 求最小 | 1 万快照下单次 ≈7.2µs（整数扫描下界，真实哈希迭代更慢） |
| 版本回收通道 | 仅后台线程（默认关）+ 每 100 语句前台触发 | 触发间隙版本堆积；`TestVacuumReclaimsOldVersions` 3 次 UPDATE 堆积 3 槽位需显式 `Vacuum` 才回收 |
| 回收判据 | `end_csn ≤ oldest_active_csn`（v3 已修正，本阶段沿用） | 正确但只被低频通道使用 |

## 优化后：方案与技术细节

### 1. 低水位缓存（CommitTracker，O(1) 读）

维护单调低水位 `low_water_mark_` = 活动快照 CSN 最小值，配合活动快照计数 `active_count_` 与脏标记 `low_water_dirty_`：

- `RegisterSnapshot(csn)`：**O(1)**。仅当「首个快照（active_count_==1）」或「csn < 当前水位」时降水位并清脏。利用**全局 CSN 单调分配**：新快照 CSN 恒 ≥ 已有最小值，故生产路径从不触发降水位，水位单调只升不降。
- `UnregisterSnapshot(csn)`：**O(1)**。仅当被注销的 CSN 恰是当前水位且该 CSN 计数归零时置 `low_water_dirty_`；否则缓存仍有效。
- `OldestActiveSnapshot()`：**O(1) 缓存读**。空集返回 0；脏时做**一次** O(N) 惰性重算推进水位（单调方向安全——只把缓存值推到更大/相等的真实最小，绝不产生「偏高」的过期值，故不会误删活动快照仍需要的版本），并累计 `low_water_recompute_count_` 供观测。**重算仅在「最小值快照注销后首次读取」发生，摊薄后读路径恒定 O(1)**。

```cpp
int64_t OldestActiveSnapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    if (active_count_ <= 0) { low_water_mark_ = 0; low_water_dirty_ = false; return 0; }
    if (low_water_dirty_) {            // 仅最小值快照注销后首次读取触发
        int64_t oldest = 0;
        for (const auto& kv : active_snapshots_)          // O(N)，罕见
            if (oldest == 0 || kv.first < oldest) oldest = kv.first;
        low_water_mark_ = oldest; low_water_dirty_ = false;
        ++low_water_recompute_count_;
    }
    return low_water_mark_;            // O(1) 缓存读
}
```

### 2. 内联轻量真空（TableHeap 写路径摊薄）

写路径（Update/Delete 写新版本、**已持页写锁**时）顺带回收本页旧版本槽位：

- **入口** `RunInlineVacuum(data, slot_count)`：取全局低水位（O(1) 缓存读）→ 无 tracker / 预算耗尽 / 低水位 0 时整体 no-op。
- **判据**（与 `Vacuum` 完全一致）：`h.end_xid != 0 && h.end_csn != 0 && h.end_csn <= low_water` —— 已被替代/删除、且替代/删除已提交不晚于低水位。**用 end_csn 而非 begin_csn**：任何活动快照都看不到「结束于它之前」的版本。
- **预算**：每语句至多回收 `kInlineVacuumBudget = 8` 个，预算由 `SetSnapshot` 语句级重置（每个执行器语句进入时调用一次），把全表真空成本摊薄到日常写路径，单次成本有界。
- **接线点**：`DeleteTuple`（墓碑/逻辑删除后）、`UpdateTuple` 的 in-place 与快照版本链两条路径，均在 WAL after-image 捕获**之前**执行——墓碑随页重放，崩溃恢复语义一致；页本就已标脏，无需额外 MarkDirty。
- **后台真空降级为低频兜底**：后台线程与每 100 语句前台触发保留，但不再是唯一通道。

### 3. 安全性论证（为什么不会误删长事务版本）

- **低水位 ≤ 任意活动快照**：回收判据 `end_csn ≤ low_water` 意味着被回收版本对所有活动快照不可见。
- **版本链遍历安全**：快照读者从 head 沿 `prev` 链找首个可见版本。对任一活动快照 S ≥ low_water：若 v_j（end_csn_j ≤ low_water）被墓碑，则读者在到达 v_j 之前必先命中 v_{j+1} 或更新的可见版本（因 S ≥ end_csn_j = begin_{j+1}），不会走到墓碑处触发 `GetTuple` 的 `IsTombstone → return false` 丢行路径。
- **FCW 安全**：first-committer-wins 只读 head.prev（本事务迁出的旧 head，end_csn 未回填），不沿链深走，不受影响。
- **写者安全**：当前写事务的读基版本对其快照 S 可见 → `end_csn > S ≥ low_water` → 绝不被本语句内联回收。

## 优化后取得的进步数据

| 指标 | 优化前 | 优化后 | 度量方式 |
|---|---|---|---|
| `OldestActiveSnapshot` 复杂度 | 每次调用 O(N) 扫 1 万项（≈7.2µs/次，下界） | **O(1) 缓存读 ≈14ns/次**（1 万次 141µs，重算 0 次） | `TestLowWaterMarkO1` 计时 + `GetLowWaterRecomputeCount()==0` 断言 |
| 惰性重算触发 | —（每次都是全扫） | 仅「最小值快照注销后首次读取」一次 O(N)，单调推进 | 重算计数逐级 +1，稳定态不变 |
| 真空滞后（未回收版本数） | 保护快照下 12 个旧版本全堆积，等后台/前台触发 | 低水位推进后 **2 条 UPDATE 内联回收到 1**（滞后 12→1，**↓91.7%**） | `TestInlineVacuum` 槽位计数 13→6→2 |
| 回收通道 | 后台线程（默认关）+ 每 100 语句前台 | **写路径内联摊薄（主）+ 后台/前台低频兜底** | `TestInlineVacuum` 全程无显式 `Vacuum`/后台线程 |
| 长事务边界 | — | 保护快照 S=1 下堆积 12 版本，老快照 S=5 仍读到 v5（不误删）；S=3 老快照不丢行 | `TestInlineVacuum` + `TestBackgroundVacuumThread` |
| 语句级成本上限 | — | 单语句至多回收 8 槽（预算重置） | `TestInlineVacuum` 一次 UPDATE 恰回收 8 |
| WAL 一致性 | — | 墓碑写入 after-image 之前，重放语义一致 | 崩溃注入回归（acid_recovery/acid_clr exit 0） |

**稳定性/安全性**：水位单调、惰性重算方向安全；回收判据沿用 v3 修正后的 `end_csn` 语义；与版本链遍历、FCW、行 X 锁（写路径已持页写锁）全部正交安全。存储 UT **27147 checks / 0 fails**（新增 `TestLowWaterMarkO1` + `TestInlineVacuum`，并适配 `TestVacuumReclaimsOldVersions` 新预期），SQL 回归 **55 passed / 0 failed**（含崩溃注入）。

## 关键文件

- `include/txn/CommitTracker.h`：低水位缓存 `low_water_mark_`/`active_count_`/`low_water_dirty_`，`OldestActiveSnapshot` O(1) 化 + `GetLowWaterMark`/`GetLowWaterRecomputeCount` 观测。
- `include/storage_engine/TableHeap.h` + `src/storage_engine/TableHeap.cpp`：`kInlineVacuumBudget=8`、`InlineVacuumPage`/`RunInlineVacuum`、`SetSnapshot` 预算重置、Update/Delete 写路径接线。
- `src/db/Database.cpp`：后台真空与每 100 语句触发保留为低频兜底（注释更新，逻辑不变）。
- `tests/storage/storage_ut.cpp`：`TestLowWaterMarkO1`、`TestInlineVacuum`、`TestVacuumReclaimsOldVersions`（适配）。
