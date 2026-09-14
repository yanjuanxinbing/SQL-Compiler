# 隔离级别深入开发 · 轮次总结报告

- 项目：小型数据库系统 SQL-Compiler（编译原理 + 操作系统 + 数据库 三模块综合实训）
- 轮次范围：隔离级别深入开发（谓词锁合并 / 行锁升级 / MVCC 后台真空 / 二级索引 MVCC 精确可见性）
- 依据：`docs/Storage_Dev_Plan.md` 既定路线图（T2 多连接并发事务 → 隔离级别深化）
- 结论：**全部完成，测试全绿（UT 6982 checks / 0 fails，SQL 回归 55 passed / 0 failed）**

---

## 1. 轮次目标

沿 Storage_Dev_Plan 推进隔离级别的四项机制深化，每项遵循「实现 → 全面测试（UT + SQL 回归 + 崩溃注入）→ 撰写含前后对比数据的技术文档」的完整闭环：

| 编号 | 机制 | 核心价值 |
|---|---|---|
| t1 | SERIALIZABLE 谓词锁：父子区间继承与合并 | 长事务谓词锁集合从「随语句膨胀」收敛为「不重叠区间」 |
| t2 | 行级锁升级（行→表多粒度收敛） | 批量写事务锁条目从 O(行) 收敛到 O(表) |
| t3 | MVCC 多版本真空：独立后台线程 + 水位边界修正 | 版本链及时回收且不误删活动快照所需版本 |
| t4 | 二级索引 MVCC 精确可见性 | 消除索引场景的丢行 / 幽灵行 / 伪行 / 重复行 |

---

## 2. 机制实现摘要

### t1 谓词锁：父子区间继承与合并
- **瓶颈**：区间谓词只增不减；全表谓词不覆盖子区间；重叠区间不合并 → 锁表、等待图、`CheckWritePredicate` 扫描全部线性膨胀。
- **方案**：`AcquireReadPredicate` 集合规约——全表父谓词覆盖一切子谓词；新区间为全表时删除全部区间；区间按 `lo` 排序合并真重叠为不重叠最小区间覆盖（合并输出用独立容器防误判重叠）。
- **数据**：重叠区间 `[10,20]+[15,25]` → **1 条**；全表谓词后区间全收敛 → **1 条**；覆盖能力经 `CheckWritePredicate` 验证不变。

### t2 行级锁升级
- **瓶颈**：万行批量写 = 万条行锁条目；`UnlockAll` 与死锁检测随行数线性放大。
- **方案**：`AcquireRowWriteLock` 登记行归属（`RegisterRowGroup`），达阈值 128 触发 `TryEscalateTable`——无他人冲突则授予表 X 锁并释放全部行锁，后续行访问 O(1)；升级失败（`kWouldBlock`）回退逐行持锁，正确性不变。
- **数据**：200 行锁 → 升级后 **`CountRowLocks==0`（1 条表锁）**；幂等重入；多粒度矩阵（表X vs 行S/X 冲突、表S vs 行S 兼容）全绿。

### t3 MVCC 多版本真空后台线程
- **瓶颈**：真空仅前台每 100 语句触发、版本链膨胀；`begin_csn` 回收判据误删「创建早、结束晚」的版本（老读者丢行）；`OldestActiveSnapshot` 依赖 `unordered_map::begin()` 迭代序（误删活动快照所需版本）；非快照读残留共享堆水位（陈旧读泄漏）。
- **方案**：`Database` 独立后台真空线程（`bg_vacuum_ms` 可配，默认关闭，复刻后台刷脏模式）；判据修正为 **`end_csn ≤ oldest_active_csn`**；水位显式求最小 CSN；非快照/自动提交读统一 `SetSnapshot(-1,nullptr)` 复位；`Shutdown` 幂等。
- **数据**：真空后槽位 **3 → 1**（边界版本保留）；老快照 S=3 读 v=30 不丢行；线程 ticks ≥ 3；无活动快照 no-op 保守。

### t4 二级索引 MVCC 精确可见性
- **瓶颈**：快照写者键改写/删除与二级索引维护不一致——急切摘除使老快照读者丢行；保留条目又让非快照读者读幽灵行、范围扫描命中越界伪行、同一 RID 重复返回。
- **方案**：非唯一二级索引延迟摘除（`IndexDeleteDeferred`，唯一/主键索引保持急切）；`IndexScanExecutor` 回表后 **RID 去重 + 可见版本键重检（`InScanBounds`）**；键未变跳过重建（`IndexKeysEqual`）；写堆失败 `RestoreDeletedIndexEntries`；`GetTuple` 非快照读过滤逻辑删除 head。
- **数据**：键改写越界 → 区间扫描正确过滤 id=2；区间内改写 → 同一行只返回一次；老快照沿链读到旧值 v=20；快照删除 → 非快照/新快照读者均 **0 行**（幽灵行消除）；等值 `v=100` 正常命中。

---

## 3. 测试证据

### 3.1 存储单元测试（storage_ut.exe）
| 里程碑 | checks | fails |
|---|---|---|
| 轮次起点（快照 DELETE/UPSERT FCW 收官） | 6658 | 0 |
| 本轮新增：`TestPredicateLockMerge` / `TestRowLockEscalation` / `TestBackgroundVacuumThread` / `TestVacuumReclaimsOldVersions` / `TestSnapshotIndexScan` | **6982** | **0** |

净增 **324 checks**；连续多次复跑稳定（无 flaky，含时序型后台真空测试）。

### 3.2 SQL 回归（tests/run_all_tests.bat）
- **55 passed / 0 failed**：53 条 SQL 脚本（00–52）+ `run_acid_recovery` + `run_acid_clr` 崩溃注入。
- 隔离级别重点用例 40 / 46 / 48 / 49 / 50 / 51 / 52 全部 exit 0；36_index_scan（索引等价性）、43_upsert、52_set_isolation 无回归。

### 3.3 崩溃注入（含于 run_all_tests）
- `49_acid_recovery`：WAL 恢复——未提交 UPDATE 回滚、已提交 UPDATE 持久化。
- `50_undo_clr`：CLR 链 + 中途崩溃——ROLLBACK 补做全部 undo，行 4/5/6 不回显。

---

## 4. 前后对比汇总

| 维度 | 优化前 | 优化后 |
|---|---|---|
| 谓词锁条目（长事务多区间） | 随语句数线性膨胀 | 收敛为不重叠区间数（2→1） |
| 批量写锁条目 | O(行数) | O(表数)（200→1） |
| 旧版本回收判据 | begin_csn（误删风险） | end_csn ≤ 最老活动快照（精确） |
| 最老快照水位 | unordered_map 迭代序（不确定） | 显式最小 CSN（确定） |
| 真空触发 | 前台、稀疏 | 独立后台线程、周期 tick |
| 二级索引键改写/删除 | 丢行 / 幽灵行 / 伪行 / 重复行 | 全部消除（键重检 + RID 去重 + end_xid 过滤） |
| 陈旧读泄漏 | 跨语句读旧快照 | 非快照读统一复位水位 |

---

## 5. 文件改动清单

**本轮（t4）**：`IndexMaintenance.h/.cpp`、`IndexScanExecutor.h/.cpp`、`UpdateExecutor.cpp`、`UpsertExecutor.cpp`、`TableHeap.cpp`（GetTuple 非快照过滤 + Vacuum 判据）、`tests/storage/storage_ut.cpp`。
**前序（t1–t3）**：`LockManager.h/.cpp`（谓词合并、行锁升级）、`Executor.cpp`（升级触发）、`Database.h/.cpp`（后台真空线程）、`CommitTracker.h`（水位修正）、各扫描/写算子（非快照读复位）。

**技术文档（本 .trae/documents/ 下新增 4 份）**：
- `Isolation_PredicateLock_Merge.md`（t1）
- `Isolation_RowLock_Escalation.md`（t2）
- `MVCC_Background_Vacuum.md`（t3）
- `Secondary_Index_MVCC_Visibility.md`（t4）
- 并同步更新 `docs/Storage_Dev_Plan.md`（进度 → 6982 checks / 55 回归，已知限制）。

---

## 6. 已知限制（后续扩展候选）

1. 谓词锁仍作用于主键键空间（非主键谓词不注册）；
2. B+Tree 树锁仍为树级粗粒度（页级 latch crabbing 未做，写者彼此串行）；
3. 多列索引仅单列最左列参与区间推导；
4. 版本链回收依赖后台真空（无立即空间归还保证）；
5. 锁升级阈值固定 128（未按表行数自适应）。

---

## 7. 结论

本轮四项隔离级别机制全部按「实现 → 测试 → 文档」闭环交付：存储 UT **6982 checks / 0 fails**、SQL 回归 **55 passed / 0 failed**（含崩溃注入）、4 份技术文档 + 开发计划同步更新。隔离级别体系（READ UNCOMMITTED / READ COMMITTED / SERIALIZABLE / SNAPSHOT）在正确性、稳定性与可扩展性上达到计划验收标准。
