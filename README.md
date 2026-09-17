# SQL-Compiler / 简化数据库系统

一个使用 **C++17 + Modern CMake** 实现的简化 SQL 数据库系统，覆盖：

1. **SQL 编译器**：词法分析 → 语法分析 → 语义分析 → 执行计划生成 → 优化 → 执行
2. **页式存储系统**：固定大小页（4 KB）的分配/回收、磁盘读写、可配置 LRU/FIFO 替换策略
3. **数据库系统**：执行引擎（火山模型算子）、存储引擎（记录↔页映射）、系统目录持久化、B+Tree 索引、ACID 事务（WAL + ARIES 风格恢复）
4. **Web 管理控制台**：基于 FastAPI 的浏览器端 IDE，复用引擎二进制并实时可视化 tokens / AST / 计划 / 缓存命中

`Database`（`include/db/Database.h`）是整个系统的门面类，对外提供 `ExecuteSQL(sql)` 一个接口，
内部串联 Lexer → Parser → SemanticAnalyzer → Planner → Optimizer → ExecutionEngine 全流程。
任一阶段失败都会被捕获并体现在 `ExecutionResult::success/message` 中。

当前测试套件：**84 个 SQL 脚本**，全部通过（详见「测试」一节）。

## 整体架构

```
                       SQL 文本
                          │
   ┌──────────────────────▼──────────────────────┐
   │                 编译器                       │
   │  Lexer → Parser → SemanticAnalyzer          │
   │         → Planner → Optimizer               │
   └──────────────────────┬──────────────────────┘
                          │ 逻辑执行计划 PlanNodePtr
   ┌──────────────────────▼──────────────────────┐
   │              执行引擎 ExecutionEngine         │
   │  把 Plan 树转换为 Executor 树（41 算子）      │
   │  并驱动 Init / Next（Volcano 模型）           │
   │  EXPLAIN ANALYZE 走 TimingProxyExecutor 包装  │
   └──────┬───────────────────────┬─────────────┘
          │                       │
   ┌──────▼────────┐     ┌────────▼─────────┐
   │ SystemCatalog │     │    TableHeap      │
   │  （元数据，本身│     │ （记录↔页映射，   │
   │  是一张持久化  │     │  存储引擎核心）    │
   │  的特殊表）    │     └────────┬─────────┘
   └──────┬────────┘              │
          └──────────┬────────────┘
                     │ StorageAccess 门面
          ┌──────────▼──────────┐
          │  BufferPoolManager   │  ← LRU / FIFO 替换策略
          │     （缓存管理）       │     （src/storage/）
          └──────────┬──────────┘
                     │
          ┌──────────▼──────────┐
          │     DiskManager      │  ← 页的分配/回收、磁盘文件读写
          │    （页式存储系统）    │
          └──────────┬──────────┘
                     │
                  数据文件 (.db)

   ┌────────────────────────────────────────────────┐
   │                WebUI 控制台                     │
   │   FastAPI (python -m webui.backend)            │
   │   ├─ /api/query/execute  → 引擎二进制          │
   │   ├─ /api/query/debug    → 引擎 --debug-output │
   │   ├─ /api/schema/tables  → SHOW TABLES         │
   │   └─ 静态前端  index.html / app.js / style.css  │
   └────────────────────────────────────────────────┘
```

## 编译流水线

### 1. 词法分析（Lexer）— `include/lexer/{Token,Lexer}.h`

`Lexer` 把 SQL 文本切成 `Token` 流。`Token` 携带种别码、词素与行列号：

- **关键字表**（`SELECT / FROM / WHERE / CREATE TABLE / ...`）以 `TokenType` 枚举表示；
- **标识符**支持 Unicode（含中文标识符，见测试 `23_chinese_idents.sql`）；
- **字面量**覆盖整数、浮点、字符串（带转义）、`NULL`、`TRUE / FALSE`；
- **错误报告**：非法字符、未闭合字符串、未闭合块注释等都会带 `line:col` 抛出 `CompilerException`。

### 2. 语法分析（Parser）— `include/parser/Parser.h`

手写**递归下降**语法分析器，构造 AST（`include/ast/AST.h`）。支持语句类型见
「SQL 语法覆盖」一节。语法错误（缺分号、表达式残缺、未知关键字等）以 `位置 + 期望符号`
格式抛出。

AST 节点自带 `ToString()` 序列化，REPL 中 `\.ast` 命令可直接打印（参见测试
`80_tokens_complete.sql` 验证完整 token 流可还原）。

### 3. 语义分析（SemanticAnalyzer）— `include/semantic/SemanticAnalyzer.h`

校验表/列存在性、类型一致性、`INSERT` 列数/列序、`GROUP BY` 合法性等。
错误的列名/表名会触发**基于 Levenshtein 编辑距离的拼写建议**
（`include/common/EditDistance.h`，参见测试 `61_typo_suggest.sql`）。
语义错误按阶段分类记录在 `SemanticErrorStage`（`include/semantic/SemanticErrorStage.h`）。
表/列名匹配走 `include/common/CaseInsensitive.h` 提供的不区分大小写查找。

### 4. 执行计划（Planner / Plan）— `include/plan/{Plan,Planner}.h`

AST → 逻辑执行计划。当前共 **48 个** `PlanNodeType`（`include/plan/Plan.h`），
覆盖 DDL / DML / Query / Txn / UDF / Trigger / View / MaterializedView / CTE / Window / LATERAL 等。
每种节点都自带 `ToString()` 序列化（REPL `\.plan` / `\.optimized` 元命令直接打印），
并提供结构化 `ToJson() / ToSExpr()`（默认走集中式 dispatcher 递归 children），
供 `EXPLAIN FORMAT JSON|SEXPR` 直接消费。

### 5. 优化器（Optimizer）— `include/optimizer/Optimizer.h`

对逻辑计划做等价改写：

- **谓词下推**（`PushDownPredicates`）：单表合取项下沉到 `SeqScanNode.predicate`；
- **列裁剪**（`PruneColumns`）：自顶向下传播"上层用到的列"，叶子节点只读必需列
  （见 `SeqScanNode.read_columns` / `IndexScanNode.read_columns`），见测试 `63_prune_columns.sql`；
- **常量折叠**（`FoldConstants`）：编译期求值常量子表达式，简化 TRUE/FALSE 谓词；
- **访问路径选择**（`ChooseAccessPaths` / `IndexAccessPath.cpp`）：把 `Filter -> SeqScan`
  改写成 `IndexScan`（等值 / 区间 / `BETWEEN`）；其余合取项作为残余谓词回表后再判。
  改写策略保守——任何不确定形态都保持原计划。

### 6. 执行引擎（ExecutionEngine）— `include/execution/ExecutionEngine.h`

把 Plan 树转为 Executor 树（`include/execution/Executor.h`，火山模型
`Init() / Next(Tuple*)`），并驱动执行。当前在 `src/execution/` 下有 **41 个** Executor
实现，对应 42 个头文件（`TypeCoercion.h` 为隐式类型转换工具，是头文件库）：

- DDL：`CreateTable / DropTable / TruncateTable / AlterTable / CreateIndex / DropIndex / SchemaSequence`
- DML：`Insert / Update / UpdateFrom / Delete / Upsert / Merge`
- 查询：`SeqScan / IndexScan / Filter / Project / Join / Sort / Limit / Aggregate / Window / Distinct / SetOp`
- 高级查询：`Subquery / CteExecutor / ValuesExecutor / ApplyExecutor`（LATERAL）
- 事务 / 变量：`TransactionExecutor`（BEGIN/COMMIT/ROLLBACK/SAVEPOINT）
- 视图 / 触发器 / UDF / 存储过程：`CreateView / CreateTrigger / CreateFunction / CreateProcedure / UdfExecutor / CallExecutor / TriggerExecutor / MaterializedViewExecutor`
- 元命令：`ExplainExecutor / ShowExecutor / NoOpExecutor / TimingProxyExecutor`（EXPLAIN ANALYZE 包装）
- 辅助：`ExpressionEvaluator / ConstraintChecker / IndexMaintenance`

`EXPLAIN ANALYZE` 在执行器外层再套一层 `TimingProxyExecutor`，逐节点统计 `Init / Next`
的真实耗时与产出行数，与 `EXPLAIN`（仅打印计划树）合并在 `69_*` 系列测试中验证三种
输出格式（TEXT / JSON / SEXPR）。

### 7. 存储引擎

**Page**（`include/storage/Page.h`）：固定大小 4 KB 物理页，提供 `Write/Read` 序列化原语。

**BufferPoolManager**（`include/storage/BufferPoolManager.h`）：
`get_page / flush_page / new_page / delete_page`，含命中率统计与替换日志。
WebUI 的可视化面板直接读 `BufferPoolStats { hit_count, miss_count, replacement_count, hit_rate }`
与 `GetReplacementLog()`（[DEBUG_JSON_START]…[DEBUG_JSON_END] 信封）。
替换策略通过 `Replacer` 抽象（`include/storage/Replacer.h`）实现：

- **LRUReplacer**（`include/storage/LRUReplacer.h`）：双向链表 + `position_map_`，
  `Pin/Unpin/Victim` 均为 O(1)；
- **FIFOReplacer**（`include/storage/FIFOReplacer.h`）：单向队列 + `position_map_` + 每帧
  的 `ref_bit`，实现 **second-chance (clock-style)** 增强。`Pin` O(1)，
  `Unpin` O(1)，`Victim` 最坏 O(N) —— 一个轮次内给被命中的"热帧"清零 ref_bit
  并推回队尾，真正可淘汰的"冷帧"才被摘掉。这样热页不会因为早期入队就被
  冷数据 churn 淘汰，而纯 FIFO 的 worst case 下就做不到这一点。详见
  `tests/sql/72_fifo_second_chance.sql`。

**DiskManager**（`include/storage/DiskManager.h`）：页级 `ReadPage / WritePage`、
空闲页分配/回收。

**StorageAccess 门面**（`include/storage/StorageAccess.h`）：把 BPM + DM 封装为单个
对象暴露给执行引擎 / 算子 / catalog，避免它们直接依赖 BPM/DM 内部细节。

**TableHeap**（`include/storage_engine/TableHeap.h`）：记录↔页映射，槽位式记录存取，
`Begin / Iterator` 顺序扫描迭代。

### 8. 系统目录（SystemCatalog）— `include/catalog/SystemCatalog.h`

表名 / 列名 / 列类型 / 索引 / 视图 / 触发器 / UDF / 过程 / FK / CHECK / Schema / Sequence
等元数据。**自身作为特殊 `TableHeap` 持久化**——schema 变化随数据文件一起落盘，
重启后无需重建。

### 9. 索引（B+Tree）— `include/index/{BPlusTree,BPlusTreePage,IndexKey,PageGuard}.h`

真正的磁盘 B+Tree：

- 节点即 4 KB 页面，走 `BufferPoolManager`；
- 根页 id 持久化在系统目录里，重启后直接可用；
- 复合键按字典序比较，复用 `Value::Compare`；
- 插入采用**下降途中预分裂**，叶子插入永不失败，不需要在页头维护父指针；
- 删除采用**path-stack 下降 + 自底向上 redistribute / merge**，下界 = 1/4 容量；
  兄弟都空则合并并 DeletePage 归还空页；根若只剩一个孩子则把该孩子搬进根页
  （root_page_id 始终不变，目录元数据免维护），见测试 `67_bptree_rebalance.sql`；
- 所有页面访问经 `PageGuard`（RAII），禁止裸 `GetPage/UnpinPage` 配对。

### 10. 事务子系统 — `include/txn/*`

Phase A（内存 undo log）→ Phase B（WAL + ARIES 风格恢复）：

- `TransactionManager`：事务生命周期、隔离级别、undo log 链表、会话变量（`@var`）钩子；
- `LogManager`：WAL 写出器（`<db_file>.wal`）；
- `LogRecord`：日志记录编码；
- `RecoveryManager`：启动期分析/重做/撤销。

崩溃注入机制（`Database::SetCrashInjectionPoint` / `TriggerCrashNow`）配合
`run_acid_recovery.bat` 与 `run_acid_clr.bat` 验证 redo + undo 链路。

## SQL 语法覆盖（节选）

| 类别 | 语句 |
| --- | --- |
| DDL | `CREATE TABLE`（含 PK / UNIQUE / FK / CHECK / DEFAULT）、`ALTER TABLE`（ADD/DROP/RENAME/MODIFY COLUMN）、`DROP TABLE`、`TRUNCATE TABLE`、`CREATE [UNIQUE] INDEX`、`DROP INDEX`、`CREATE SCHEMA`、`DROP SCHEMA`、`CREATE SEQUENCE`、`DROP SEQUENCE` |
| DML | `INSERT [INTO] ... VALUES / SELECT`、`INSERT ... ON DUPLICATE KEY UPDATE`、`REPLACE INTO`、`UPDATE`、`UPDATE ... FROM`、`DELETE`、`MERGE`、`RETURNING` |
| Query | `SELECT`（含 `DISTINCT`、`WHERE`、`GROUP BY`、`HAVING`、`ORDER BY`、`LIMIT/OFFSET`、`UNION/INTERSECT/EXCEPT`、`WITH` CTE、`LATERAL`、`APPLY`）、子查询（SCALAR / EXISTS / IN / ANY / 在 DML 上下文里）、窗口函数（`OVER` / `PARTITION BY` / `RANK`）、`JOIN`（INNER / LEFT / RIGHT / FULL / CROSS、自连接、`SELECT *` 含表限定符） |
| 表达式 | 算术 / 比较 / 逻辑 / `BETWEEN` / `IN` / `LIKE` / `IS NULL` / `CASE` / `CAST` / `TYPEOF` / `LENGTH` / 标量函数（含日期时间 `75_*` / `90_*`）、中文标识符、首字面量后 `SELECT *`、`*` 列限定 |
| 事务 | `BEGIN [TRANSACTION]`、`COMMIT`、`ROLLBACK`、`SAVEPOINT`、`ROLLBACK TO`、`RELEASE SAVEPOINT`、读己之写（read-your-own-writes） |
| 视图 | `CREATE VIEW`、`DROP VIEW`、`CREATE MATERIALIZED VIEW`、`ALTER MATERIALIZED VIEW ... REFRESH` |
| 触发器 | `CREATE TRIGGER`（BEFORE/AFTER、ROW/STATEMENT 级） |
| 程序对象 | `CREATE FUNCTION`、`CREATE PROCEDURE`、`CALL`（含 `OUT` / `INOUT` 参数、`EXIT` / `UNDO HANDLER`） |
| 元命令 | `EXPLAIN [FORMAT TEXT|JSON|SEXPR]`、`EXPLAIN ANALYZE`（实际执行统计）、`SHOW TABLES / COLUMNS / INDEX / CREATE TABLE` |

## 构建方式

```bash
mkdir build && cd build
cmake ..
cmake --build .
./sqlcompiler [数据文件路径，默认 sqlcompiler.db]
```

Windows 下可执行文件为 `build\Debug\sqlcompiler.exe`。所有源文件由
`CMakeLists.txt` 通过 `file(GLOB_RECURSE ...)` 收集，自动覆盖新文件。

CLI 参数：

```text
sqlcompiler [db_file] [-f script.sql ...]
  db_file         数据文件路径（默认: sqlcompiler.db）
  -f <file>       把 <file> 当 SQL 脚本执行后退出（可重复）
  --file, --source   -f 的别名
  --debug-output  每条语句后输出一段 [DEBUG_JSON_START]…[DEBUG_JSON_END] 信封
                  （WebUI 的 /api/query/debug 端点消费此格式）
Inside the REPL:
  .source <file> / .read <file>   加载并执行 SQL 脚本
```

## 测试

测试套件包含 **84 个 SQL 脚本**，覆盖：

- 基础 DDL/DML/Query（`tests/sql/00_*.sql` ~ `06_*.sql`、`08_*.sql` ~ `26_*.sql`）
- 表达式 / 边界 / 中文标识符 / `CASE` / `CAST`（`27_*` ~ `29_*`，空号位说明见下）
- 子查询 / CTE / 集合运算 / 窗口函数（`30_*` ~ `33_*`）
- B+Tree 索引生命周期与扫描（`34_*` ~ `36_*`）
- DML 扩展 / 事务视图 UDF（`37_*` ~ `40_*`）
- ALTER / CHECK / DEFAULT / UPSERT / 模式匹配 / 日期时间（`41_*` ~ `45_*`，空号位说明见下）
- 元命令 / UDF/Trigger/View / ACID undo + WAL + CLR / 嵌套 savepoint（`47_*` ~ `51_*`）
- 数据类型 / DDL / DML / Query / 模式 / 函数 / 约束 / 存储过程 / 视图与触发器（`52_*` ~ `60_*`）
- 拼写建议 / 调试元命令 / 列裁剪 / FIFO 替换策略 / StorageAccess 门面（`61_*` ~ `65_*`）
- B+Tree 删除再平衡（`67_*`：redistribute / merge / root-collapse）
- 存储过程 EXIT / UNDO HANDLER（`68_*`：基本 EXIT、嵌套 block EXIT、UNDO +
  SAVEPOINT、handler 优先级、SQLSTATE 特化）
- EXPLAIN ANALYZE 实际执行统计（`69_*` 系列测试按 TEXT / JSON / SEXPR 三种格式覆盖
  完整计划 + 失败部分统计——见 `tests/expected/69_explain_format_*.out`）
- 物化视图 schema 集成（`70_*`）
- 存储过程 OUT / INOUT 参数通过 session variable 回传（`71_*`：基本 OUT、INOUT、
  多 OUT、标量子查询进入 OUT、错误校验、持有表形式回归）
- FIFO second-chance 替换策略（`72_*`：热页存活 + 冷数据 churn 场景，
  以及与 `64_fifo_replacer.sql` 同等的回归断言）
- `UNIQUE` 约束 / `NOT BETWEEN` / `TEXT LENGTH`（`74_*` ~ `76_*`）
- 视图别名、负整数 `(-N)` 前缀解析、DML 子查询、`tokens` 全量完整性（`77_*` ~ `80_*`）
- 双 CTE 拼接、`SELECT *` 进 CTE、`VALUES` 在派生表内的集合运算、`DECIMAL` 算术（`81_*` ~ `84_*`）
- 读己之写（`85_*`）、`RANK` 窗口函数（`86_*`）、`SELECT 1, *` 列首字面量（`87_*`）、
  `SELECT t.*` 多表限定（`88_*`）、自连接 `LEFT JOIN` + 限定符（`89_*`）、`TYPEOF()` 函数（`90_*`）

> 编号留空（`07`、`29`、`46`、`57`、`66`、`69`、`73`）表示该编号的脚本被
> 后续修复或合并/废弃而**有意跳过**——verify_outputs.py 只统计现有脚本，不
> 试图连续编号，因此遗漏不代表缺失用例。

验证脚本 `tests/verify_outputs.py` 支持串行或 `-j N` 并行执行，自动发现
`build/Debug/sqlcompiler.exe` / `build/Release/sqlcompiler.exe` / `build/sqlcompiler.exe`，
或通过 `--bin` / `SQLCOMPILER_BIN` 显式指定。

```bash
# 默认串行跑全部 84 个用例
python tests/verify_outputs.py

# 仅跑子集
python tests/verify_outputs.py 80_tokens_complete 90_typeof_function

# 8 并行 + 失败时落盘 actual 输出
python tests/verify_outputs.py -j 8 -k

# 自定义二进制路径
python tests/verify_outputs.py --bin build/Release/sqlcompiler.exe
```

外加四组批处理驱动的多阶段冒烟测试（在 Windows 上 `tests/*.bat`）：

- `run_acid_recovery.bat`：WAL + 崩溃注入（Phase B）；
- `run_acid_clr.bat`：CLR 链 + 中途崩溃恢复；
- `run_trigger_persistence.bat`：触发器定义跨进程持久化；
- `run_debug_meta.bat`：REPL `\.tokens` / `\.ast` / `\.plan` / `\.optimized` 元命令。

**当前通过率：84 / 84**。

## WebUI 控制台 — `webui/`

`webui/` 提供一个浏览器端管理控制台，后端用 FastAPI 包裹 `sqlcompiler.exe`，
前端是一个单页 vanilla-JS 应用。

```
SQL-Compiler/
├── build/Debug/sqlcompiler.exe   ← C++ 引擎（必须先编译）
└── webui/
    ├── backend/
    │   ├── engine.py             ← subprocess 包装 + 表格输出解析
    │   ├── api.py                ← FastAPI 路由
    │   ├── schemas.py            ← Pydantic 模型
    │   ├── main.py               ← FastAPI app factory
    │   ├── __main__.py           ← python -m webui.backend 入口
    │   └── static/
    │       ├── index.html        ← 页面
    │       ├── style.css         ← 深色设计系统
    │       └── app.js            ← 单文件控制器
    ├── requirements.txt
    ├── run.bat                   ← Windows 一键启动
    └── run.sh                    ← git-bash / WSL 启动
```

### 快速启动（Windows）

```bat
cd webui
run.bat
```

`run.bat` 会：

1. 若 `build\Debug\sqlcompiler.exe` 不存在，则先编译 C++ 引擎；
2. 若 `fastapi` / `uvicorn` 未安装则自动安装；
3. 在 <http://127.0.0.1:8765> 启动服务。

打开 URL 后点击右上 **No database open**：

- 输入/粘贴一个 `.db` 路径（不存在会自动新建）；或
- 从 **Recent `.db` files** 列表里点选（扫描当前路径所在目录）。

### 主要能力

- **数据库绑定**：打开任意 `.db` 文件，新文件在首次打开时自动创建。
- **表清单**：每次 DDL（`CREATE` / `DROP` / `ALTER` / `TRUNCATE`）后自动刷新；点击表名打开其 schema 页签。
- **Schema 查看**：列名、类型、可空、PK 标记、默认值、`SHOW CREATE TABLE` 原文、以及 `SELECT * LIMIT 100` 样例。
- **SQL 编辑器**：多行 textarea，`Ctrl+Enter` 执行，`Tab` 插入两空格，**Format** 按钮大写关键字。
- **结果渲染**：表格，固定表头，NULL 样式，数值右对齐，per-block kind 徽章
  （`SELECT` / `DDL` / `DML` / `TXN` / `error`），耗时，以及可切换显示原 SQL 的 **SQL** 开关。
- **EXPLAIN / EXPLAIN ANALYZE**：引擎的计划文本以结果表渲染（每行一个计划节点）。
- **可视化调试**：调用引擎的 `--debug-output`，把 tokens / AST / 计划（优化前 + 后）/
  缓冲池命中率 / 替换日志整体打包成 `[DEBUG_JSON_START]…[DEBUG_JSON_END]` 信封，
  在前端拆开后分别渲染到独立面板。
- **查询历史**：保留最近 50 条到 `localStorage`，点回填到编辑器。

### HTTP 接口

| 方法 | 路径 | 描述 |
| --- | --- | --- |
| GET | `/api/health` | 健康检查 + 引擎二进制路径 |
| POST | `/api/db/open` | 把会话绑定到一个 `.db` 文件 |
| GET | `/api/db/ls?directory=…` | 列出目录下的 `.db` 文件 |
| POST | `/api/schema/tables` | `SHOW TABLES` + 每表列数 |
| POST | `/api/schema/table` | `SHOW COLUMNS` + `SHOW CREATE TABLE` |
| POST | `/api/query/execute` | 执行单条 SQL |
| POST | `/api/query/debug` | 执行并返回 tokens / AST / 计划 / 存储统计 |

Swagger UI 在 `/docs`。

### 设计取舍

- **引擎集成走进程隔离**：每次 `execute` 都 `subprocess` 启动
  `sqlcompiler.exe <db_file> -f <temp.sql>`，抓 stdout / stderr 后解析。
  C++ 引擎保持 CLI REPL 不动，零源码侵入。
- **多语句在服务端切分**：用户给的 `"CREATE …; INSERT …; SELECT …;"` 在服务端
  按 `;` 切，每条一行写入临时文件，让引擎的 `HasCompleteStatement` 行缓冲
  能正确识别每条语句边界。
- **输出解析走行扫描**：引擎 REPL 对 `SELECT` / `SHOW` / `EXPLAIN` 输出表格块，
  对 DDL / DML / TXN 输出单行（"OK"、"(N rows)" 或 stderr 上的 `Error:`）。
  解析器沿行流剥出每条语句一个块。
- **单用户 / 单数据库 / 单线程**：引擎本身不支持并发，WebUI 与之对齐。
  一个浏览器标签同一时刻只持有一个 DB。

### 故障排查

- **"engine binary not found"** —— 后端依次查找 `build/Debug/sqlcompiler.exe`、
  `build/Release/sqlcompiler.exe`、`$SQLCOMPILER_BIN`。编译 C++ 工程或设置环境变量。
- **"table not found" 紧跟 CREATE 之后** —— 多半前一条语句已经失败；看 banner，
  解析器会把引擎 stderr 上的错误消息挂回失败 block。
- **8765 端口被占用** —— 编辑 `run.bat` / `run.sh` / `__main__.py` 改端口。

详细使用见 [webui/README.md](webui/README.md)。

## CLI / REPL

REPL 入口在 `src/main.cpp`：

- `sqlcompiler> ` 提示符下输入 SQL，以 `;` 结束；
- 支持**多语句**（脚本）和**行续行**（提示符切换为 `       -> `）；
- 自动识别 `CREATE FUNCTION/PROCEDURE` 内部的 `BEGIN ... END` 块，避免块内的 `;`
  提前截断语句；
- **调试元命令**（在累加器为空时立即生效）：
  - `\.tokens` — 打印最近一次成功通过词法阶段的 Token 流；
  - `\.ast` — 打印最近一次成功通过语法阶段的 AST；
  - `\.plan` — 打印 Optimizer **改写前** 的逻辑计划（`Planner` 直出文本快照）；
  - `\.optimized` — 打印 Optimizer **改写后** 的逻辑计划（也是执行器真正跑的那份）；
- 脚本加载：`.source <file>` / `.read <file>`（前缀 `\` 也可）；
- `exit / quit` 退出。

调试元命令与 `--debug-output` 共用同一份缓存
（`Database::LastTokens / LastAst / LastPlan / LastPlanBeforeOptText`），保证 REPL
与 WebUI 看到的是同一份数据。

## 目录结构

```
SQL-Compiler/
├── CMakeLists.txt                # Modern CMake（target-based，全局 include 仅一处）
├── README.md                     # 本文件
├── include/
│   ├── common/                   # Error / DateTime / EditDistance / CaseInsensitive
│   ├── lexer/                    # Token / Lexer
│   ├── ast/                      # AST 节点
│   ├── parser/                   # 递归下降 Parser
│   ├── semantic/                 # SymbolTable / SemanticAnalyzer / SemanticErrorStage
│   ├── plan/                     # PlanNode + Planner（48 个 PlanNodeType）
│   ├── optimizer/                # Optimizer + IndexAccessPath
│   ├── storage/                  # Page / DiskManager / BufferPoolManager /
│   │                             #   Replacer / LRUReplacer / FIFOReplacer / StorageAccess
│   ├── index/                    # BPlusTree / BPlusTreePage / IndexKey / PageGuard
│   ├── storage_engine/           # Value / Tuple / TableHeap
│   ├── catalog/                  # SystemCatalog / IndexInfo
│   ├── txn/                      # Transaction / TransactionManager /
│   │                             #   LogManager / LogRecord / RecoveryManager
│   ├── execution/                # Executor + 41 个算子实现
│   └── db/                       # Database 门面
├── src/                          # 与 include 一一对应的实现
├── tests/
│   ├── verify_outputs.py         # 主测试入口（84 用例，可并行）
│   ├── run_acid_recovery.bat     # WAL + 崩溃注入
│   ├── run_acid_clr.bat          # CLR 链恢复
│   ├── run_trigger_persistence.bat
│   ├── run_debug_meta.bat
│   ├── sql/                      # 84 个 .sql 测试脚本
│   └── expected/                 # 84 个 .out 期望输出
└── webui/                        # 浏览器端管理控制台（FastAPI + 原生 JS）
    ├── backend/                  # FastAPI 应用
    ├── requirements.txt
    ├── run.bat / run.sh
    └── README.md                 # WebUI 详细文档
```