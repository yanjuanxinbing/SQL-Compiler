# SQL-Compiler / 简化数据库系统

一个使用 C++17 完整实现的简化数据库系统，覆盖 **SQL 编译器 → 执行引擎 → 存储引擎 → 页式存储 → 事务/恢复/并发控制** 全链路，并以「操作系统页面管理」为主线实践了分页、缓存与置换、锁与死锁、读者-写者、内存回收、崩溃恢复、I/O 调度等操作系统核心概念。

- 语言/构建：C++17 · CMake · MinGW GCC（`D:/mingw64/bin/g++.exe`）
- 验收基线：存储单元测试 **58953 checks / 0 fails**；SQL 全量回归 **55 passed / 0 failed**（含两套崩溃注入）；`-Wall -Wextra` **零告警**

---

## 1. 整体架构

```
                     SQL文本
                        │
   ┌────────────────────▼────────────────────┐
   │              编译器 Compiler              │
   │  Lexer → Parser → SemanticAnalyzer       │
   │       → Planner → Optimizer → CodeGen    │
   └────────────────────┬────────────────────┘
                        │ 逻辑执行计划 PlanNodePtr
   ┌────────────────────▼────────────────────┐
   │           执行引擎 ExecutionEngine        │
   │  SeqScan/IndexScan/Filter/Project/Join/  │
   │  Sort/Aggregate/Insert/Update/Delete/... │
   └───────┬───────────────────────┬─────────┘
           │                       │
   ┌───────▼────────┐     ┌────────▼─────────┐
   │  SystemCatalog  │     │    TableHeap      │  MVCC 版本链 + O(1) 版本索引缓存
   │ （元数据持久化） │     │  BPlusTree 二级索引 │  乐观页级并发（乐观重启 + 快慢写路径）
   └───────┬────────┘     └────────┬─────────┘
           │          事务子系统（横切）         │
           │  LogManager(WAL+组提交) · LockManager(S/X+谓词锁+分片) │
           │  TransactionManager · CommitTracker(快照低水位 O(1)) │
           └───────────┬───────────┘
                        │ get_page / write_page
             ┌──────────▼──────────┐
             │  BufferPoolManager   │  LRU/FIFO/CLOCK/LRU-K 可插拔替换、
             │     （缓存管理）      │  温度感知刷脏、后台刷脏、命中构成统计
             └──────────┬──────────┘
             ┌──────────▼──────────┐
             │     DiskManager      │  块设备抽象（File/Memory/Sparse/Network）、
             │    （页式存储系统）    │  .fpl 空闲页位图、.crc 页校验、64 位定位
             └──────────┬──────────┘
                        │
                     数据文件(.db) + .wal + .fpl + .crc
```

`Database`（`db/Database.h`）是整个系统的门面类，对外提供 `ExecuteSQL(sql)` 接口与交互式 CLI。

## 2. 核心能力

### 2.1 编译器链路
词法 → 语法 → 语义 → 逻辑计划 → 优化（谓词下推 / 索引访问路径 / 复合索引前缀区间推导）→ 代码生成，支持 SELECT（含 JOIN/GROUP BY/ORDER BY/LIMIT/窗口函数/子查询）、INSERT/UPSERT、UPDATE、DELETE、DDL、视图/触发器/函数、事务语句、EXPLAIN / SHOW。

### 2.2 页式存储（对应「操作系统页面管理」）
- 固定 4KB 页、唯一页号、页分配/释放/读写、空闲页位图跨重启持久化（`.fpl`，向后兼容）
- 页 CRC 损坏检测（`.crc`）、64 位文件定位（>2GB 文件）、Windows `_commit` / POSIX `fsync` 真持久
- **可交换块设备**：`FileBlockDevice`（默认）、`MemoryBlockDevice`、`SparseFileBlockDevice`、`LoopbackNetworkBlockDevice`、`FaultInjectingBlockDevice`（测试）——注入即换介质，业务零改动

### 2.3 缓存管理与置换
- LRU / FIFO / CLOCK / LRU-K（K=1 退化为 LRU），`Replacer` 策略模式可插拔
- 温度感知刷脏（`SQLCOMPILER_TEMP_FLUSH`）+ 自适应温度阈值（`SQLCOMPILER_HOT_RATIO_PERCENT`）
- 内存上限可配（`SQLCOMPILER_BUFFER_MEMORY`）、后台刷脏（`SQLCOMPILER_BG_FLUSH_MS`）
- 可观测：命中构成（cold/warm/hot）、脏页年龄分布、后台刷脏直方图、IO 队列（`\stats`）

### 2.4 事务、锁与隔离级别
- WAL-before-data + 组提交（时间窗 `SQLCOMPILER_GROUPCOMMIT_WINDOW_MS`）+ ARIES redo/undo + CLR 链崩溃恢复
- 锁管理器：行/表多粒度 S/X 锁、谓词锁（`(表, 列, 区间)` 居中区间树）、等待图 DFS 死锁检测、等待超时、自适应锁升级、按表/命名空间**分片锁**（16 shard）
- 隔离级别：READ UNCOMMITTED / READ COMMITTED / SERIALIZABLE / SNAPSHOT（MVCC）
- MVCC：48B 版本头版本链、快照低水位 O(1) 查询、FCW 防丢失更新、内联 + 后台真空、二级索引精确可见性、索引墓碑回收

### 2.5 B+Tree 索引并发
- 页级读写闩 + 乐观重启：读路径无锁等待（版本校验失败整体重启），写路径快/慢分档 + 预分裂
- 实测吞吐：4 线程 **1.93×** / 8 线程 **2.71×** / 16 线程 **3.18×**（vs 树级锁基线）

### 2.6 诊断
- `\stats`：存储统计（命中率、替换、写回、IO、WAL fsync、温度分档、脏页年龄、刷脏直方图）
- `\analyze`：页映射快照（pid → 帧号/脏/访问计数）、底层介质名、CRC 校验失败累计计数
- `\crash` / `\crash_after_undo_steps N`：崩溃注入，用于崩溃恢复验证

## 3. 构建与测试

```powershell
# 构建（Debug）
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=D:/mingw64/bin/g++.exe
cmake --build build -- -j8

# 存储单元测试（58953 checks / 0 fails）
.\build\storage_ut.exe

# 全量 SQL 回归 + 崩溃注入（55 passed / 0 failed）
powershell -ExecutionPolicy Bypass -File tests\run_sql_regression.ps1

# 告警审计（-Wall -Wextra 零告警）
cmake -S . -B build_audit -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=D:/mingw64/bin/g++.exe -DCMAKE_CXX_FLAGS="-Wall -Wextra"
cmake --build build_audit -- -j8
```

**一键复现**：`tests\run_repro_all.ps1`（构建 → storage_ut → SQL 回归 → 参数扫描，证据包输出到 `docs\test_evidence\`）。

## 4. 使用方式

```powershell
.\build\sqlcompiler.exe mydb.db
```

交互式 CLI 输入 SQL（以 `;` 结尾），也支持 `\stats;`、`\analyze;`、`\crash;` 等调试指令；多语句脚本可通过 stdin 重定向或 `tests\run_sql_regression.ps1` 驱动。

### 环境变量（均为可选项，默认行为保守）

| 变量 | 作用 |
|---|---|
| `SQLCOMPILER_BUFFER_MEMORY` | 缓冲池内存上限（字节），默认 64 帧（256 KB） |
| `SQLCOMPILER_BG_FLUSH_MS` | 后台刷脏间隔（ms），默认 0 = 关闭 |
| `SQLCOMPILER_BG_VACUUM_MS` | 后台真空间隔（ms），默认 0 = 关闭 |
| `SQLCOMPILER_GROUPCOMMIT_WINDOW_MS` | 组提交时间窗（ms），默认 0 = 纯跟随者聚合 |
| `SQLCOMPILER_TEMP_FLUSH` | 设置即开启温度感知刷盘 + 自适应阈值 |
| `SQLCOMPILER_HOT_RATIO_PERCENT` | 目标热页占比（1..99），覆盖默认 20% |

## 5. 目录结构

```
SQL-Compiler/
├── CMakeLists.txt
├── include/  src/            # 与 include 一一对应的实现
│   ├── common/ lexer/ ast/ parser/ semantic/ plan/ optimizer/ codegen/   # 编译器链路
│   ├── storage/              # 页式存储：Page/DiskManager/BlockDevice/BufferPoolManager/Replacer 族/PageAllocator/LockManager/OsModuleOptimizations
│   ├── storage_engine/       # Value/Tuple/TableHeap（MVCC 版本链 + 版本索引缓存）
│   ├── index/                # BPlusTree 乐观页级并发 + Cursor + Vacuum
│   ├── catalog/              # SystemCatalog 元数据持久化 + 真空调度
│   ├── execution/            # 火山模型算子 + ExpressionEvaluator + ExecutionEngine
│   ├── txn/                  # LogManager/RecoveryManager/TransactionManager/CommitTracker
│   └── db/                   # Database 门面类
├── tests/
│   ├── sql/                  # 53 条 SQL 回归脚本（含崩溃注入）
│   ├── storage/storage_ut.cpp# 存储子系统单元测试（58953 checks）
│   ├── run_sql_regression.ps1 / run_repro_all.ps1 / run_param_sweep.ps1
└── docs/                     # 开发计划、设计文档、验收报告、test_evidence/ 测试证据
```

## 6. 验收与证据

| 项目 | 结果 |
|---|---|
| 存储单元测试 | **58953 checks / 0 fails**（`docs/test_evidence/storage_ut_run.log`） |
| SQL 回归 | **55 passed / 0 failed**（53 条脚本 + 崩溃注入；`docs/test_evidence/sql_regression_run.log`） |
| 崩溃恢复 | 49_acid_recovery / 50_undo_clr 两阶段跨重启验证通过 |
| 告警审计 | `-Wall -Wextra` 零告警（`docs/test_evidence/audit_build.log`） |
| 性能基准 | B+Tree 并发写 3.18×（16 线程）、组提交 fsync 9.8× 削减、快照低水位读取约 465×、分片锁异表 5.8× 吞吐 |

完整文档见 `docs/`（开发计划 `Storage_Dev_Plan.md`、设计文档 `upload_os_module/04_模块设计文档.md`、需求比对与后续计划 `upload_os_module/06_需求比对与后续计划.md`）。
