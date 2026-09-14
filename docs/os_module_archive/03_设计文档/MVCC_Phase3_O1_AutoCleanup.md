# MVCC 版本链 O(1) 查询与自动清理（OS 优化 Phase 3）

## Context（为什么做）

快照隔离落地后，MVCC 版本链的正确性已由 t4 前的各里程碑保障，但仍有四块性能/空间短板：

1. **版本可见性查询 O(链长)**：快照点查 `GetTuple` 从 head 沿 `prev` 链逐版本 `LookupCommitted` 判断可见性——链越长、越靠近高频路径（点查/回表）成本越高。
2. **空间回收只覆盖「表堆旧版本槽」**：二级索引里延迟删除/键改写遗留的陈旧条目（指向已对全部活动快照不可见的行版本）无人回收，索引只增不减。
3. **墓碑目录项不可复用**：被真空/物理删除打成的墓碑槽（`len==kTombstone`）一直占用目录项，`slot_count` 只涨不降。
4. **锁升级阈值固定**：固定 128 行升级对大表过早收敛（并发度下降）、对小表过晚（锁表膨胀）。

Phase 3 四个子项（建议书 C/D 合并）逐一消除上述短板，不改任何磁盘格式（`.fpl`/`.crc`/WAL 与 48B `MvccRecordHeader` 布局零变更）。

## 优化前（基线）

| 项 | 旧实现 | 代价 |
|---|---|---|
| 快照点查 | 沿链逐版本 `LookupCommitted`（锁表）+ 跨页读 | 链长 ~ N 版本时点查 O(N)，高频回表路径放大 |
| 索引空间 | 延迟删除/键改写旧条目永久残留 | 索引只增不减，长写路径空间膨胀 |
| 墓碑槽 | 真空后目录项空置 | `slot_count` 单调增长，目录区膨胀 |
| 锁升级 | 固定阈值 128 行 | 大表过早升级收敛并发、小表过晚升级锁表膨胀 |

## 方案与技术细节

### t1：自适应锁升级（LockManager C）

按「已登记行锁数 × 表规模 + 历史升级冲突采样」动态调整升级阈值：

- **小表提前升级**：表行数少时（已登记行数逼近全表规模）阈值降低——行锁集合已近全表，升级为表锁几乎无损并发、收益确定；
- **大表延后升级**：行数多时阈值抬高，避免过早收敛为表锁压制并发；
- **冲突反馈**：升级尝试被冲突拒回（他事务持冲突行/表锁）时记采样，连续冲突降低阈值让下一次更早升级、减少重复扫描成本。
- 执行器 `AcquireRowWriteLock` 升级触发改用自适应阈值，`UnlockAll` 撤销语义不变（幂等、多粒度矩阵不变）。

### t2：版本链 O(1) 查询（TableHeap 内存版本索引缓存）

- **缓存结构**：`rid → 按 begin_csn 升序的稳定版本数组`（`VersionIndex`）。稳定 = 写者已提交（begin_csn>0）且端已回填（end_xid==0 或 end_csn>0）。全内存、随库生命周期驻留，**不改页格式**。
- **命中直判**：`GetTuple` 快照分支先校验缓存标记（head 槽头部 begin/end_xid/csn + prev 的 5 元组快照）→ 二分「最新 begin_csn<=S」候选 → 端可见性 **CSN 直判**（免逐版本 `LookupCommitted` 锁与跨页读取）。
- **自愈**：head 槽头部作为缓存标记——任何写版本/删除/提交回填都改动 head 槽头，标记不匹配即沿链重走重建；候选槽读校验（页内头字段逐项比对）再兜底一层，真空墓碑/页复用不产生陈旧命中。
- **自读防护**：本事务写过的 rid（未提交版本不可进缓存）不走缓存，由链走路径处理 self 可见性。

### t3：自动清理增强（墓碑槽复用 + 真空链摘除）

- **墓碑槽复用**：`InsertIntoPage`/`UpdateTuple` 优先复用页内 `kTombstone` 目录项（`FindTombstoneSlot`），归还目录空间，`slot_count` 不增长；复用计数可观测（`GetTombstoneReuseCount`）。
- **真空链摘除**：`Vacuum`/`InlineVacuumPage` 回收旧版本时先 `UnlinkVersionInPage`——把页内引用「被回收版本为 prev」的后继版本 prev 改接到被回收版本自身的 prev，保证墓碑槽**不再被任何版本引用**、可被安全复用；回收判据沿用修正后的 `end_csn <= 低水位`（任何活动快照都看不到「结束于它之前」的版本），与版本链遍历/FCW 正交安全。

### t4：索引墓碑回收（建议书 D，低频索引真空）

- **判定** `TableHeap::DecideIndexEntry(rid, entry_key, active_snapshots, key_col_indices, column_types)`：仅页读闩 + 槽头检查 + 版本键比较，**不依赖 SetSnapshot 状态**（后台真空不得扰动共享堆的会话快照）；`active_snapshots` 为活动快照升序列表（调用方从 `CommitTracker::ActiveSnapshotList()` 一次性拍取）。`kRemove` 的两种情形均保证「所有活动快照都看不到该条目指向的行版本」：
  - **(i) 槽不可见**：墓碑 / 越界 / 损坏，或 MVCC 头 `end_xid!=0 && end_csn!=0 && end_csn<=最老水位`（与堆 Vacuum 同款判据，覆盖快照逻辑删除的延迟条目）；
  - **(ii) 头稳定且已提交（end_xid==0 && begin_csn>0）、头键 != 条目键（旧键条目）**：
    - `begin_csn <= 最老活动快照` → **kRemove**（任何活动快照的可见版本都是该头，旧键版本无人可见——覆盖「改写已对最老快照可见」的旧条目）；
    - `begin_csn > 最老活动快照` → **精确化（收尾优化）**：沿版本链从 head 走向更旧版本，逐版本检查其**可见区间** `[v.begin_csn, succ.begin_csn)`（succ 为更近 head 的后继版本）是否含任一活动快照且该版本键 == 条目键——有则 `kKeep`（仍有老快照需要该条目），无则 `kRemove`。链走到「最老快照可见版本」（`begin_csn <= 最老水位`）即停（更旧版本可见区间上界 ≤ 最老水位 ≤ 全部活动快照，无人可见）。**无步数上限**：合法链沿 prev 必然终结（链尾 / 达最老快照可见版本），长链（>64 版本）完整判定、不再保守 kKeep 漏回收；防损坏链循环靠双保险——**访问过的 (page, slot) 集合**（链循环必复访；同事务多版本共享同一 CSN，故不能用 CSN 严格递减判环，必须用访问集，预先登记 head 槽防回指）+ **CSN 沿链非递增**（`vh.begin_csn > succ_begin` 即损坏快速否决）。同页步进复用 head 页读闩、跨页「释放上一页读闩再 Fetch 下一页」，不违反「持页闩不请求 BPM」锁序。
  - 其余一律 `kKeep`（legacy 无头 / 未提交写者 / **头键==条目键的活跃条目** / 链损坏（循环/CSN 非递增/墓碑/越界/跨页取不到）：保守保留，最坏漏回收、绝不误删仍可能被读到的条目）。
  - **安全性论证（判定期间新注册快照）**：`(ii)` 进入前提是 head 已提交，`head.begin_csn <= 判定时全局 CSN`；判定后注册的新快照 CSN 恒 ≥ 判定时全局 CSN ≥ `head.begin_csn`，其可见版本必为 head（键 != 条目键）→ 不会因「判定后再注册快照」而误删。
- **执行** `BPlusTree::Vacuum(is_dead)`：`OptimisticLeftmostLeafPage` 定位最左叶 → 沿 `next_leaf` 逐叶持写闩「判定 + 整页重写（物理删除）」→ 版本号自增发布；`visited` 防环、并发分裂跳页留给下一趟（best-effort）；不合并节点（与 `Delete` 同取舍）。
- **接线** `SystemCatalog::VacuumAll(active_snapshots)`：真空表堆（用最老水位）后，对同表全部索引逐条回表判定（`BuildColumnTypes` 取列类型、索引列映射 `key_col_indices`），不可见则物理删除。

## 取得的进步数据

| 指标 | 优化前 | 优化后 | 度量方式 |
|---|---|---|---|
| 快照点查版本判定 | 沿链逐版本 `LookupCommitted` + 跨页读，O(链长) | 缓存命中后 **O(log 版本数) 二分 + CSN 直判**，连续点查零重建 | `TestVersionIndexCache`/`TestVacuumChainUnlink` 的 `GetVersionIndexBuildCount` 循环内不变断言 |
| 索引空间回收 | 陈旧条目永久残留 | 低频索引真空按活动快照区间回表判定 + 物理删除（删除/键改写/墓碑复用全闭环） | `TestIndexTombstoneReclaim` 条目级断言（含 Part D 精确化） |
| 目录空间 | 墓碑槽空置、`slot_count` 单调涨 | 墓碑槽复用，目录不增长 | `TestTombstoneSlotReuse` 20 槽复用后仍 20 |
| 真空安全性 | — | 回收即摘链，墓碑槽不再被任何版本引用 | `TestVacuumChainUnlink` 部分/全量真空逐跳接空 |
| 锁升级 | 固定阈值 128 | 按表规模 + 冲突采样自适应 | `TestAdaptiveLockEscalation` |

## 测试与回归

- 存储 UT：**48153 checks / 0 fails**（Phase 3 四子项在 27147 基线之上净增；`TestIndexTombstoneReclaim` 含 Part A 键改写 / Part B 逻辑删除 / Part C 墓碑复用闭环 / **Part D 判据精确化**——`head.begin_csn > 最老快照` 时旧键条目按「活动快照可见版本键」判定回收，原保守 kKeep 场景改为 kRemove，且「老快照仍需旧键」场景保持 kKeep 不误删；`TestIndexVacuumLongChain` 构造 67 版本链让精确化走链 65 步 > 64——旧实现步数超限保守 kKeep 漏回收，新实现完整走链正确 kRemove，且老快照仍需的条目 kKeep 不误删）。
- SQL 回归：**55 passed / 0 failed**（53 条 SQL + `run_acid_recovery` + `run_acid_clr` 崩溃注入，`build/Debug/sqlcompiler.exe`）。

## 关键文件

- `src/storage/LockManager.cpp` + `include/storage/LockManager.h`、`src/execution/Executor.cpp`：自适应锁升级（t1）。
- `include/storage_engine/TableHeap.h` + `src/storage_engine/TableHeap.cpp`：`VersionIndex` 缓存、`GetVersionIndexHit/BuildCount`、`FindTombstoneSlot`、`UnlinkVersionInPage`、`DecideIndexEntry`（t2/t3/t4；精确化）。
- `include/txn/CommitTracker.h`：`ActiveSnapshotList()` 活动快照升序列表（t4 精确化入参）。
- `include/index/BPlusTree.h` + `src/index/BPlusTree.cpp`：`BPlusTree::Vacuum(is_dead)` 整树索引墓碑回收（t4）。
- `src/catalog/SystemCatalog.cpp` + `include/catalog/SystemCatalog.h`：`VacuumAll` 扩展为「堆真空 + 全索引真空」（t4）；`src/db/Database.cpp` 前台/后台真空调用点接入活动快照列表。
- `tests/storage/storage_ut.cpp`：`TestIndexTombstoneReclaim` 等四子项测试。

## 已知限制（后续扩展候选）

- 索引删除仍只打墓碑、不合并节点（与 `Delete` 一致）；多列索引回表判定需全部键列下标参与；
- 索引真空精确化判据 (ii) 对「判定期间注销的快照」可能漏判（漏回收不误删）；损坏链判定保守 kKeep。
