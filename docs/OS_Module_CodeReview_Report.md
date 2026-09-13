# 操作系统模块代码审阅与检索报告

- 项目：小型数据库系统 SQL-Compiler（编译原理 + 操作系统 + 数据库 三模块综合实训）
- 审阅范围：**操作系统模块** = ① 操作系统页面管理子系统（分页 / 缓冲池 / 页面置换 / 磁盘 I/O / 崩溃恢复 / 后台线程）；② 隔离级别并发控制子系统（多粒度锁 / 死锁检测 / 事务 / MVCC）
- 依据代码基线：storage_ut **6982 checks / 0 fails**；SQL 回归 **55 passed / 0 failed**（含崩溃注入）

---

## 一、模块组成（Modular Composition）

操作系统模块由两个子系统、13 个组件构成：

### A. 操作系统页面管理子系统（6 组件）

```
┌──────────────────────────────────────────────────────────────┐
│  Page 抽象层         Page / page_id_t / PageReadGuard /        │
│                      PageWriteGuard（页级读写闩 RAII）          │
│  ─────────────────────────────────────────────────────────────  │
│  内存层              缓冲池 BufferPoolManager（帧 + 替换策略 +   │
│                      latch_ + I/O/内存统计）                    │
│                      页面置换 LRUKReplacer（LRU-K 双集合）       │
│  ─────────────────────────────────────────────────────────────  │
│  设备层              BlockDevice 抽象（File / FaultInjecting）  │
│                      DiskManager（64 位 I/O、fsync、.fpl 空闲页  │
│                      位图、.crc 页校验、磁盘 I/O 锁）            │
│  ─────────────────────────────────────────────────────────────  │
│  恢复层              LogManager（WAL：Redo/Undo/CLR）            │
│  调度层              后台线程（BG_FLUSH 刷脏 / BG_VACUUM 真空，  │
│                      环境变量可配、默认关闭）                    │
│  集成层              OsModuleOptimizations（osopt:: 分阶段优化   │
│                      + Legacy 孪生：CRC32 表 / LRU Unpin /       │
│                      页内分配头读取合并）                        │
└──────────────────────────────────────────────────────────────┘
```

### B. 隔离级别并发控制子系统（7 组件）

```
┌──────────────────────────────────────────────────────────────┐
│  事务模型        Transaction（隔离级别/状态）                    │
│                 TransactionManager（Begin/Commit/Rollback）    │
│                 CommitTracker（CSN + 最老活动快照水位）          │
│  ─────────────────────────────────────────────────────────────  │
│  锁管理          LockManager（多粒度表锁/行锁、死锁 DFS、         │
│                 谓词锁区间继承合并、行锁升级、UnlockAll）         │
│                 执行上下文 ExecutionContext（行读写锁获取、      │
│                 谓词写前检查、行读锁登记/释放）                  │
│  ─────────────────────────────────────────────────────────────  │
│  多版本          TableHeap 版本链（begin/end_xid/end_csn/prev）  │
│                 SetSnapshot / GetTuple 快照可见性 / Vacuum       │
│                 索引 MVCC 精确可见性（延迟摘除 + 键重检 + 去重）  │
│  ─────────────────────────────────────────────────────────────  │
│  执行引擎接线    ExecutionEngine（语句级读锁释放、谓词收集、      │
│                 表锁超时）+ 各扫描/写算子                        │
│  SQL 语法        SET TRANSACTION ISOLATION LEVEL（词法/语法层） │
└──────────────────────────────────────────────────────────────┘
```

---

## 二、文件路径清单（File Inventory）

### A. 操作系统页面管理

| 组件 | 头文件 | 实现 | 功能 |
|---|---|---|---|
| 页抽象 | `include/storage/Page.h` | `src/storage/Page.cpp` | `Page`、`page_id_t`、页头格式 |
| 页级锁 RAII | `include/index/PageGuard.h` | — | `PageReadGuard`/`PageWriteGuard`（锁序约束、防与 BPM 全局锁冲突） |
| 缓冲池 | `include/storage/BufferPoolManager.h` | `src/storage/BufferPoolManager.cpp` | 帧管理、`NewPage/GetPage/UnpinPage/Flush/FlushAll/DeletePage`、`latch_`、`writeback_count/io_read_count/io_write_count/内存 used/cap` |
| LRU-K 置换 | `include/storage/LRUKReplacer.h` | `src/storage/LRUKReplacer.cpp` | `recent_store_`/`historic_store_` 双集合、`Evict/RecordAccess/SetEvictable` |
| 块设备抽象 | `include/storage/BlockDevice.h` | `src/storage/BlockDevice.cpp` | `FileBlockDevice`/`FaultInjectingBlockDevice` |
| 磁盘管理 | `include/storage/DiskManager.h` | `src/storage/DiskManager.cpp` | 64 位定位 `_fseeki64/_ftelli64`、`_commit` 同步、页分配/回收、`db_io_latch_` |
| 空闲页持久化 | （并入 DiskManager） | `src/storage/DiskManager.cpp` | `<db>.fpl`：24 字节头（magic=0x46504C31, version=1, pages, size）+ 位图；`LoadFreePageBitmap/DeallocatePage/AllocatePage`；惰性创建 |
| 页校验 | （并入 DiskManager） | `src/storage/DiskManager.cpp` | `<db>.crc`：无头 4 字节 LE CRC32/页（IEEE 0xEDB88320），0=无记录 |
| WAL 日志 | `include/txn/LogManager.h` | `src/txn/LogManager.cpp` | Redo/Undo/CLR、`LogManager::mutex_`、与 BPM 锁序约束 |
| 分阶段优化 | `include/storage/OsModuleOptimizations.h` | `src/storage/OsModuleOptimizations.cpp` | `osopt::` 命名空间；CRC32 constexpr 表、LRU `Unpin` 单哈希、页内分配头读取合并；`RunBenchmarks`/`TestOptimizations` |
| 后台线程/配置 | `src/main.cpp:L203-L228` | — | 读取 `SQLCOMPILER_BG_FLUSH_MS` / `SQLCOMPILER_BUFFER_MEMORY` / `SQLCOMPILER_BG_VACUUM_MS` |

### B. 隔离级别并发控制

| 组件 | 头文件 | 实现 | 功能 |
|---|---|---|---|
| 事务模型 | `include/txn/Transaction.h` | `src/txn/Transaction.cpp` | `IsolationLevel`（kReadUncommitted/kReadCommitted/kSerializable/kSnapshot）、txn 状态 |
| 事务管理 | `include/txn/TransactionManager.h` | `src/txn/TransactionManager.cpp` | `Begin/Commit/Rollback`、`SetIsolationLevel`、注入 LockManager/CommitTracker |
| 提交跟踪 | `include/txn/CommitTracker.h` | `src/txn/CommitTracker.cpp` | CSN 分配、`LookupCommitted`、`OldestActiveSnapshot`（显式最小 CSN）、活动快照登记 |
| 锁管理器 | `include/storage/LockManager.h` | `src/storage/LockManager.cpp` | 表/行多粒度锁、DFS 死锁检测、`AcquireReadPredicate/CheckWritePredicate`（区间继承合并）、`RegisterRowGroup/CountRowLocks/TryEscalateTable/IsTableEscalated`（升级阈值 128）、`UnlockAll` |
| 执行上下文 | `include/execution/Executor.h`（ExecutionContext） | `src/execution/Executor.cpp` | `AcquireRowReadLock/AcquireRowWriteLock`（kOk/kDeadlock/kTimeout/kWouldBlock）、`CheckSerializablePredicate`、`RowReadLocks` 登记 |
| MVCC 版本链 | `include/storage_engine/TableHeap.h` | `src/storage_engine/TableHeap.cpp` | `SetSnapshot`、`GetTuple`（`VisibleForSnapshot` L101-L117、版本链遍历 L296-L352）、`UpdateTuple` 旧 head 迁移、MVCC 删除、`Vacuum`（end_csn ≤ 最老活动快照） |
| 索引 MVCC | `include/execution/IndexMaintenance.h` | `src/execution/IndexMaintenance.cpp` | `IndexDeleteDeferred`（L29-L35，快照+非唯一延迟摘除）、`IndexKeysEqual`、`InsertIntoIndexes/DeleteFromIndexes/RestoreDeletedIndexEntries`（L119-L156） |
| 索引扫描过滤 | `include/execution/IndexScanExecutor.h` | `src/execution/IndexScanExecutor.cpp` | `InScanBounds` 键重检、`seen_rids_`+`RidHash` 去重、`index_key_columns_` |
| 引擎接线 | `include/execution/ExecutionEngine.h` | `src/execution/ExecutionEngine.cpp` | `ReleaseRowReadLocks`（L44-L69）、`ExecuteSubplan` 隔离注入（L443-L503）、`CollectScanPredicates`、`kIsolationLockWaitMs=5000` |
| 扫描/写算子 | — | `SeqScanExecutor.cpp` / `IndexScanExecutor.cpp` / `DeleteExecutor.cpp` / `UpdateExecutor.cpp` / `UpsertExecutor.cpp` | `SetSnapshot` 前置（非快照复位水位）、快照读免锁、行锁、FCW 挂载顺序、索引接线 |
| 后台真空 | `include/db/Database.h` | `src/db/Database.cpp` | `bg_vacuum_ms`、`GetBackgroundVacuumTicks`、`IsBackgroundVacuumEnabled`、`Shutdown` 幂等 |
| SQL 语法 | `include/token/Token.h`、`include/parser/Parser.h` | `src/parser/Parser.cpp`（L2007-L2034） | `KEYWORD_SNAPSHOT` 等关键字；`SET TRANSACTION ISOLATION LEVEL` 映射 |
| 回归测试 | — | `tests/sql/52_set_isolation.sql` 等 | 隔离级别 SQL 用例 |
| 单元测试 | — | `tests/storage/storage_ut.cpp` | 锁/谓词/升级/MVCC/真空/二级索引用例 |

---

## 三、各代码段功能与实现细节（Segment Details）

### A. OS 页面管理

**1) 页抽象（Page / PageReadGuard / PageWriteGuard）**
- `Page.h`：`Page` 承载一帧数据（`PAGE_SIZE`）；`page_id_t` 为页号类型（`INVALID_PAGE_ID` 哨兵）。页头记录分配状态/类型。
- `PageGuard.h`：页级读写闩 RAII——`Fetch/New/Release` 在获取后立即释放 BPM 全局 `latch_` 再持页锁，避免锁序反转；同一帧二次独占锁必须先 `Release()` 再嵌套（防自锁）。
- **OS 映射**：页面 = 内存分页概念；页锁 = 细粒度并发控制（操作系统「锁」章节）。

**2) 缓冲池 BufferPoolManager**
- 帧数组 + 空闲帧 `free_list_` + `page_table_`（页号→帧号）；`latch_` 串行化单次帧访问（非事务级）。
- 核心链路：`GetPage`（命中 / `LRUKReplacer::Evict` 换出脏页→`writeback_count`++）→ `NewPage` → `UnpinPage` → `Flush/FlushAll` → `DeletePage`。
- 所有写回路径（换出、Flush、DeletePage）必须先取页读锁（防写撕裂）；帧/页号在异常路径归还 `free_list_`（防泄漏）。
- 内存容量由 `SQLCOMPILER_BUFFER_MEMORY` 配置（默认 64 帧 = 256KB）。
- **OS 映射**：缓冲 = 主存；换页 = 虚拟内存管理；脏页写回 = 延迟写。

**3) LRU-K 置换**
- 双集合结构：`recent_store_`（访问次数 < K）与 `historic_store_`（≥ K）；K=1 退化为 LRU。
- `RecordAccess` 维护访问频率，`Evict` 优先逐出低频页，抗顺序扫描污染。
- `Unpin` 用 `position_map_.emplace(id, --list.end())` 单哈希（替代 find+operator[]，一次哈希）。
- **OS 映射**：页面置换算法（LRU-K 是对经典 LRU 的频率感知改进）。

**4) 磁盘管理 DiskManager**
- 64 位文件定位（`_fseeki64/_ftelli64`）支持 >2GB 库文件；`_commit(_fileno(f))` 实现真正持久 fsync（等价 POSIX `fsync`）。
- 页分配/回收：`next_page_id_ = 文件大小 / PAGE_SIZE`（从数据文件推导，不独立持久化）；空闲页写 `<db>.fpl` 位图（bit i=1 空闲）；`DeallocatePage` 幂等。
- `<db>.crc`：每页 4 字节 CRC32 校验记录，崩溃后校验页完整性。
- `db_io_latch_` 串行化磁盘 I/O；锁序约束 `BPM::latch_ → db_io_latch_`。
- **OS 映射**：文件系统 I/O、扇区/块读写、校验与容错。

**5) 块设备抽象 BlockDevice**
- `FileBlockDevice`（默认，真实文件 I/O）与 `FaultInjectingBlockDevice`（测试注入故障，模拟坏块/写失败）。
- **OS 映射**：设备驱动层抽象、故障注入（容错测试方法论）。

**6) WAL 日志 LogManager**
- `include/txn/LogManager.h`：Redo/Undo/CLR 三类日志；`LogManager::mutex_` 与 BPM 锁序约束；崩溃恢复时按 LSN 重放。
- **OS 映射**：日志文件系统 / 崩溃一致性（持久化顺序）。

**7) 后台线程与分阶段优化**
- 后台刷脏（`SQLCOMPILER_BG_FLUSH_MS`）与后台真空（`SQLCOMPILER_BG_VACUUM_MS`）均默认 0（关闭）、环境变量可配、`Shutdown` 幂等。
- `OsModuleOptimizations`：把热路径优化（CRC32 constexpr 查找表、LRU Unpin 单哈希、页内分配头读取合并）做成 **staged 分阶段 + Legacy 孪生**，`RunBenchmarks()` 进程内输出前后指标、`TestOptimizations` 校验正确性——保证「优化可度量、可回退」。
- **OS 映射**：操作系统级优化方法论（先测量、后优化、保正确）。

### B. 隔离级别并发控制

**1) 事务模型与提交跟踪**
- `Transaction`：txn_id、活跃状态、隔离级别；`CommitTracker` 为提交事务分配单调递增 CSN，作为 MVCC 版本可见性的时间基准；`OldestActiveSnapshot` **显式遍历求最小 CSN**（修复前用 `unordered_map::begin()`，迭代序不定会误删活动快照所需版本）。
- 快照事务在 `Begin` 捕获 `SnapshotCsn`；FCW 检测要求**先挂载事务再读行**（`SetActiveTransaction` 在行 X 锁前），确保 `RecordSnapshotRead` 记录正确快照基。

**2) 锁管理器 LockManager**
- 多粒度表锁/行锁；`LockState.waiters` 为 `vector<pair<int64_t,LockMode>>`（`w->first/w->second`）；行资源 id 用符号位区分命名空间（行锁与表锁不重叠）。
- **死锁检测**：维护等待图 `waits_on_`，新阻塞边后 DFS 检测环；`TryLock` 探针保留等待登记边（环事务可被检出），victim 返回 kDeadlock 前撤销自身等待边（防残留边污染）。
- **谓词锁**（SERIALIZABLE 防幻读）：`AcquireReadPredicate` 集合规约——全表父谓词覆盖子区间；新区间为全表时删除全部区间；区间按 lo 合并真重叠为不重叠最小区间覆盖；`CheckWritePredicate` 写前检查。谓词作用于主键键空间。
- **行锁升级**：`AcquireRowWriteLock` 每行登记归属，`CountRowLocks ≥ 128` 触发 `TryEscalateTable`（无冲突→授予表锁+释放全部行锁→O(1) 放行；失败回退逐行持锁）；升级幂等。
- **UnlockAll**：提交/回滚释放事务全部锁；行锁升级标记同步撤销。

**3) MVCC 版本链（TableHeap）**
- 每条记录头：`begin_xid / end_xid / end_csn / prev`（上版本槽位）；`SetSnapshot(csn, tracker)` 把共享堆挂到本事务快照水位。
- `GetTuple`：快照读沿版本链用 `VisibleForSnapshot` 过滤（自删不可见、删除者未提交可见、end_csn>S 可见…）；**非快照读**过滤逻辑删除 head（`end_xid != 0` → 行不可见，消除幽灵行）。
- `UpdateTuple`：旧 head 迁移到新 slot（记 end_xid=me、保留 prev），head 槽覆写新版本；MVCC 删除仅置 `end_xid`（保槽位/RID 稳定）。
- `Vacuum`：`end_xid != 0 && end_csn != 0 && end_csn <= oldest_active_csn` 才写 `kTombstone` 回收——任何活动快照都看不到「结束于它之前」的版本；无活动快照整体 no-op。

**4) 索引 MVCC 精确可见性（IndexMaintenance / IndexScanExecutor）**
- `IndexDeleteDeferred`：快照写者 + 非唯一索引 → 逻辑删除/键改写**延迟摘除**旧条目（唯一/主键索引保持急切，保唯一性预检）。
- `IndexKeysEqual`：键未变跳过重建/摘除（避免同 `(key,rid)` 重复条目）；`RestoreDeletedIndexEntries`：写堆失败只恢复急切摘除条目。
- `IndexScanExecutor::Next`：回表 → `seen_rids_` 按 RID 去重 → 可见版本重建键 `InScanBounds` 键重检（过滤越界伪行/陈旧条目）。

**5) 执行引擎接线（ExecutionEngine / 算子）**
- `ReleaseRowReadLocks`：READ COMMITTED 语句末（成功与异常路径都）释放行读锁；SERIALIZABLE 持到提交。
- `ExecuteSubplan`：按隔离级别注入锁行为（DDL 表锁、行读锁释放、SERIALIZABLE 谓词注册）；表锁等待超时 5000ms。
- 扫描/写算子：kSnapshot 时 `SetSnapshot` 挂快照（免读锁），非快照/自动提交统一 `SetSnapshot(-1,nullptr)` 复位水位（防陈旧读泄漏）；写算子读行前挂载事务（FCW 正确基）。

---

## 四、审阅发现（Review Findings）

### 亮点（设计正确性）
1. **锁序纪律严格**：`BPM::latch_ → LogManager::mutex_`、`BPM::latch_ → DiskManager::db_io_latch_`、页锁在释放全局 latch 后获取——无锁序反转，配合 DFS 死锁检测形成闭环。
2. **崩溃一致性分层**：`.crc` 先于数据文件同步、`.fpl` 位图预清后才分配（防崩溃双重分配）、WAL 持久化顺序——每层防护对应独立故障模型。
3. **优化可度量**：`OsModuleOptimizations` 的 staged + Legacy 孪生结构使每项优化都有进程内前后对照，避免「优化不可验证」。
4. **隔离级别矩阵完整**：4 个级别 ×（读锁/写锁/MVCC）语义齐全；MVCC 快照 + FCW + 谓词锁 + 行锁升级各司其职。
5. **测试证据充分**：6982 UT checks 覆盖死锁/谓词/升级/真空/二级索引场景，SQL 回归含崩溃注入。

### 风险与已知限制（审阅中确认的边界）
1. B+Tree 树级粗粒度锁：写者（Insert/Delete）彼此串行，多连接写并发未达页级。
2. 谓词锁仅覆盖主键键空间：非主键谓词不注册（存在理论幻读窗口）。
3. 锁升级阈值固定 128：未按表行数/冲突率自适应。
4. 版本回收依赖后台真空（默认关闭）：关闭时版本链仅靠前台低频触发回收。
5. 多列索引仅单列最左列参与区间推导。
6. `LockManager` 全局互斥：单锁表在高并发下为热点。

---

## 五、测试与验证证据

| 层 | 结果 |
|---|---|
| 存储单元测试 `storage_ut.exe` | **6982 checks / 0 fails**（重复运行稳定） |
| SQL 回归 `tests/run_all_tests.bat` | **55 passed / 0 failed**（53 SQL + 崩溃注入 49/50） |
| 隔离重点用例 | 40 / 46 / 48 / 49 / 50 / 51 / 52 全过 |
| 崩溃注入 | acid_recovery（WAL 恢复）、acid_clr（CLR 链）exit 0 |
