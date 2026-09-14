# 存储子系统最终验收报告

- 项目：小型数据库系统 SQL-Compiler（编译原理 + 操作系统 + 数据库 三模块综合实训）
- 验收对象：**存储子系统（页式存储 + 缓冲池 + 替换策略 + 持久化 + 并发扩展 + 可观测性）**
- 验收日期：2026-09-14（T5 收口最终版）
- 依据文档：`Manual.md`（指导书）、`docs/Storage_Dev_Plan.md`（后续开发计划 T1–T5）、`docs/upload_os_module/04_模块设计文档.md`（设计文档）
- 验收结论：**通过**

---

## 1. 验收范围

存储子系统是操作系统模块的核心交付物，为上层 SQL 编译器与存储引擎提供页式存储、缓存与持久化底座。本报告覆盖：

1. 指导书 **操作系统页面管理** 的 12 项基础需求（页式存储 / 缓存替换 / 访问接口 / 日志统计等）；
2. 面向「平台软件设计」延伸出的 **8 项择机扩展**（E1–E8）及两项支撑改进（D5 页 CRC、D7 全局锁）；
3. 后续开发计划 **T1–T5 全部里程碑**：多会话并发事务、隔离级别与行级锁、MVCC 快照、乐观 B+Tree 并发、OS 优化 Phase 1–4、介质扩展与可观测性、-Wall 审计收口；
4. 以上功能的单元测试、SQL 回归、ACID 恢复与崩溃恢复验证证据。

---

## 2. 系统架构概述

存储子系统沿三大分层组织，职责单一、接口解耦：

```
上层：存储引擎 TableHeap（MVCC 版本链）/ 索引 BPlusTree（乐观页级并发）/ 系统目录
  │       经 RAII 句柄 PageGuard / PageRead·WriteGuard 访问；写路径 per-table write_mutex_ 串行化
  ▼
缓冲池 BufferPoolManager（帧数组 + 页表 + 可插拔 Replacer + 页级读写锁 + 温度感知刷脏 + 后台刷脏）
  │       PoolSize 可配置内存上限；命中/替换日志/IO/写回统计；脏页年龄/刷脏直方图可观测
  ▼
磁盘 DiskManager（页分配/回收 + 页 CRC 旁路 + 空闲页 `<db>.fpl` 位图 + 64 位定位）
  │       页 I/O 经 BlockDevice 抽象（File / Memory / SparseFile / LoopbackNetwork / FaultInjecting）
  ▼
持久化介质（WAL 组提交：日志先落盘、数据页后落盘、批内并发提交共享一次 fsync）
```

并发正确性由「**四层锁层次 + 严格锁序 + 乐观并发兜底 + 后台线程调度**」共同保证（设计文档 §7）：

- **L4 全局/元数据层**：`BPM::latch_`（缓冲池全局）、`LockManager::meta_mutex_`（跨分片元数据）；
- **L3 分片锁层**：`LockManager` 16 分片（表/行/谓词多粒度 S/X，行锁与所属表锁同片不变量）；
- **L2 页级层**：Page 读写闩（RAII）+ TableHeap per-table `write_mutex_`；
- **L1 磁盘/日志层**：`DiskManager::db_io_latch_`、`LogManager::mutex_`。

关键不变量（贯穿所有实现）：

- **锁序恒定**：`BPM::latch_ → LogManager::mutex_`、`BPM::latch_ → DiskManager`、`shard mutex → meta_mutex_`、持页闩不请求 BPM，反向路径不存在 → 多线程无死锁环；
- **WAL-before-data**：任何页落盘前保证 `page_lsn ≤ durable_lsn`，崩溃后不丢已提交数据；
- **旁路 + 向后兼容**：所有持久化新增（`.crc`、`.fpl`、块设备层、组提交）不改变既有数据文件布局，旧库可直接打开。

---

## 3. 基础需求验收（指导书操作系统页面管理）

下表逐项对照验收，均为 **✅ 已满足**：

| # | 指导书要求 | 实现载体 | 验证证据 |
|---|-----------|----------|----------|
| 1 | 每页固定大小（4KB）、页编号唯一 | `Page.h`（`PAGE_SIZE`、`page_id`） | `TestPage`、`TestDiskManager` |
| 2 | 页的分配 / 释放 / 读写 | `AllocatePage / DeallocatePage / ReadPage / WritePage` | `TestDiskManager`、`\stats` 可观测磁盘页/空闲页数 |
| 3 | 数据表映射到页集合（物理结构） | `TableHeap` 页串链表 + tuple↔page 序列化 | SQL 建表/插入/查询回归 |
| 4 | 统一存储访问接口 | `DiskManager` + `BufferPoolManager`（get_page / flush_page / write_page…） | `storage_ut` 全量 |
| 5 | 页缓存机制提升访问效率 | `BufferPoolManager`（默认 64 帧，`SQLCOMPILER_BUFFER_MEMORY` 可配内存上限） | `TestBufferPool`、`TestBufferPoolMemory` |
| 6 | LRU / FIFO 替换策略 | `LRUReplacer` + `FIFOReplacer`（策略模式，可插拔） | `TestLRU`、`TestFIFO`；CLOCK/LRU-K 亦接入 |
| 7 | 缓存命中统计 | `BufferPoolStats`（hit/miss/replacement + 温度分档命中构成）→ `\stats` 命中率 | `TestBufferPool`、`TestT4Observability` |
| 8 | 页替换日志输出 | `ReplacementLogEntry`（evict/loaded/dirty）+ 最近 20 条、1024 环形上限 | `TestReplacementLogCap`、`\stats` |
| 9 | 与执行计划对接（物理访问） | `ExecutionEngine → 存储引擎 → 缓冲池 → 磁盘` | SQL DDL/DML/JOIN/索引回归 |
| 10 | 数据持久化（重启不丢） | WAL-before-data + 真持久 fsync + 两阶段恢复 + CLR 链 | ACID 恢复用例（49/50）跨重启 |
| 11 | 管理空闲页列表（扩展/回收） | `free_pages_` 会话内复用 + `<db>.fpl` 位图跨重启持久化（magic/版本/页数校验防误复用） | `TestFreePagePersistence` |
| 12 | 错误处理：语法/语义/IO 错误清晰反馈 | `ExecuteSQL` 统一错误通道 + `DiskManager`/块设备异常上抛 | 负面用例 + 故障注入 |

---

## 4. 扩展项验收（E1–E8 + 支撑改进 + T 里程碑扩展）

### 4.1 基础扩展（E1–E8 + D5/D7）

| 编号 | 扩展项 | 实现要点 | 单测证据 |
|------|--------|----------|----------|
| **D5** | 页 CRC32（`<db>.crc` 旁路） | 无头、每页 4 字节 CRC；惰性创建；写路径只 flush、组提交 Sync 统一落盘；读回校验拦截静默损坏；CRC 校验失败累计计数 | `TestPageCrc`、`TestT4Diagnostics` |
| **D7** | 缓冲池全局锁 `latch_` | 恒定锁序串行化帧表；内部自调用拆免锁私有辅助防重入死锁；统计改快照返回 | 并发单测 |
| **E1** | Clock 替换算法 | 环形指针 `hand_` + 参考位二次机会，沿 `Replacer` 接入 | `TestClock`、`TestBufferPoolClock` |
| **E2** | 页内内存分配器 | 页内 free-list 堆：first-fit、左右合并、碎片统计；与磁盘 slotted-page 解耦 | `TestPageAllocator` |
| **E3** | 缓冲池统计增强（IO 计数） | DiskManager 物理读/写计数 + `BufferPoolStats::writeback_count`（四条脏回路径统一记账） | `TestIOStats` |
| **E4** | 页级读写锁并发 | `Page` 帧 `std::shared_mutex` + `PageRead/WriteGuard` RAII；BPM 四条写回路径加帧读锁防撕裂 | `TestPageRWLock`（撕裂恒 0 + 共享并发 ≥2） |
| **E5** | 后台异步刷脏线程 | 周期刷脏仅写 OS 缓存不 `Sync()` → 组提交 fsync 次数不变；默认 opt-in（`SQLCOMPILER_BG_FLUSH_MS`） | `TestBackgroundFlush` |
| **E6** | 缓冲池内存上限可配置 | 字节上限 `FramesForBytes` 换算 + `\stats` 内存统计 + 环境变量配置 | `TestBufferPoolMemory` |
| **E7** | 块设备抽象层 | `BlockDevice` 接口 + `FileBlockDevice` + 故障注入装饰器；与 D5 联动拦截坏块 | `TestBlockDeviceFaultInjection` |
| **E8** | LRU-K 替换算法 | `recent_store_`（<K）/ `historic_store_`（≥K）双集合；抗顺序扫描 | `TestLRUK`（命中率对比胜 LRU） |

### 4.2 T 里程碑扩展（T2–T5，2026-09-12 ~ 2026-09-14）

| 里程碑 | 扩展项 | 实现要点 | 单测/验证证据 |
|--------|--------|----------|---------------|
| **T2** | 多会话并发事务 | `Session` + 独立事务/自动提交；per-table `write_mutex_` 串行化写序列（消除同表并发丢行竞态）；事务级 LockManager（S/X + 等待图 DFS 死锁检测 + 超时） | `TestConcurrentSessions`、`TestLockManager` |
| **T2** | 隔离级别 + 行级并发 | READ UNCOMMITTED / READ COMMITTED / SERIALIZABLE / SNAPSHOT；行级 S/X 锁、谓词锁防幻读、自适应行锁升级；RC 语句末释放读锁（成功/异常双路径） | `TestIsolationLevels`、`TestRowLevelConcurrency`、`TestSerializablePredicatePhantom`、`TestRowLockEscalation` |
| **T2** | MVCC 快照隔离 | 48B 版本链 + 共享 CommitTracker + FCW 防丢失更新 + 二级索引精确可见性 + 内联/后台真空 + O(1) 版本索引缓存 | `TestSnapshotIsolation`、`TestSnapshotDeleteFcw`、`TestSnapshotUpsertFcw`、`TestSnapshotIndexScan`、`TestBackgroundVacuumThread` |
| **T3** | 乐观 B+Tree 并发（OS Phase 1） | 页级读写闩 + 乐观重启（版本号复用页头保留字段，零格式变更）；快/慢写路径 + 预分裂；「先 pin 后加闩」分裂；物化快照游标 | `TestOptimisticSplitConcurrency`；吞吐 4 线程 1.93× / 8 线程 2.71× / 16 线程 3.18× |
| **T3** | O(1) 快照水位 + 内联真空（OS Phase 2） | 低水位单调缓存（约 465× 读加速）；写路径顺带回收旧版本（滞后 ↓91.7%） | `TestLowWaterMarkO1`、`TestInlineVacuum` |
| **T3** | 版本链 O(1) 查询与自动清理（OS Phase 3） | 内存版本索引缓存 + CSN 直判；墓碑槽复用 + 真空链摘除；索引墓碑回收精确判定 | `TestAdaptiveLockEscalation`、`TestTombstoneSlotReuse`、`TestVacuumChainUnlink`、`TestIndexTombstoneReclaim`、`TestIndexVacuumLongChain` |
| **T3** | 谓词区间树 + 组提交 + 温度刷盘（OS Phase 4） | 居中区间树 O(log P + K)；组提交领导者-跟随者（128 提交 12 次 fsync，10.7×）；温度感知刷盘 + 自适应阈值 | `TestPredicateIntervalTree`、`TestGroupCommitTimeWindow`、`TestTemperatureFlush` |
| **T4** | 介质与可观测性 | Memory / SparseFile / LoopbackNetwork 三种可交换块设备；`\stats` 命中构成/脏页年龄/刷脏直方图/IO 队列；`\analyze` 页映射 + CRC 累计计数 | `TestT4BlockDevices`、`TestT4Observability`、`TestT4Diagnostics` |
| **T5** | 收口与审计 | `-Wall -Wextra` 零告警（清理 234 条）；全量回归复核行为零变化 | `audit_build.log`、storage_ut 58953 checks |

实现哲学：全部遵守「沿既有抽象扩展、不改数据文件布局、默认行为逐字节兼容」，并逐项固化在 `docs/Storage_Dev_Plan.md` 与设计文档中。

---

## 5. 测试与验证结论

### 5.1 存储单元测试

```
build\storage_ut.exe
======== Storage UT ========
checks: 58953   fails: 0
RESULT: PASS
```

覆盖页管理、空闲页持久化、页 CRC、缓冲池并发、LRU/FIFO/Clock/LRU-K 替换、页分配器、IO/内存统计、后台刷脏、块设备故障注入、页级读写锁、多会话并发、死锁检测、隔离级别（RC/RR/SERIALIZABLE/SNAPSHOT）、谓词锁、行锁升级、MVCC 版本链、FCW、二级索引快照扫描、乐观 B+Tree 分裂/扫描、谓词区间树、快照低水位 O(1)、内联真空、索引墓碑回收、后台真空线程、T4 介质与可观测性等全部存储路径。另随测输出 OS 模块性能对比基准（CRC/LRU/页分配 3 组 staged 基准）。

### 5.2 SQL 功能回归

- 全量 **55 个用例**（53 条 SQL 脚本 + 2 套崩溃注入）**exit=0**，其中负面/语义错误用例行为符合预期，与存储改动无关；
- 覆盖 DDL / DML / JOIN / 索引 / 系统目录 / 事务隔离（40/46/48/51/52）/ 崩溃恢复（49/50）；
- CLI 实测：`\stats` 输出命中构成（cold/warm/hot）、脏页年龄分布、后台刷脏直方图、IO 队列、WAL fsync 计数；`\analyze` 输出页映射快照与 CRC 计数，均在位。

### 5.3 ACID 与崩溃恢复

| 用例 | 验证内容 | 结果 |
|------|----------|------|
| test 49（\crash） | COMMIT 后崩溃，重启 redo 不丢已提交数据；未提交 UPDATE 被 undo 回滚 | PASS |
| test 50（\crash_after_undo_steps N） | 回滚途中崩溃，重启动态 undo 经 CLR 链补做完整 | PASS |

跨重启恢复在页 CRC、WAL-before-data、组提交 fsync 与 CLR 链下稳定无误报。

### 5.4 告警审计（T5）

- `-Wall -Wextra` 全量编译 **0 warning / 0 error**（`build_audit/`，证据 `docs/test_evidence/audit_build.log`）；
- 清理 234 条告警：8 处 switch 补 `default`、删除未用函数/变量/参数、修复 sign-compare / type-limits / 初始化序、MSVC `#pragma` 加 `_MSC_VER` 防护；
- 清理后 storage_ut 58953 checks / SQL 回归 55/55，与清理前基线一致，行为零变化。

### 5.5 兼容性 / 无回归承诺

- 所有扩展默认关闭或与旧路径行为一致：默认 `ReplacementPolicy::LRU`、默认 64 帧、后台刷脏/真空 opt-in、温度刷盘 opt-in、组提交时间窗默认 0、块设备默认 `FileBlockDevice`；
- 全部持久化新增走旁路文件（`.crc` / `.fpl`），**不改既有数据文件与 WAL 格式**，旧库可直接打开；
- 单线程行为逐字节不变（页级锁无争用）。

---

## 6. 已知限制与前瞻

以下为审慎声明的边界，非验收缺陷：

- 谓词锁覆盖任意列区间但**全表谓词与列谓词**按区间相交判定（设计文档 §6 已闭环 G1）；多列索引区间推导遵循最左前缀原则（G2 已闭环）；
- B+Tree 乐观重启的路径校验为「下降后整体复核」，长树高下重启成本随树高线性增长（当前树高 ≤3–4 层，影响可忽略）；
- 删除仍只打墓碑、不做节点合并与再平衡（索引真空负责物理回收）；
- 多块大小页面（如混合 4KB/8KB）未纳入，与既定「旁路 + 向后兼容」约束冲突，收益需另行论证。

---

## 7. 验收结论

- ✅ 指导书 **12 项基础需求**全部实现并验证通过；
- ✅ **8 项扩展 + 2 项支撑改进**全部落地，均含独立单测与文档记录；
- ✅ **T1–T5 里程碑全部闭环**：多会话并发事务、隔离级别与行级锁、MVCC 快照、乐观 B+Tree 并发、OS 优化 Phase 1–4、T4 介质与可观测性、T5 告警审计；
- ✅ 存储单元测试 **58953 checks / 0 fails**，SQL 回归 **55 passed / 0 failed**（含 49/50 崩溃注入两阶段），`-Wall -Wextra` **零告警**；
- ✅ 无回归、向后兼容，扩展均遵守既有的锁序 / WAL / 旁路持久化不变量；
- ✅ 一键复现：`tests\run_repro_all.ps1`（构建 → storage_ut → SQL 回归 → 参数扫描 → 曲线图，证据包输出 `docs\test_evidence\`）。

**评估：存储子系统满足设计目标与指导书验收要求，准予通过。**

---

*本报告数据来源：最新一轮 `build/storage_ut` 实测（2026-09-14，58953 checks）与 `docs/test_evidence/` 证据日志（storage_ut_run.log / sql_regression_run.log / audit_build.log / t4_*_console.log）。*
