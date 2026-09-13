# OS 优化 Phase 4 · 验收报告

- 项目：小型数据库系统 SQL-Compiler（编译原理 + 操作系统 + 数据库 三模块综合实训）
- 轮次范围：**谓词锁区间树 + WAL 组提交 + 温度感知刷盘**（优化建议书创新特性 E/F）
- 依据：`docs/OS_Module_Optimization_Proposal.md` Phase 4 + `docs/Storage_Dev_Plan.md` 路线图
- 结论：**全部完成，测试全绿（存储 UT 58325 checks / 0 fails，SQL 回归 55 passed / 0 failed，含崩溃注入）**

---

## 1. 轮次目标

沿建议书 Phase 4 推进两个创新特性（E：谓词锁区间树；F：WAL 组提交 + 温度感知刷盘），每项遵循「实现 → 全面测试（UT + SQL 回归 + 崩溃注入）→ 撰写含前后对比数据的技术文档」完整闭环：

| 编号 | 机制 | 核心价值 |
|---|---|---|
| t1 | 谓词锁区间树（建议书 E） | 写前谓词检查从线性扫描 O(P) 降为区间树点查询 O(log P + K) |
| t2 | WAL 组提交（建议书 F） | 领导者-跟随者批量 fsync，并发提交吞吐不再受磁盘 IOPS 上限 |
| t3 | 温度感知刷盘（建议书 F） | 冷热分级刷盘：冷页优先落盘、热页留池，削减写 IO 并保留热页命中率 |

**硬约束遵守**：`.fpl`/`.crc`/WAL 与 48B `MvccRecordHeader` 磁盘布局**零变更**；锁序约定（`BPM::latch_` 内 + 页读锁下写盘、组提交只在 `mutex_`/`gc_cv_` 等待、持页闩不调 BPM）未破坏；WAL-before-data 与崩溃恢复（ARIES redo/undo）语义不变。

---

## 2. 机制实现摘要

### t1 谓词锁区间树（LockManager）

- **瓶颈**：SERIALIZABLE 谓词锁按全局向量线性存储，`CheckWritePredicate` 写前逐条判定；谓词条目随长事务多次范围扫描累积到万级时，每次 INSERT/UPDATE/DELETE 的写前检查都是 O(P) 全扫。
- **方案**：谓词存储迁移为**按表组织的居中区间树**（`pred_tables_`：`table_rid -> PredicateTable`）：
  - 节点分裂点取该表全部区间 lo 值去重升序的中位数（`lo_values` + `split_idx`）；**跨过分裂点**的区间（`lo <= split <= hi`）存于节点（`by_lo_asc`/`by_hi_desc` 双有序表），完全在左/右的区间递归下放子树；全表谓词作**全域区间哨兵**（`full_holders`）单独存放；
  - **点查询** stabbing query 沿分裂点二分下降：`key < split` 输出 `by_lo_asc` 中 `lo <= key` 前缀、`key > split` 输出 `by_hi_desc` 中 `hi >= key` 前缀、`key == split` 整段输出，逐层下探子树 → **O(log P + K)**（K 为命中区间数）；
  - **惰性重建**：谓词注册/释放只动「源向量 `intervals` + 脏标记」，下次查询前脏则 O(P log P) 一次性建树——低频更新成本摊薄在热路径之外；`AcquireReadPredicate` 的区间继承/合并规约（v2）不变；
  - **内存安全修复**：递归建树期间不得持有跨递归调用的 `IntervalNode&` 引用（递归 `push_back` 触发 `nodes` 重分配致引用悬垂→堆损坏，崩溃延后到任意代码）；改为局部作用域填充节点、递归后按下标重取引用回填 `left/right`。
- **数据**（`TestPredicateIntervalTree`）：
  - 1 万区间（txn i 持 `[i*1000, i*1000+500]`）下，覆盖键 `12345 ∈ [12000,12500]` → 阻塞（kTimeout）；未覆盖键 `600` → 通过（kGranted）；
  - **O(log P) 断言**：未覆盖键点查询累计键比较次数 **< 100**（`log2(1e4) ≈ 14` 层，上限给足余量）——线性扫描需 ~1 万次判定；
  - 全表哨兵挡住任意键；`UnlockAll` 移除哨兵后**他事务区间谓词不被误删**（键 10 仍被 txn 1 的 `[0,500]` 挡住）。

### t2 WAL 组提交（LogManager + TransactionManager）

- **瓶颈**：每次事务提交同步 `Flush()`（fdatasync）——串行提交一条事务一次 fsync，并发提交吞吐受磁盘 IOPS 上限。
- **方案**：`LogManager::GroupCommit(target)` **领导者-跟随者**模式：
  1. 持 `mutex_` 检查 `durable_lsn_ >= target` 已满足 → 直接返回（免落盘）；
  2. 已有领导者执行 `SyncOs` → 本线程作**跟随者**在 `gc_cv_` 等待，领导者 `notify_all` 后重新检查；
  3. 无人领导 → 本线程**上任领导者**：拍下 `flush_to = next_lsn_-1`（当前缓冲全部 LSN），解锁执行一次 `SyncOs()`，回锁后 `durable_lsn_ = max(durable_lsn_, flush_to)`、`notify_all` 唤醒全部跟随者；
  4. `durable_lsn_` 仍 < target（上任期间又有追加）→ 循环再刷，直到覆盖 target。
  - **接入点**：`TransactionManager::Commit` 把 `Flush()` 替换为 `GroupCommit(commit_lsn)`；失败抛异常由既有异常路径兜底（回滚语义不变）；
  - **并发安全**：`durable_lsn()` 起持锁读取；`sync_count_`（原子）累计真实 `SyncOs` 次数供对照。
- **数据**（`TestGroupCommit` 组提交基准）：**128 次并发提交（32 线程 × 4）仅触发 12 次 fsync**（`syncs_concurrent=12`），全部 `durable_lsn >= 各自 target` 且最终 `durable_lsn >= 最大 target`；对照**串行 128 次提交恰好 128 次 fsync** → **10.7× I/O 削减**（基准输出：`concurrent fsyncs=12, serial fsyncs=128 (10.7x fewer)`）。

### t3 温度感知刷盘（BufferPoolManager + Page）

- **瓶颈**：`FlushAllDirtyPages` 无条件写回全部脏页——热页（高频访问、大概率很快再改脏）也被落盘，既浪费写 IO 又可能触发不必要的淘汰。
- **方案**：
  - **温度采集**：`Page` 新增原子 `access_count_`，`GetPage`/`NewPage` 命中 `RecordAccess()`；帧复用（`ResetMemory`）清零避免跨复用温度污染；阈值默认 `kDefaultHotAccessThreshold = 8`，开关 `SetTemperatureFlushEnabled`（默认关闭，关闭时与旧版行为完全一致）；
  - **冷热分级**：`FlushAllDirtyUnlocked` 开启温度感知时只写回**冷脏页**（`access_count < 阈值`），**热脏页留池**——NO-FORCE + WAL-before-data 保证热页即便不落盘，崩溃后也能由 WAL redo 恢复，换来提交路径写 IO 削减与热页命中率保留；
  - **统计扩展**：`BufferPoolStats` 新增 `writeback_cold_count`/`writeback_hot_count`；所有写回路径（`FlushPageUnlocked`/`FlushAllDirtyUnlocked`/`DeletePage`/`FindFreeFrame`）统一经 `RecordWritebackStat` 记账（`writeback_count` 恒增、按温度分档；关闭时全记冷档，与旧版一致）。
- **数据**（`TestTemperatureAwareFlush`）：3 冷 + 3 热脏页，全量刷盘只落 3 冷（`writeback_cold_count==3`、`writeback_hot_count==0`），冷页已清脏、**热页仍留池保持脏**；显式 `FlushPage` 热页后 `writeback_hot_count==3`、`writeback_count==6`；关闭开关时 4 热页全写回且全记冷档（`cold==4`、`hot==0`）。

### 配套修复

- **`Database` 成员析构顺序**：`LogManager` 必须声明于 `BufferPoolManager` 之前——逆序析构时 BPM 的 `FlushAllPages` 仍访问 `log_manager_->durable_lsn()`/`Flush()`，GroupCommit 起 `durable_lsn` 持锁读取后若 LogManager 先销毁则构成 use-after-free 必现崩溃。

---

## 3. 测试证据

### 3.1 存储单元测试（storage_ut.exe）

| 里程碑 | checks | fails |
|---|---|---|
| 轮次起点（OS 优化 Phase 3 收官） | 48153 | 0 |
| Phase 4 收官：`TestPredicateIntervalTree` / `TestGroupCommit` / `TestTemperatureAwareFlush` | **58325** | **0** |

净增 **10172 checks**；多次复跑稳定（无死锁/超时/flaky）。`TestPredicateIntervalTree` 覆盖：覆盖/未覆盖键冲突语义、O(log P) 比较次数断言、全表哨兵收敛与 `UnlockAll` 后他事务区间不误删。`TestGroupCommit` 覆盖：128 并发提交的 fsync 合并（12 < 128/4）、`durable_lsn` 全量推进、串行对照恰好 1:1。`TestTemperatureAwareFlush` 覆盖：冷热分档落盘、热页留池保持脏、显式刷计入热档、关闭开关回退旧行为。

### 3.2 SQL 回归（build/Debug/sqlcompiler.exe）

- **55 passed / 0 failed**：53 条 SQL 脚本 + `run_acid_recovery` + `run_acid_clr` 崩溃注入。
- 隔离级别/索引重点用例 36_index_scan、40/43/46/48/49/50/51/52 全部 exit 0，无回归。

### 3.3 崩溃注入（49/50 两阶段复验）

- `49_acid_recovery`：phase1 `\crash` 退出码 1（崩溃注入生效）；phase2 恢复后 `1 | 100`（未提交 UPDATE 999 被 undo）、`2 | 60`（已提交 +10 持久化）、`ROLLBACK 777` 回滚后 `1 | 100`——exit 0。
- `50_undo_clr`：phase1 `\crash_after_undo_steps 1` 退出码 1；phase2 恢复后仅 3 行（1/2/3），`4/5/6` 由 CLR 链补做全部 undo 撤销——exit 0。
- **Phase 4 接入后 49/50 仍全部 exit 0**，达成建议书验收指标「崩溃注入 49/50 仍 exit 0」。

---

## 4. 前后对比汇总

| 维度 | 优化前 | 优化后 |
|---|---|---|
| 谓词写前检查 | 全局向量线性扫描 O(P)，1 万条目 ~1 万次判定 | 按表居中区间树点查询 **O(log P + K)**，未覆盖键 < 100 次比较 |
| 谓词存储 | 全局 `pred_locks_` 向量 | 按表 `PredicateTable`（源向量 + 脏标记 + 惰性重建），全表谓词作哨兵 |
| WAL 提交 | 每次 COMMIT 同步 `Flush()`（一次 fsync） | `GroupCommit` 领导者-跟随者，批内共享一次 fsync |
| 组提交 I/O | 128 次串行提交 = 128 次 fsync | 128 次并发提交 = **12 次 fsync（10.7× 削减）** |
| 全量刷脏 | 全部脏页无条件写回 | 冷页优先落盘、热页留池（NO-FORCE + WAL-before-data 保证恢复正确） |
| 写回统计 | `writeback_count` 单计数 | 扩展 `writeback_cold_count`/`writeback_hot_count` 刷盘温度分布 |
| 磁盘格式 | —（本阶段基线） | `.fpl`/`.crc`/WAL 与 48B `MvccRecordHeader` 布局零变更 |
| 提交持久性 | —（本阶段基线） | `durable_lsn()` 持锁读取，与 GroupCommit 并发安全，恢复语义不变 |

---

## 5. 文件改动清单

**t1（谓词锁区间树）**：`include/storage/LockManager.h` + `src/storage/LockManager.cpp`（`Interval`/`IntervalNode`/`PredicateTable` 结构、`pred_tables_` 按表组织、`PredicateTreeRebuild`/`PredicateTreeBuildRange`/`PredicateTreeQuery`、观测接口 `CountTotalPredicates`/`GetPredicateQueryComparisons`/`ResetPredicateQueryComparisons`；`AcquireReadPredicate`/`CheckWritePredicate` 迁移到区间树；移除旧 `PredicateCovers` 线性扫描）。

**t2（WAL 组提交）**：`include/txn/LogManager.h` + `src/txn/LogManager.cpp`（`GroupCommit` 领导者-跟随者、`gc_cv_`/`gc_leader_`/`sync_count_`、`durable_lsn()` 持锁读取、`GetSyncCount()`）、`src/txn/TransactionManager.cpp`（Commit 路径 `Flush()` → `GroupCommit(commit_lsn)`）。

**t3（温度感知刷盘）**：`include/storage/Page.h` + `src/storage/Page.cpp`（原子 `access_count_` + `RecordAccess`/`GetAccessCount`、`ResetMemory` 清零）、`include/storage/BufferPoolManager.h` + `src/storage/BufferPoolManager.cpp`（`SetTemperatureFlushEnabled`/`SetHotAccessThreshold`、`FlushPageUnlocked`/`FlushAllDirtyUnlocked` 免锁主体、`RecordWritebackStat` 统一记账、`writeback_cold_count`/`writeback_hot_count` 统计）。

**配套修复**：`include/db/Database.h`（`LogManager` 声明移至 `BufferPoolManager` 之前，修复析构顺序 use-after-free）。

**测试**：`tests/storage/storage_ut.cpp`（`TestPredicateIntervalTree` / `TestGroupCommit` / `TestTemperatureAwareFlush` + main 注册）。

**技术文档**：
- `.trae/documents/Phase4_PredicateTree_GroupCommit_TempFlush.md`（方案 + 前后对比数据 + 测试证据 + 已知限制）
- 并同步更新 `docs/Storage_Dev_Plan.md`（进度 → 58325 checks / 55 回归，Phase 4 遗留已知限制）

---

## 6. 已知限制（后续扩展候选）

1. 谓词区间树惰性重建 O(P log P)：高频谓词增删（单语句内大量区间注册）未做增量插入；
2. 组提交批量窗口未加显式计时器：当前靠跟随者被动聚合，高频小事务场景可引入「时间窗聚合」进一步摊薄 fsync 开销；
3. 温度阈值静态常量（默认 8）：未按访问分布动态自适应，热页判定未与 LRU-K 淘汰策略联动；
4. 谓词锁仍作用于主键键空间（非主键谓词不注册）；多列索引仅单列最左列参与区间推导（前序遗留）。

---

## 7. 结论

Phase 4 两个创新特性（E：谓词锁区间树；F：WAL 组提交 + 温度感知刷盘）全部按「实现 → 测试 → 文档」闭环交付并**验收达标**：写前谓词检查从 O(P) 线性扫描降为 **O(log P + K)** 区间树点查询（1 万条目下未覆盖键 < 100 次比较）；WAL 提交从每次 COMMIT 一次 fsync 变为**领导者-跟随者组提交**（128 次并发提交仅 12 次 fsync，**10.7× I/O 削减**）；全量刷盘升级为**冷热分级**（冷页落盘、热页留池，WAL 保证崩溃恢复正确）。存储 UT **58325 checks / 0 fails**（较基线 48153 净增 10172）、SQL 回归 **55 passed / 0 failed**（含崩溃注入 49/50）、技术文档 + 开发计划 + 验收报告同步更新。磁盘格式零变更、锁序/并发纪律（页闩 RAII、持页闩不调 BPM、WAL-before-data）与既有隔离级别语义全部保持。
