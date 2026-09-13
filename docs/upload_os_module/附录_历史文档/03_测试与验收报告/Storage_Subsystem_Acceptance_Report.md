# 存储子系统最终验收报告

- 项目：小型数据库系统 SQL-Compiler（编译原理 + 操作系统 + 数据库 三模块综合实训）
- 验收对象：**存储子系统（页式存储 + 缓冲池 + 替换策略 + 持久化 + 并发扩展）**
- 验收日期：2026-09-12
- 依据文档：`Manual.md`（指导书）、`docs/OS_Module_Development_Plan.md`（OS 模块设计方案与实施记录）
- 验收结论：**通过**

---

## 1. 验收范围

存储子系统是操作系统模块的核心交付物，为上层 SQL 编译器与存储引擎提供页式存储、缓存与持久化底座。本报告覆盖：

1. 指导书 **操作系统页面管理** 的 12 项基础需求（页式存储 / 缓存替换 / 访问接口 / 日志统计等）；
2. 面向「平台软件设计」延伸出的 **8 项择机扩展**（E1–E8）及两项支撑改进（D5 页 CRC、D7 全局锁）；
3. 以上功能的单元测试、SQL 回归、ACID 恢复与崩溃恢复验证证据。

---

## 2. 系统架构概述

存储子系统沿三大分层组织，职责单一、接口解耦：

```
上层：存储引擎 TableHeap / 索引 BPlusTree / 系统目录（sys_tables 等特殊表）
  │       经 RAII 句柄 PageGuard / PageRead·WriteGuard 访问
  ▼
缓冲池 BufferPoolManager（帧数组 + 页表 + 可插拔 Replacer + 页级读写锁 + 后台刷脏）
  │       PoolSize 可配置内存上限；命中/替换日志/IO/写回统计
  ▼
磁盘 DiskManager（页分配/回收 + 页 CRC 旁路 + 空闲页 `<db>.fpl` 位图）
  │       页 I/O 经 BlockDevice 抽象（FileBlockDevice / FaultInjecting 装饰器）
  ▼
持久化介质（WAL 日志先落盘，数据页后落盘，Commit 统一 fsync）
```

关键不变量（贯穿所有实现）：

- **锁序恒定**：`BPM::latch_ → LogManager::mutex_`、`BPM::latch_ → DiskManager/DiskManager(BlockDevice)`，反向路径不存在 → 多线程无死锁环；
- **WAL-before-data**：任何页落盘前保证 `page_lsn ≤ durable_lsn`，崩溃后不丢已提交数据；
- **旁路 + 向后兼容**：所有持久化新增（`.crc`、`.fpl`、块设备层）不改变既有数据文件布局，旧库可直接打开。

---

## 3. 基础需求验收（指导书操作系统页面管理）

下表逐项对照验收，均为 **✅ 已满足**：

| # | 指导书要求 | 实现载体 | 验证证据 |
|---|-----------|----------|----------|
| 1 | 每页固定大小（4KB）、页编号唯一 | `Page.h`（`PAGE_SIZE`、`page_id`） | `TestPage`、`TestDiskManager` |
| 2 | 页的分配 / 释放 / 读写 | `AllocatePage / DeallocatePage / ReadPage / WritePage` | `TestDiskManager`、`\stats` 可观测磁盘页/空闲页数 |
| 3 | 数据表映射到页集合（物理结构） | `TableHeap` 页串链表 + tuple↔page 序列化 | SQL 建表/插入/查询回归 |
| 4 | 统一存储访问接口 | `DiskManager` + `BufferPoolManager`（get_page / flush_page / write_page…） | `storage_ut` 全量 |
| 5 | 页缓存机制提升访问效率 | `BufferPoolManager`（默认 64 帧，E6 可配置内存上限） | `TestBufferPool`、`TestBufferPoolMemory` |
| 6 | LRU / FIFO 替换策略 | `LRUReplacer` + `FIFOReplacer`（策略模式，可插拔） | `TestLRU`、`TestFIFO`；CLOCK/LRU-K 亦接入 |
| 7 | 缓存命中统计 | `BufferPoolStats`（hit/miss/replacement）→ `\stats` 命中率 | `TestBufferPool`、`TestIOStats` |
| 8 | 页替换日志输出 | `ReplacementLogEntry`（evict/loaded/dirty）+ 最近 20 条、1024 环形上限 | `TestReplacementLogCap`、`\stats` |
| 9 | 与执行计划对接（物理访问） | `ExecutionEngine → 存储引擎 → 缓冲池 → 磁盘` | SQL DDL/DML/JOIN/索引回归 |
| 10 | 数据持久化（重启不丢） | WAL-before-data + 真持久 fsync + 两阶段恢复 | ACID 恢复用例（test 49）跨重启 |
| 11 | 管理空闲页列表（扩展/回收） | `free_pages_` 会话内复用 + `<db>.fpl` 位图跨重启持久化（magic/版本/页数校验防误复用） | `TestFreePagePersistence` |
| 12 | 错误处理：语法/语义/IO 错误清晰反馈 | `ExecuteSQL` 统一错误通道 + `DiskManager`/块设备异常上抛 | 负面用例 + 故障注入 |

---

## 4. 扩展项验收（E1–E8 + 支撑改进）

| 编号 | 扩展项 | 实现要点 | 单测证据 |
|------|--------|----------|----------|
| **D5** | 页 CRC32（`<db>.crc` 旁路） | 无头、每页 4 字节 CRC；惰性创建；写路径只 flush、组提交 Sync 统一落盘；读回校验拦截静默损坏 | `TestPageCrc` |
| **D7** | 缓冲池全局锁 `latch_` | 恒定锁序串行化帧表；内部自调用拆免锁私有辅助防重入死锁；统计改快照返回 | 并发单测 |
| **E1** | Clock 替换算法 | 环形指针 `hand_` + 参考位二次机会，沿 `Replacer` 接入 | `TestClock`、`TestBufferPoolClock` |
| **E2** | 页内内存分配器 | 页内 free-list 堆：first-fit、左右合并、碎片统计；与磁盘 slotted-page 解耦 | `TestPageAllocator` |
| **E3** | 缓冲池统计增强（IO 计数） | DiskManager 物理读/写计数 + `BufferPoolStats::writeback_count`（四条脏回路径） | `TestIOStats` |
| **E4** | 页级读写锁并发 | `Page` 帧 `std::shared_mutex` + `PageRead/WriteGuard` RAII；BPM 四条写回路径加帧读锁防撕裂 | `TestPageRWLock`（撕裂恒 0 + 共享并发 ≥2） |
| **E5** | 后台异步刷脏线程 | 周期刷脏仅写 OS 缓存不 `Sync()` → 组提交 fsync 次数不变；默认 opt-in | `TestBackgroundFlush` |
| **E6** | 缓冲池内存上限可配置 | 字节上限 `FramesForBytes` 换算 + `\stats` 内存统计 + 环境变量配置 | `TestBufferPoolMemory` |
| **E7** | 块设备抽象层 | `BlockDevice` 接口 + `FileBlockDevice` + 故障注入装饰器；与 D5 联动拦截坏块 | `TestBlockDeviceFaultInjection` |
| **E8** | LRU-K 替换算法 | `recent_store_`（<K）/ `historic_store_`（≥K）双集合；抗顺序扫描 | `TestLRUK`（命中率对比胜 LRU） |

实现哲学：全部遵守「沿既有抽象扩展、不改数据文件布局、默认行为逐字节兼容」，并逐项固化在 `OS_Module_Development_Plan.md §11.4`。

---

## 5. 测试与验证结论

### 5.1 存储单元测试

```
build\storage_ut.exe
======== Storage UT ========
checks: 5424   fails: 0
RESULT: PASS
```

覆盖页管理、空闲页持久化、页 CRC、缓冲池并发、LRU/FIFO/Clock/LRU-K 替换、页分配器、IO/内存统计、后台刷脏、块设备故障注入、页级读写锁等全部存储路径。

### 5.2 SQL 功能回归

- 全量 52 个 SQL 用例（DDL / DML / JOIN / 索引 / 系统目录）**exit=0**，其中 45 个非负面用例全部通过；另有 7 个为测试文件预设的语义/负面错误用例，行为符合预期，与存储改动无关。
- CLI 冒烟实测：`\stats` 输出 `buffer memory / hits / dirty writebacks / disk reads·writes / background flush` 等统计行，均在位。

### 5.3 ACID 与崩溃恢复

| 用例 | 验证内容 | 结果 |
|------|----------|------|
| test 49（\crash） | COMMIT 后崩溃，重启 redo 不丢已提交数据 | PASS |
| test 50（\crash_after_undo_steps N） | 回滚途中崩溃，重启动态 undo 幂等 | PASS |

跨重启恢复在 D5 页 CRC、WAL-before-data 与组提交 fsync 下稳定无误报。

### 5.4 兼容性 / 无回归承诺

- 所有扩展默认关闭或与旧路径行为一致：默认 `ReplacementPolicy::LRU`、默认 64 帧、后台刷脏 opt-in、块设备默认 `FileBlockDevice`；
- 全部持久化新增走旁路文件（`.crc` / `.fpl`），**不改既有数据文件与 WAL 格式**，旧库可直接打开；
- 单线程行为逐字节不变（页级锁无争用）。

---

## 6. 已知限制与前瞻

以下为审慎声明的边界，非验收缺陷：

- 并发收益聚焦**缓冲池层**：DB 层事务仍为单连接逐条执行，页级读写锁为多线程访问提供的业务价值需在引入多连接时才充分体现；
- LRU-K 的 `K`、后台刷脏间隔等参数未做全局自动调优，保留为构造/环境变量人工可配；
- 多块大小页面（如混合 4KB/8KB）未纳入，与既定「旁路 + 向后兼容」约束冲突，收益需另行论证。

---

## 7. 验收结论

- ✅ 指导书 **12 项基础需求**全部实现并验证通过；
- ✅ **8 项扩展 + 2 项支撑改进**全部落地，均含独立单测与文档记录；
- ✅ 存储单元测试 **5424 checks / 0 fails**，SQL 回归、ACID 恢复、崩溃恢复全部通过；
- ✅ 无回归、向后兼容，扩展均遵守既有的锁序 / WAL / 旁路持久化不变量。

**评估：存储子系统满足设计目标与指导书验收要求，准予通过。**

---

*本报告数据来源：最新一轮 `build/storage_ut` 实测（2026-09-12）与 `docs/OS_Module_Development_Plan.md §10/§11.4` 实施记录。*