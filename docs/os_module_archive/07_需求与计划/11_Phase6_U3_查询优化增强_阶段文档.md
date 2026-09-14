# Phase 6 · U3 — 查询优化增强（阶段性文档）

> 归属：操作系统页面管理模块 · Phase 6（里程碑 U1–U5）
> 状态：**U3-1（JOIN 顺序启发式）、U3-2（聚合下推）、U3-3（子查询去关联）已闭环**（2026-09-14）
> 关联：`09_下一阶段开发计划.md`（U3 一行）、`01_源码与文档清单.md`（新增源码登记）
> 证据：`docs/test_evidence/join_reorder_u3_1.log`、`docs/test_evidence/preagg_pushdown_u3_2.log`、`docs/test_evidence/subquery_decorrelate_u3_3.log`

---

## 1. 关闭的需求缺口

| 编号 | 缺口 | 本阶段处理 |
|---|---|---|
| N5 | 优化器仅基础谓词下推/索引路径，JOIN 顺序、聚合下推、子查询效率可提升 | U3-1：全 INNER 左深 JOIN 链按估计基数小表驱动重排；U3-2：单表裸 COUNT/SUM 聚合下推至扫描层预聚合；U3-3：相关子查询（EXISTS/IN/ANY）改写 SEMI/ANTI JOIN + 非相关子查询物化缓存 + 递归 CTE 迭代边界失效缓存 |

---

## 2. U3-2 本阶段实现的功能与技术细节（聚合下推）

### 2.1 总体设计

U3-2 目标：把「**单表 + 全部裸 COUNT/SUM 聚合**」的 `AggregateNode` 改写为 `PreAggScanNode`，
由 `PreAggScanExecutor` 在扫描循环内**直接累计聚合状态**、以**哈希表分组**，替代
`AggregateExecutor` 的「先构造逐行 Tuple 流水 → 线性扫描分组（O(组数) 键比较）」。

计划形态（改写前后对比，EXPLAIN 实证）：

```
改写前（AggregateExecutor 路径）          改写后（PreAggScanExecutor 路径）
Project(grp, COUNT(1), SUM(val))         Project(grp, COUNT(1), SUM(val))
  Aggregate(GROUP BY [grp], AGG [...])     PreAggScan(GROUP BY [grp], AGG [...])
    SeqScan(t1)                              SeqScan(t1)
```

`PreAggScanNode` **继承 `AggregateNode`** 是关键设计决策：执行引擎中所有按 `AggregateNode`
读取 `aggregate_exprs`/`aliases` 的下游代码（HAVING Filter 的 cmap、Project 直通判断、
Sort 的 cmap、`DeriveTerminalColumns` 列名推导）无需任何分支即可透明复用。

### 2.2 优化器改写（`Optimizer::PushDownAggregates`）

1. **资格判定 `IsPreAggEligible`（三重保守守底）**：
   - ① 子树为**单表访问路径**：`SeqScan` 或 `Filter* -> SeqScan`；JOIN / Project / 无 FROM 一律不改写；
   - ② 每个 `aggregate_expr` 为「**裸 COUNT/SUM**（`IsBareCountSumExpr`：无 DISTINCT、参数 ≤1、
     参数不含嵌套聚合/窗口调用）」或「**不含任何聚合调用的普通表达式**」（典型为 GROUP BY 列，
     输出阶段在组内样本行上求值）；
   - ③ GROUP BY 表达式不含聚合调用（防御）。
   - 含 AVG/MIN/MAX、COUNT(DISTINCT)、COALESCE(SUM(x),0) 标量包装、多表 JOIN 聚合等任一不合格项 → **整棵保持 Aggregate 零回归**。
2. **改写时机**：在 `ChooseAccessPaths` **之前**执行（资格判定基于改写前的 Filter/SeqScan 形态）；
   改写后子树仍可被后续索引路径改写（Filter→IndexScan），IndexScan 输出全表行，PreAggScan 列解析不受影响。

### 2.3 执行器（`PreAggScanExecutor`）

- 状态集 `AggState` 仅保留 COUNT/SUM 所需：`count`（COUNT(*) 计行）、`count_non_null`（COUNT(col)）、
  `sum`（数值累计，INT 转 double 与 FLOAT 统一）、`any_numeric`（SUM 空输入输出 NULL 的判据）。
- 分组用 `unordered_map<string,size_t>`（分组键串 → 组下标）：**O(1) 均摊分组查找**，
  替代 AggregateExecutor 对 `groups_` 的线性扫描 + 每轮重新拼接键串。
- 与 AggregateExecutor 语义**逐项对齐**（含空输入）：无 GROUP BY 且输入为空仍发射初始状态行
  （COUNT=0、SUM=NULL）；`COUNT(col)` 不计 NULL；`SUM(col)` 仅数值列累加，非数值列只计非 NULL 数；
  组内样本行输出普通表达式（与 Aggregate 一致取最近一行）。
- 上层 HAVING / Project / Sort / Limit 的列映射全部复用既有 Aggregate 路径，无需新代码。

### 2.4 测试覆盖（`TestPreAggPushDown`，52 项断言）

| 组 | 断言 |
|---|---|
| 改写生效 | `COUNT(*)`、`GROUP BY grp, COUNT(*), SUM(val)` 的 EXPLAIN 含 `PreAggScan` |
| 语义等价 | COUNT(*)=4 / COUNT(col)=3（NULL 不计）/ SUM=600.0 / 分组三组含 SUM(NULL)→NULL / ORDER BY |
| 上下文 | WHERE 先过滤后聚合、HAVING 作用于聚合输出 |
| 零改写守底 | AVG、COUNT(DISTINCT)、COALESCE 包装、JOIN 聚合的 EXPLAIN 不含 `PreAggScan` 且结果正确 |
| 空表 | COUNT(*)=0、SUM(x)=NULL |

---

## 3. 与备选方案的对比（技术优越性 / 性能优势 / 适用场景）

U3-2 聚合下推曾考虑三种方案：

| 备选方案 | 本方案（PreAggScan） | 优劣势 |
|---|---|---|
| **A：复用 AggregateExecutor，仅把计划节点标记为「下推」** | 新建 PreAggScanExecutor，扫描循环内累计 + 哈希分组 | 方案 A 无任何性能收益（仍构造逐行 Tuple 流水、仍线性分组），只是换了个名字；本方案省一次行流水构造、分组 O(1)，且状态集裁剪到 COUNT/SUM 最小（内存减半以上） |
| **B：两阶段聚合（部分聚合 + 最终聚合）** | 单阶段扫描内预聚合 | 两阶段需物化中间结果并二次扫描，单会话串行扫描下中间态无分摊价值（组提交实验已证明本引擎单会话串行路径摊不开并行收益）；复杂度高、可维护性差。多线程并行扫描聚合属 U5 之后的并行执行课题，届时再评估 |
| **C：全函数下推（COUNT/SUM/AVG/MIN/MAX 全进扫描）** | 只下推**裸 COUNT/SUM** | AVG 需两态（sum+count）、MIN/MAX 需逐值比较与 DISTINCT 语义，下推后状态机复杂度剧增且收益占比小；保守限定 COUNT/SUM 保证与 AggregateExecutor **逐项语义等价**（含 NULL/空输入边界），回归风险最小 |

性能/可靠性/可维护性结论：
- **性能**：分组从 O(行数×组数) 线性扫描降为 O(行数) 均摊（哈希）；省掉逐行 Tuple 构造与
  `GroupKeyOf` 的每行重复拼接；状态集仅 4 字段/项。
- **可靠性**：资格判定保守，任何不确定形态回退 Aggregate；COUNT/SUM 边界语义（空表、全 NULL 列、
  SUM(NULL)、NULL 输入）与 AggregateExecutor 逐项一致，UT 52 项断言 + SQL 回归 55/55 兜底。
- **可维护性**：PreAggScanNode 继承 AggregateNode，下游零改动；执行器独立成文件、职责单一；
  后续如要扩展 AVG/MIN/MAX 下推只需扩 `AggState` 与资格判定，不触碰上层。

---

## 4. 测试结果与问题修复记录

### 4.1 质量基线（U3-2 验收）

- 构建：`cmake -S . -B build`（file(GLOB) 重新拾取 `PreAggScanExecutor.cpp`）+ `cmake --build build -j 8`：**0 错误 0 告警**（`-Wall -Wextra`）。
- storage_ut：**292554 checks / 0 fails**（相对 U3-1 基线 292502，新增 TestPreAggPushDown 52 项断言）。
- SQL 回归：**PASSED=55 FAILED=0**。
- CLI 冒烟：EXPLAIN 断言 + 结果一致性（见证据日志 §3）。

### 4.2 问题修复记录

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | `ExecutionEngine` 链接缺 PreAggScanExecutor | BuildExecutor 尚无 PRE_AGG_SCAN 分支、头文件未包含（初始实现遗留） | 补 `#include "execution/PreAggScanExecutor.h"` 与 `case PlanNodeType::PRE_AGG_SCAN` 分支（cmap 面向子扫描列序构建） |
| 2 | 集成冒烟确认 `COUNT(1)`（解析器对 `COUNT(*)` 的归一化形态）计数正确 | PreAggScanExecutor 对非空参数走 `Evaluate(args[0])` 路径，字面量 1 恒非空 → 计行 | 与 AggregateExecutor 行为天然一致（`COUNT(1)` 等价 `COUNT(*)`），冒烟实测 `SELECT COUNT(*)`=4 正确，无需改码 |

未发现遗留问题：合格形态改写/语义/上下文/零改写守底/空表全部一次通过。

---

## 5. U3-3 本阶段实现的功能与技术细节（子查询去关联）

### 5.1 总体设计

U3-3 目标：把 WHERE 中「**可静态证明安全**」的相关子查询从 Filter 谓词中抽出，改写为
**SEMI / ANTI JOIN**（`EXISTS → SEMI`、`NOT EXISTS → ANTI`、`IN/ANY → SEMI`），消除逐行
重跑子计划；**非相关子查询**由执行引擎**物化缓存**（首行求值一次，后续 O(1) 命中）；
**递归 CTE** 每轮迭代边界失效缓存，防止陈旧结果。

计划形态（改写前后对比，EXPLAIN 实证）：

```
改写前（逐行 EvaluateSubquery）                改写后（SEMI/ANTI JOIN）
Project(name)                                Project(name)
  Filter(EXISTS (SELECT 1 FROM orders o        Join(SEMI, (o.cust_id = c.id))
                  WHERE o.cust_id = c.id))       SeqScan(customers c)
    SeqScan(customers c)                         SeqScan(orders o)
```

`JoinType::SEMI/ANTI` 由 `JoinExecutor` 半/反连接执行（只输出左行，右表命中/未命中即判），
等价于相关子查询的「存在性」语义，且连接条件只按行求值一次。

### 5.2 优化器改写（`Optimizer::DecorrelateSubqueries` / `TryDecorrelateFilter`）

1. **入口顺序**：`Optimize()` 最先执行 `DecorrelateSubqueries`（在 PushDownPredicates /
   ReorderJoins / PushDownAggregates / ChooseAccessPaths 之前），改写后的 Join 子树仍可被
   后续索引路径与谓词下推继续优化。
2. **形态守卫 `IsSubqueryDecorrelatable`**：内层**单表**、无 GROUP BY/HAVING/ORDER BY/
   DISTINCT/LIMIT/派生表/JOIN；内层计划形状仅 `Project(顶层可选) → Filter* → SeqScan`
   （`IsSimpleSubqueryPlanShape`）；IN/ANY 另要求 select_list 为**单裸列**。
3. **相关合取项迁移**：`CollectCorrelatedConjuncts` 抽出子查询 WHERE 中引用外层表的合取项
   （`ReferencesOuterTable`：限定列不在内层表限定符集合即外层引用），移入连接条件；内层计划
   经 `ClonePlan`（仅 PROJECT/FILTER/SEQ_SCAN 深克隆）+ `StripCorrelatedFilters` 剥除相关
   子句，**不污染原 subquery_plan**（非相关子查询执行路径保持原样）。IN/ANY 的连接条件由
   「外列 op 内列」构造，未限定内列补内层表名/别名。
4. **保守守底四道闸**（任一不过 → 整项保持原 Filter 逐行求值，零回归）：
   - ① NOT IN / NOT ANY：NULL 三值语义与 ANTI **不等价**（NULL 不匹配 → ANTI 会误放行）；
   - ② 相关合取项含**嵌套子查询**（`ExprHasNestedSubquery`）：连接上下文无法提供外层绑定；
   - ③ 相关合取项含**未限定列**（`HasUnqualifiedColumnRef`）：移入连接条件后首表优先解析歧义；
   - ④ `CondRefsResolve` 无法把连接条件所有限定列解析到左/右子树的扫描表（catalog 可用时
     校验列名存在）。
5. **嵌套相关子查询**：改写成功的新子树递归 `DecorrelateSubqueries`，内层 SubqueryExprNode
   再被去关联为内层 SEMI/ANTI（EXPLAIN 显示两层 Join(SEMI)）。

### 5.3 非相关子查询物化（执行引擎）

- `ExecutionContext::subquery_cache_`：键 = `SubqueryExprNode.subquery_plan` 指针（同语句
  唯一）；`EvaluateSubquery` 首次求值 `RunPlanToCompletion` 一次并缓存，后续外层行直接复用
  （`GetSubqueryCacheHitCount` 命中计数）；相关子查询（`IsSubqueryCorrelated`）逐行求值。
- **保守判定**：`WalkExprForOuterRefs` 对**未限定列**视为可能的外层引用（不做完整 schema
  解析排歧义）→ 少量非相关子查询被当相关逐行重跑、不入缓存（宁可多跑不能缓存错）。
- **观测**：`SubqueryCacheStats`（原子）sink 注入 ExecutionEngine 默认 ctx 与会话路径 ctx，
  跨语句累计到 Database；`\stats` 新增 `subquery cache : materialize=N hits=M` 行
  （白盒单元测试与 \stats 同源断言）。

### 5.4 递归 CTE 缓存失效（`CteDefineExecutor`）

- 递归 CTE 每轮迭代的工作集（delta）变化：递归项内引用 CTE 结果的非相关子查询若沿用上一轮
  物化结果即陈旧。`CteDefineExecutor::Init` 在**每轮迭代边界**调用 `context_->ClearSubqueryCache()`
  使缓存失效、重新物化（见 CteExecutor.cpp 迭代循环头）。

### 5.5 测试覆盖

| 组 | 断言 |
|---|---|
| `TestSubqueryDecorrelation`（10 场景）| 相关 EXISTS→SEMI / NOT EXISTS→ANTI（内层非相关子句保留 Filter）/ IN→SEMI / ANY→SEMI / 混合合取左子保留 Filter / NOT IN 保守不改写 / 非相关 EXISTS→SEMI 无条件 / 空表（EXISTS=0、NOT EXISTS=全行）/ NULL 外列经 SEMI 按 UNKNOWN 过滤 / 嵌套两层均递归去关联（SEMI≥2）——EXPLAIN 断言与结果断言相互印证 |
| `TestSubqueryMaterialization`（4 步白盒计数）| 非相关标量 5 外层行 materialize=1/hits=4；相关标量计数不变；第二条非相关 materialize=2/hits=8；未限定列保守不入缓存计数不变 |
| `TestRecursiveCteCacheInvalidation` | 递归项内 `(SELECT max(acc.n) FROM acc)` 走缓存 → 序列 1,2,4,8（cnt=4, mx=8）；materialize=3（每轮迭代边界重物化，第 4 轮 WHERE 先过滤未求值）——不清缓存将得 1,2,3,4,5 |
| SQL 回归 `53_subquery_decorrelate.sql`（13 项 + \stats）| 上述场景全量结果一致性 + `subquery cache : materialize=4 hits=4` |

---

## 6. 与备选方案的对比（技术优越性 / 性能优势 / 适用场景）

U3-3 子查询去关联曾考虑三种方案：

| 备选方案 | 本方案（安全改写 + 物化 + CTE 失效） | 优劣势 |
|---|---|---|
| **A：仅物化非相关子查询，不去关联** | 相关子查询也改写为 JOIN | 方案 A 只解决了非相关重复执行，相关 EXISTS/IN 仍逐行重跑子计划（N×M 代价）；本方案把相关子查询改为一次连接扫描，且条件提升为连接谓词后可与索引/谓词下推叠加 |
| **B：全量去关联（含 NOT IN、复杂内层）** | 只改写可静态证明安全的形态，疑点整项守底 | NOT IN / NOT ANY 的 NULL 三值语义与 ANTI 不等价（如 `NOT IN (1,NULL)` 恒 UNKNOWN → 空结果，ANTI 会误放行全部）；复杂内层（聚合/JOIN/LIMIT）改写风险高。方案 B 语义漏洞大；本方案保守守底零回归，收益面已覆盖 EXISTS/IN/ANY 主体 |
| **C：子查询缓存提升为跨语句/跨会话共享** | 缓存仅语句级生命周期 + 递归 CTE 迭代边界显式失效 | 快照隔离下子查询与主查询同快照，语句级缓存即保证一致性；跨语句缓存需额外的版本/依赖失效机制（基表变更、CTE 工作集变化），复杂度高收益小。本方案在保证正确性的前提下给出可观测计数（\stats） |

性能/可靠性/可维护性结论：
- **性能**：相关子查询从 N×M 逐行重跑降为一次 SEMI/ANTI 连接扫描（连接条件只按行求值一次）；
  非相关子查询从每行重跑降为每语句一次；递归 CTE 避免陈旧缓存带来的错误放大（正确性兜底）。
- **可靠性**：四道保守守底闸 + 语句级缓存生命周期 + 迭代边界失效，全部可静态证明等价；
  UT 10+4+1 组断言 + SQL 回归 56/56 兜底。
- **可维护性**：改写逻辑集中在 Optimizer（复用既有 JoinNode/FilterNode 计划节点），
  SEMI/ANTI 复用 JoinExecutor 执行器；缓存与计数集中在 ExecutionContext；`\stats` 可观测。

---

## 7. 测试结果与问题修复记录

### 7.1 质量基线（U3-3 验收）

- 构建：`cmake -S . -B build && cmake --build build`：**0 错误 0 告警**（`-Wall -Wextra`，
  强制重编译 Optimizer/Execution/Join/CTE 相关 TU 复核）。
- storage_ut：**292643 checks / 0 fails / RESULT: PASS**（相对 U3-2 基线 292554，
  新增 TestSubqueryDecorrelation 10 场景 + TestSubqueryMaterialization + TestRecursiveCteCacheInvalidation）。
- SQL 回归：**PASSED=56 FAILED=0**（新增 53_subquery_decorrelate.sql，原 55 条零回归）。
- CLI 冒烟：EXPLAIN 改写断言 + 结果一致性 + `\stats` 子查询缓存计数（见证据日志）。

### 7.2 问题修复记录

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | EXPLAIN 打印 Join(SEMI/ANTI) 时抛异常/缺名 | `JoinTypeName` switch 缺 SEMI/ANTI 分支 | Plan.cpp 补两个 case |
| 2 | 递归 CTE 内非相关子查询结果陈旧 | 缓存语句级生命周期，迭代边界未失效 | CteDefineExecutor 每轮迭代 ClearSubqueryCache() |
| 3 | 非相关子查询每外层行重复执行 | ExpressionEvaluator 未接缓存 | 增 subquery_plan 指针键缓存 + 物化/命中计数 |
| 4 | 会话路径子查询计数不累计 | Database.cpp 会话路径 `session_ctx` 未注入 sink（引擎默认 ctx 有、会话 ctx 漏） | 补 `&subquery_cache_stats_` 第三参 |
| 5 | 白盒断言 `amount>100` 计数=3 失败 | 测试手误（100 非 >100，实为 2） | 断言改 2 |
| 6 | 未限定列子查询不入缓存（计数不符预期）| `WalkExprForOuterRefs` 对未限定列保守视为外层引用（设计取舍） | 测试改限定列 `o.amount`，并补断言记录该保守行为 |
| 7 | storage_ut 首跑偶发失败 | `TestOptimisticSplitConcurrency` 已知 flaky（B+Tree 并发删除+扫描，重跑通过，与本次变更无关）| 复跑确认 |

---

## 8. 下一步计划（U4 与后续）

### 8.1 U4 运维与数据安全（`\backup`/`\restore`、慢查询日志、锁等待事件日志）

### 8.2 U5 集成与发布（崩溃注入扩面至后台线程/页合并中途、全量回归 + 零告警复核、文档收口）

时间节点与排程建议、风险与应对见 `09_下一阶段开发计划.md`（U3–U5 行）。

### 8.3 风险与应对

| 风险 | 应对 |
|---|---|
| 子查询去关联改写破坏相关子查询语义（外层绑定丢失） | 仅改写可静态证明安全的形态（条件可提升为连接谓词）；无法证明保持原逐行求值；回归用例兜底 |
| 非相关子查询物化后基表并发变更导致结果过期 | 本引擎快照隔离下子查询与主查询同快照，物化结果与快照一致；写入并发由锁机制保证可见性 |
| 聚合下推后续扩展 AVG/MIN/MAX 引入语义偏差 | 保持保守资格判定 + 逐项语义对照 UT；不在本里程碑扩大下推面 |
| 并行执行（U5 之后）落地时两阶段聚合的必要性 | 届时按实测数据决定是否引入部分聚合中间态，不做预先设计 |
