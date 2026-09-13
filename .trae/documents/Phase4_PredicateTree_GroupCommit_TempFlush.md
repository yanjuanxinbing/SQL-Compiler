# Phase 4（创新特性 E+F）：谓词锁区间树 + WAL 组提交 + 温度感知刷盘

## Context（为什么做）

1. **谓词检查线性扫描 O(P)**：SERIALIZABLE 谓词锁按 `pred_locks_` 向量线性存储，`CheckWritePredicate` 写前逐条判定 `PredicateCovers`。谓词条目随长事务多次范围扫描累积到万级时，每次 INSERT/UPDATE/DELETE 的写前检查都是 O(P) 全扫。
2. **提交吞吐受 fsync 频率限制**：每次事务提交 `TransactionManager::Commit` 都同步 `LogManager::Flush()`（fdatasync）。串行提交下一条事务一次 fsync，磁盘 IOPS 直接成为并发提交吞吐的瓶颈。
3. **全量刷盘无冷热调度**：`FlushAllDirtyPages` 无条件写回全部脏页——热页（高频访问、大概率很快又被改脏）也被落盘，既浪费写 IO 又可能触发不必要的淘汰。

Phase 4 以建议书 E/F 两个创新特性收口：把「有序索引」思想引入锁表结构（谓词区间树），把「组提交 + 冷热分离刷盘」这一 OS/文件系统经典手段落地到 WAL 与缓冲池。

## 优化前（基线）

| 项 | 旧实现 | 代价 |
|---|---|---|
| 谓词写前检查 | 全局向量线性扫描 O(P) | 1 万谓词条目下每次写前检查 ~1 万次判定 |
| WAL 提交 | 每次 COMMIT 一次 `Flush()`（fsync） | 并发提交吞吐受磁盘 IOPS 上限，一条事务一次落盘 |
| 全量刷脏 | 所有脏页无条件写回 | 热页写 IO 浪费、页常驻率下降 |

## 方案与技术细节

### t1：谓词锁区间树（LockManager E）

**数据结构**：谓词存储从全局向量迁移为**按表组织的居中区间树**（centered interval tree，CLRS 区间树形态）：

- `pred_tables_`：`table_rid -> PredicateTable`。每表一棵树；
- 节点分裂点取**该表全部区间 lo 值集合去重升序的中位数**（`lo_values` + `split_idx`）；
- **跨过分裂点**的区间（`lo <= split <= hi`）存于节点本身：`by_lo_asc`（lo 升序）+ `by_hi_desc`（hi 降序）双有序表；
- **完全在分裂点左侧/右侧**的区间递归下放左右子树；
- 全表谓词（`is_full`）作为**全域区间哨兵**单独存放于 `full_holders`，不进树。

**点查询**（`PredicateTreeQuery`）：stabbing query 沿分裂点二分下降：

- `key < split`：本节点所有区间都有 `hi >= split > key`，只需输出 `by_lo_asc` 中 `lo <= key` 的前缀；
- `key > split`：本节点所有区间都有 `lo <= split < key`，只需输出 `by_hi_desc` 中 `hi >= key` 的前缀；
- `key == split`：本节点全部区间覆盖该键，整段输出；
- 每层输出后下探对应子树，直至叶子 → **O(log P + K)**（K 为命中区间数）。

**惰性重建**：谓词注册/注销频率低（语句级），采用「源向量 `intervals` + 脏标记 `dirty` + 惰性重建」——注册/释放只动 `intervals`（合并后的不重叠最小覆盖，复用 v2 区间继承与合并规约）并置脏；下次查询前若脏则 `PredicateTreeRebuild`（O(P log P)）一次性建树，热路径之外摊薄。

**兼容性**：`CheckWritePredicate` 对外签名不变（`txn_id, table_rid, key, wait_ms`），查询命中区间后按既有语义判冲突（命中持有者中排除自己、检查是否全表哨兵/区间覆盖，冲突则按等待/超时/死锁处理）；`UnlockAll` 提交时统一清除本事务全表与区间谓词。

**内存安全**：递归建树 `PredicateTreeBuildRange` 在递归期间不得持有跨递归调用的 `IntervalNode&` 引用（递归 `push_back` 触发 `nodes` 向量重分配会令引用悬垂→堆损坏）；改为局部作用域填充节点、递归后按下标重取引用回填 `left/right`。

### t2：WAL 组提交（LogManager F）

**领导者-跟随者模式**：`LogManager::GroupCommit(target)` 替代提交路径的同步 `Flush()`：

1. 持 `mutex_` 检查 `durable_lsn_ >= target` 已满足 → 直接返回（无需落盘）；
2. 已有领导者在执行 `SyncOs`（`gc_leader_ == true`）→ **跟随者** `gc_cv_.wait`，等待领导者 `notify_all` 唤醒后重新检查；
3. 无人领导 → 本线程**成为领导者**：记 `gc_leader_ = true`、把 `flush_to = next_lsn_ - 1`（当前缓冲全部 LSN）拍下，解锁执行一次 `SyncOs()`（真正 fsync），回锁后清除领导标志、`durable_lsn_ = max(durable_lsn_, flush_to)`、`notify_all` 唤醒所有跟随者；
4. `durable_lsn_` 仍 < target（领导者上任期间又有新追加）→ 循环再次成为领导者继续刷，直到覆盖 target。

**接入点**：`TransactionManager::Commit` 把 `log_manager_->Flush()` 替换为 `log_manager_->GroupCommit(commit_lsn)`。组内所有并发提交共享一次 fsync；失败时 `GroupCommit` 抛异常由 Commit 既有异常路径兜底（回滚语义不变）。

**观测**：`sync_count_`（原子）累计真正执行 `SyncOs` 的次数，`GetSyncCount()` 供对照实验；`durable_lsn()` Phase 4 起持锁读取，与 GroupCommit 并发安全（修复此前解锁读造成的竞态）。

### t3：温度感知刷盘（BufferPoolManager F）

- **温度采集**：`Page` 新增原子 `access_count_`，`GetPage`/`NewPage` 命中时 `RecordAccess()`；页帧复用（`ResetMemory`）清零，避免跨复用温度污染。阈值默认 `kDefaultHotAccessThreshold = 8`，可经 `SetHotAccessThreshold` 配置；开关 `SetTemperatureFlushEnabled`（默认关闭，关闭时行为与旧版完全一致）。
- **冷热分级刷盘**：`FlushAllDirtyUnlocked` 开启温度感知时只写回**冷脏页**（`access_count < 阈值`）；**热脏页留池**——NO-FORCE + WAL-before-data 保证热页即便不落盘，崩溃后也能由 WAL redo 恢复，换来提交路径写 IO 削减与热页命中率保留。
- **统计扩展**：`BufferPoolStats` 新增 `writeback_cold_count`/`writeback_hot_count`。`RecordWritebackStat(page)` 统一记账：`writeback_count` 恒增，按访问温度分档；关闭温度感知时全部记入冷档（与旧版 `writeback_count` 语义一致）。
- **统一接入**：`FlushPageUnlocked`、`FlushAllDirtyUnlocked`、`DeletePage`、`FindFreeFrame` 等写回路径全部改经 `RecordWritebackStat`，且冷/热判定与写盘均在持 `latch_`（免锁内部实现 `Flush*Unlocked`）与页读锁下完成，不引入锁序违背。

## 取得的进步数据

| 指标 | 优化前 | 优化后 | 度量方式 |
|---|---|---|---|
| 谓词写前检查 | 线性扫描 O(P)，1 万条目 ~1 万次判定 | 区间树点查询 O(log P + K)，未覆盖键 < 100 次比较 | `TestPredicateIntervalTree` 断言 `comps < 100`（log2(1e4)≈14 层） |
| 组提交 fsync 次数 | 128 次串行提交 = 128 次 fsync | 128 次并发提交 = 12 次 fsync，**10.7× I/O 削减** | `TestGroupCommit` 组提交基准输出 |
| 全量刷盘写回 | 全部脏页写回 | 冷页落盘、热页留池（显式刷才计热档） | `TestTemperatureAwareFlush` 断言 cold=3/hot=0 → hot=3 |

## 测试证据

- 新增 `TestPredicateIntervalTree`：1 万区间建树 → 覆盖键阻塞（kTimeout）/未覆盖键通过（kGranted）；未覆盖键比较次数 < 100（O(log P) 断言）；全表哨兵挡住任意键、`UnlockAll` 后他事务区间不被误删。
- 新增 `TestGroupCommit`：32 线程 × 4 次 = 128 次并发提交，`durable_lsn` 全部 ≥ 各自 target、最终 ≥ 最大 target；并发 fsyncs 12 < 128/4；对照串行 128 次提交恰好 128 次 fsync。
- 新增 `TestTemperatureAwareFlush`：3 冷 + 3 热脏页，全量刷盘只落 3 冷（`writeback_cold_count==3`，热页保持脏）；显式 `FlushPage` 热页记入热档（`writeback_hot_count==3`、`writeback_count==6`）；关闭开关时 4 热页全写回且全记冷档。
- **storage_ut：58325 checks / 0 fails**（Phase 3 基线 48153 之上净增 10172）；**SQL 回归 55 passed / 0 failed**（53 条 SQL + acid_recovery + acid_clr 崩溃注入，Phase 4 后仍全部 exit 0）。

## 约束与兼容性

- 磁盘格式**零变更**：`.fpl`/`.crc`/WAL 与 48B `MvccRecordHeader` 布局不变，旧库向后兼容；谓词区间树、组提交、温度刷盘均只在内存/统计层生效。
- 组提交与既有 WAL-before-data 顺序不变（先 AppendRecord 串 prev_lsn 链、再 GroupCommit 落盘）；崩溃恢复（ARIES redo/undo）语义不变，49/50 崩溃注入验证通过。
- 温度感知刷盘默认关闭（`temp_flush_enabled_ = false`），关闭时全量刷盘行为与旧版一致（全部写回、全记冷档），可经配置开关随时回退。
- 锁序约定不变：写盘路径在 `latch_` 内、页读锁下执行；`LogManager::GroupCommit` 只在 `mutex_` 与 `gc_cv_` 上等待，不触碰 BPM，无锁序环。
- 修复 `Database` 成员析构顺序：`LogManager` 必须声明于 `BufferPoolManager` 之前（逆序析构时 BPM 的 FlushAllPages 仍访问 `log_manager_->durable_lsn()`/`Flush()`，GroupCommit 起 `durable_lsn` 持锁读取后该 use-after-free 必现）。

## 文件改动清单

| 文件 | 改动 |
|---|---|
| `include/storage/LockManager.h` / `src/storage/LockManager.cpp` | 谓词存储迁移为按表居中区间树；`PredicateTreeRebuild/BuildRange/Query`；观测接口 `CountTotalPredicates`/`GetPredicateQueryComparisons` |
| `include/txn/LogManager.h` / `src/txn/LogManager.cpp` | `GroupCommit` 领导者-跟随者；`gc_cv_`/`gc_leader_`/`sync_count_`；`durable_lsn()` 持锁读取 |
| `src/txn/TransactionManager.cpp` | Commit 路径 `Flush()` → `GroupCommit(commit_lsn)` |
| `include/storage/Page.h` / `src/storage/Page.cpp` | 原子 `access_count_` + `RecordAccess`/`GetAccessCount`；`ResetMemory` 清零温度 |
| `include/storage/BufferPoolManager.h` / `src/storage/BufferPoolManager.cpp` | 温度刷盘开关/阈值配置；`FlushAllDirtyUnlocked` 冷热分级；`RecordWritebackStat` 统一记账；`writeback_cold_count`/`writeback_hot_count` 统计 |
| `include/db/Database.h` | 修复 `LogManager` 先于 `BufferPoolManager` 声明的析构顺序 |
| `tests/storage/storage_ut.cpp` | `TestPredicateIntervalTree`/`TestGroupCommit`/`TestTemperatureAwareFlush` |

## 已知限制（后续扩展候选）

1. 谓词区间树按表惰性重建：高频谓词增删（如单语句内大量区间注册）下重建成本仍为 O(P log P)，未做增量插入；
2. 组提交批量窗口未做显式计时器（当前靠跟随者被动聚合）：高频小事务场景可引入「时间窗聚合」进一步摊薄 fsync 开销；
3. 温度阈值目前为静态常量（默认 8）：未按访问分布动态自适应；热页判定未与淘汰策略（LRU-K）联动；
4. 组提交与温度刷盘开关沿用环境变量/构造参数注入模式，默认关闭时行为与旧版一致，未做生产默认启用论证。
