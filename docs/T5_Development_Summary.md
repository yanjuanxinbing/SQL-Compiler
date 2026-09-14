# T5 阶段开发总结报告 —— 收口与发布

- 项目：SQL-Compiler 小型数据库系统
- 阶段：T5「收口与发布」（`docs/Storage_Dev_Plan.md` 里程碑 T5，最后一个里程碑）
- 周期：2026-09-14
- 前置：T1–T4 里程碑 + Phase 5 周期 1–4 全部验收闭环（storage_ut 58953 checks / 0 fails）
- 结论：**全部任务完成，全量回归全绿，交付文档与实现一致**

---

## 1. 阶段目标与验收口径

| 目标 | 关键任务 | 验收标准 | 结果 |
|---|---|---|---|
| 审计闭环 | `-Wall -Wextra` 零告警 + 全量 UT + SQL 回归 + 崩溃注入 | 全绿 | ✅ |
| 验收 | 最终验收报告更新为新基线；输出 README | 交付文档与实现一致 | ✅ |
| 评审 | 对照「进程调度/内存/置换」映射，文档补上并发模型章节 | 评审通过 | ✅ |

---

## 2. 任务完成情况

### 2.1 告警审计：`-Wall -Wextra` 零告警

在独立构建目录 `build_audit/` 以 `-Wall -Wextra` 全量编译（`sqlcompiler` + `storage_ut`），最终 **0 warning / 0 error**。共清理 **234 条告警**，分类明细：

| 类别 | 数量 | 说明 |
|---|---|---|
| `-Wswitch` | 208 条 | 8 处 `switch` 枚举分支未穷尽（Token 关键字、语句类型、连接类型、字面量类型、计划节点类型等），逐处补 `default` 分支 |
| `-Wunused-function` | 5 个 | 删除未使用的匿名命名空间函数（`AST.cpp` 的 `LiteralTypeToString`/`UnaryOpToSt`、`ExecutionEngine.cpp` 的 `ToPairs`、`UdfExecutor.cpp` 的 `CoerceToType`、`OsModuleOptimizations.cpp` 的 `kBlockHdr` 等） |
| `-Wunused-variable` | 2 个 | `SystemCatalog.cpp` 的 `user_pid` 等 |
| `-Wunused-parameter` | 2 个 | `AlterTableExecutor.cpp` 的 `src_type` 等，注释标记未使用 |
| `-Wsign-compare` | 3 处 | `DiskManager.cpp`、`TableHeap.cpp`、`storage_ut.cpp` 有符号/无符号比较，显式 `static_cast` 对齐类型 |
| `-Wtype-limits` | 1 处 | `ExpressionEvaluator.cpp` 无符号数与 0 的恒真比较，改写判断逻辑 |
| `-Wreorder` | 1 处 | `DiskManager` 构造函数成员初始化顺序与声明不一致，调整初始化列表 |
| MinGW 兼容 | 2 处 | `LoopbackNetworkBlockDevice.cpp` 的 `#pragma comment(lib, "ws2_32.lib")`、`LogManager.cpp` 的 `#pragma warning` 加 `_MSC_VER` 条件编译防护（MinGW 不识别这些 pragma） |

**原则**：审计期间**不改变任何行为**——仅补默认分支、删除死代码、修复类型比较与初始化序，纯静态清理。

### 2.2 全量回归复核（行为零变化）

| 验证项 | 命令 | 结果 |
|---|---|---|
| 存储单元测试 | `build\storage_ut.exe` | **58953 checks / 0 fails，RESULT: PASS** |
| SQL 全量回归 | `tests\run_sql_regression.ps1` | **55 passed / 0 failed**（elapsed ≈ 4.23 s） |
| 崩溃注入 49_acid_recovery | 两阶段 | Phase1 exit=1（崩溃生效）→ Phase2 exit=0，未提交回滚 + 已提交持久化 |
| 崩溃注入 50_undo_clr | 两阶段 | Phase1 exit=1（撤销中途崩溃）→ Phase2 exit=0，CLR 链补做完整撤销 |

清理前后基线一致（storage_ut 均为 58953 checks），确认告警清理**零行为变化**。

### 2.3 交付文档（与实现一致）

| 文档 | 更新内容 |
|---|---|
| `README.md` | 重写为最终交付版：整体架构图、核心能力（编译器/页式存储/缓存置换/事务锁/MVCC/B+Tree 并发/诊断）、构建与测试命令、CLI 与 6 个环境变量矩阵、目录结构、验收证据表 |
| `docs/Storage_Subsystem_Acceptance_Report.md` | 从 2026-09-12 旧基线（5424 checks）全面更新至 T5 新基线（58953 checks / 55 SQL / 零告警）：新增四层并发模型架构、T2–T5 里程碑扩展验收表、告警审计记录、一键复现 |
| `docs/upload_os_module/03_测试执行记录.md` | 更新为 T5 基线，新增 §7「T5 收口审计记录」（告警清理明细 + 回归复核） |
| `docs/upload_os_module/06_需求比对与后续计划.md` | 新增 T5 里程碑完成记录；结论更新为「周期 1–4 + T1–T5 全部闭环，无剩余功能项/里程碑项」 |
| `docs/Storage_Dev_Plan.md` | T5 标记 ✅ 已完成，记录审计数据、验证结果、验收文档与证据清单 |

### 2.4 并发模型评审（对应「进程调度/内存/置换」映射）

`docs/upload_os_module/04_模块设计文档.md` 新增 **§7 并发模型**章节，作为 T5 评审交付：

- **§7.1 四层锁层次**：L4 全局/元数据层（`BPM::latch_`、`LockManager::meta_mutex_`）→ L3 分片锁层（16 shard，表/行/谓词多粒度 S/X）→ L2 页级层（Page 读写闩 + per-table `write_mutex_`）→ L1 磁盘/日志层；
- **§7.2 锁序不变量（死锁防护契约）**：持页闩不请求 BPM（分裂用「先 pin 后加闩」绕过）、`BPM::latch_ → LogManager::mutex_`、`BPM::latch_ → DiskManager::db_io_latch_`、`shard mutex → meta_mutex_`、等待不持锁（条件变量上等待）；
- **§7.3–7.5**：分片锁路由与行锁同片不变量、乐观并发兜底（乐观重启无锁等待环）、后台线程调度（后台刷脏/真空/组提交领导者-跟随者）；
- **OS 课程映射**：进程调度（后台线程调度与同步）、内存回收（MVCC 真空）、置换（LRU-K/温度感知刷盘）在 §6 映射表中同步补全。

---

## 3. 证据文件

| 文件 | 内容 |
|---|---|
| `docs/test_evidence/audit_build.log` | `-Wall -Wextra` 全量编译日志（100% 构建成功，0 warning / 0 error） |
| `docs/test_evidence/audit_cfg.log` | `build_audit/` CMake 配置日志 |
| `docs/test_evidence/storage_ut_run.log` | 存储单元测试完整输出（58953 checks / 0 fails + OS 模块性能基准） |
| `docs/test_evidence/sql_regression_run.log` | SQL 回归摘要（passed 55 / failed 0，含 49/50 崩溃注入两阶段） |
| `docs/test_evidence/t4_obs_diag_console.log` | `\stats` + `\analyze` 全指标输出（命中构成/脏页年龄/刷脏直方图/页映射） |
| `docs/test_evidence/t4_bg_flush_console.log` | 后台刷脏运行日志 |

**一键复现**：`tests\run_repro_all.ps1`（缺构建自动 cmake → storage_ut → SQL 回归（含崩溃注入）→ 参数扫描 → 性能曲线图 → 统一证据包，退出码 0=全绿）。

---

## 4. 阶段总结

- **质量收口**：以 `-Wall -Wextra` 零告警为收口标准，系统性清理 234 条静态告警而不引入任何行为变化；
- **验证闭环**：UT（58953 checks）+ SQL 回归（55/55）+ 崩溃注入（49/50 两阶段）三层全绿，与告警清理前基线一致；
- **交付一致**：README、最终验收报告、测试执行记录、需求比对、开发计划五份文档同步至同一基线，代码-文档-证据三者一致；
- **评审交付**：并发模型章节（§7）补齐「进程调度/内存/置换」的 OS 课程映射，四层锁层次 + 锁序契约 + 乐观并发 + 后台线程调度形成可评审的并发正确性论证。

**T5 是 Storage_Dev_Plan 的最终里程碑。至此 T1–T5 全部收官，Phase 5 计划全部周期（1–4）与全部里程碑（1–5）验收闭环，交付包自包含、可一键复现。**
