# 存储子系统 后续开发计划

> 基于 2026-09-12 的当前进度（存储 UT **5433 checks / 0 fails**、SQL 回归通过、E1–E8 + D5/D7 全部收官）。本计划以**里程碑（T1–T5）** 为阶段，每阶段含目标、关键任务、可量化验收与估算资源。均为相对开发周期，可依团队节奏平移。

---

## 0. 现状基线（出发点）

- ✅ 页式存储 / 缓冲池 / LRU·FIFO·CLOCK·LRU-K 替换
- ✅ WAL-before-data、组提交、页 CRC、空闲页位图、块设备抽象、页级读写锁、后台刷脏、内存上限配置
- ✅ 四项正确性/健壮性修复（异常路径帧页回滚、fsync 顺序、重复释放防护、64 位定位）已合一并通过回归
- ⬜ 待深化的骨架：**多连接并发事务、真正的页间事务并发、WAL 组提交批量化数据、调参实验、崩溃注入覆盖面**

## T2 进行中进度（2026-09-12）

- ✅ 会话/连接抽象：`Session` + `Database::CreateSession` + 全局 `TxnIdSequencer`，多会话并发 DML 独立事务/自动提交。`TestConcurrentSessions` 覆盖独立表 + 同表并发插入。
- ✅ **修复同表并发插入丢行竞态**：`TableHeap` 写路径（`InsertIntoPage`/`InsertTuple`/`DeleteTuple`/`UpdateTuple`/`ClearAll`）此前仅靠缓冲池 `latch_`（单次帧访问）串行化，跨多次缓冲池访问的「找页→判满→新建页→链接→插入」check-then-act 序列会并在同一尾部页导致链表分叉/丢行（偶然性 FAIL）。改为 per-table `write_mutex_` 串行化整段写序列 + 写路径改用页级写锁（`PageWriteGuard`），读路径用 `PageReadGuard`，使`同表并发写`与`顺序执行`结果等价。6/6 复跑绿色。
- ✅ **事务级锁管理器（锁粒度到事务 + 死锁处理）**：新增 `storage/LockManager`（S/X 锁兼容矩阵、`UnlockAll` 按事务整体释放、**等待图 DFS 死锁检测**（检测到环立即返回 `kDeadlock`，victim 中止不阻塞）、可选等待超时）。`TestLockManager` 覆盖 S-S 兼容 / S-X・X-X 冲突 / 死锁成环 / 超时 / 阻塞授予。→ 存储 UT 达到 6525 checks / 0 fails（含既有会话并发测试）；52 例 SQL 回归 exit 0。
- ✅ **隔离级别接入执行边界（表粒度过渡方案）**：`ExecutionEngine::ExecuteSubplan` 在语句执行前按表加锁——读表取共享锁、写表取独占锁（资源 id = 表堆首页页号），并在语句/事务边界按隔离级别持有。`Transaction`/`TransactionManager` 增加 `IsolationLevel`（默认 `kSerializable`；`BEGIN` 时采样本会话默认值）。语义：**READ UNCOMMITTED** 不取读锁 / **READ COMMITTED** 读锁语句末释放（写锁持有到提交）/**SERIALIZABLE** 读锁与写锁全部持有到 Commit/Rollback。`Commit`/`Rollback` 经 `LockManager::UnlockAll` 汇总释放。→ 新增 `TestIsolationLevels`：用「并发写是否被阻塞」验证 RC 语句末释放读锁与 SERIALIZABLE 读锁持有到提交，全程确定性无长等待。
- ✅ **锁管理健壮性**：等待超时 `kIsolationLockWaitMs=5000ms`（死锁/超时在语句边界中止并释放已获读锁）；异常路径对 READ COMMITTED 读锁同样回收，避免抛异常后锁泄漏。→ 存储 UT **6541 checks / 0 fails**；40/46/48 等 SQL 回归 exit 0（42 的 Error 为 CHECK 约束负向用例，属预期）。
- ✅ **隔离级别推进：从表级锁下放到全面行级并发 + 谓词锁**：
  - **B+Tree 树级读写锁（`std::shared_mutex`）**：`Insert`/`Delete` 取独占锁，`FindFirst`/`LowerBound`/`Begin`（附 `Cursor` 扫描期持共享锁）取共享锁——取消表级锁后为索引并发提供物理安全第一道防线（代价是粗粒度，写彼此串行；页级 latch crabbing 列为后续可选项）。
  - **放宽语句边界表锁**：`ExecutionEngine::ExecuteSubplan` 中 DML/只读语句不再整表取锁，仅 **DDL**（`TRUNCATE/ALTER/DROP/CREATE`）保留表级独占锁至提交；资源 id = 表堆首页页号。
  - **SERIALIZABLE 谓词锁防幻读**：`LockManager` 新增 `AcquireReadPredicate`/`CheckWritePredicate`，读前注册主键区间/全表谓词，INSERT/UPDATE/DELETE/UPSERT 写前检查冲突，避免幻读。`ExecuteSubplan` 为 SERIALIZABLE 语句收集扫描谓词注册，`UnlockAll` 提交时统一清除。
  - **行级锁接入写算子**：`InsertExecutor`/`UpdateExecutor`/`DeleteExecutor`/`UpsertExecutor` 写行前 `AcquireRowWriteLock`（资源 id = `RowResourceId`，符号位标记行锁命名空间，与表锁非负 id 不相交），读扫描逐行 `AcquireRowReadLock`。READ COMMITTED 行读锁语句末释放，SERIALIZABLE 持有到提交。
  - **修复会话路径 RC 读锁漏放死锁**：`Database` 的 Session 执行路径此前直连 `ExecuteSubplan`、绕过 `Execute()` 的 `ReleaseRowReadLocks`，导致 READ COMMITTED 下 SELECT 取得的行 S 锁跨语句泄漏，阻塞他会话对该行的 X 锁（可复现死锁）。改为在 `ExecuteSubplan` 内部成功/异常路径统一释放，两个入口均覆盖。
  - **测试**：改写 `TestIsolationLevels` 适配行级语义；新增 `TestBPlusTreeConcurrency`（多线程并发 Insert + 持续 LowerBound 扫描，验证树锁防撕裂/不丢键）、`TestRowLevelConcurrency`（两个 RC 事务同表不同行并发写不互斥，同行为阻塞，验证行锁承接并发隔离）、`TestSerializablePredicatePhantom`（A 全表扫描注册谓词不提交，B 插入命中范围键被阻塞至 A 提交）。→ 存储 UT **6598 checks / 0 fails**；txn/隔离相关 SQL 回归（40/46/48/49/50/51/52）exit 0。
- ✅ **MVCC 快照隔离（完整版本链 `kSnapshot`）**：读者免读锁、写者行 X 锁串行 + 提交期 first-committer-wins 防丢失更新。
  - **共享 `CommitTracker`**（header-only）：`Database` 持唯一实例注入各会话 TM；`Begin()` 捕获 snapshot_csn_ 并登记活动快照，最外层 `Commit` 先 FCW 冲突（中止输家）、再 `Commit(xid)` 赋 CSN 并回填全部版本头的 begin_csn/end_csn，`Rollback` 登记 aborted。
  - **版本链字节格式**：记录前加 48B `MvccRecordHeader`（begin/end_xid、begin/end_csn、prev_page/slot）；Update 同页迁旧 head 到新 slot 再覆写 head（RID 稳定）；Delete 仅置 `end_xid`；`GetTuple/FindNextRid/Iterator` 沿链做 `VisibleForSnapshot` 过滤，跳过被引为 prev 的旧版本。
  - **读/写接入**：`AcquireRowReadLock` 对 `kSnapshot` 返回 `kUnused`（免读锁）；`SeqScan/IndexScan::Init` 挂 `SetSnapshot(csn, tracker)`；Vacuum 由 `ExecuteSQLImpl` 每 100 语句低频触发 `SystemCatalog::VacuumAll(oldest_active_snapshot)`。
  - **SQL 支持**：`SET TRANSACTION ISOLATION LEVEL SNAPSHOT`（新增 `KEYWORD_SNAPSHOT` 词法/解析）。
  - **FCW 基捕获修复**：`UpdateExecutor` 需在读行**前** `SetActiveTransaction`，否则读行时刻未记录快照读基、UpdateTuple 以物理 head 为基，FCW 误判而放行丢失更新；已修复并在读行循环前挂载、语句末解除。
  - **测试**：`TestSnapshotIsolation`（可重复读 / 免阻塞读 / 写者 FCW）+ SQL 52（SNAPSHOT）全绿。→ 存储 UT **6630 checks / 0 fails**；40/46/48/49/50/51/52 exit 0。
- ✅ **快照 DELETE / UPSERT 的 FCW 冲突检测**：原先仅 UPDATE 正确走 FCW；DELETE/UPSERT 的读堆路径未挂快照（`SetSnapshot`），导致把同一逻辑行的新旧版本槽位都当独立行删除/改写，回滚后数据损坏，且丢失更新无法被 FCW 拦截。
  - **根因**：`SeqScanExecutor`/`IndexScanExecutor` 在扫描前调用 `table_heap_->SetSnapshot(txn->GetSnapshotCsn(), tracker)` 启用 MVCC 可见性过滤，但 `DeleteExecutor`/`UpdateExecutor`/`UpsertExecutor::UpdateConflictingRow` 未调用——`GetTuple` 的 `commit_tracker_==nullptr` 分支退化为「只读物理 head」，不看版本可见性。
  - **修复**：三个写算子在读堆前，若当前为活动 kSnapshot 事务则 `SetSnapshot(快照水位, CommitTracker)`，与 SeqScan 一致；删除/更新/改写只命中快照可见的唯一版本，配合既有行 X 锁 + 提交期 FCW，B 改写后 A 已提交的行被中止回滚、A 的值落盘。
  - **测试**：`TestSnapshotDeleteFcw`/`TestSnapshotUpsertFcw`（B 基于旧值 DELETE/UPSERT 被 A 阻塞→A 提交→B 被 FCW 中止→行保留 A 值）。→ 存储 UT **6658 checks / 0 fails**；全部 53 条 SQL 回归 exit 0。
- ✅ **SERIALIZABLE 谓词锁：父子区间继承与合并（v2）**：`LockManager::AcquireReadPredicate` 改为集合规约式注册——已持全表谓词时子谓词一律被父谓词覆盖丢弃；新谓词为全表时删除全部区间谓词；区间按 lo 排序合并真重叠区间为「不重叠最小区间覆盖」（合并输出用独立容器，避免与 kept 尾元素误判重叠）。覆盖能力不变，谓词锁表随语句数膨胀收敛为区间数。`TestPredicateLockMerge` 8 组断言（重叠合并 / 不相交保留 / 桥接不伪合并 / 跨事务跨表独立 / 全表收敛 / 合并后覆盖能力）。
- ✅ **行级锁升级（多粒度收敛，v2）**：`ExecutionContext::AcquireRowWriteLock` 逐行取 X 锁时 `RegisterRowGroup` 登记归属；`CountRowLocks ≥ 128` 触发 `TryEscalateTable(txn, table, X)`——无他人冲突则授予表锁并释放本事务全部行锁（锁条目从 O(行) 收敛到 O(表)），后续行访问 O(1) 放行；升级失败（他事务持冲突行/表锁）回退逐行持锁，正确性不变。多粒度冲突矩阵：表 X vs 行 S/X、表 S vs 行 X 冲突，表 S vs 行 S 兼容；升级幂等、`UnlockAll` 撤销标记。`TestRowLockEscalation` 覆盖升级/回退/幂等/多粒度互斥。
- ✅ **MVCC 多版本真空：独立后台线程 + 水位边界修正（v3）**：新增 `Database` 后台真空线程（`bg_vacuum_ms` 可配，默认 0 关闭，复刻后台刷脏模式），周期性以「最老活动快照」为界做全表真空；`Vacuum` 回收判据修正为 **end_csn ≤ oldest_active_csn**（原 begin_csn 判据会误删「创建早、结束晚于活动快照」的版本导致老读者丢行）；`CommitTracker::OldestActiveSnapshot` 改为显式求最小 CSN（原 `unordered_map::begin()` 迭代序不定，会误删活动快照所需版本）；非快照/自动提交读统一复位共享堆快照水位（`SetSnapshot(-1,nullptr)`）消除陈旧读泄漏；`Shutdown` 幂等收尾。`TestBackgroundVacuumThread`（线程 ticks、S=3 老快照不丢行、新事务读最新值）+ `TestVacuumReclaimsOldVersions`（槽位级 3→1 回收、边界版本保留、无活动快照 no-op）。
- ✅ **二级索引 MVCC 精确可见性（v4）**：快照写者的键改写/逻辑删除对非唯一二级索引**延迟摘除**（`IndexDeleteDeferred`，唯一/主键索引仍急切维护以保唯一性预检），旧条目由扫描侧精确过滤：`IndexScanExecutor` 回表后**按 RID 去重**（同一逻辑行命中新旧多条目只返回一次）+ **可见版本键重检**（`InScanBounds` 校验可见版本重建键落在扫描区间，过滤越界伪行）；键未变更新跳过重建（`IndexKeysEqual`，避免同 `(key,rid)` 重复条目）；写堆失败 `RestoreDeletedIndexEntries` 只恢复急切摘除条目；`TableHeap::GetTuple` 非快照读过滤逻辑删除 head（消除幽灵行）。`TestSnapshotIndexScan` 覆盖越界过滤/去重/老快照链读/删除幽灵行/等值查找。
- → 本轮隔离级别深入开发收官：存储 UT **6982 checks / 0 fails**（较 6658 净增 324）；全量 SQL 回归 **55 passed / 0 failed**（53 条 SQL + run_acid_recovery + run_acid_clr 崩溃注入）。
- ✅ **B+Tree 乐观页级并发（OS 优化 Phase 1，创新特性 A）**：树级粗粒度锁升级为**页级读写闩 + 乐观重启（optimistic restart）**：
  - **版本号机制**：页头 offset 12 保留字段复用为单调版本号，页内容每次改写（整页重写 / next·prev / first_child 指针改写）一律自增发布，版本号只用于进程内并发控制，不改页格式、落盘/恢复语义不变；
  - **乐观只读下降**：`OptimisticFindLeafPage`/`OptimisticLeftmostLeafPage` 逐页取**共享闩**记录 `(pid, version)` 路径快照，`ValidatePath` 复核未变则通过，任一变化整体重启（上限 256 次兜底）——读不阻塞写、无锁等待、无等待环，与 DFS 死锁检测正交；
  - **写路径分档**：快路径（叶子放得下）乐观下降 + 路径校验后**只对目标叶子**取独占写闩（写闩下复核叶版本）；慢路径（叶子放不下）带写闩下降、下降前 `MayOverflow` 检查**就地预分裂**，保证叶子一定放得下，避免级联分裂；
  - **分裂并发安全**：`SplitChild`/`SplitRoot` 采用**「先 pin 后加闩」**（`PageGuard::Fetch` pin → `PageWriteGuard::LatchPinned` 直接对已 pin 帧加闩）同时持父/子/后继多把写闩而不违反「持页闩不请求 BPM」锁序；加闩后**复读内容**（pin 与加闩之间可能被并发改写），检测到 child 不再是 parent 直接孩子（结构已被并发改写）则释放全部闩与多余 pin、**回收误分配的新页**（`bpm_->DeletePage`）、返回重启信号让调用方整体重下；prev 回链只在探测的 next 仍是当前 next 时更新（过期即跳过，不影响导航正确性）；
  - **游标乐观语义**：`Cursor` 持有「叶子页内容物化快照 + 装载时版本号」，跨叶前进前校验版本，并发分裂后重新装载并越过已发出条目继续——保证不跳过新分裂出的右半页、不重复发出同一条目；`LowerBound`/`Begin` 用 `GetLeafVersion()` 复核下降路径末端叶版本；
  - **undo/WAL 语义不变**：`SetActiveTransaction` 挂载事务抓 undo、`EmitPageImageRecord` 写 before/after 均在写闩内完成，与树级锁版本完全一致；
  - **测试**：新增 `TestOptimisticSplitConcurrency`——16 帧小缓冲池 + 4 线程×2500 键并发插入强制「分裂 + 淘汰」同时发生（验证「先 pin 后加闩」在淘汰压力下不泄漏 pin、不丢页），插入期间 5 万轮并发全扫描（读不阻塞写），抽样点查 103 键复核 (key,rid)，再 4 线程各删 500 键（区间互不重叠）并并发扫描，删除后键集合精确校验（已删全消失、未删全保留）；
  - **吞吐实测**（Debug 构建、1024 帧页全常驻、预热 1000 键；前基线 = 当前实现 + 全局互斥模拟树级锁写者串行）：4 线程 10k 键 **1.93×**、8 线程 40k 键 **2.71×**、16 线程 80k 键 **3.18×**（达标「异键空间并发 INSERT 吞吐 ≥ 现状 3×」验收指标），每场景 total 断言一致无丢键无重复；加速比随写者数增长，写并发不再归零；
  - → 存储 UT **17091 checks / 0 fails**（t4 收官基线 6982 之上净增，多次复跑无死锁/超时）；SQL 回归 **55 passed / 0 failed**（含崩溃注入 acid_recovery/acid_clr）。
- ✅ **O(1) 快照水位 + 无后台真空（OS 优化 Phase 2，创新特性 B）**：
  - **低水位缓存**：`CommitTracker` 维护单调低水位 `low_water_mark_`（= 活动快照 CSN 最小值），`RegisterSnapshot` 仅在「首个快照 / CSN 更小」时降水位（O(1)），`UnregisterSnapshot` 仅在「被注销的恰是最小值且计数归零」时置脏标记，`OldestActiveSnapshot` 退化为 **O(1) 缓存读**（脏时惰性重算推进一次，水位单调只升不降，与全局 CSN 单调分配一致；重算次数可观测）。1 万活跃快照下 1 万次读取 **141µs（≈14ns/次，重算 0 次）** vs 旧实现每次 O(N) 扫描全表（10k 项/次，整数下界 ~7.2µs/次）——**约 500×**；
  - **内联轻量真空**：写路径（Update/Delete 写新版本、持页写锁时）顺带回收「本页已确认 end_csn ≤ 低水位」的旧版本槽位（判据与 `Vacuum` 一致：end_xid!=0 && end_csn!=0 && end_csn<=低水位；至多每语句 k=8 个，预算由 `SetSnapshot` 语句级重置，WAL after-image 含墓碑、重放一致）；后台真空/每 100 语句前台真空降级为**低频兜底**而非唯一通道；
  - **安全性论证**：低水位 ≤ 任意活动快照 → 回收的版本对所有活动快照不可见（版本链遍历在命中可回收版本前必先命中可见版本，FCW 只读 head.prev 不受影响），长事务下边界版本绝不误删；
  - **测试**：`TestLowWaterMarkO1`（1 万快照下 O(1) 读缓存、重算计数、单调推进、同 CSN 多读者计数）+ `TestInlineVacuum`（保护快照下 12 版本堆积→边界老快照 S=5 仍读到 v5→低水位推进后 2 条 UPDATE 内联回收，滞后 12→1，**↓91.7%（≥60%）**，无后台真空参与）；`TestVacuumReclaimsOldVersions` 适配内联回收后新预期（堆积 3→2）。
  - → 存储 UT **27147 checks / 0 fails**；SQL 回归 **55 passed / 0 failed**（含崩溃注入 acid_recovery/acid_clr）。
- ✅ **MVCC 版本链 O(1) 查询与自动清理（OS 优化 Phase 3，建议书 C/D 合并，四子项）**：
  - **自适应锁升级（C）**：`LockManager` 按「已登记行锁数 × 表规模 + 历史升级冲突采样」动态调整升级阈值——小表提前升级（行锁集合已近全表、升级收益确定）、大表延后升级（避免过早收敛表锁压制并发）、冲突反馈降低阈值；执行器 `AcquireRowWriteLock` 触发改用自适应阈值，多粒度矩阵/`UnlockAll` 撤销语义不变。`TestAdaptiveLockEscalation`；
  - **版本链 O(1) 查询**：`TableHeap` 内存版本索引缓存（`rid → 按 begin_csn 升序稳定版本数组`，全内存、不改 48B 页格式/磁盘布局）；`GetTuple` 快照分支校验 head 槽头部 5 元组缓存标记 → 二分「最新 begin_csn<=S」候选 → **CSN 直判**（免逐版本 `LookupCommitted` 锁与跨页读）；写版本/删除/提交回填改动 head 槽头触发标记失配即沿链重建自愈；本事务未提交版本走链路径绕过缓存。连续点查零重建（`GetVersionIndexBuildCount` 不变断言）；
  - **自动清理增强**：墓碑槽复用（`FindTombstoneSlot`，`InsertIntoPage`/`UpdateTuple` 优先复用 `kTombstone` 目录项，`slot_count` 不增长）+ **真空链摘除**（`UnlinkVersionInPage`，回收旧版本时把引用「被回收版本为 prev」的后继 prev 接到被回收版本自身的 prev，墓碑槽不再被任何版本引用、可安全复用）；`TestTombstoneSlotReuse`（20 槽复用后仍 20）/`TestVacuumChainUnlink`；
  - **索引墓碑回收（D，低频索引真空）**：`TableHeap::DecideIndexEntry` 仅页读闩 + 槽头检查 + 版本键比较（不依赖 SetSnapshot 状态，不扰动会话快照）——槽不可见（墓碑/越界/损坏/`end_csn<=最老水位`）或「头稳定、头键 != 条目键」判定 `kRemove`，其中头 `begin_csn > 最老快照` 时**判据精确化**：沿版本链逐版本检查其可见区间 `[v.begin_csn, succ.begin_csn)` 是否含任一活动快照且该版本键 == 条目键（有则 kKeep、无则 kRemove；`active_snapshots` 由 `CommitTracker::ActiveSnapshotList()` 升序拍取，走到最老快照可见版本即停，**无步数上限**——访问过的 `(page,slot)` 集合防环 + CSN 沿链非递增快速否决，同页步进复用 head 读闩）；legacy/未提交/头键==条目键的活跃条目/链损坏保守 `kKeep`（漏回收不误删，且判定后新注册快照必见 head，不会误删）；`BPlusTree::Vacuum(is_dead)` 乐观定位最左叶 → 逐叶持写闩判定 + 整页重写物理删除（版本号自增发布、防环、best-effort）；`SystemCatalog::VacuumAll(active_snapshots)` 扩展为「堆真空 + 全索引真空」（索引列→表列映射 `key_col_indices` 供回表提取版本键）。`TestIndexTombstoneReclaim`（键改写/快照删除/墓碑槽复用/legacy 保守 + **Part D 精确化**全闭环）+ `TestIndexVacuumLongChain`（67 版本链走链 65 步 > 64：旧实现步数超限 kKeep 漏回收，新实现完整判定 kRemove 不漏、老快照仍需条目 kKeep 不误删）；
  - → 存储 UT **48153 checks / 0 fails**（Phase 2 基线 27147 之上净增）；SQL 回归 **55 passed / 0 failed**（含崩溃注入 acid_recovery/acid_clr）。
- ✅ **谓词锁区间树 + WAL 组提交 + 温度感知刷盘（OS 优化 Phase 4，建议书 E/F）**：
  - **谓词锁区间树（E）**：谓词存储从全局向量 `pred_locks_` 迁移为**按表组织的居中区间树**（`pred_tables_`：`table_rid -> PredicateTable`）——节点分裂点取该表全部区间 lo 去重中位数，跨分裂点区间存于节点（`by_lo_asc`/`by_hi_desc` 双有序表），完全在左/右的递归下放子树；点查询 stabbing query 沿分裂点二分下降、每层只输出必然覆盖键的区间前缀 → `CheckWritePredicate` 从 **O(P) 线性扫描降为 O(log P + K)**；全表谓词作全域区间哨兵（`full_holders`）单独存放；「源向量 `intervals` + 脏标记 + 惰性重建」（注册/释放只动源向量置脏，下次查询前 O(P log P) 一次性建树）；`AcquireReadPredicate` 的区间继承/合并规约不变。修复递归建树跨递归调用持有 `IntervalNode&` 引用导致的悬垂引用堆损坏（改为局部作用域填充 + 递归后按下标回填 left/right）。观测接口 `CountTotalPredicates`/`GetPredicateQueryComparisons`（累计键比较次数）。`TestPredicateIntervalTree`：1 万区间下未覆盖键比较次数 < 100（log2(1e4)≈14 层）；
  - **WAL 组提交（F）**：`LogManager::GroupCommit(target)` **领导者-跟随者**——无领导时本线程上任执行一次 `SyncOs()`（fsync `next_lsn_-1`），`durable_lsn_` 推进后 `notify_all` 唤醒全部跟随者；跟随者只在 `gc_cv_` 上等 `durable_lsn_ >= target`；批内所有并发提交共享一次 fsync。`TransactionManager::Commit` 把 `Flush()` 替换为 `GroupCommit(commit_lsn)`（失败抛异常由既有异常路径兜底）。`durable_lsn()` Phase 4 起持锁读取（与组提交并发安全）；`sync_count_` 累计真实 SyncOs 次数供对照。组提交基准：**128 次并发提交仅 12 次 fsync（对照串行 128 次恰好 128 次）→ 10.7× I/O 削减**；
  - **温度感知刷盘（F）**：`Page` 新增原子 `access_count_`（`GetPage`/`NewPage` 命中采集，`ResetMemory` 帧复用清零）；`FlushAllDirtyUnlocked` 开启温度感知（`SetTemperatureFlushEnabled`，默认关闭）时只写回**冷脏页**（`access_count < 阈值`，默认 8），热脏页留池——NO-FORCE + WAL-before-data 保证崩溃后由 WAL redo 恢复，换取提交路径写 IO 削减与热页命中率保留；`BufferPoolStats` 扩展 `writeback_cold_count`/`writeback_hot_count`，所有写回路径（`FlushPageUnlocked`/`FlushAllDirtyUnlocked`/`DeletePage`/`FindFreeFrame`）统一经 `RecordWritebackStat` 记账（关闭时全记冷档，与旧版一致）；
  - **配套修复**：`Database` 成员析构顺序——`LogManager` 必须声明于 `BufferPoolManager` 之前（逆序析构时 BPM 的 FlushAllPages 仍访问 `log_manager_->durable_lsn()`/`Flush()`，GroupCommit 起 durable_lsn 持锁读取后 use-after-free 必现崩溃）。
  - → 存储 UT **58325 checks / 0 fails**（Phase 3 基线 48153 之上净增 10172）；SQL 回归 **55 passed / 0 failed**（含崩溃注入 acid_recovery/acid_clr，Phase 4 后全部 exit 0）。技术文档：`.trae/documents/Phase4_PredicateTree_GroupCommit_TempFlush.md`、验收报告 `docs/Phase4_Acceptance_Report.md`。
- ⬜ 已知限制（后续扩展候选）：谓词锁仍作用于「主键键空间」（非主键谓词不注册）；多列索引仅单列最左列参与区间推导；乐观重启的路径校验为「下降后整体复核」，长树高下重启成本随树高线性增长（当前树高 ≤3-4 层，影响可忽略）；删除仍只打墓碑、不做节点合并与再平衡；索引真空精确化判据 (ii) 对「判定期间注销的快照」可能漏判（漏回收不误删）、损坏链判定保守 kKeep；**Phase 4 遗留**：谓词区间树惰性重建 O(P log P) 未做增量插入、组提交批量窗口未加显式计时器（高频小事务可引入时间窗聚合）、温度阈值静态常量未按访问分布自适应、热页判定未与 LRU-K 淘汰联动。

---

## T1 — 稳定性与工程化加固（约 1 期）

| 目标 | 关键任务 | 验收标准 |
|---|---|---|
| 巩固错误路径 | 为 BufferPool/Recovery/Disk 全部 I/O 与崩溃点补 `try/catch` 回滚覆盖；审查事务路径是否在异常后保持可用 | 故障注入单测≥90% 覆盖；全量 UT 不回归 |
| sanitizer 落地 | 在 CI/脚本启用 ASan/UBSan（本机 MinGW 缺 `-lubsan`，改用 Valgrind 或 MSVC /fsanitize） | 内存/UB 清零报告 |
| 崩溃注入扩面 | 扩展 `\crash*` 到 **Sync 内部双 fsync 之间**、后台刷脏中途、索引页 redo 途中 | 跨重启一致性用例 ≥8 组全过 |
| 文档补全 | 生成 README 总入口 + CI 说明 | markdown 链接无死链 |

**资源需求**：1 名存储工程师（约 60% 工时）+ 1 名测试（约 40%）。

---

## T2 — 多连接并发事务（页面管理上层主要收益点）

| 目标 | 关键任务 | 验收标准 |
|---|---|---|
| 会话/连接抽象 | 引入独立 `Session`（隔离的活动集、undo、保存点），`ExecuteSQL` 改为 session 内执行 | 两连接独立提交互不污染 |
| 锁粒度到事务 | 用已就绪的页级读写锁升级为事务级行/页锁（读共享、写独占），缓解 D7 全局 `latch_` 争议 | TPC-C 式并发吞吐不因隔离而崩 |
| 隔离级别 | 实现 READ COMMITTED / SERIALIZABLE 快照 | 并发 `A+B` 追加/更新一致性断言通过 |
| 死锁处理 | 锁等待图 + 超时 kill（why：持页锁不调 BPM 的锁序保障就此兑现） | 死锁用例能被回收、无活锁 |

**收益论证**：本模块 E4 页锁当前对单连接「空转」，多连接正是其设计价值的兑现点。**资源**：2 名（存储+事务）约 65% 工时。

---

## T3 — 性能与调参

| 目标 | 关键任务 | 验收标准 |
|---|---|---|
| 基准 | 建立 `bench/`：顺序扫描 / 随机点查 / 顺序+热点混合 / 长写事务 | 指标可复现、图表化 |
| 替换策略参数 | 对 CLOCK 的 `hand_`、LRU-K 的 `K` 做网格实验 | 输出命中率/吞吐/资源表，明确默认值 |
| 组提交批量化 | 将 `Sync()` 从「每 Commit 一次」细化为「多 Commit 合批+一 fsync」 | fsync 次数↓且持久性不变 |
| 内存可调性 | 打通 `SQLCOMPILER_BUFFER_MEMORY` 与热区命中曲线 | 内存↔命中率回归曲线 |

**资源**：1 名性能工程师（约 60%）+ 脚本化。

---

## T4 — 介质与可观测性

| 目标 | 关键任务 | 验收标准 |
|---|---|---|
| 更多块设备 | 实现内存/稀疏文件/网络块设备（复用 `BlockDevice` 抽象） | 交换 `FileBlockDevice` 零业务改动 |
| 可观测 | `\stats` 补：替换策略命中构成、脏页年龄分布、后台刷脏直方图、IO 队列 | 指标与 `1-系统调用` 自洽 |
| 诊断 | 崩溃转储、页 CRC 误报率统计、`\analyze` 页映射可打印 | 诊断命令可作为事故复盘依据 |

**资源**：1 名（介质+可观测，约 60%）。

---

## T5 — 收口与发布

| 目标 | 关键任务 | 验收标准 |
|---|---|---|
| 审计闭环 | 再跑一轮 -Wall -Wextra 零告警 + 全量 UT + SQL 回归 + 崩溃注入 | 全绿 |
| 验收 | 更新最终验收报告为新基线；输出 README | 交付文档与实现一致 |
| 评审 | 对照「进程调度/内存/置换」映射，文档补上并发模型章节 | 评审通过 |

**资源**：1 名（30%）集中收口。

---

## 里程碑依赖与风险

- **T2 依赖 T1**（稳定性底子先立住）；T3/T4 可并行。
- **主要风险**：多连接会把 D7 全局 `latch_` 与 E4 页锁的锁序契约推到极限——需在 T2 前把「持页锁期间禁止调 BPM」从注释约定提升为 **代码层守卫（断言/检查）**，否则并发死锁风险无法被测试提前捕捉。
- **次要风险**：MinGW 缺 `-lubsan`、Valgrind 在 Windows 不可用，sanitizer 策略需用 MSVC 或 WSL 兜底。

## 资源汇总（相对工时）

| 阶段 | 人数配置 | 估算占比 |
|---|---|---|
| T1 | 存储×1 + 测试×1 | 2.0 人期 |
| T2 | 存储+事务×1 + 并发×1 | 2.5 人期 |
| T3 | 性能×1 | 1.0 人期 |
| T4 | 介质×1 | 1.0 人期 |
| T5 | 1 名 | 0.5 人期 |
| **合计** | — | **≈ 7.0 人期** |

> 以上为依赖排程后的粗估；各任务均以「可通过单测/SQL/崩溃注入/基准的量化验收」为准入门，与既有 5433-check 测试体系一致。