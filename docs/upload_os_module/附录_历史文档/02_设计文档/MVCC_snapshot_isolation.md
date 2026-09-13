# MVCC 快照隔离（完整版本链）

## Context（为什么做）
当前数据库的隔离完全基于 `LockManager` 的 S/X 行锁 + 谓词锁：读者取读锁会阻塞写者，写者取写锁会阻塞读者，无法同时保证「读者不阻塞写者」与「读一致性」。用户要求引入**完整版本链 MVCC 快照隔离**：读者不取读锁，而是捕获一个稳定快照，只能看到「截至快照时刻已提交」的版本；写者仍在每行上串行（沿用现有行 X 锁），但在提交时用 first-committer-wins 检测并中止输家，避免丢失更新/写偏斜。

复用与扩展现有基础设施，不重写：
- 现有行写锁（`ExecutionContext::AcquireRowWriteLock`）已串行化每行写者——继续使用。
- 现有 per-table `write_mutex_` 串行化堆物理写、页级 18 字节页头 + 槽位目录布局、`.crc`/`.fpl`/WAL/undo ——布局保持不变，仅改动**记录级字节格式**。
- 现有共享注入模式：`Database` 持有 `LockManager` 并通过 `SetLockManager` 注入每个会话的 `TransactionManager`（见 [Database.cpp](file:///c:/Users/Lenovo/Desktop/SQL-Compiler/src/db/Database.cpp#L210-L221)）。共享 `CommitTracker` 完全复刻此模式。

## 设计要点

### 1. 磁盘记录格式：MVCC 头 + 当前版本在稳定 RID 槽位（head）
每条序列化记录前加固定头 `MvccRecordHeader`。**最新可见版本保留在 head 槽位（RID 稳定，索引始终指向它）**；被替换/删除的旧版本作为独立的 slot 记录，用 `prev_page_id/prev_slot_num` 链接成链。

```cpp
constexpr uint32_t kMvccMagic = 0x4D564343u; // "MVCC"
struct MvccRecordHeader {
    uint32_t   magic;          // 兼容判定（旧库无此头）
    int64_t    begin_xid;      // 写者 txn_id
    int64_t    end_xid;        // 0=仍为最新可见；否则为替换/删除本版的写者
    int64_t    begin_csn;      // begin_xid 提交的 CSN（提交时回填）
    int64_t    end_csn;        // 使本版失效的 CSN（0=尚未被替代）
    int64_t    prev_page_id;   // 更旧版本记录页号（INVALID_PAGE_ID=链尾）
    int32_t    prev_slot_num;
};
```
- `Tuple::Serialize/Deserialize` 只产/读列字节；头由 `TableHeap` 拼接与解析。
- `GetTuple` 读到 `magic != kMvccMagic` → 按旧格式单版处理（可见、链尾），实现向后兼容。
- 旧 `.bin` 首改可能因加头变长放不进原槽 → **文档化局限**：SNAPSHOT 推荐新库，旧库保持低隔离只读（可选在 database/catalog 增加格式守卫，超范围则不加，避免过度设计）。

### 2. 提交时间戳 + 可见性：共享 CommitTracker + 事务快照 CSN
`CommitTracker` 做成 **header-only**（放 `include/txn/`，避免为它新建 `.cpp` 触发 cmake 重配置）。成员：`mutex m_; unordered_map<int64_t,int64_t> committed_; unordered_set<int64_t> aborted_; int64_t next_csn_;`；接口 `Commit(xid)->csn`、`Abort(xid)`、`LookupCommitted(xid,&csn)`、`CurrentCSN()`、`OldestActiveSnapshot()`。**提交赋 CSN 与快照读 CurrentCSN() 在同一把互斥锁下串行化**，保证提交序正确。

- `Transaction` 增加 `int64_t snapshot_csn_`（BEGIN 时捕获，全事务快照）。`TransactionManager::Begin()` 赋值。
- 可见性谓词（版本 `v` 对快照 `S`）：
  ```
  if (!LookupCommitted(v.begin_xid,&bcsn))：           // begin 未提交
      if (v.begin_xid == 我) bcsn = +∞（自写可见）
      else return 不可见                                   // 排斥脏读
  满足 bcsn <= S  （创建于快照前提交）
  且 (v.end_xid==0 || !LookupCommitted(v.end_xid,&ecsn) || ecsn > S)
      （快照时刻未被删除/替换）
  ```
  行可见 = 从 head 沿链找到首个满足谓词的版本；若该版 end_xid 已提交且 end_csn<=S → 行整体不可见。

### 3. 写打戳与版本链
全部在 `TableHeap` 内、`write_mutex_` + 页写锁保护下完成，且**必须已 SetActiveTransaction**。
- **InsertTuple**：新 head，`begin_xid=me,end_xid=0,end_csn=0,prev=INVALID`；base 指纹记入 txn.write_set。
- **UpdateTuple（MVCC）**：页内先分配新 slot，把旧 head 载荷+头迁入新 slot，其 `end_xid=me, prev=旧.head.prev`；再原地覆写 head：`begin_xid=me,end_xid=0,prev=新slot`。**RID 稳定 → 索引不用移动**（主键键不变时 `UpdateExecutor` 可跳过摘/挂索引）。若新值放不进 head 槽 → 回退既有 relocate（Delete+Insert）路径，RID 变化由 `UpdateExecutor`/`IndexMaintenance` 既有摘/挂逻辑补齐（作验证点）。
- **DeleteTuple（MVCC）**：不再写槽位墓碑，仅把 head 置 `end_xid=me`；物理保留待 Vacuum。旧墓碑路径仅在读取到 legacy 无头记录时保留。

### 4. 读路径：快照过滤 + 免读锁
- `TableHeap`/`Iterator` 增加 `snapshot_csn_` 与 `CommitTracker*`；`GetTuple`/`FindNextRid`/`Iterator` 按可见性跳过不可见行（替换当前「跳过墓碑」逻辑；快照下跳过他人锁定/未提交行是安全的，互不阻塞）。
- `SeqScanExecutor::Init` / `IndexScanExecutor::Init`：当 `iso==kSnapshot`，`table_heap_->SetSnapshot(txn->GetSnapshotCsn(), txn_manager_->GetCommitTracker())`。
- `ExecutionContext::AcquireRowReadLock`（[Executor.cpp](file:///c:/Users/Lenovo/Desktop/SQL-Compiler/src/execution/Executor.cpp#L65-L81)）：`kSnapshot` 同 `kReadUncommitted` 直接返回 `kUnused`，不取读锁。
- **幻读**：快照隔离天然防幻读，不注册谓词锁；`kSerializable` 路径（`CheckWritePredicate`/谓词注册）完全不变。

### 5. First-committer-wins
`Transaction` 增加 `write_set_`（`RID + base_begin_xid + base_begin_csn`，写前捕获 head 的 base 指纹）。
`TransactionManager::Commit()` 在赋 CSN 之前（仅 `kSnapshot` 且最外层）：遍历 write_set，重读各 RID head 的 `(begin_xid,begin_csn)`；若与 base 不一致且对方已提交（其 csn < 本次）→ 冲突：打印冲突、走 Rollback（中止本事务）。自写跳过。纯 INSERT 的重复键由既有 `CheckUniqueIndexes` 先行拦截。成功后 `tracker_.Commit(me)` 并回填本事务所有版本头的 `begin_csn/end_csn`。

### 6. Vacuum（惰性回收）
`TableHeap::Vacuum(oldest_active_csn)`：回收所有 `begin_csn < oldest_active_csn` 的**非 head 旧版本 slot**（清槽位）。触发点：登记进现成后台线程 `BufferPoolManager::BackgroundFlushLoop`（每 N tick 一次，惰性无害）或 `Execute` 尾部低频调用。回收用页写锁/`write_mutex_` 与读扫描串行，以 oldest_active_csn 为界防误删活动快照所需版本。

### 7. 隔离级别并入
- `IsolationLevel`（[Transaction.h](file:///c:/Users/Lenovo/Desktop/SQL-Compiler/include/txn/Transaction.h#L23-L27)）新增 `kSnapshot`；`TransactionManager::SetIsolationLevel` 传递即可（`storage_ut` 直接设，SQL 侧 `SET ... ISOLATION LEVEL SNAPSHOT` 为可选）。
- 词法/解析：可选加 `SNAPSHOT` 关键字映射（镜像 `52_set_isolation.sql` 的 RC/SERIALIZABLE）。

## 文件级改动清单
| 文件 | 改动 |
|---|---|
| `include/txn/Transaction.h` | `kSnapshot` 枚举；`snapshot_csn_`；`write_set_`(WriteSetEntry) |
| `include/txn/CommitTracker.h`（新，header-only） | `CommitTracker` 类 |
| `include/txn/TransactionManager.h` | `SetCommitTracker/GetCommitTracker`（镜像 LockManager 注入） |
| `src/txn/TransactionManager.cpp` | Begin 设 snapshot_csn_；Commit 接 FCW + CSN 回填；Rollback 登记 aborted |
| `include/storage_engine/TableHeap.h` | `MvccRecordHeader`,`kMvccMagic`；`snapshot_csn_/commit_tracker_`；`SetSnapshot`；改 `GetTuple/FindNextRid/Iterator` 带快照；`Vacuum/RemoveVersion` |
| `src/storage_engine/TableHeap.cpp` | Insert/Delete/Update 打戳与链重接；可见性过滤；Vacuum |
| `include/storage_engine/Tuple.h` | （如需）`kMvccMagic` 常量 |
| `src/execution/Executor.cpp` | `AcquireRowReadLock` 对 `kSnapshot` 返回 `kUnused` |
| `src/execution/SeqScanExecutor.cpp` / `IndexScanExecutor.cpp` | Init 挂 snapshot + tracker |
| `src/db/Database.cpp` | 持有共享 `commit_tracker_`，注入默认 TM 与各 `CreateSession` TM |
| `tests/storage/storage_ut.cpp` | `TestSnapshotIsolation`：可重复读（同一快照两次读到一致集）、免阻塞读、first-committer-wins 冲突、旧库兼容；`SetIsolationLevel(kSnapshot)` |

> 若新增非 header-only 的 `.cpp`，需 `cmake -S . -B build` 重配置（`file(GLOB)` 配置期求值）。

## 实施顺序（最小可验证递进）
1. `kSnapshot` 枚举 + Transaction 字段 + TM Begin 捕获 snapshot_csn_（`storage_ut` 通过）。
2. `CommitTracker`（header-only）+ Database 注入共享实例。
3. `MvccRecordHeader` + Insert 打头（新库读回一致）。
4. GetTuple/Iterator/FindNextRid 可见性过滤（他人未提交不可见、自写可见）。
5. Update 版本链（RID 稳定）+ write_set base 捕获。
6. Delete 改 end_xid + 行删除可见性。
7. Commit 接 FCW 冲突回滚 + CSN 回填。
8. Executor 门控（kSnapshot 免读锁）+ 扫描挂快照。
9. Vacuum（惰性）+ legacy magic 回退。
10. `TestSnapshotIsolation` 并发测试（读-改-写两事务竞争；两次读一致）。

## 验证
- 存储单测：`cmake --build build --target storage_ut`，需新增 `TestSnapshotIsolation`，全部 PASS（当前 6598 checks）。
- SQL 回归：`tests/sql/*.sql`（含 40/46/48/49/50/51/52）`exit code = 0`；默认隔离不变（仍 kSerializable），旧行为不回归。
- 手写并发场景：A 快照读两次一致；B 提交后 A 快照看不到 B 的新增；两事务竞争同一行为 first-committer-wins 一方中止。
- 更新 `docs/Storage_Dev_Plan.md` 记录本里程碑与已知限制。

## 已知限制（写入文档）
- 版本链每 UPDATE 多占一个 slot，旧版本逃逸到溢出页，靠 Vacuum 回收。
- 记录级字节布局变化：旧库仅读兼容，SNAPSHOT 特性针对新库。
- 非主键二级索引的 MVCC 精确可见性沿用行锁+树锁保证；快照对未提交行跳过。
- Vacuum 惰性、以最老活动快照为界（尚无独立后台线程）。

## ✅ 已完成（本里程碑落地情况）
- `IsolationLevel::kSnapshot` 链：`Token.h` 新增 `KEYWORD_SNAPSHOT`（+ Lexer/Token/SetIsolation 解析），`SET TRANSACTION ISOLATION LEVEL SNAPSHOT` 可用（`52_set_isolation.sql` 已覆盖，CLI 验证 exit 0）。
- 共享 `CommitTracker`：`Database` 持有唯一实例，`SetCommitTracker` 注入各会话 TM；`Begin()` 捕获 snapshot_csn_ 并 `RegisterSnapshot`，最外层 `Commit` 先 FCW 冲突检测、后 `Commit(xid)` 赋 CSN 并 `BackfillVersionCsn`（回填所有 version_slots 的 begin_csn/end_csn），`Rollback()` 登记 `Abort(xid)` 并注销活动快照。
- `MvccRecordHeader`（48B）打头 + 版本链：Insert 打头；Update 同页先迁旧 head 到新 slot（end_xid=me）再覆写 head（prev=新 slot，RID 稳定）；Delete 仅置 head `end_xid=me`；`GetTuple/FindNextRid/Iterator` 沿链做 `VisibleForSnapshot` 过滤。
- 读路径：`AcquireRowReadLock` 对 `kSnapshot` 返回 `kUnused`（免读锁）；`SeqScan/IndexScan::Init` 挂 `SetSnapshot(csn, tracker)`。
- Vacuum：`SystemCatalog::VacuumAll(oldest_active_snapshot)`，由 `Database::ExecuteSQLImpl` 每 100 语句低频触发；`FindNextRid` 跳过被引为 prev 的旧版本 slot。
- 测试：`TestSnapshotIsolation`（可重复读 / 免阻塞读 / 写者 FCW）全绿；存储单测 `checks: 6630  fails: 0`；SQL 回归 40/46~52 exit 0。
- **FCW 基捕获修复（关键）**：`UpdateExecutor` 原先在取行 X 锁**之后**才 `SetActiveTransaction`，导致读行时刻 `RecordSnapshotRead` 未记录，UpdateTuple 退化为以**物理 head** 为基（读到对方已提交的新版本），FCW 误判「基于对方」而放行，丢失更新落盘。修复：读行循环**前**挂载 `SetActiveTransaction(txn)`，语句结束后解除——写集基取自快照可见版本，FCW 能正确识别「B 基于旧值改写 A 已提交的新版」并中止 B。
- **快照 DELETE / UPSERT 的 FCW 冲突检测（本里程碑补齐）**：仅 UPDATE 走 FCW 不够——`DeleteExecutor`/`UpdateExecutor`/`UpsertExecutor::UpdateConflictingRow` 在扫描/读行前未调用 `SetSnapshot`（只有 `SeqScan/IndexScan` 调了），`GetTuple` 退化为只读物理 head、不看版本可见性，于是同一逻辑行的新旧版本槽位被当作独立行删除/改写（快照删除把两个槽位都删掉，回滚后行损坏；UPSERT 丢失更新未被 FCW 拦截）。修复：三个写算子在读堆前对活动 kSnapshot 事务 `SetSnapshot(快照水位, CommitTracker)`，写路径只命中快照可见的唯一版本，配合既有行 X 锁 + 提交期 FCW 中止输家、保留 A 的值。新增 `TestSnapshotDeleteFcw`/`TestSnapshotUpsertFcw` 覆盖；存储单测 `checks: 6658  fails: 0`，全部 53 条 SQL 回归 exit 0。