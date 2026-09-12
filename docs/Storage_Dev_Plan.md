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
- ⬜ 待续：谓词锁目前作用于「主键键空间」且树锁为树级粗粒度；行级锁尚未支持锁升级与父子谓词区间继承；多版本真空后台线程与二级索引 MVCC 精确可见性为后续扩展。

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