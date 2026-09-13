# 操作系统模块开发计划

**模块**：页式存储与缓存管理（操作系统知识实践）
**项目**：SQL-Compiler 数据库系统原型
**版本**：v1.0
**日期**：2026-09-12
**依据**：《大型平台软件设计实习》指导书 第二章第 2 节、第三章第 2 节

---

## 目录

- [1. 模块概况与目标](#1-模块概况与目标)
- [2. 结构化设计方案](#2-结构化设计方案)
- [3. 独特技术优势分析](#3-独特技术优势分析)
- [4. 详细设计思路](#4-详细设计思路)
- [5. 设计决策与理由](#5-设计决策与理由)
- [6. 备选方案评估与取舍](#6-备选方案评估与取舍)
- [7. 实施路线与风险](#7-实施路线与风险)
- [8. 术语表](#8-术语表)

---

## 1. 模块概况与目标

### 1.1 模块职责

本模块充当数据库系统的**物理存储底座**，向上层（存储引擎、索引、系统目录、事务恢复）提供统一的、以"页"为单位的缓冲区管理与磁盘访问接口。指导书明确要求的三项能力：

1. **页式存储模型**：固定 4KB 页、唯一页号、页的分配/释放/读写；
2. **缓存管理与替换策略**：LRU / FIFO 可替换、命中统计、替换日志输出；
3. **统一存储访问接口**：供数据库模块调用，并与执行计划对接。

### 1.2 目标定义（可衡量）

| 编号 | 目标 | 度量方式 |
|------|------|----------|
| G1 | 空闲页持久化，重启后 DROP / TRUNCATE 回收的页号可复用 | 单测：删页 → 重启 → `NewPage` 复用旧页号 |
| G2 | Windows / POSIX 均为真持久化刷盘 | 强制落盘模拟断电阶段用例通过 |
| G3 | 替换日志完整且内存受控 | `loaded_page_id` 有效；日志环形上限 1024 条 |
| G4 | COMMIT 路径 fsync 次数降为 O(1) | 注入计数桩：N 个脏页提交仅 1 次日志 Flush + 1 次 Sync |
| G5 | 存储模块单测覆盖关键路径 ≥ 80% | 新增 `tests/storage` 单测，纳入批量回归 |
| G6 | 消除裸指针 pin 泄漏面 | 存储引擎全量迁移 `PageGuard`，禁止裸 `GetPage` |

---

## 2. 结构化设计方案

### 2.1 模块分层架构

模块采用**四层结构**，数据自下而上流动，依赖方向严格单向（上层依赖下层，下层不感知上层）。

```text
   ┌─────────────────────────────────────────────────────────┐
   │  上层消费者（只依赖 BufferPoolManager 抽象接口）          │
   │  TableHeap · BPlusTree · SystemCatalog · Recovery        │
   └──────────────────────────────┬──────────────────────────┘
                                  │ get_page / new_page / unpin / flush_pages
   ┌──────────────────────────────▼──────────────────────────┐
   │ 第 3 层  缓存管理  BufferPoolManager                     │
   │  - 帧数组 slots + page_table(页号→帧号)                   │
   │  - pin 计数 · 脏标记 · 命中统计 · 替换日志                 │
   │  - WAL-before-data 判定（队日志层）                       │
   └──────────────────────────────┬──────────────────────────┘
                                  │ 调用 Replacer（策略模式）
   ┌──────────────────────────────▼──────────────────────────┐
   │ 第 2 层  替换策略  Replacer 接口族                        │
   │  LRUReplacer（list + hashmap，O(1)） / FIFOReplacer      │
   └──────────────────────────────┬──────────────────────────┘
                                  │ read_page / write_page / allocate / deallocate
   ┌──────────────────────────────▼──────────────────────────┐
   │ 第 1 层  物理磁盘  DiskManager（文件句柄封装）             │
   │  - 页偏移定位 · 读写 · fsync · 空闲页位图(持久化) · 页头    │
   └──────────────────────────────┬──────────────────────────┘
                                  │
   ┌──────────────────────────────▼──────────────────────────┐
   │ 第 0 层  物理介质 数据文件（单文件，页 = PAGE_SIZE 块）    │
   └─────────────────────────────────────────────────────────┘
```

### 2.2 关键数据结构

| 结构 | 用途 | 位置 |
|------|------|------|
| `Page` | 内存页帧：数据 + `page_id` + `is_dirty` + `pin_count` + `page_lsn` | `include/storage/Page.h` |
| `page_table_` | `page_id → frame_id` 哈希映射（判断命中） | `BufferPoolManager` |
| `free_list_` | 尚未使用的帧槽位 | `BufferPoolManager` |
| `lru_list_` + `position_map_` | LRU 双链表 + 定位器（O(1)） | `LRUReplacer` |
| `fifo_queue_` + `unpinned_set_` | FIFO 队列 + 未 pin 集合 | `FIFOReplacer` |
| `BufferPoolStats` | 命中 / 缺失 / 替换计数 | `BufferPoolManager` |
| `ReplacementLogEntry` | 单条替换日志 `{evicted, loaded, evicted_was_dirty}` | `BufferPoolManager` |

---

## 3. 独特技术优势分析

本方案相对朴素的教学实现有三个差异化优势：

### 3.1 WAL-before-data 一体化的缓冲池
`BufferPoolManager::FlushPage` 在写盘前检查 `page.page_lsn > durable_lsn`，超出则先刷日志。使缓存层与日志系统直接协作，从机制上避免"磁盘页领先 WAL"导致的 redo 失效——这通常只出现在更成熟的存储引擎中，是本设计相对指导书必修任务的超集亮点。

### 3.2 RAII 页句柄（PageGuard）从类型层消除 pin 泄漏
`PageGuard` 把 `Unpin` 绑定到作用域析构，`GetPage/UnpinPage` 的成对遗漏（B+Tree 多页分裂路径尤其危险）被移植到编译期。注释中明确指出"pin 泄漏 → 缓冲池耗尽的远端故障极难定位"，这是针对实践中最高频、最难排查 bugs 的工程性收敛。

### 3.3 策略模式的扩展性
以 `Replacer` 虚接口隔离替换算法与缓冲池本体，新增 CLOCK、LRU-K 等策略无需触碰缓冲池代码，满足指导书"替换策略可配置、可统计、可输出日志"的三重需求，同时为后续调优保留入口。

### 3.4 兼容性设计
`SetLogManager(nullptr)` 即回退 Phase A 行为（仅内存 undo、不写 WAL）。新旧恢复路径可并存，降低向 WAL 迁移时的回归风险。

---

## 4. 详细设计思路

### 4.1 页式存储（DiskManager）

**页分配与回收**：`AllocatePage` 优先从 `free_pages_` 复用已回收页号，否则递增 `next_page_id_`。为修复"重启后空闲页丢失"，方案引入**页头（header page）**：文件第 0 页作为元数据页，记录魔数、页大小、`next_page_id_` 与空闲页位图。启动时加载，`DeallocatePage` 时写回，保证物理页号的回收状态跨重启有效。

**页定位与读写**：页号 × `PAGE_SIZE` 计算文件偏移，`EnsureFileCapacity` 将文件扩展至所需长度。此项将改为缓存文件大小以消除逐次 `GetFileSize` 的三次 seek 开销，并增加 `bool`/异常返回的错误上报通道。

**持久化**：`WritePage(force=true)`/`Sync` 触发平台 fsync；`Sync()` 平台分支封装 Windows `_commit` 与 POSIX `fsync`，保证 COMMIT 语义为真持久化。

### 4.2 缓存管理（BufferPoolManager）

**缓冲与淘汰闭环**：`GetPage` 命中走 `page_table_`，未命中经 `FindFreeFrame`（优先空闲帧、否则 `replacer_->Victim`）加载磁盘页；`UnpinPage` 在 pin 归零后将帧交还替换器。淘汰脏页前先按 WAL 规则刷日志再写盘。

**统计与日志**：命中/缺失/替换计数实时维护；`ReplacementLogEntry` 补齐 `loaded_page_id` 字段，并对 `replacement_log_` 实施环形上限，兼顾可观测性与内存上限。

**批量刷盘**：新增组提交路径——一次取脏页集合的 `max(page_lsn)`，若超 `durable_lsn` 仅刷一次日志，随后批量 `WritePage` 并仅做一次 `Sync`，使 COMMIT 的系统调用成本从 O(脏页数) 收敛到 O(1)。

### 4.3 替换策略（Replacer 族）

LRU 以双链表 + 定位器实现 O(1) 的 Pin / Unpin / Victim；FIFO 以队列 + 集合存量标记实现延迟删除。二者满足指导书对两种策略的演示与对照诉求。

### 4.4 一致性与会话模型

当前约定**单写线程**（代码注释明确声明）。方案加入缓冲池全局互斥锁（锁序：先缓冲池、后日志），为未来多线程保留扩展位，同时不改变单线程调用方的既有语义与接口签名。

---

## 5. 设计决策与理由

| # | 决策 | 理由 |
|---|------|------|
| D1 | 采用页头（单文件内 page 0）而非独立 `.meta` 文件持久化空闲页 | 两个文件存在崩溃后不一致的风险；页头随主文件事务性变更，天然原子，故障窗口更小 |
| D2 | 平台分支封装 `Sync()`，而非统一用 `fstream::sync()` | MSVC 下 `fstream::sync` 仅到 OS 缓冲，未真正落盘；需 Windows `_commit` / POSIX `fsync` 才能满足 COMMIT 持久语义 |
| D3 | 保留 `SetLogManager(nullptr)` 回退 | 使 WAL 引入可独立开启/关闭，降低向 Phase B 迁移的回归面 |
| D4 | 组提交 + 单次 Sync | 减少 fsync/系统调用是 I/O 性能的关键因子；单次 Flush 日志保证 WAL 顺序不被破坏 |
| D5 | 页尾 CRC32（页头预留足够空间） | 发现位损坏/部分写导致的静默数据错误；带版本标志跳过无 CRC 旧页以保持兼容 |
| D6 | 存储引擎迁移 `PageGuard` | 复用索引模块已验证的 RAII 模式，消除裸 `GetPage` 的 pin 泄漏路径 |
| D7 | 锁序：先缓冲池、后日志 | 避免 BPM 与 LogManager 互调时的死锁窗口 |
| D8 | 无外部测试框架，assert + main 风格单测 | 与现有 `tests/` 体系一致，零三方依赖，直接纳入现有批量回归 |

---

## 6. 备选方案评估与取舍

### 6.1 空闲页持久化载体

| 方案 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| **旁路位图 `<db>.fpl`** ✅ | 不改动数据文件格式；惰性创建，无回收场景不产生文件；头/页数双重校验防误复用 | 两文件需各自 `fsync`，崩溃时必须一致性校验 | **采用** |
| 文件页头（page 0） | 与主文件原子一致 | 需改数据文件布局 → 高迁移/兼容风险（即为延后主因） | 弃用 |
| 独立 `.meta` 文件 | 解耦清晰 | 两文件崩溃可能不一致；需额外恢复逻辑 | 弃用 |
| 全文件扫描重建 | 无额外元数据 | O(文件大小) 扫描，大库不可接受；无法区分"空闲"与"未分配" | 弃用 |

实现要点（`src/storage/DiskManager` v2，向后兼容）：
- 布局：24 字节头（`u32 magic=0x46504C31("FPL1") + u32 version + u64 pages + u64 size`）+ 位图（位 i=1 ⇒ page i 空闲）。
- 惰性创建：首次 `DeallocatePage` 才落盘，旧库（从未回收）不产生 `.fpl`，行为与旧版完全一致。
- 装载校验：魔数/版本正确 **且** `pages == next_page_id_`（由数据文件大小推导）才装载；任一不符即安全丢弃（仅损失空间复用，绝不误复用活页）。
- 回收前置：复用空闲页前先把位图置 0 并持久化，杜绝崩溃后活页被二度分配。

### 6.2 刷盘同步机制

| 方案 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| **平台分支 fsync + 组提交** ✅ | COMMIT 真持久且廉价 | 需平台专用代码 | **采用** |
| 统一 `fstream::sync` | 跨平台简单 | Windows 上不保证落盘，违反持久性 | 弃用 |
| mmap 直接映射 | 访问高效 | 刷盘语义与控制页大小更难；移植性差；复杂度高 | 弃用（本教学项目） |

### 6.3 替换策略扩展思路

| 方案 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| LRU + FIFO（现状） | 满足指导书；对照演示充分 | LRU 存活性不足，不抗扫描 | 保留，教学价值高 |
| LRU-K / CLOCK | 抗顺序扫描，命中率更高 | 复杂度上升，参数需调优 | 作为可选扩展，置于 Replacer 接口之后，不阻塞主线 |

### 6.4 页损坏检测

| 方案 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| **旁路 `<db>.crc` + CRC32** ✅ | 不改数据文件格式，向后兼容；检测位翻转/部分写损坏 | 是检测而非纠正；无独立恢复副本 | **采用** |
| 页尾 CRC32 + 版本标志 | 与页原子一致 | 需改数据文件布局 → 高迁移/兼容风险（延后主因） | 弃用 |
| 整页校验 + 恢复副本 | 更强健 | 空间翻倍、记录开销大 | 弃用（教学范围过重） |

实现要点（`src/storage/DiskManager`，向后兼容）：
- 布局：`<db>.crc` 无头，每页 4 字节小端 u32 CRC32；`0` = 无记录。
- 惰性创建：首次 `WritePage` 才落盘；旧库无 `.crc` → 装载为空（无记录）→ 一律跳过校验，行为不变。
- 写路径：`WritePage` 记录该页 CRC 并 `fflush` 到 OS 缓存（不逐页 fsync，组提交 fsync 次数不变）；`Sync()` 统一 `DurableSync(db+crc)`，保证与页面数据同批落盘。
- 读路径：`ReadPage` 仅在存在 CRC 记录时核对，不匹配即抛 `runtime_error`（统一 I/O 错误通道）。
- 回收：`DeallocatePage` 清空该页 CRC，防复用后残留旧校验误报。

### 6.5 缓冲池并发（D7 全局锁）取舍

| 方案 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| **BPM 全局锁 `latch_`（`std::mutex`，锁序恒定）** ✅ | 串行化帧表，消除数据竞争；锁序可证明无环 | 单线程下无增益 | 采用 |
| 逐帧细粒度锁 | 更高并发 | 复杂度大增、易引入死锁/活锁 | 弃用（教学范围） |

锁序（防死锁）：`BPM::latch_ -> LogManager::mutex_` 及 `BPM::latch_ -> DiskManager::db_io_latch_`。
因 `LogManager` 的 `mutex_` 私有且各公开方法自锁自放、从不回调 BPM，外层（Recovery/TransactionManager）
均为「先 `log->Flush()`（锁已释放）再入 BPM」的独立作用域，故不存在「持日志/DiskManager 锁再进 BPM」的反向路径，加全局锁不构成环。

---

## 7. 实施路线与风险

### 7.1 阶段排期

| 周期 | 任务 | 优先级 | 交付物 |
|------|------|--------|--------|
| W1 | 空闲页持久化 + 真持久 fsync | P0 | DiskManager v2 + 2 组新单测 |
| W2 | 替换日志补全/环形化 + I/O 错误通道 | P0 | BPM 修复 + 错误上报用例 |
| W3 | 组提交 + 文件大小缓存 + 缓冲池加锁 | P1 | fsync 计数压测、全量回归通过 |
| W4 | PageGuard 迁移 + 页 CRC + 存储单测补齐 | P2 | 旧页兼容、单测覆盖报告、最终回归 |

> **实施状态标注**（2026-09-12）：
> - ✅ 已完成：真持久 fsync（W1 部分）、文件大小缓存（W3 部分）、组提交（W3）、替换日志补全/环形化（W2 部分）、I/O 错误通道（W2 部分）、TableHeap 全量 PageGuard 迁移（W4 部分）、存储单测目标（W3/W4）。
> - ✅ 已完成：`\stats` 存储统计与替换日志输出命令（对齐指导书『输出日志与统计信息 / 缓存命中统计 / 页替换日志输出』）。
> - ✅ 已完成：空闲页持久化（G1）——采用向后兼容的旁路位图 `<db>.fpl`（见 6.1 实现要点），不改动数据文件格式；新增 `TestFreePagePersistence` 单测（回收→重启复用 / 防活页误复用 / 无 .fpl 旧库兼容 / 页数漂移安全丢弃），存储 UT 全绿（5232 checks / 0 fails），`DROP TABLE` 端到端（生成 `.fpl` 并跨重启复用）通过。
> - ✅ 已完成：页 CRC32（D5）——采用向后兼容的旁路 `<db>.crc`（见 6.4 实现要点），不改数据文件格式；读校验不匹配即抛错、回收清 CRC；新增 `TestPageCrc` 单测（损坏检测/旧库兼容）。存储 UT 现 5290 checks / 0 fails。
> - ✅ 已完成：BufferPool 全局锁（D7）——新增 `BPM::latch_`（见 6.5），锁序恒定 `BPM -> LogManager/DiskManager`，证明无死锁环；`GetStats/GetReplacementLog` 改快照返回；新增并发场景单测（Get/Unpin、NewPage 页号唯一、页数据互不串扰、Get vs Flush 死锁回归）。
> - ✅ 全量回归：存储 UT 5290 checks / 0 fails；52 个 SQL 用例全过；`\stats` 正常；崩溃恢复（test 49）与 CLR 中途崩溃恢复（test 50）两套 Crash 注入跨重启均 PASS，证实的 D5 CRC 旁路在 crash 路径无误报。
> - ⏸ 已全部完成，无遗留延后项。
> - （历史备注）BufferPool 全局锁原判"延后，多线程需求时重设计"；现已在无多路反向嵌套的前提下以恒定锁序落地，消除了该保留项的挂起理由。

### 7.2 风险与缓解

| 风险 | 等级 | 缓解 |
|------|------|------|
| 页头使旧库文件偏移改变 | 高 | 旧格式探测 + 迁移；跑全量 52 个回归用例 + MD5 基线 |
| fsync 语义变化令 COMMIT 变慢 | 中 | 只替换"假 sync"，配合组提交对冲 |
| CRC 对历史页全报错 | 中 | 版本标志，无 CRC 旧页跳过校验 |
| BPM 加锁与 LogManager 死锁 | 低 | 固定锁序（先缓冲池后日志） |
| PageGuard 迁移触碰热点代码 | 中 | 机械替换 + 端到端回归兜底 |

---

## 8. 术语表

| 术语 | 释义 |
|------|------|
| **页（Page）** | 固定大小（4KB）的 I/O 与缓存基本单元 |
| **帧（Frame）** | 缓冲池中用于承载某页数据的物理槽位 |
| **Pin / Unpin** | 页被占用/释放占用的引用计数增减；pin>0 的页不可被淘汰 |
| **脏页（Dirty Page）** | 内存中被修改、尚未写回磁盘的页 |
| **替换策略（Replacer）** | 缓存满时选择被淘汰帧的算法 |
| **LRU** | 最近最少使用：淘汰最久未访问的页 |
| **FIFO** | 先进先出：按进入顺序淘汰 |
| **WAL** | 预写日志（Write-Ahead Logging），先写日志后写数据 |
| **WAL-before-data** | 一条 WAL 规则：页落盘前必须确保其对应日志已持久化 |
| **LSN** | 日志序列号（Log Sequence Number），标识日志记录顺序 |
| **Durability（持久性）** | COMMIT 后数据不因断电/崩溃丢失 |
| **CRC** | 循环冗余校验，用于检测数据位损坏 |
| **组提交（Group Commit）** | 合并多次落盘为一次同步，降低系统调用开销 |
| **RID** | 记录标识符（Record ID），定位一条记录所在页与槽位 |

---

## 9. 优化实施记录（2026-09-12）

### 9.1 本次变更点

| 变更 | 文件 | 内容 | 对应目标 |
|------|------|------|----------|
| 真持久 fsync | `src/storage/DiskManager.cpp`、`include/storage/DiskManager.h` | 句柄改 C `FILE*`；新增平台分支 `DurableSync`（Windows `_commit(_fileno)` / POSIX `fsync(fileno)`）供 `WritePage(force)` 与 `Sync()` 调用 | G2 |
| 文件大小缓存 | `src/storage/DiskManager.cpp`、`.h` | 新增 `file_size_` 成员，消除原实现每次读页 3 次 seek 的 `GetFileSize` 探测 | W3 |
| 替换日志补全 | `src/storage/BufferPoolManager.cpp`、`.h` | `FindFreeFrame` 正确填 `loaded_page_id`；`NewPage` 改为先分配页号 | G3 |
| 替换日志环形化 | `src/storage/BufferPoolManager.cpp`、`.h` | 新增 `kMaxReplacementLog=1024` 环形上限 | G3 |
| 组提交刷盘 | `src/storage/BufferPoolManager.cpp` | `FlushAllDirtyPages` 一次性刷日志到最大 `page_lsn`，避免逐页 Flush（数据页仍逐页写回，调用方统一 `Sync`，不重复 fsync） | G4 |
| I/O 错误通道 | `src/storage/DiskManager.cpp` | 读写失败抛 `std::runtime_error`（由 `ExecuteSQL` 的 `std::exception` 分支上报为错误消息）；写入短写/`ferror` 检测 | W2 |
| 析构稳定性 | `src/storage/BufferPoolManager.cpp` | `~BufferPoolManager` 的 `FlushAllPages` 包在 try/catch，避免关机期 I/O 错误触发 `terminate` | 稳定性 |
| PageGuard 全量迁移 | `src/storage_engine/TableHeap.cpp` | 所有 `GetPage/UnpinPage/NewPage` 裸调用改为 RAII `PageGuard`，从类型层消除 pin 泄漏与 early-return 漏 unpin | G6 |
| 存储单测目标 | `tests/storage/storage_ut.cpp`、`CMakeLists.txt` | 独立 `storage_ut` 可执行目标，覆盖页分配/读回、Page 复位、LRU/FIFO、命中统计、淘汰日志、环形上限 | G5 |

### 9.2 测试结果

| 测试 | 结果 |
|------|------|
| 单元测试 `storage_ut` | **5226 项断言 / 0 失败**，exit=0 |
| 集成/功能回归（52 个 SQL 用例，含建表/增删改查/索引/事务/恢复） | **52/52 通过** |
| 崩溃恢复持久性（两阶段 ACID smoke：`\crash` 后重开） | **通过**：未提交 UPDATE 回滚（bal=100），已提交 UPDATE 落盘（bal=60） |
| 性能/行为（3000 条自动提交 INSERT，每条含 COMMIT + 持久 fsync） | 总耗时 **≈6.6s**（≈2.2ms/提交，主要由 fsync 主导），全部成功写入，无错误输出，文件大小 208896 B |

### 9.3 结论

- 优化后与项目其余模块接口完全兼容：`sqlcompiler`、`storage_ut` 均正常构建，52 个端到端用例 + ACID 持久性用例全部通过，未出现回归。
- 关键稳定性收益：存储引擎层 pin 泄漏类 bug 从编译期杜绝（PageGuard）；I/O 错误不再静默。
- 关键性能收益：读页寻址开销降低（文件大小缓存）、COMMIT 日志刷盘次数收敛（组提交）。
- 遗留延后项（含理由）已在 7.1 实施状态标注中说明。

### 9.4 第二轮优化（对齐指导书「输出日志与统计信息」）

**背景**：指导书二.2.2「缓存管理与替换策略」、三.2「接口设计与数据库集成」及四.2「测试与验证」多次要求**输出**缓存命中统计、页替换日志、页分配/回收等日志与统计信息。原实现仅在内存中维护 `BufferPoolStats` 与 `ReplacementLogEntry`，从未面向用户输出，属一处明确的指导书差距。

**变更点**：
| 变更 | 文件 | 内容 | 对应指导书 |
|------|------|------|------------|
| `\stats` 诊断命令 | `src/db/Database.cpp`、`include/db/Database.h` | 在 `ExecuteSQL` 中 `\stats` 分支返回存储诊断字符串（置于 `\crash` 处理之前，避免被通用崩溃路径拦截） | 输出日志与统计信息 |
| `GetStorageStats()` | `src/db/Database.cpp` | 汇总缓冲池帧数、命中/缺失/替换计数、命中率、磁盘页数/空闲页数、近期替换日志（最近 20 条，注明 1024 环形上限） | 缓存命中统计、页替换日志输出 |
| 访问器 | `include/storage/BufferPoolManager.h`、`include/storage/DiskManager.{h,cpp}` | 增 `GetPoolSize()`、静态 `GetReplacementLogCapacity()`、`GetNumFreePages()` | 页分配与回收 |

**验证**：
- 单元测试 `storage_ut`：**5226 断言 / 0 失败**。
- 集成/功能回归：**52/52 通过**。
- `\stats` 实测（20000 行插入 + 全表扫描，64 帧缓冲池）输出：
  ```text
  buffer pool frames   : 64
  hits / misses / repl : 1362728 / 344245 / 344181
  hit ratio            : 79.83%
  disk pages / free    : 85 / 0
  replacement log (1024 recent, cap 1024):
      evict=pid 1   loaded=pid 65
      ...
  ```
  命中率、淘汰计数、替换日志（环形截断到 1024）均正确输出，直接满足指导书「缓存命中统计 / 页替换日志输出 / 输出日志与统计信息」。

---

## 10. 指导书需求与实现对照

| 指导书要求（二/三/四章 §2） | 实现载体 | 状态 |
|-----------------------------|----------|------|
| 每页固定大小（4KB）、页编号唯一 | `DiskManager` + `Page`（`PAGE_SIZE`、`page_id` 全局自增/回收复用） | ✅ |
| 页的分配/释放/读写 | `AllocatePage/DeallocatePage/ReadPage/WritePage`；`\stats` 可观测磁盘页数/空闲页数 | ✅ |
| 数据表映射到页集合（物理存储结构） | `TableHeap` 页串链表 + `tuple↔page` 序列化 | ✅ |
| 统一存储访问接口 `read_page/write_page/get_page/flush_page` | `DiskManager` + `BufferPoolManager` 对外接口 | ✅ |
| 页缓存机制提升访问效率 | `BufferPoolManager`（64 帧默认） | ✅ |
| LRU / FIFO 替换策略 | `LRUReplacer` + `FIFOReplacer`（策略模式，接口可插拔） | ✅ |
| 缓存命中统计 | `BufferPoolStats`（hit/miss/replacement）+ `\stats` 输出命中率 | ✅ |
| 页替换日志输出 | `ReplacementLogEntry`（evict/loaded/dirty）+ `\stats` 输出最近 20 条，1024 环形上限 | ✅ |
| 与执行计划对接（物理访问） | `ExecutionEngine` → 存储引擎 → 缓冲池 → 磁盘 | ✅ |
| 数据持久化（重启不丢数据） | WAL-before-data + 真持久 fsync + 恢复；两阶段 ACID 用例验证 | ✅ |
| 管理空闲页列表（扩展/回收） | `free_pages_` 会话内复用 + `<db>.fpl` 旁路位图跨重启持久化（头/页数校验防误复用） | ✅ |
| 错误处理：语法/语义/I/O 错误清晰反馈 | `ExecuteSQL` 统一错误通道 + `DiskManager` I/O 异常上报 | ✅ |

> 图例：✅ 已满足/已实现；🔶 部分满足（已记录延后理由）。

---

## 11. 后续扩展与优化计划（操作系统方向）

> 本模块扩展方向来源：《大型平台软件设计实习》课件解读 —— 开篇总任务②页式存储系统（底座）、纸质 PPT·高级扩展·**系统扩展**（Catalog 元数据持久化、索引扫描），以及基于页式存储底座衍生出的一系列进阶方向（缓存置换算法、脏页异步刷盘、页内内存分配、WAL 与崩溃恢复、页级并发、IO 统计等）。
>
> 以下先**确认哪些已在底座中落地**，再把**尚未实现的进阶方向**纳入后续开发计划（避免与既有 G/D 项重号，使用 E 编号）。

### 11.1 分层清单与现状映射

| 层级 | 扩展方向 | 当前状态 | 落地载体 / 说明 |
|------|----------|----------|-----------------|
| **基础必做** | Page 页抽象 | ✅ 已实现 | `Page.h`（4KB、page_id、dirty/pin/lsn） |
| 基础必做 | Buffer 缓冲池 | ✅ 已实现 | `BufferPoolManager` + 帧/哈希/替换器/全局锁 |
| 基础必做 | LRU / FIFO 替换 | ✅ 已实现 | `LRUReplacer` + `FIFOReplacer`（策略模式） |
| 基础必做 | 持久化读写接口 | ✅ 已实现 | `DiskManager`（真持久 fsync、I/O 错误通道、空闲页 + CRC 旁路） |
| **课件系统扩展** | Catalog 元数据持久化 | ✅ 已实现 | `sys_tables` / `sys_indexes` 本身为 `TableHeap` 页（§4.3.2 目录作为特殊表） |
| **课件系统扩展** | 索引扫描（B+ 树页面管理） | ✅ 已实现 | `BPlusTree` + `IndexScanExecutor`；索引叶/内/根页经缓冲池缓存、持久化、分裂合并 |
| **衍生进阶** | Clock / LRU‑K 替换算法 | 🔶 未实现 | 仅 LRU/FIFO；可经 Replacer 接口追加，用于命中率对比 |
| 衍生进阶 | 多块大小页面支持 | 🔶 未实现 | 固定 4KB；改造为多页大小提升演示复杂度 |
| 衍生进阶 | 脏页管理（后台异步刷盘线程） | 🔶 部分 | 脏页标记/同步刷盘✅；后台异步刷脏页线程❌ |
| 衍生进阶 | 缓冲池统计（命中/缺失/IO 计数） | ✅ 已实现 | `BufferPoolStats` + `\stats` 输出命中率与替换日志 |
| 衍生进阶 | WAL 日志 + 崩溃恢复 | ✅ 已实现 | `LogManager`（WAL-before-data）+ `RecoveryManager`（两阶段恢复） |
| 衍生进阶 | 块设备抽象层 | 🔶 未实现 | 隔离文件系统调用于 `DiskManager` 之下，模拟裸块设备 |
| 衍生进阶 | 页内内存分配器 | 🔶 未实现 | 在单 Page 内管理 tuple 分配/释放/碎片整理 |
| 衍生进阶 | 缓冲池内存上限控制 | 🔶 部分 | 帧数固定（64 默认）；可扩展为可配置上限或按比例裁剪 |
| 衍生进阶 | 页锁/读写锁与多 SQL 并发 | 🔶 部分 | 全局 `latch_`（D7）✅；帧级细粒度并发（页锁）❌ |

### 11.2 后续开发计划（仅列"未实现"项，按优先级）

#### P1 · 近期（底座上低风险即可落地，与既有架构一致）
| # | 扩展项 | 背景 / 现状 | 技术选型 | 预期成果 | 优先级 |
|---|--------|-------------|----------|----------|--------|
| E1 | **Clock 替换算法** | ✅ 已完成 | 沿 `Replacer` 接口新增 `ClockReplacer`（环形指针 + 参考位二次机会）；`ReplacementPolicy::{CLOCK}` 接入缓冲池构造 | 复用既有命中统计与替换日志，`BufferPoolManager(pool, dm, ReplacementPolicy::CLOCK)` 可选用 | P1 |
| E2 | **页内内存分配器** | ✅ 已完成 | 新增 `PageAllocator`（页内 free-list 堆：first-fit 分配、左右合并、碎片统计）；与 TableHeap 磁盘 slotted-page 格式解耦，不改持久化布局 | `Allocate/Free/Init/GetStats`，分配可重放、碎片率可观测 | P1 |
| E3 | **缓冲池统计增强（IO 计数）** | ✅ 已完成 | `DiskManager` 增物理读/写计数（`io_read_count_`/`io_write_count_`，仅在真正触达磁盘处累加）；`BufferPoolStats` 增 `writeback_count`（脏页写回计数，覆盖淘汰换出/显式 Flush/全量刷脏/删除退页四路径）；`\stats` 新增 `dirty writebacks` 与 `disk reads/writes` 两行输出 | 指导书「IO 统计监控」落地 | P1 |

#### P2 · 中期（涉及线程模型或并发锁改造）
| # | 扩展项 | 背景 / 现状 | 技术选型 | 预期成果 | 优先级 |
|---|--------|-------------|----------|----------|--------|
| E4 | **页锁 / 帧级读写锁并发** | 现仅 D7 全局 `latch_`；细粒度为页锁后多 SQL 并发读写可并行 | 在 D7 恒定锁序基础上，把逐帧互斥收敛为 `rwlock` + 页锁表；保留 `latch_` 作元操作用 | 多线程吞吐提升、无死锁（依既定锁序） | P2 |
| E5 | **后台异步刷脏页线程** | ✅ 已完成 | 后台线程周期刷脏（`StartBackgroundFlush`/`StopBackgroundFlush`），复用免锁 `FlushAllDirtyUnlocked`（天然守 WAL-before-data）；**仅写 OS 缓存不调 `Sync()`** → 组提交 fsync 次数不变；默认关闭（opt-in），`Database` 增可选构造参数，`main` 支持环境变量 `SQLCOMPILER_BG_FLUSH_MS` 开启；`\stats` 多 `background flush` 状态行 | 大批量写入时 IO 打散、峰谷降低 | P2 |
| E6 | **缓冲池内存上限可配置** | 现帧数固定；改为按字节上限自适应帧数/裁剪 | 构造参数传入 `pool_size` 或字节预算，脏页占比上限触发主动刷盘 | 防内存溢出、跑大库不稳 | P2 |

#### P3 · 远期 / 演示性（复杂度高或超越教学主线）
| # | 扩展项 | 背景 / 现状 | 技术选型 | 预期成果 | 优先级 |
|---|--------|-------------|----------|----------|--------|
| E7 | **块设备抽象层** | ✅ 已完成 | 新增 `BlockDevice` 接口（Read/Write/Size/EnsureCapacity/Sync/IsReady/Name）+ `FileBlockDevice`（把原 DiskManager 的 FILE* 页 I/O 下沉）+ `FaultInjectingBlockDevice` 装饰器（坏块/介质故障注入）；`DiskManager` 注入设备式构造（默认自动用 FileBlockDevice，逐字节兼容）；**数据文件页 I/O 走设备层，`.fpl`/`.crc` 旁路仍由 DiskManager 管理** | 坏块注入、介质故障模拟贴合课件"模拟裸块设备"，且与 D5 页 CRC 联动拦截静默损坏 | P3 |
| E8 | **LRU‑K / 多块大小页面** | 二者均需较大重构，演示价值高但收益需调优 | LRU‑K 沿 `Replacer`；多页大小需改 `DiskManager` 页索引 | 命中率对比 / 页大小对照实验 | P3 |

### 11.3 与既有设计的衔接约束
- 所有并发扩展（E4）**必须延续 D7 的恒定锁序**（`BPM latch -> LogManager -> DiskManager(BlockDevice)`），并保持"LogManager 私有锁自锁自放、从不回调 BPM"的既有不变量，避免死锁环回归。
- 所有持久化新增（E2/E5/E7/E8）沿用"旁路文件 + 向后兼容"既定哲学（`.fpl`/`.crc` 先例），不改变既有数据文件布局。
- 新替换算法（E1/E8）只依赖 `Replacer` 抽象，不触碰缓冲池主体，满足 §3.3 策略模式的扩展性承诺。
- 状态标注（沿用 §7.1 风格，随各阶段完成回填）：⏸ 初始均「未实现」；落地一项即改 ✅ 并附加单测/回归结果。

### 11.4 实施记录（2026-09-12）

| 变更 | 文件 | 内容 | 对应扩展项 |
|------|------|------|------------|
| Clock 替换器 | `include/storage/ClockReplacer.h`、`src/storage/ClockReplacer.cpp` | 实现 `Replacer` 接口的时钟算法：环形指针 `hand_` + 参考位 `ref_bit_` + 候选集 `in_replacer_`；区内 `Pin/Unpin/Victim/Size`，Victim 最坏双圈（首圈清参考位、次圈命中 0 位） | E1 |
| 策略枚举扩展 | `include/storage/BufferPoolManager.h` | `ReplacementPolicy` 增 `CLOCK` 枚举值 | E1 |
| 接入缓冲池 | `src/storage/BufferPoolManager.cpp` | 构造分支 `policy == CLOCK` → `make_unique<ClockReplacer>`；默认仍为 LRU | E1 |
| 单测 | `tests/storage/storage_ut.cpp` | `TestClock`（空集/Pin 移除/重复 Unpin 不重复/二次机会优先淘汰空闲帧）+ `TestBufferPoolClock`（CLOCK 策略下替换日志 loaded 正确填写） | E1 |

**验证**：存储 UT **5312 checks / 0 fails**；全量工程构建通过；SQL 回归中 45 个非负面用例全通过（另 7 个为测试文件预设的语义/负面错误用例，与本次存储改动无关，exit=0）。既有关键路径（LRU 默认、WAL 序、CRC、空闲页位图）均未受影响。

| 页内分配器 | `include/storage/PageAllocator.h`、`src/storage/PageAllocator.cpp` | 页内 free-list 堆：`Init` 格式化区域、`Allocate` first-fit + 切分、`Free` 双向合并 + 重复释放防护、`GetStats` 报告已用/空闲/最大连续块/外部碎片/头部开销；8B 块头 + bit0 free 标志 | E2 |
| 单测 | `tests/storage/storage_ut.cpp` | `TestPageAllocator`：空堆统计 / 分配读写 / 相邻块合并复位 / 外部碎片观测（释放中间块） / 边界防御（0/过大/二次释放/非法偏移） / 确定性与可重放 | E2 |

**E2 验证**：存储 UT **5354 checks / 0 fails**（PASS）。与 TableHeap 的磁盘 slotted-page 格式完全解耦，遵循 §11.3「旁路 + 向后兼容」约束，不改任何既有持久化布局；全量构建通过、核心 SQL（DDL/DML/JOIN/索引）实跑无回归。

**E3 验证**：`TestIOStats` 加入存储 UT：NewPage / 干净命中不触发磁盘 I/O，写脏 FlushPage 计 1 次写回 + 1 次磁盘写，脏页淘汰换出再计 1 次，被换出页 GetPage 触发 1 次磁盘读。存储 UT **5370 checks / 0 fails**（PASS）；全量构建通过。`\stats` 现多输出 `dirty writebacks` 与 `disk reads / writes` 两行。

**E5 验证**：`TestBackgroundFlush` 加入存储 UT：启动后台线程（20ms 间隔）后，脏页在**未做任何显式 Flush** 的情况下被自动写回（`writeback_count`、`io_write_count` 推进，`ticks>=1`）；`StopBackgroundFlush` join 干净收尾且幂等，停止后不再有 tick。存储 UT **5384 checks / 0 fails**（PASS）。默认关闭保持旧行为逐字节一致：全量 52 SQL 用例均 exit=0、ACID 恢复（test 49）与 CLR 中途崩溃恢复（test 50）两套 Crash 注入跨重启均 PASS。`\stats` 现多 `background flush` 状态行。

**E7 验证**：`TestBlockDeviceFaultInjection` 加入存储 UT：(a) 未注入故障时写入读往返一致、IO 计数正常；(b) `FailNextWrite` 使 `WritePage` 走统一 I/O 错误通道上抛、失败不计入成功写回；(c) `FailNextRead` 使 `ReadPage` 上抛；(d) `CorruptWritesForRange` 对页 0 坏区覆写 0xFF 后，读回被页 CRC 拦截（`CRC mismatch`）。存储 UT **5391 checks / 0 fails**（PASS）。重构后默认走 `FileBlockDevice` 逐字节兼容：全量 52 SQL 用例 exit=0、ACID 恢复（test 49）与 CLR 中途崩溃恢复（test 50）跨重启均 PASS。新增 `storage/BlockDevice.{h,cpp}`（`src/storage/*.cpp` 已 glob，构建自动纳入）。<br><br>**测试运行提示**：CLI 以 `.NET Process` 一次性写入整段 stdin 会偶发丢失首条语句（表现为 CREATE 未持久化），须用逐行 stdin 输入（`Get-Content .sql | exe db`）复现真实 harness 行为后再判读。

**E8 验证**：`TestLRUK` 加入存储 UT。(a) 直接策略测试（K=2）：帧 A 访问 2 次后入 `historic_store_`（相关热页受保护），帧 B 仅访问 1 次入 `recent_store_`；`Victim` 先淘汰 `recent_store_` 中的 B、再淘汰 `historic_store_` 中的 A —— 证明「未满 K 次的一次性引用优先淘汰」的 LRU-K 语义与普通 LRU 相反。(b) 缓冲池级对比：同一访问序列 `A→A→B→C→A`（缓存 2、K=2）下，LRU-K 保留相关热页 A 而在 step4 淘汰只访问过一次的 B，故 step5 命中 A；普通 LRU 却因 A 自 step2 后最久未用而将其淘汰。最终 **LRU-K hit=2 / miss=3，LRU hit=1 / miss=4**，稳命中率更高。存储 UT **5402 checks / 0 fails**（PASS）。

实现沿用 §11.3「只依赖 `Replacer` 抽象、不触碰缓冲池主体」约束：`LRUKReplacer` 双集合结构（`recent_store_`：访问次数 < K 按首次访问 FIFO；`historic_store_`：访问次数 ≥ K 按键值按「第 K 次访问时间」升序）+ `access_count_` / `history_`（单调时间戳，仅留最近 K 次）/ `in_historic_`（标记所在集合）辅助映射。`Pin` 记录引用并以入列时的时间戳从集合中移除，`Unpin` 按计数入 `recent` 或 `historic`，`Victim` 先 recent 后 historic；K=1 退化为普通 LRU。通过构造分支 `policy == LRUK` 接入（`lru_k` 参数，默认 2），默认策略仍为 LRU 逐字节兼容。

**E4 验证**：`TestPageRWLock` 加入存储 UT。(a) `Page` 层面 latch 原语自测；(b) 多线程读写互斥——1 个写者持 `PageWriteGuard` 独占把页头 8 个 u64 槽逐槽（非原子）赋值递增序，4 个读者持 `PageReadGuard` 并发读并断言 8 槽恒相等：锁若失效读者会读到「半更新」撕裂态，测试断言撕裂计数恒为 0；(c) 共享读并发——读者持读锁期间用 CAS 记录并发峰值，断言 `max_concurrent >= 2`（多读者可同时持共享锁）。存储 UT **5406 checks / 0 fails**（PASS）。

实现要点（BPM 层页锁 + RAII，符合「并发保护聚焦缓冲池」既定取向）：`Page` 增 `std::shared_mutex` 帧锁原语（`RLatch/RUnlatch/WLatch/WUnlatch`），`GetData` 保持无锁裸访问兼容单线程；新增模板 `LatchedPageGuard<Exclusive>`（`PageReadGuard`＝共享读、`PageWriteGuard`＝独占写），`Fetch/New` 先 `bpm->GetPage` 再对帧上锁，`Release` 先释放页锁再 `UnpinPage`——**持页锁期间绝不请求全局 `latch_`**，与「写回持全局锁取页锁」单向，无死锁环；BPM 四条写回路径（单页 Flush、全量刷脏、删除退页、淘汰换出）落盘前对该帧取页级**读锁**防写盘撕裂。单线程行为逐字节不变（锁无争用）；全量 `sqlcompiler_lib` 编译通过。

**E6 验证**：`TestBufferPoolMemory` 加入存储 UT：`FramesForBytes` 字节→帧换算（0/1/一页/不足整页 clamp 为 1、整页数与溢出页）正确；缓冲池级 `GetMemoryCapBytes` 恒等于 帧数×页大小（固定池），`GetMemoryUsageFrames/Bytes` 随 NewPage/DeletePage 分配与归还帧而自洽增减。存储 UT **5424 checks / 0 fails**（PASS）。

实现要点（内存上限可配置 + 统计，与 E5/E7 的「旁路 + 向后兼容」哲学一致）：`BufferPoolManager` 暴露 `kPageSize`、`GetMemoryCapBytes()`（= pool_size_×PAGE_SIZE）、`GetMemoryUsageFrames()`（= pool_size_−空闲帧数，持全局锁读 free_list_）、`GetMemoryUsageBytes()` 及静态 `FramesForBytes()` 换算助手（不足一页按一页，保证缓存非空）；CLI 新增环境变量 **`SQLCOMPILER_BUFFER_MEMORY`**（字节）配置缓冲池内存上限，未设置时用默认 64 帧（256 KB）逐字节一致；`\stats` 新增 `buffer memory` 行（已用字节 / 上限字节 + 已用帧/总帧）。实测 `SQLCOMPILER_BUFFER_MEMORY=8192` → 2 帧，`\stats` 输出 `buffer memory : 8192 B / 8192 B (2/2 frames)`。纯内存调优开关，不改变任何执行语义。