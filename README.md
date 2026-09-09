# SQL-Compiler / 简化数据库系统

一个使用 C++17 实现的简化数据库系统框架，覆盖：

1. **SQL 编译器**：词法分析 → 语法分析 → 语义分析 → 执行计划生成 → 优化 → 代码生成
2. **页式存储系统**：固定大小页的分配/回收、磁盘读写、LRU/FIFO 缓存管理
3. **数据库系统**：执行引擎（火山模型算子）、存储引擎（记录↔页映射）、系统目录持久化

当前仓库只包含**框架代码**（头文件声明 + 空函数体骨架），所有函数逻辑均标记 `TODO`，需要自行实现。全部文件已通过 g++ 编译与链接验证（空实现可通过编译，但运行不产生正确结果）。

## 整体架构

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
   │  将Plan树转换为算子树(Executor)并驱动运行  │
   │  CreateTable / Insert / SeqScan /        │
   │  Filter / Project / Delete / Update      │
   └───────┬───────────────────────┬─────────┘
           │                       │
   ┌───────▼────────┐     ┌────────▼─────────┐
   │  SystemCatalog  │     │    TableHeap      │
   │ （元数据，本身   │     │ （记录↔页映射，    │
   │  也是一张持久化   │     │  存储引擎核心）    │
   │  的特殊表）      │     └────────┬─────────┘
   └───────┬────────┘              │
           └───────────┬───────────┘
                        │ get_page / write_page
             ┌──────────▼──────────┐
             │  BufferPoolManager   │  ← LRU/FIFO替换策略、命中率统计、替换日志
             │     （缓存管理）      │
             └──────────┬──────────┘
                        │
             ┌──────────▼──────────┐
             │     DiskManager      │  ← 页的分配/回收、磁盘文件读写
             │    （页式存储系统）    │
             └──────────┬──────────┘
                        │
                     数据文件(.db)
```

`Database`（`db/Database.h`）是整个系统的门面类，对外提供 `ExecuteSQL(sql)` 一个接口，内部串联上图所有模块。

## 目录结构

```
SQL-Compiler/
├── CMakeLists.txt
├── include/
│   ├── common/Error.h              # 统一异常类型 CompilerException
│   │
│   │   ── 编译器模块 ──
│   ├── lexer/{Token,Lexer}.h       # 词法分析：Token流（种别码+词素+行列号）
│   ├── ast/AST.h                   # AST：语句节点 + 表达式节点
│   ├── parser/Parser.h             # 递归下降语法分析器
│   ├── semantic/{SymbolTable,SemanticAnalyzer}.h  # 表/列元数据 + 语义检查
│   ├── plan/{Plan,Planner}.h       # 逻辑执行计划节点 + AST→Plan转换
│   ├── optimizer/Optimizer.h       # 谓词下推/列裁剪/常量折叠
│   ├── codegen/CodeGenerator.h     # Plan→指令序列
│   │
│   │   ── 存储子系统（对应"操作系统知识的实践"）──
│   ├── storage/Page.h              # 固定大小(4KB)物理页
│   ├── storage/DiskManager.h       # 页级磁盘读写、页分配/回收
│   ├── storage/Replacer.h          # 替换策略抽象接口
│   ├── storage/LRUReplacer.h       # LRU替换策略
│   ├── storage/FIFOReplacer.h      # FIFO替换策略
│   ├── storage/BufferPoolManager.h # 缓冲池：get_page/flush_page + 命中统计+替换日志
│   │
│   │   ── 数据库系统：存储引擎 ──
│   ├── storage_engine/Value.h      # 运行时值类型（INT/FLOAT/VARCHAR/NULL）
│   ├── storage_engine/Tuple.h      # 元组（行）与RID（记录标识符）
│   ├── storage_engine/TableHeap.h  # 表↔页集合映射，槽位式记录存取，SeqScan迭代器
│   │
│   │   ── 数据库系统：系统目录 ──
│   ├── catalog/SystemCatalog.h     # 元数据管理，自身作为特殊表持久化
│   │
│   │   ── 数据库系统：执行引擎 ──
│   ├── execution/Executor.h            # 算子基类（火山模型）+ ExecutionContext
│   ├── execution/ExpressionEvaluator.h # 在Tuple上对AST表达式求值
│   ├── execution/SeqScanExecutor.h     # 顺序扫描
│   ├── execution/FilterExecutor.h      # 条件过滤
│   ├── execution/ProjectExecutor.h     # 投影
│   ├── execution/CreateTableExecutor.h # 建表
│   ├── execution/DropTableExecutor.h   # 删表
│   ├── execution/InsertExecutor.h      # 插入
│   ├── execution/DeleteExecutor.h      # 删除
│   ├── execution/UpdateExecutor.h      # 更新（可选扩展语法）
│   ├── execution/ExecutionEngine.h     # Plan树 → Executor树，驱动执行
│   │
│   └── db/Database.h               # 门面类：ExecuteSQL(sql) 一站式入口
│
├── src/                             # 与include一一对应的实现文件（骨架，全部为TODO）
│   └── main.cpp                     # CLI入口
└── tests/                           # 预留测试目录
```

## 模块职责一览

| 层 | 模块 | 职责 |
| --- | --- | --- |
| 编译器 | `lexer` | SQL源码 → Token流，识别关键字/标识符/常量/运算符/分隔符，非法字符报错(类型+位置) |
| 编译器 | `ast` / `parser` | Token流 → AST，支持 SELECT/INSERT/UPDATE/DELETE/CREATE TABLE/DROP TABLE，语法错误报错(位置+期望符号) |
| 编译器 | `semantic` | 表/列存在性检查、类型一致性检查、INSERT列数/列序检查，维护Catalog |
| 编译器 | `plan` / `optimizer` / `codegen` | AST → 逻辑执行计划(SeqScan/Filter/Project等算子) → 优化 → 指令序列 |
| 存储 | `storage` | 页的分配/释放/读写（DiskManager），LRU/FIFO缓存（BufferPoolManager），命中统计与替换日志 |
| 数据库 | `storage_engine` | Row(Tuple)与Page的映射关系，记录的序列化，表数据在磁盘上的物理组织（TableHeap） |
| 数据库 | `catalog` | 维护表名/列名/列类型等元数据，元数据本身作为一张特殊表持久化存储 |
| 数据库 | `execution` | 解析并执行逻辑计划，实现 CreateTable/Insert/SeqScan/Filter/Project/Delete/Update 等算子 |
| 数据库 | `db` | 门面类，对外提供CLI/API：输入SQL文本，返回Token流/AST/语义结果/执行计划/查询结果或错误信息 |

## 数据流转细节

**写路径（如 INSERT）**：
`SQL文本 → Lexer → Parser(AST) → SemanticAnalyzer(校验) → Planner(InsertNode)
→ InsertExecutor对VALUES求值为Tuple → TableHeap::InsertTuple()
→ BufferPoolManager::GetPage()/NewPage()（缓存未命中则触发替换）
→ DiskManager::WritePage()落盘`

**读路径（如 SELECT ... WHERE）**：
`SQL文本 → ... → Planner生成 Project(Filter(SeqScan)) 计划树
→ ExecutionEngine::BuildExecutor()构造对应的算子树
→ SeqScanExecutor通过TableHeap::Iterator逐条读取Tuple（背后是GetPage/ReadPage）
→ FilterExecutor用ExpressionEvaluator对WHERE表达式求值，筛选记录
→ ProjectExecutor按SELECT列表计算输出列
→ ExecutionEngine收集所有输出Tuple，包装为ExecutionResult返回`

## 已支持 / 可扩展的SQL语法

**核心语法（AST已建模，需自行实现解析与执行）**：
- `SELECT [DISTINCT] col1, col2, ... FROM table [JOIN ...] [WHERE ...] [GROUP BY ...] [HAVING ...] [ORDER BY ...] [LIMIT n]`
- `INSERT INTO table [(col1, col2, ...)] VALUES (...), (...), ...`
- `UPDATE table SET col1 = expr1, ... [WHERE ...]`
- `DELETE FROM table [WHERE ...]`
- `CREATE TABLE table (col1 TYPE [PRIMARY KEY] [NOT NULL], ...)`
- `DROP TABLE table`

**可选扩展（题目中的"可选扩展"部分，已预留相应结构）**：
- `UPDATE`：`UpdateStatement` / `UpdateNode` / `UpdateExecutor` 均已建模
- `JOIN` / `ORDER BY` / `GROUP BY`：`JoinClause` / `OrderByItem` / `AggregateNode` 已在AST与Plan中建模，但对应的 `JoinExecutor` / `SortExecutor` / `AggregateExecutor` 尚未创建，需要时可参照现有Executor风格自行添加
- 查询优化（谓词下推等）：`Optimizer::PushDownPredicates()` 已留出接口

## 测试用例建议（对应题目要求）

```sql
CREATE TABLE student(id INT, name VARCHAR, age INT);
INSERT INTO student(id,name,age) VALUES (1,'Alice',20);
SELECT id,name FROM student WHERE age > 18;
DELETE FROM student WHERE id = 1;
```

**错误测试**：缺分号、列名拼写错误、类型不匹配、值个数不一致、未闭合字符串等，
应分别在 Parser（语法错误）与 SemanticAnalyzer（语义错误）阶段被捕获并给出 `位置+原因`。

## 构建方式

```bash
mkdir build && cd build
cmake ..
cmake --build .
./sqlcompiler [数据文件路径，默认 sqlcompiler.db]
```

> 当前所有函数体均为空/占位实现（标记 `TODO`），可以编译通过（已用 g++ 验证全部33个源文件
> 编译、链接、运行均无错误），但运行时不会产生正确结果，需要逐个模块补充实现。

## 建议的实现顺序

1. **词法/语法**：`lexer/Token.cpp` → `lexer/Lexer.cpp` → `ast/AST.cpp`（补ToString便于调试）→ `parser/Parser.cpp`
2. **页式存储**（可独立于编译器先行开发、单独测试）：
   `storage/Page.cpp` → `storage/DiskManager.cpp` → `storage/LRUReplacer.cpp`/`FIFOReplacer.cpp` → `storage/BufferPoolManager.cpp`
3. **存储引擎**：`storage_engine/Value.cpp` → `storage_engine/Tuple.cpp` → `storage_engine/TableHeap.cpp`（依赖BufferPoolManager）
4. **系统目录**：`semantic/SymbolTable.cpp` → `catalog/SystemCatalog.cpp`（依赖TableHeap，实现元数据的持久化与加载）
5. **语义分析 / 计划生成**：`semantic/SemanticAnalyzer.cpp` → `plan/Plan.cpp` → `plan/Planner.cpp`
6. **执行引擎**：`execution/Executor.cpp` → `execution/ExpressionEvaluator.cpp` → 各 `*Executor.cpp`
   （建议顺序：SeqScan → CreateTable → Insert → Filter → Project → Delete → Update）→ `execution/ExecutionEngine.cpp`
7. **优化器（可选，最后做）**：`optimizer/Optimizer.cpp`
8. **代码生成（可选，若只需要执行结果可跳过；若要求输出独立的指令序列则实现）**：`codegen/CodeGenerator.cpp`
9. **总入口**：`db/Database.cpp` → `main.cpp`，实现CLI，串联全部流程并支持多语句脚本、结果打印

## 关于"执行计划输出格式"

题目要求执行计划可输出为树形结构/JSON/S表达式。当前 `PlanNode::ToString()` 与 `Instruction::ToString()`
均预留了文本化接口；若需要JSON格式，可在 `PlanNode` 基础上另行实现一个 `ToJson()` 方法，
或在 `CodeGenerator` 中新增一种"序列化为JSON"的输出模式，不影响现有算子结构。
