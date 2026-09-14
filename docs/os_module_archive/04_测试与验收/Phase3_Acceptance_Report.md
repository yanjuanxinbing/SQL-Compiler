# OS 优化 Phase 3 · 验收报告

- 项目：小型数据库系统 SQL-Compiler（编译原理 + 操作系统 + 数据库 三模块综合实训）
- 轮次范围：**MVCC 版本链 O(1) 查询与自动清理**（优化建议书 C/D 合并，四子项）
- 依据：`docs/OS_Module_Optimization_Proposal.md` Phase 3 + `docs/Storage_Dev_Plan.md` 路线图
- 结论：**全部完成，测试全绿（存储 UT 48153 checks / 0 fails，SQL 回归 55 passed / 0 failed，含崩溃注入）**

---

## 1. 轮次目标

沿建议书 Phase 3 推进四项机制，每项遵循「实现 → 全面测试（UT + SQL 回归 + 崩溃注入）→ 撰写含前后对比数据的技术文档」完整闭环：

| 编号 | 机制 | 核心价值 |
|---|---|---|
| t1 | 自适应锁升级（建议书 C） | 锁升级阈值按表规模 + 冲突采样动态调整，小表提前、大表延后 |
| t2 | MVCC 版本链 O(1) 查询 | 快照点查从「沿链逐版本查锁」降为「缓存二分 + CSN 直判」 |
| t3 | 自动清理增强（墓碑槽复用 + 真空链摘除） | 目录空间不随删除/真空单调增长，墓碑槽安全复用 |
| t4 | 索引墓碑回收（建议书 D，低频索引真空） | 二级索引陈旧条目（键改写/逻辑删除遗留）按低水位回收，索引空间不无限膨胀 |

**硬约束遵守**：`.fpl`/`.crc`/WAL 与 48B `MvccRecordHeader` 磁盘布局**零变更**（版本索引为纯内存缓存）；页闩 RAII（`PageReadGuard`/`PageWriteGuard`）、持页闩禁调 BPM、写路径 `SetSnapshot` 挂载纪律均未破坏。

**收尾优化（验收报告已知限制逐项清零）**：
- ① 索引真空判据 (ii) **精确化**：`head.begin_csn > 最老快照` 时不再保守 kKeep，沿版本链按「每个活动快照的可见版本键」判定回收（`CommitTracker::ActiveSnapshotList()` 提供活动快照升序列表）；
- ② 精确化走链**步进上限 64 消除**：改为「访问过的 `(page,slot)` 集合防环 + CSN 沿链非递增快速否决」的无步数上限走链，长链（>64 版本）完整判定不漏回收、不误删。

两轮均以新增专项测试（Part D 精确化 / 超长链）+ 全量回归（存储 UT + SQL 回归 + 崩溃注入）收尾。

---

## 2. 机制实现摘要

### t1 自适应锁升级（LockManager）

- **瓶颈**：固定阈值 128 行一刀切——大表（万行）128 行远未覆盖足够比例，过早收敛表锁压制并发；小表（几十行）批量写时行锁条目已占表大半，过晚升级锁表膨胀。
- **方案**：`ComputeEscalationThreshold(registered_rows, conflict_count)` 动态求阈值：
  - 小表（登记行 < 256）→ **提前升级**，阈值 ≈ 登记行数/2（下限 8）；
  - 大表（登记行 ≥ 4096）→ **延后升级**，每 4096 行在基准 128 上上调 64；
  - **冲突采样**：`TryEscalateTable` 因他人持冲突行/表锁返回 `kWouldBlock` 的次数越多阈值越低（冲突一消解立即再次尝试升级收敛）。
- **数据**（`TestAdaptiveLockEscalation` 单元断言）：`(64,0)=32`、`(200,0)=100`、`(300,0)=128`、`(10000,0)=256`；冲突降阈 `(64,1)=16`、`(10000,1)=128`；下限 `(8,0)=8`、`(4,5)=8`。64 行小表第 **32** 行即升级（固定 128 时此处不升）；10000 行大表 128 行仍不升级、第 **256** 行才升；冲突场景阈值 32→16，对手释放后下一次行写立即升级成功。

### t2 版本链 O(1) 查询（TableHeap 内存版本索引缓存）

- **瓶颈**：快照点查 `GetTuple` 从 head 沿 `prev` 链逐版本 `LookupCommitted`（锁表）+ 跨页读判断可见性——链越长、越靠近高频点查/回表路径成本越高。
- **方案**：`rid → 按 begin_csn 升序的稳定版本数组`（`VersionIndex`，全内存、随库生命周期驻留，**不改页格式**）：
  - **命中直判**：head 槽头部 5 元组（begin/end_xid、begin/end_csn、prev）作缓存标记 → 二分「最新 begin_csn ≤ S」候选 → 端可见性 **CSN 直判**（免逐版本 `LookupCommitted` 锁与跨页读取）；
  - **自愈**：任何写版本/删除/提交回填都改动 head 槽头 → 标记失配即沿链重走重建；候选槽读校验兜底，真空墓碑/页复用不产生陈旧命中；
  - **自读防护**：本事务未提交版本（begin_csn=0 不可进缓存）走链路径，self 可见性正确。
- **数据**（`TestVersionIndexCache`）：12 版本堆积（保护快照挡内联真空）下，老快照 S=5 首次点查重建缓存（builds=1），**连续 9999 次点查全部命中、零重建**（hit=9999、builds 不变）；写 1 新版本后 head 标记失配自动重建（builds=2）并读到新值；写者事务内读自己刚写的 rid **不命中缓存**（hit 不增、builds+1，走链路径）。

### t3 自动清理增强（墓碑槽复用 + 真空链摘除）

- **瓶颈**：真空/物理删除打成的墓碑槽（`len==kTombstone`）永久占用目录项，`slot_count` 只涨不降；回收旧版本后墓碑槽可能仍被后继版本 `prev` 引用，无法安全复用。
- **方案**：
  - **墓碑槽复用**：`InsertIntoPage`/`UpdateTuple` 优先复用页内 `kTombstone` 目录项（`FindTombstoneSlot`），归还目录空间、`slot_count` 不增长；复用计数可观测（`GetTombstoneReuseCount`）；
  - **真空链摘除**：`Vacuum`/`InlineVacuumPage` 回收旧版本时 `UnlinkVersionInPage`——把引用「被回收版本为 prev」的后继版本 prev 改接到被回收版本自身的 prev，保证墓碑槽**不再被任何版本引用**、可安全复用；回收判据沿用 `end_csn ≤ 低水位`（任何活动快照都看不到「结束于它之前」的版本），与版本链遍历/FCW 正交安全。
- **数据**（`TestTombstoneSlotReuse`）：非快照 20 插 → 10 删 → 10 插，**slot_count 仍 20**、复用计数 ≥ +10、20 行读写正确；快照 4 次更新堆积 v1..v4（slot 25）→ 真空后 head.prev 接空 → 再更新**复用墓碑槽**（slot_count 不增长）且读到最新值。`TestVacuumChainUnlink`：6 版本链部分真空（边界=3）回收 v1/v2 → **v3.prev 被接空**、head.prev 未动（v5 未回收）；老快照 S=4 走链读到 v4=22（不触碰墓碑）；全量真空 head.prev 逐跳接空、旧槽全墓碑；新行插入复用 v1 的墓碑槽（slot1），缓存自愈后读写正确。

### t4 索引墓碑回收（低频索引真空）

- **瓶颈**：快照写者的键改写/逻辑删除对二级索引延迟保留的陈旧条目（指向已对全部活动快照不可见的行版本）无人回收——索引空间只增不减。
- **方案**：
  - **判定** `TableHeap::DecideIndexEntry(rid, entry_key, active_snapshots, key_col_indices, column_types)`：仅页读闩 + 槽头检查 + 版本键比较，**不依赖 SetSnapshot 状态**（后台真空不得扰动共享堆的会话快照）；`active_snapshots` 为活动快照升序列表（`CommitTracker::ActiveSnapshotList()` 一次性拍取）。`kRemove` 两种情形均保证「所有活动快照都看不到该条目指向的行版本」：
    - (i) **槽不可见**：墓碑 / 越界 / 损坏，或 MVCC 头 `end_xid!=0 && end_csn!=0 && end_csn ≤ 最老水位`（与堆 Vacuum 同款判据，覆盖快照逻辑删除的延迟条目）；
    - (ii) **头稳定且已提交、头键 != 条目键**（旧键条目）：
      - 头 `begin_csn ≤ 最老活动快照` → `kRemove`（任何活动快照的可见版本都是该头，旧键版本无人可见）；
      - 头 `begin_csn > 最老活动快照` → **精确化**：沿版本链逐版本检查其可见区间 `[v.begin_csn, succ.begin_csn)` 是否含任一活动快照且该版本键 == 条目键——有则 `kKeep`（老快照仍需该条目），无则 `kRemove`；走到最老快照可见版本即停（更旧版本无人可见），**无步数上限**（访问过的 `(page,slot)` 集合防环 + CSN 沿链非递增快速否决，同页步进复用 head 读闩、跨页「释放上一页读闩再 Fetch」不违反锁序）；判定后新注册快照 CSN ≥ 全局 CSN ≥ head.begin_csn → 必见 head（键 != 条目键），不会误删。
    - 其余一律 `kKeep`（legacy 无头 / 未提交写者 / **头键==条目键的活跃条目** / 链损坏（循环/CSN 非递增/墓碑/越界/跨页取不到））：保守保留，最坏漏回收、**绝不误删仍可能被读到的条目**。
  - **执行** `BPlusTree::Vacuum(is_dead)`：`OptimisticLeftmostLeafPage` 定位最左叶 → 沿 `next_leaf` 逐叶持写闩「判定 + 整页重写物理删除」→ 版本号自增发布；`visited` 防环、并发分裂跳页留给下一趟（best-effort）；不合并节点（与 `Delete` 同取舍）。
  - **接线** `SystemCatalog::VacuumAll(active_snapshots)`：真空表堆（最老水位）后，对同表全部索引逐条回表判定（`BuildColumnTypes` 取列类型、索引列→表列映射 `key_col_indices` 供提取版本键），不可见则物理删除。
- **数据**（`TestIndexTombstoneReclaim`）：
  - **键改写遗留**：`(1,10)→(1,20)→(2,30)` 三版后，快照 `{2}`（S=2 尚需 id=1 的 CSN2 版本）→ 旧条目 `(1,rid)` **保守保留**；快照 `{3}` → `kRemove`；整树真空移除 1 条，`count_key(1)=0`、`count_key(2)=1`；
  - **判据精确化（Part D）**：`(10,·)→(20,·)→(30,·)` 三版（head begin=sc > sb > sa），快照 `{sb}`（可见版本 = (20,·)）→ 旧条目 `(10,rd)` 由原保守 `kKeep` 改为 **`kRemove`**（无快照需要）；快照 `{sa}`（可见 (10,·)）→ `(10,rd)` **`kKeep` 不误删**；多快照 `{sa,sb}` 保守 kKeep；整树真空（`{sb}`）仅移除 1 条；
  - **快照逻辑删除**：CSN=4 删除后快照 `{3}`（S=3 仍可见）→ 保留；`{4}` → `kRemove`，真空后 `count_key(2)=0`；
  - **墓碑槽复用闭环**：堆真空后新行插入**复用 slot0**、条目按活性判定保留（`kKeep`），无陈旧别名；
  - **legacy 保守**：无 MVCC 头行条目一律 `kKeep`，真空 0 移除，键集合精确保持。
- **数据**（`TestIndexVacuumLongChain`，判据 (ii) 走链步进上限 64 消除）：
  - 1 列整数元组（48B 头 + 4B 载荷 + 8B 槽项 = 60B/版本 ≤ 页容量 4080B）构造 **67 版本链全部同页**（`(100,·)→(101,·)→…→(166,·)`，RID 稳定不 relocate），老快照 `{c1}`（oldest=c1 < head.begin=c66）让精确化走链 **65 步 > 64**；
  - 旧条目 `(100,rid)`：链上无「键==100 且可见区间含 c1」的版本 → **`kRemove`**（旧实现步数超限一律保守 kKeep 漏回收；新实现无步数上限完整走链正确回收）；
  - 安全边界全部 **kKeep 不误删**：老快照 `{c0}` 仍需旧键（可见版本 = v0(100)）、可见版本键 `(101,rid)`、多快照 `{c0,c1}`、活跃条目 `(166,rid)`（head 键==条目键）；
  - 整树真空（`{c1}`）精确移除 **65 条**（仅保留可见版本键 101 与 head 键 166）。

---

## 3. 测试证据

### 3.1 存储单元测试（storage_ut.exe）

| 里程碑 | checks | fails |
|---|---|---|
| 轮次起点（OS 优化 Phase 2 收官） | 27147 | 0 |
| 四子项收官（t1–t4）：`TestAdaptiveLockEscalation` / `TestVersionIndexCache` / `TestTombstoneSlotReuse` / `TestVacuumChainUnlink` / `TestIndexTombstoneReclaim`（含 Part D 精确化） | 47938 | 0 |
| 收尾优化：步进上限 64 消除（`TestIndexVacuumLongChain` 超长链完整判定） | **48153** | **0** |

净增 **21006 checks**（较四子项收官 +215）；多次复跑稳定（无死锁/超时/flaky）。

### 3.2 SQL 回归（tests/run_all_tests.bat，build/Debug/sqlcompiler.exe）

- **55 passed / 0 failed**：53 条 SQL 脚本 + `run_acid_recovery` + `run_acid_clr` 崩溃注入。
- 隔离级别/索引重点用例 36_index_scan、40/43/46/48/49/50/51/52 全部 exit 0，无回归。

### 3.3 崩溃注入（含于 run_all_tests）

- `49_acid_recovery`：WAL 恢复——未提交 UPDATE 回滚、已提交 UPDATE 持久化。
- `50_undo_clr`：CLR 链 + 中途崩溃——ROLLBACK 补做全部 undo。

---

## 4. 前后对比汇总

| 维度 | 优化前 | 优化后 |
|---|---|---|
| 锁升级阈值 | 固定 128 行一刀切 | 按表规模 + 冲突采样自适应（64 行表 32 升、万行表 256 升） |
| 快照点查版本判定 | 沿链逐版本 `LookupCommitted` 锁 + 跨页读，O(链长) | 缓存命中后 **O(log 版本数) 二分 + CSN 直判**，连续点查零重建 |
| 目录空间 | 墓碑槽空置，`slot_count` 单调增长 | 墓碑槽复用，目录不增长（20 槽复用后仍 20） |
| 真空回收 | 墓碑槽可能仍被后继 prev 引用，无法安全复用 | 回收即摘链，墓碑槽不再被任何版本引用 |
| 索引空间 | 键改写/逻辑删除陈旧条目永久残留 | 低频索引真空按低水位回表判定 + 物理删除（全闭环） |
| 索引真空判据 (ii) | 头 `begin_csn > 最老快照` 一律保守 kKeep（漏回收） | 精确化：沿版本链按「活动快照可见版本键」判定，有则 kKeep、无则 kRemove |
| 索引真空走链 | 步数上限 64，超长链保守 kKeep 漏回收 | 无步数上限（访问集防环 + CSN 非递增 + 同页复用读闩），67 版本链 65 步完整判定 |
| 磁盘格式 | —（本阶段基线） | `.fpl`/`.crc`/WAL 与 48B `MvccRecordHeader` 布局零变更 |

---

## 5. 文件改动清单

**本轮（t4）**：`include/storage_engine/TableHeap.h` + `src/storage_engine/TableHeap.cpp`（`DecideIndexEntry` + `IndexVacuumDecision`）、`include/index/BPlusTree.h` + `src/index/BPlusTree.cpp`（`Vacuum(is_dead)` 整树索引墓碑回收）、`src/catalog/SystemCatalog.cpp`（`VacuumAll` 扩展为堆真空 + 全索引真空）、`tests/storage/storage_ut.cpp`（`TestIndexTombstoneReclaim`）。

**判据精确化收尾（两轮）**：`include/txn/CommitTracker.h`（`ActiveSnapshotList()` 活动快照升序列表）、`src/storage_engine/TableHeap.cpp`（`DecideIndexEntry` 精确化走链：无步数上限 + 访问集防环 + CSN 非递增校验 + 同页复用 head 读闩）、`src/catalog/SystemCatalog.cpp` + `src/db/Database.cpp`（前台/后台 `VacuumAll` 接入活动快照列表）、`tests/storage/storage_ut.cpp`（`TestIndexTombstoneReclaim` Part D 精确化 + `TestIndexVacuumLongChain` 超长链）。

**前序（t1–t3）**：`include/storage/LockManager.h` + `src/storage/LockManager.cpp`（自适应阈值 `ComputeEscalationThreshold`/`GetRegisteredRowCount`/`GetTableConflictCount`）、`src/execution/Executor.cpp`（升级触发改用自适应阈值）、`include/storage_engine/TableHeap.h` + `src/storage_engine/TableHeap.cpp`（`VersionIndex` 缓存、`GetVersionIndexHit/BuildCount`、`FindTombstoneSlot`、`UnlinkVersionInPage`、`GetTombstoneReuseCount`）。

**技术文档（.trae/documents/ 下 1 份）**：
- `MVCC_Phase3_O1_AutoCleanup.md`（四子项方案 + 前后对比数据 + 测试证据）
- 并同步更新 `docs/Storage_Dev_Plan.md`（进度 → 48153 checks / 55 回归，已知限制）

---

## 6. 已知限制（后续扩展候选）

1. ~~索引真空的 (ii) 判据对「改写提交晚于最老快照」（`begin_csn > 边界`）保守保留~~ —— **已精确化**：`CommitTracker::ActiveSnapshotList()` 暴露活动快照升序列表，`DecideIndexEntry` 沿版本链检查每个版本的可见区间 `[v.begin_csn, succ.begin_csn)` 是否含任一活动快照且版本键 == 条目键（有则 kKeep、无则 kRemove，走到最老快照可见版本即停）。~~残余限制：沿链步进上限 64（超长链保守 kKeep）~~ —— **已消除**：改为「访问过的 `(page,slot)` 集合防环 + CSN 沿链非递增快速否决」的无步数上限走链，长链（>64 版本）完整判定（`TestIndexVacuumLongChain` 67 版本链、65 步 > 64 验证 kRemove 不漏回收、老快照仍需条目 kKeep 不误删）。残余限制：对「判定期间注销的快照」可能漏判（漏回收不误删）；
2. 索引删除仍只打墓碑、不合并节点（与 `Delete` 一致）；多列索引回表判定需全部键列下标参与；
3. 谓词锁仍作用于主键键空间（非主键谓词不注册）；
4. 多列索引仅单列最左列参与区间推导；
5. 乐观重启的路径校验为「下降后整体复核」，长树高下重启成本随树高线性增长（当前树高 ≤3-4 层，影响可忽略）。

---

## 7. 结论

Phase 3 四子项（自适应锁升级 / 版本链 O(1) 查询 / 自动清理增强 / 索引墓碑回收）全部按「实现 → 测试 → 文档」闭环交付并**验收达标**；按验收报告已知限制实施的**判据精确化**与**走链步进上限消除**两项收尾优化亦全部完成。存储 UT **48153 checks / 0 fails**（较基线 27147 净增 21006）、SQL 回归 **55 passed / 0 failed**（含崩溃注入）、技术文档 + 开发计划 + 验收报告同步更新。磁盘格式零变更、并发正确性（版本链遍历 / FCW / 行 X 锁 / 乐观页级闩）与既有隔离级别语义全部保持。
