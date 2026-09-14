# T5 阶段交付包清单

- 项目：SQL-Compiler 小型数据库系统
- 阶段：T5「收口与发布」（`docs/Storage_Dev_Plan.md` 里程碑 T5，最终里程碑）
- 交付日期：2026-09-14
- 基线：storage_ut **58953 checks / 0 fails** · SQL 回归 **55 passed / 0 failed** · `-Wall -Wextra` **零告警**
- 用途：评审者按本清单逐项核对交付物，全部 ☑ 即验收通过；可用 `tests\run_repro_all.ps1` 一键复现全部测试与证据

---

## 1. 构建产物（build/）

| # | 交付物 | 说明 | 状态 |
|---|---|---|---|
| 1.1 | `build/sqlcompiler.exe`（≈94 MB） | 数据库主程序（编译器 + 执行引擎 + 存储 + 事务/恢复 + 并发控制），交互式 CLI | ☑ |
| 1.2 | `build/storage_ut.exe`（≈96 MB） | 存储子系统单元测试（58953 checks / 0 fails，含 OS 模块性能基准） | ☑ |

---

## 2. 源码交付（include/ + src/，与 include 一一对应）

| # | 交付物 | 说明 | 状态 |
|---|---|---|---|
| 2.1 | `include/common/` `src/common/` | 基础类型（page_id、Value、错误通道等） | ☑ |
| 2.2 | `include/storage/` `src/storage/` | 页式存储核心：`Page`/`DiskManager`/`BlockDevice` 族（File/Memory/SparseFile/LoopbackNetwork/FaultInjecting）/`BufferPoolManager`/`Replacer` 族（LRU/FIFO/CLOCK/LRU-K）/`PageAllocator`/`LockManager`（S/X+谓词+分片）/`OsModuleOptimizations` | ☑ |
| 2.3 | `include/storage_engine/` `src/storage_engine/` | `TableHeap`（MVCC 版本链 + 版本索引缓存 + 内联真空） | ☑ |
| 2.4 | `include/index/` `src/index/` | `BPlusTree` 乐观页级并发（乐观重启 + 快慢写路径 + 游标）+ `Vacuum` + `PageGuard` | ☑ |
| 2.5 | `include/catalog/` `src/catalog/` | `SystemCatalog` 元数据持久化 + `VacuumAll`（堆 + 索引真空） | ☑ |
| 2.6 | `include/txn/` `src/txn/` | `LogManager`（WAL + 组提交 + 时间窗）/`RecoveryManager`/`TransactionManager`/`CommitTracker`（O(1) 低水位） | ☑ |
| 2.7 | `include/execution/` `src/execution/` | 火山模型执行器（SeqScan/IndexScan/Filter/Project/Join/Sort/Aggregate/Insert/Update/Delete/Upsert…）+ `ExpressionEvaluator` + `ExecutionEngine` | ☑ |
| 2.8 | `include/lexer~codegen/` 对应 `src/` | 编译器链路（词法/语法/语义/逻辑计划/优化/代码生成），含复合索引前缀区间推导 | ☑ |
| 2.9 | `include/db/` `src/db/` | `Database` 门面类：`ExecuteSQL`、Session、后台刷脏/真空线程、`\stats`/`\analyze`/`\crash*` | ☑ |
| 2.10 | `CMakeLists.txt` | 构建定义（MinGW Makefiles、Debug、`ws2_32` 链接、storage_ut 目标） | ☑ |

---

## 3. 单元测试与 SQL 回归

| # | 交付物 | 说明 | 状态 |
|---|---|---|---|
| 3.1 | `tests/storage/storage_ut.cpp` | 存储单元测试源码（58953 checks，覆盖页存储/替换/缓冲池/锁/隔离级别/MVCC/B+Tree 并发/谓词树/组提交/温度刷盘/T4 介质可观测/T5 审计后复核） | ☑ |
| 3.2 | `tests/sql/`（53 条 .sql） | SQL 回归脚本：`00_smoke` ~ `52_set_isolation`（DDL/DML/JOIN/索引/事务隔离/负面用例） | ☑ |
| 3.3 | `49_acid_recovery` + `50_undo_clr`（崩溃注入两阶段） | 由回归脚本驱动：49 = 未提交回滚 + 已提交持久化；50 = CLR 链补做完整撤销 | ☑ |

---

## 4. 测试与复现脚本（tests/）

| # | 交付物 | 说明 | 状态 |
|---|---|---|---|
| 4.1 | `tests/run_sql_regression.ps1` | 全量 SQL 回归 + 崩溃注入复刻脚本（UTF-8 字节级 stdin，逐脚本独立建库，exit 0 且无 `Error:` 即通过） | ☑ |
| 4.2 | `tests/run_repro_all.ps1` | **一键复现**：缺构建自动 cmake → storage_ut → SQL 回归（含崩溃注入）→ 参数扫描 → 性能曲线图 → 统一证据包（退出码 0=全绿） | ☑ |
| 4.3 | `tests/run_param_sweep.ps1` | 参数扫描实验（10 组合：缓冲帧数/温度刷盘/后台刷脏/组提交窗口/后台真空等） | ☑ |
| 4.4 | `tests/make_sweep_charts.ps1` | 解析 `param_sweep.log` 生成 4 张性能曲线图 + 控制台演示截图 | ☑ |
| 4.5 | `tests/run_all_tests.bat` / `run_acid_recovery.bat` / `run_acid_clr.bat` | Windows 批处理回归入口（崩溃注入两阶段） | ☑ |
| 4.6 | `tests/dump_wal.cpp` + `baseline_md5*.txt` / `current_md5.txt` | WAL 转储工具与 MD5 校验基线（磁盘格式兼容性证据） | ☑ |

---

## 5. 测试证据（docs/test_evidence/，18 个文件）

| # | 交付物 | 说明 | 状态 |
|---|---|---|---|
| 5.1 | `storage_ut_run.log` | 存储单元测试完整输出（58953 checks / 0 fails + OS 模块性能基准） | ☑ |
| 5.2 | `sql_regression_run.log` | SQL 回归日志（passed 55 / failed 0，含 49/50 崩溃注入两阶段） | ☑ |
| 5.3 | `audit_build.log` + `audit_cfg.log` | `-Wall -Wextra` 全量编译日志（0 warning / 0 error）+ 构建配置日志 | ☑ |
| 5.4 | `t4_obs_diag_console.log` | `\stats` + `\analyze` 全指标输出（命中构成/脏页年龄/刷脏直方图/页映射/CRC 计数） | ☑ |
| 5.5 | `t4_bg_flush_console.log` | 后台刷脏运行日志（ticks 与直方图记账） | ☑ |
| 5.6 | `param_sweep.log` + `param_sweep_console.log` | 参数扫描 10 组合结果与控制台输出 | ☑ |
| 5.7 | `sweep_hitrate.png` / `sweep_fsync.png` / `sweep_writeback.png` / `sweep_elapsed.png` | 性能曲线图（命中率 vs 缓冲池 / WAL fsync / 写回 cold-hot 分流 / 耗时） | ☑ |
| 5.8 | `demo_console.png` | 推荐配置下建表/查询/更新演示运行截图 | ☑ |
| 5.9 | `repro_summary.log` + `build.log` + `storage_ut.log` + `sql_regression_console.log` + `charts_console.log` | `run_repro_all.ps1` 一键复现汇总与各环节日志 | ☑ |

---

## 6. 交付文档（docs/）

| # | 交付物 | 说明 | 状态 |
|---|---|---|---|
| 6.1 | `README.md` | 项目总入口：架构/能力/构建测试/CLI/环境变量矩阵/目录结构/验收证据（最终交付版） | ☑ |
| 6.2 | `docs/Storage_Dev_Plan.md` | 后续开发计划 T1–T5，全部标记 ✅ 完成并附实现与证据记录 | ☑ |
| 6.3 | `docs/Storage_Subsystem_Acceptance_Report.md` | **最终验收报告**（58953 checks / 55 SQL / 零告警，T5 新基线） | ☑ |
| 6.4 | `docs/T5_Development_Summary.md` | **T5 阶段开发总结报告**（审计明细/全量验证/文档交付/并发模型评审/证据清单） | ☑ |
| 6.5 | `docs/upload_os_module/04_模块设计文档.md` | 模块设计文档 + **§7 并发模型**（四层锁层次/锁序不变量/分片锁/乐观并发/后台线程调度/OS 映射） | ☑ |
| 6.6 | `docs/upload_os_module/03_测试执行记录.md` | 测试执行记录（T5 基线 + §7 审计记录） | ☑ |
| 6.7 | `docs/upload_os_module/06_需求比对与后续计划.md` | 指导书逐条比对（100% 覆盖）+ G1–G7 闭环 + 周期 1–4 与 T1–T5 完成记录 | ☑ |
| 6.8 | `docs/upload_os_module/07_环境变量与推荐配置.md` | 环境变量矩阵与推荐配置、演示启动命令 | ☑ |
| 6.9 | `docs/upload_os_module/00~02`（总览索引/源码文档清单/功能测试方案） | 交付索引与测试依据 | ☑ |
| 6.10 | `docs/upload_os_module/附录_历史文档/` | 历史开发计划/设计文档/阶段验收报告（Phase 3/4、隔离级别深研等存档） | ☑ |

---

## 7. 一键验收流程

```powershell
# 1) 构建（Debug，MinGW）
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=D:/mingw64/bin/g++.exe
cmake --build build -- -j8

# 2) 存储单元测试（58953 checks / 0 fails）
.\build\storage_ut.exe

# 3) 全量 SQL 回归 + 崩溃注入（55 passed / 0 failed）
powershell -ExecutionPolicy Bypass -File tests\run_sql_regression.ps1

# 4) 一键复现（构建 → UT → 回归 → 参数扫描 → 曲线图 → 证据包，退出码 0=全绿）
powershell -ExecutionPolicy Bypass -File tests\run_repro_all.ps1
```

---

## 8. 交付结论

| 验收项 | 结果 |
|---|---|
| 存储单元测试 | **58953 checks / 0 fails**（`storage_ut_run.log`） |
| SQL 全量回归 | **55 passed / 0 failed**（53 条脚本 + 49/50 崩溃注入两阶段） |
| 告警审计 | `-Wall -Wextra` **0 warning / 0 error**（`audit_build.log`） |
| 崩溃恢复 | 49（未提交回滚 + 已提交持久化）、50（CLR 链补做撤销）两阶段跨重启通过 |
| 性能基准 | B+Tree 并发写 3.18×（16 线程）、组提交 fsync 10.7× 削减、快照低水位约 465×、分片锁异表 5.8× |
| 文档一致性 | README / 验收报告 / 开发计划 / 测试记录 / 设计文档 / 需求比对 / 总结报告七件套与实现一致 |

**T5 交付包自包含、可复现，全部验收项通过，准予交付。**
