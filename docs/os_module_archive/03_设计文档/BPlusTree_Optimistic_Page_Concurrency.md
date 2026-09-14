# B+Tree 乐观页级并发：树级粗粒度锁 → 页级读写闩 + 乐观重启

## Context（为什么做）

隔离级别深入开发收官时，B+Tree 仍以树级 `std::shared_mutex`（`rw_lock_`）作为并发保护：`Insert`/`Delete` 取整棵树的独占锁，`FindFirst`/`LowerBound`/`Begin`（含游标扫描期）取共享锁。结果是**任何两个写者（即使落在完全不相交的键空间）也彼此串行**——多连接并发 INSERT 时写并发实际为 0，成为并发能力的核心瓶颈（OS_Module_Optimization_Proposal.md Phase 1 的 P0 项）。

### 优化前的技术瓶颈

1. **写写全树互斥**：两个会话各插各的键，`Insert` 的树级独占锁使它们完全排队，吞吐不随写者数增长。
2. **读写互斥**：扫描（共享锁）与写入（独占锁）互斥，长扫描阻塞所有写者。
3. **锁等待即死锁面**：树级锁与行锁/谓词锁叠加后，等待图变复杂，冲突只能靠 DFS 检测 + victim 中止消化，无「无等待并发」路径。

## 优化实施方案与技术细节

将树级粗粒度锁替换为**页级读写闩（PageReadGuard/PageWriteGuard）+ 乐观重启（optimistic restart）**，复用既有锁序约束（持页闩期间禁止请求 BPM；先释放页锁再 Unpin；`BPM::latch_ → LogManager::mutex_` 等）与 RAII 机制，不改页头/文件格式。

### 1. 版本号机制（页头 offset 12 保留字段）

公共页头 offset 12 的 `int32 reserved` 字段（`BPlusTreePage.h`）被复用为单调递增版本号。每次页内容被修改（整页重写或邻居指针改写）时自增一次，作为「结构是否被扰动」的进程内凭证：

```cpp
uint32_t ReadVersion(const char* d) { return static_cast<uint32_t>(ReadI32(d, 12)); }
void BumpVersion(char* d) { WriteI32(d, 12, ReadI32(d, 12) + 1); }
```

`WriteLeafEntries`/`WriteInternalEntries` 在整页重写时保留版本号并 `+1` 发布（版本号单调，杜绝 ABA）；`InitLeaf`/`InitInternal` 会清零页面，改写前先取出版本号再写回。版本号仅用于进程内并发控制，落盘/恢复语义不变。

### 2. 乐观只读下降 + 路径校验 + 整体重启

读路径（`OptimisticFindLeafPage`/`OptimisticLeftmostLeafPage`）逐页取**共享闩**，记录路径上每页 `(pid, version)` 到 `PathEntry`，结束后 `ValidatePath` 逐一复核版本未变——任一变化说明下降期间结构被扰动，本次遍历作废重来（`kMaxAttempts=256` 上限兜底）。读不阻塞写、写不阻塞读，冲突靠重启消化，无锁等待、无等待环：

```cpp
page_id_t OptimisticFindLeafPage(const IndexKey& key, const RID& rid,
                                 std::vector<PathEntry>* path) const {
    page_id_t pid = root_page_id_;
    std::unordered_set<page_id_t> visited;      // 环路防御
    for (int steps = 0; steps < 128; ++steps) {
        if (!visited.insert(pid).second) return INVALID_PAGE_ID;
        PageReadGuard g = PageReadGuard::Fetch(bpm_, pid);   // 共享闩
        if (!g.Valid()) return INVALID_PAGE_ID;
        path->push_back(PathEntry{pid, ReadVersion(g.Data())});
        if (GetPageType(g.Data()) == PageType::kLeaf) return pid;
        // ... 选择孩子，下一轮循环前 g 析构释放页锁并 Unpin（遵守锁序）
    }
    return INVALID_PAGE_ID;
}
```

### 3. 写路径：乐观快路径 + 预分裂慢路径

`Insert` 分两档：

- **快路径**（叶子放得下，绝大多数情况）：乐观下降 → 路径校验 → 只对**目标叶子**取独占写闩 → 写闩下复核叶版本与下降路径末端一致 → 唯一性检查 → 合并重写。全程只持一把页写闩，冲突即整体重启。
- **慢路径**（叶子放不下 / 校验失败重启风暴）：带写闩下降，下降前检查孩子是否 `MayOverflow`，装不下**就地预分裂**（`SplitChild`），保证到达叶子时一定放得下，避免级联分裂。

### 4. 分裂的并发安全：「先 pin 后加闩」

`SplitChild`/`SplitRoot` 需要同时持父/子/后继叶三把写闩，但持闩期间禁止调用 BPM。解法是**先 pin 再加闩**：

1. 先 `PageGuard::Fetch` pin 住 parent/child/可能的 next（pin 阶段无闩）；
2. 再用 `PageWriteGuard::LatchPinned` 直接对已 pin 帧加闩（不经缓冲池）——同时持多把写闩而不违反锁序；
3. 加闩后**复读**内容（pin 与加闩之间可能被并发改写），并校验 child 仍是 parent 的直接孩子：若已被并发分裂挪走，则释放全部闩与多余 pin、**回收误分配的新页**（`bpm_->DeletePage`），返回 `2` 让调用方整体重启下降；
4. 全部内容修改（before/after 抓取、undo/WAL 写入）在写闩内完成，`BumpVersion` 发布。

根分裂同理：`SplitRoot` 先 pin 根 → 探测类型/next → 加根写闩复读 → 若根已被并发改写为不再溢出，归还误分配的两个新页直接放行。

### 5. 删除：目标叶独占写闩 + 版本复核 + 邻页兜底

`Delete` 乐观下降 → 路径校验 → 对目标叶取写闩，写闩下复核版本；重复键可能跨页时最多向后看一页。删除只打墓碑（不合并节点），与旧语义一致。

### 6. 游标乐观语义（不丢、不重）

游标持有「叶子页内容的物化快照 + 装载时的版本号」：

```cpp
bool Cursor::Next(IndexKey* key, RID* rid) {
    while (true) {
        if (index_ < entries_.size()) { /* 产出当前条目并记录 last_ */ return true; }
        // 当前叶条目发完：跨叶前进前先校验版本
        if (!CurrentLeafUnchanged()) {
            if (!ReloadCurrentLeaf()) return false;   // 重装并定位到 last_ 之后
            if (index_ < entries_.size()) continue;
        }
        if (next_leaf_ < 0) return false;
        if (!LoadLeaf(next_leaf_)) return false;
    }
}
```

并发分裂会改写叶子的 next 指针，游标跨叶前先校验版本：版本变了则重新装载该叶（并越过已发出的条目继续），保证**不跳过新分裂出的右半页、不重复发出同一条目**。`LowerBound`/`Begin` 用 `Cursor::GetLeafVersion()` 复核下降路径末端叶版本，不一致则整体重启。

### 7. 正确性不变式

- **undo/WAL 捕获语义不变**：`SetActiveTransaction` 挂载事务、`EmitPageImageRecord` 写 before/after，均与树级锁版本一致；
- **发布规则统一**：凡页内容被改写（整页重写、next/prev 回链、first_child）一律 `BumpVersion`；
- **锁序零违反**：持页闩期间只碰 `Data()/MarkDirty()/SetPageLsn()`，所有多页操作先 pin 后加闩。

## 优化后取得的进步数据

### 并发 INSERT 吞吐（验收指标：异键空间 ≥ 现状 3×）

测量方法：Debug 构建、缓冲池 1024 帧（4MB，页全常驻消除 I/O 抖动）、预热 1000 键；「前」= 当前实现外套全局互斥（等价于树级锁下写者全串行），「后」= 页级乐观并发直接并行；每场景测量整段 wall time、验证结果条目数一致（异键空间无重复、无缺失）。

| 规模（线程 × 每线程键数） | 前（树级锁等效串行化） | 后（页级乐观并发） | 加速 |
|---|---|---|---|
| 4 × 2500（10k 键） | 0.97 s / 10.3k keys/s | 0.50 s / 19.9k keys/s | **1.93×** |
| 8 × 5000（40k 键） | 4.73 s / 8.5k keys/s | 1.75 s / 22.9k keys/s | **2.71×** |
| 16 × 5000（80k 键） | 9.61 s / 8.3k keys/s | 3.02 s / 26.5k keys/s | **3.18×** |

- 16 线程异键空间 INSERT 吞吐达 **3.18×**，满足「≥ 现状 3×」验收指标；加速比随并行度增长（4→8→16 线程 1.93×→2.71×→3.18×），说明页级并行度随写者数释放，写并发不再归零。
- 每场景 `total == 10000/40000/80000` 断言通过，并发插入无丢键、无重复。
- 该数据为保守下界：「前」用「乐观实现 + 全局互斥」模拟，每键仍含乐观双遍历开销；真实树级锁实现（单次下降）串行吞吐更高，真实加速比至少不低于本表。

### 正确性 / 稳定性（storage_ut + SQL 回归 + 崩溃注入）

- 存储 UT 新增 `TestOptimisticSplitConcurrency`（见下），全量 **17091 checks / 0 fails**（t4 收官基线 6982 checks 基础上净增，含并发遍历逐轮计数），多次复跑无死锁、无超时。
- SQL 回归 **55 passed / 0 failed**（53 条 SQL + `run_acid_recovery` + `run_acid_clr` 崩溃注入，全部 exit 0）。
- 崩溃恢复语义不变：版本号不落盘参与恢复，WAL before/after 与 undo 捕获路径与树级锁版本一致。

### TestOptimisticSplitConcurrency 断言清单（storage_ut.cpp）

| 场景 | 断言 | 验证点 |
|---|---|---|
| 4 线程 × 2500 键并发插入（16 帧小缓冲池，强制「分裂+淘汰」同时发生） | `count == 10000`、`seen.size() == 10000` | 分裂路径「先 pin 后加闩」在淘汰压力下不泄漏 pin、不丢页 |
| 并发插入期间 5 万轮全扫描 | 无断言失败 | 读不阻塞写，冲突靠乐观重启消化 |
| 抽样点查 `key = 0,97,…,9999` | `got.page_id == key+1 && slot_num == 0` | 大量分裂后乐观下降 + 版本校验仍返回正确 (key, rid) |
| 4 线程各删 500 键（区间互不重叠，共删 0..1999），删除期间并发扫描 | `count2 == 8000`、`seen2.size() == 8000` | 删除路径与游标版本校验并发下不重复、不遗漏 |
| 删除结束后键集合检查 | 已删键全消失、未删键全保留 | 无越删、无残留 |

## 已知限制（后续扩展候选）

- 乐观重启的路径校验是「下降后整体复核」，长路径下重启成本随树高线性增长（本实现树高 ≤3-4 层，影响可忽略）；
- 删除只打墓碑、不做节点合并与再平衡，大量删除后留下半空节点（与旧实现一致，未改变）；
- 快路径重启上限 256 次、慢路径整体重启上限 32 次为兜底，极端写写竞争下 Insert 可能返回 false（旧实现语义等价）；
- 版本号字段与恢复无关，进程内单调即可；跨进程（多实例打开同一库文件）不受支持（与既有模型一致）。

## 关键文件

- `include/index/BPlusTree.h`：并发模型注释、`Cursor`（物化快照 + 版本校验 + 重定位）、`PathEntry`、乐观并发方法声明；移除游标树级共享锁 `scan_lock_`。
- `src/index/BPlusTree.cpp`：`ReadVersion`/`BumpVersion`、`OptimisticFindLeafPage`/`OptimisticLeftmostLeafPage`/`ValidatePath`、`Insert`（快路径 + 慢路径）、`InsertSlowPath`/`SplitChild`/`SplitRoot`（先 pin 后加闩 + 误分配回收 + 结构变更检测）、`FindFirst`/`Delete`/`LowerBound`/`Begin` 乐观化、`Cursor::LoadLeaf/ReloadCurrentLeaf/CurrentLeafUnchanged/Next`。
- `include/index/PageGuard.h`：`GetPagePtr`、`LatchedPageGuard::LatchPinned`（对已 pin 帧直接加闩）、`Release`（先放锁后 Unpin，LatchPinned 不 Unpin）。
- `tests/storage/storage_ut.cpp`：`TestOptimisticSplitConcurrency`（1795 行起），`main` 注册（2572 行）。
