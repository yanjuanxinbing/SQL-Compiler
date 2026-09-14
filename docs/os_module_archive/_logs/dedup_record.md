# 去重记录与合并归档操作日志

- 操作日期：2026-09-15
- 操作人：自动化脚本（git mv 批处理 + PowerShell 索引）
- 范围：`docs/upload_os_module/`（21 份） + `docs/upload_os_module/附录_历史文档/`（23 份） = **44 份**
- 新位置：`docs/os_module_archive/{01-08}_*/`

---

## 2. 去重检测结果

### 2.1 物理哈希去重

工具：`Get-FileHash -Algorithm SHA256`
覆盖：44 份文件
结果：**0 对完全重复**（所有 hash 唯一）

### 2.2 内容相似度检测（人工精读）

对 44 份文件做语义簇分析，扫描了 6 组命名/主题相邻候选：

| 候选组 | 对比文件 | 检测结论 | 处理 |
|---|---|---|---|
| 治理入口类 | 05 vs 20 | 20 是目录索引+规范、05 是归类副本策略；视角不同 | 保留两份 |
| 时间线类 | 18 vs 03+06+08 | 18 是 A 类入口（时间线），03 是测试执行数据；视角不同 | 保留 18 |
| 机制对比类 | 14 vs 19 | 14 是单机制纵深（备选方案），19 是跨机制横向（总览）；互补 | 保留两份 |
| 附录 MVCC 4 份 | Snapshot_LowWater / MVCC_Phase3 / MVCC_Background / MVCC_snapshot | 主题各异（水位/版本链/后台真空/SI 完整） | 保留 4 份 |
| 附录 Isolation 3 份 | PredicateLock_Merge / RowLock_Escalation / 隔离级别全面 | 主题各异（合并/升级/总览） | 保留 3 份 |
| 附录验收报告 4 份 | Storage / Phase3 / Phase4 / Isolation_DeepDev | 验收轮次各异（总/Phase3/Phase4/隔离深入） | 保留 4 份 |

### 2.3 去重结论

**0 份被识别为"内容完全重复或高度相似到可删除其一"**。所有 44 份文件具有独立视角、时点或细节增量，删除任一份均会损失信息。

依据用户决策"执行删除与归类（仅限明显重复者）"——既然无明显重复对象，则**全部 44 份保留**，按预设分类体系归类。

---

## 3. 被删除的文档清单

**无**。本轮操作未删除任何源文件。

---

## 4. 合并与归类映射表（44 → 8 类）

| 类别 | 数量 | 源路径 → 目标路径 |
|---|---|---|
| **01_开发日志** | 4 | 见下表 01 |
| **02_功能特色介绍** | 4 | 见下表 02 |
| **03_设计文档** | 14 | 见下表 03 |
| **04_测试与验收** | 5 | 见下表 04 |
| **05_使用说明** | 2 | 见下表 05 |
| **06_项目治理** | 8 | 见下表 06 |
| **07_需求与计划** | 6 | 见下表 07 |
| **08_项目依据** | 1 | 见下表 08 |

### 4.1 01_开发日志（4）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `docs/upload_os_module/18_开发优化日志_时间线.md` | `docs/os_module_archive/01_开发日志/18_开发优化日志_时间线.md` |
| 2 | `docs/upload_os_module/08_阶段性工作总结报告.md` | `docs/os_module_archive/01_开发日志/08_阶段性工作总结报告.md` |
| 3 | `docs/upload_os_module/附录_历史文档/03_测试与验收报告/Isolation_DeepDev_Summary.md` | `docs/os_module_archive/01_开发日志/Isolation_DeepDev_Summary.md` |
| 4 | `docs/upload_os_module/附录_历史文档/03_测试与验收报告/Storage_Subsystem_Acceptance_Report.md` | `docs/os_module_archive/01_开发日志/Storage_Subsystem_Acceptance_Report.md` |

### 4.2 02_功能特色介绍（4）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `docs/upload_os_module/13_模块功能展示文档.md` | `docs/os_module_archive/02_功能特色介绍/13_模块功能展示文档.md` |
| 2 | `docs/upload_os_module/15_展示流程与脚本.md` | `docs/os_module_archive/02_功能特色介绍/15_展示流程与脚本.md` |
| 3 | `docs/upload_os_module/19_模块机制功能对比总览.md` | `docs/os_module_archive/02_功能特色介绍/19_模块机制功能对比总览.md` |
| 4 | `docs/upload_os_module/附录_历史文档/04_优化与评审/Optimization_Stage_Report.md` | `docs/os_module_archive/02_功能特色介绍/Optimization_Stage_Report.md` |

### 4.3 03_设计文档（14）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `docs/upload_os_module/04_模块设计文档.md` | `docs/os_module_archive/03_设计文档/04_模块设计文档.md` |
| 2 | `docs/upload_os_module/14_核心机制备选方案对比分析.md` | `docs/os_module_archive/03_设计文档/14_核心机制备选方案对比分析.md` |
| 3 | `附录/02_设计文档/Page_Management_Design.md` | `docs/os_module_archive/03_设计文档/Page_Management_Design.md` |
| 4 | `附录/02_设计文档/BPlusTree_Optimistic_Page_Concurrency.md` | `docs/os_module_archive/03_设计文档/BPlusTree_Optimistic_Page_Concurrency.md` |
| 5 | `附录/02_设计文档/Snapshot_LowWater_InlineVacuum.md` | `docs/os_module_archive/03_设计文档/Snapshot_LowWater_InlineVacuum.md` |
| 6 | `附录/02_设计文档/MVCC_Phase3_O1_AutoCleanup.md` | `docs/os_module_archive/03_设计文档/MVCC_Phase3_O1_AutoCleanup.md` |
| 7 | `附录/02_设计文档/Phase4_PredicateTree_GroupCommit_TempFlush.md` | `docs/os_module_archive/03_设计文档/Phase4_PredicateTree_GroupCommit_TempFlush.md` |
| 8 | `附录/02_设计文档/MVCC_snapshot_isolation.md` | `docs/os_module_archive/03_设计文档/MVCC_snapshot_isolation.md` |
| 9 | `附录/02_设计文档/Secondary_Index_MVCC_Visibility.md` | `docs/os_module_archive/03_设计文档/Secondary_Index_MVCC_Visibility.md` |
| 10 | `附录/02_设计文档/MVCC_Background_Vacuum.md` | `docs/os_module_archive/03_设计文档/MVCC_Background_Vacuum.md` |
| 11 | `附录/02_设计文档/Isolation_PredicateLock_Merge.md` | `docs/os_module_archive/03_设计文档/Isolation_PredicateLock_Merge.md` |
| 12 | `附录/02_设计文档/Isolation_RowLock_Escalation.md` | `docs/os_module_archive/03_设计文档/Isolation_RowLock_Escalation.md` |
| 13 | `附录/02_设计文档/隔离级别全面行级并发与谓词锁.md` | `docs/os_module_archive/03_设计文档/隔离级别全面行级并发与谓词锁.md` |
| 14 | `附录/02_设计文档/isolation-set-and-rowlock.md` | `docs/os_module_archive/03_设计文档/isolation-set-and-rowlock.md` |

### 4.4 04_测试与验收（5）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `docs/upload_os_module/02_功能测试方案.md` | `docs/os_module_archive/04_测试与验收/02_功能测试方案.md` |
| 2 | `docs/upload_os_module/03_测试执行记录.md` | `docs/os_module_archive/04_测试与验收/03_测试执行记录.md` |
| 3 | `docs/upload_os_module/12_模块功能测试用例明细.md` | `docs/os_module_archive/04_测试与验收/12_模块功能测试用例明细.md` |
| 4 | `附录/03_测试与验收报告/Phase3_Acceptance_Report.md` | `docs/os_module_archive/04_测试与验收/Phase3_Acceptance_Report.md` |
| 5 | `附录/03_测试与验收报告/Phase4_Acceptance_Report.md` | `docs/os_module_archive/04_测试与验收/Phase4_Acceptance_Report.md` |

### 4.5 05_使用说明（2）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `docs/upload_os_module/07_环境变量与推荐配置.md` | `docs/os_module_archive/05_使用说明/07_环境变量与推荐配置.md` |
| 2 | `附录/05_使用说明/QUICKSTART.md` | `docs/os_module_archive/05_使用说明/QUICKSTART.md` |

### 4.6 06_项目治理（8）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `docs/upload_os_module/00_总览_索引.md` | `docs/os_module_archive/06_项目治理/00_总览_索引.md` |
| 2 | `docs/upload_os_module/01_源码与文档清单.md` | `docs/os_module_archive/06_项目治理/01_源码与文档清单.md` |
| 3 | `docs/upload_os_module/05_文档整合归类.md` | `docs/os_module_archive/06_项目治理/05_文档整合归类.md` |
| 4 | `docs/upload_os_module/16_代码同步与合并预案.md` | `docs/os_module_archive/06_项目治理/16_代码同步与合并预案.md` |
| 5 | `docs/upload_os_module/17_文件处理动作清单.md` | `docs/os_module_archive/06_项目治理/17_文件处理动作清单.md` |
| 6 | `docs/upload_os_module/20_文档目录索引与归档说明.md` | `docs/os_module_archive/06_项目治理/20_文档目录索引与归档说明.md` |
| 7 | `附录/04_优化与评审/OS_Module_CodeReview_Report.md` | `docs/os_module_archive/06_项目治理/OS_Module_CodeReview_Report.md` |
| 8 | `附录/04_优化与评审/OS_Module_Optimization_Proposal.md` | `docs/os_module_archive/06_项目治理/OS_Module_Optimization_Proposal.md` |

### 4.7 07_需求与计划（6）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `docs/upload_os_module/06_需求比对与后续计划.md` | `docs/os_module_archive/07_需求与计划/06_需求比对与后续计划.md` |
| 2 | `docs/upload_os_module/09_下一阶段开发计划.md` | `docs/os_module_archive/07_需求与计划/09_下一阶段开发计划.md` |
| 3 | `docs/upload_os_module/10_Phase6_U1_索引删除合并与再平衡_阶段文档.md` | `docs/os_module_archive/07_需求与计划/10_Phase6_U1_索引删除合并与再平衡_阶段文档.md` |
| 4 | `docs/upload_os_module/11_Phase6_U3_查询优化增强_阶段文档.md` | `docs/os_module_archive/07_需求与计划/11_Phase6_U3_查询优化增强_阶段文档.md` |
| 5 | `附录/01_开发计划/OS_Module_Development_Plan.md` | `docs/os_module_archive/07_需求与计划/OS_Module_Development_Plan.md` |
| 6 | `附录/01_开发计划/Storage_Dev_Plan.md` | `docs/os_module_archive/07_需求与计划/Storage_Dev_Plan.md` |

### 4.8 08_项目依据（1）

| 序号 | 源 | 目标 |
|---|---|---|
| 1 | `附录/00_项目依据/Manual.md` | `docs/os_module_archive/08_项目依据/Manual.md` |

---

## 5. 迁移执行结果

- `git mv` 调用总数：44
- 成功：44
- 失败：0
- `git status` 识别：**44 个 R（rename）**，git 历史完整保留
- 旧目录 `docs/upload_os_module/` 已删除（无剩余内容）

## 6. 后续

- 新归档根索引：`docs/os_module_archive/22_新归档索引.md`
- 操作日志：本文件