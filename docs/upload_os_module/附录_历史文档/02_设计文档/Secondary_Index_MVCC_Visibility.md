# 二级索引 MVCC 精确可见性：延迟摘除 + 回表键重检 + RID 去重

## Context（为什么做）

MVCC 快照隔离上线后，快照写者（键改写 UPDATE / DELETE）与二级索引的维护方式存在一致性缺口：索引叶子条目按「条目键 → RID」组织，而 MVCC 下同一逻辑行（稳定 RID）可能沿版本链存在多个版本，其**可见版本键**随写者提交而改变。索引条目与「本快照可见版本」不再一一对应。

### 优化前的技术瓶颈（t4 之前的缺陷）

1. **键改写急切摘除 → 老快照读者丢行**：快照写者把 `v=20` 改为 `v=100` 时，若立即从二级索引摘除条目 `(20,rid)`，则**老快照读者**（快照早于该提交）沿索引找不到该行——但版本链中 `(rid, v=20)` 对它仍可见。索引先行摘除破坏了快照一致性（丢行）。
2. **删除急切摘除 → 非快照读者幽灵行**：快照 DELETE 置 `end_xid` 保留物理行，若立即摘除条目，老快照读者丢行；反之若保留条目，**非快照（SERIALIZABLE/READ COMMITTED）读者**经索引回表会命中该 head——若不按 `end_xid` 过滤，读到已删除的行（幽灵行）。
3. **延迟摘除的反向缺陷（若不做回表过滤）**：若因上述原因对非唯一索引一律延迟摘除，则：
   - **伪行**：键改写后旧条目 `(20,rid)` 残留，范围扫描 `[10,30]` 命中它，回表读到可见版本 `(rid, v=100)` —— 键 100 越出扫描区间，不该返回却返回了；
   - **重复行**：键改写落在区间内（`30→25`）时，新条目 `(25,rid)` 与旧条目 `(30,rid)` 都被扫描命中，同一逻辑行被返回两次；
   - **键未变更新重复插入**：键未变的 UPDATE 若不跳过重建，同一 `(key,rid)` 会出现两份条目。
4. **唯一索引不可延迟**：唯一性预检（`CheckUniqueIndexes`）依赖条目存在性，延迟会让同键出现两份、预检失效。

### 优化实施方案与技术细节

按「**唯一/主键索引急切维护，非唯一二级索引延迟摘除 + 精确过滤**」策略落地：

1. **索引维护侧**（`IndexMaintenance.h/.cpp`）：
   - `IndexDeleteDeferred(info, txn)`：快照事务 + 非唯一索引 → 逻辑删除/键改写**延迟摘除**旧条目（唯一/主键索引仍急切）。
   - `IndexKeysEqual`：`InsertIntoIndexes` 增 `old_row` 参数——键未变时跳过重建（旧条目仍指向稳定 RID，复用）；`DeleteFromIndexes` 增 `new_row` 参数——键未变时跳过摘除。
   - `RestoreDeletedIndexEntries`：写堆失败后的索引恢复，只恢复**被急切摘除**的条目（延迟保留的旧条目若放回会产生同 `(key,rid)` 重复）。
2. **扫描侧**（`IndexScanExecutor`）：
   - `Init` 从目录解析被扫索引的键列（`index_key_columns_`），并清空 `seen_rids_`。
   - `Next` 回表成功后：先按 RID 去重（`seen_rids_`，RID 无默认哈希，就地补 `RidHash`）——同一逻辑行命中新旧多条目时只返回一次；再把可见版本各键列重建成 `IndexKey`，`InScanBounds` 校验其落在 `[low_key, high_key]` 扫描区间内——残留陈旧条目因可见版本键越界被精确过滤。
3. **堆侧**（`TableHeap::GetTuple`）：非快照（`commit_tracker_ == nullptr`）读时，head 若已逻辑删除（`end_xid != 0`）→ 行不可见返回 false，消除经索引回表的幽灵行。
4. **写算子接线**：`UpdateExecutor`/`UpsertExecutor` 在 `DeleteFromIndexes`/`InsertIntoIndexes` 传入 `old_row`/`new_row`，写堆失败走 `RestoreDeletedIndexEntries`。

```cpp
// IndexScanExecutor::Next 的精确过滤（节选）
if (!table_heap_->GetTuple(rid, &t, column_types_)) continue;   // 幽灵行/不可见 → 跳过
if (!index_key_columns_.empty()) {
    if (!seen_rids_.insert(rid).second) continue;               // RID 去重
    IndexKey tuple_key; /* 可见版本重建键 */ 
    if (key_ok && !InScanBounds(tuple_key)) continue;           // 键重检
}
```

### 优化后取得的进步数据（`TestSnapshotIndexScan` 实测）

| 场景 | 优化前（急切/无过滤） | 优化后 | 断言 |
|---|---|---|---|
| 键改写越界（`20→100`）后区间扫 `[10,30]` | 误返回 id=2（伪行，v=100 越界） | **正确过滤 id=2**，只剩 (1,10),(3,25) | `count_rows==2`、`val_of(2)==-999` |
| 键改写区间内（`30→25`）同一行命中新旧条目 | 返回 id=3 两次（重复行） | **RID 去重只返回一次** | `count_rows==2`（id=2 被键重检过滤的叠加） |
| 老快照读者（改写前捕获）经索引重读 | 急切摘除则**丢行** | **沿版本链读到旧值 v=20**，键重检通过 | `val_of(rA,2,1)==20` |
| 快照 DELETE 后非快照（SERIALIZABLE）读者 `WHERE v=10` | 幽灵行（读到已删行） | **0 行**（GetTuple 按 end_xid 过滤） | `count_rows(rC)==0` |
| 快照 DELETE 后新快照读者 | 同上 | **0 行**（版本链可见性） | `count_rows(rD)==0` |
| 等值查找 `v=100` | — | 正确命中新条目 | `count_rows==1`、`val_of(2)==100` |
| 表其余行 | — | 不受影响 | `count_rows(rF)==1`（(3,25)） |

**稳定性/安全性**：延迟摘除只作用于快照写者的非唯一二级索引，唯一性约束（唯一/主键索引急切维护 + 写前预检）语义完全不变；扫描侧过滤（RID 去重 + 键重检）在快照读者与非快照读者路径均生效。存储 UT 新增 `TestSnapshotIndexScan` 后 **6982 checks / 0 fails**，SQL 回归 **55 passed / 0 failed**（含 36_index_scan 索引等价性、40/46/48/49/50/51/52）。

## 关键文件
- `include/execution/IndexMaintenance.h` + `src/execution/IndexMaintenance.cpp`：`IndexDeleteDeferred`/`IndexKeysEqual`/`RestoreDeletedIndexEntries`，`InsertIntoIndexes`/`DeleteFromIndexes` 增参。
- `include/execution/IndexScanExecutor.h` + `src/execution/IndexScanExecutor.cpp`：`InScanBounds`/`RidHash`/`seen_rids_`/`index_key_columns_`。
- `src/storage_engine/TableHeap.cpp`：`GetTuple` 非快照读过滤逻辑删除 head；`Vacuum` 判据（见 MVCC_Background_Vacuum）。
- `src/execution/UpdateExecutor.cpp` / `UpsertExecutor.cpp`：索引维护传参 + 写堆失败恢复。
- `tests/storage/storage_ut.cpp`：`TestSnapshotIndexScan`。
