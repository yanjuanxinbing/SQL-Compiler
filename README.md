# SQL-Compiler

一个使用 C++ 实现的 SQL 编译器项目框架。当前仓库只包含**框架代码**（头文件声明 + 空函数体骨架），
所有函数逻辑均标记为 `TODO`，需要自行实现。

## 编译流程

```
SQL文本
   │  Lexer（词法分析）
   ▼
Token序列
   │  Parser（语法分析）
   ▼
AST（抽象语法树）
   │  SemanticAnalyzer（语义分析，依赖 SymbolTable）
   ▼
校验后的AST
   │  Planner（生成逻辑执行计划）
   ▼
逻辑执行计划树（Plan）
   │  Optimizer（查询优化：谓词下推/列裁剪/常量折叠）
   ▼
优化后的执行计划
   │  CodeGenerator（生成目标指令）
   ▼
指令序列（可交由自行实现的执行引擎运行）
```

## 目录结构

```
SQL-Compiler/
├── CMakeLists.txt              # 顶层构建脚本
├── include/                    # 头文件（声明）
│   ├── common/
│   │   └── Error.h             # 统一异常类型 CompilerException
│   ├── lexer/
│   │   ├── Token.h             # Token / TokenType 定义
│   │   └── Lexer.h             # 词法分析器
│   ├── ast/
│   │   └── AST.h               # AST节点：语句 + 表达式
│   ├── parser/
│   │   └── Parser.h            # 递归下降语法分析器
│   ├── semantic/
│   │   ├── SymbolTable.h       # 表/列元数据目录（Catalog）
│   │   └── SemanticAnalyzer.h  # 语义检查（表/列是否存在等）
│   ├── plan/
│   │   ├── Plan.h              # 逻辑执行计划节点（Scan/Filter/Project/Join...）
│   │   └── Planner.h           # AST -> 逻辑执行计划
│   ├── optimizer/
│   │   └── Optimizer.h         # 查询优化规则
│   └── codegen/
│       └── CodeGenerator.h     # 执行计划 -> 指令序列
├── src/                         # 与include一一对应的实现文件（骨架）
│   └── main.cpp                 # 命令行入口，串联整个编译流程
└── tests/                       # 预留测试目录
    ├── CMakeLists.txt
    └── lexer_test.cpp.example   # 测试写法示例（未接入构建）
```

## 模块说明

| 模块 | 职责 |
| --- | --- |
| `lexer` | 将SQL源字符串切分为Token序列，识别关键字、标识符、字面量、运算符 |
| `ast` | 定义SELECT/INSERT/UPDATE/DELETE/CREATE TABLE/DROP TABLE语句及表达式节点 |
| `parser` | 递归下降解析，将Token序列构造为AST |
| `semantic` | 基于`SymbolTable`（表结构目录）对AST做语义检查（表/列是否存在、类型是否匹配等） |
| `plan` | 将AST转换为树形逻辑执行计划（SeqScan/Filter/Project/Join/Sort/Limit/Aggregate等节点） |
| `optimizer` | 对逻辑执行计划进行等价改写优化（谓词下推、列裁剪、常量折叠） |
| `codegen` | 将优化后的执行计划编译为线性指令序列，供后续自行实现的执行引擎运行 |
| `common` | 跨模块共享的错误类型 `CompilerException` 与格式化函数 |

## 已支持的SQL语法（AST层面已建模，需自行实现解析与执行逻辑）

- `SELECT [DISTINCT] col1, col2, ... FROM table [JOIN ...] [WHERE ...] [GROUP BY ...] [HAVING ...] [ORDER BY ...] [LIMIT n]`
- `INSERT INTO table [(col1, col2, ...)] VALUES (...), (...), ...`
- `UPDATE table SET col1 = expr1, ... [WHERE ...]`
- `DELETE FROM table [WHERE ...]`
- `CREATE TABLE table (col1 TYPE [PRIMARY KEY] [NOT NULL], ...)`
- `DROP TABLE table`

表达式支持：算术运算（`+ - * /`）、比较运算（`= != <> < <= > >=`）、逻辑运算（`AND OR NOT`）、
括号、列引用（`table.column`）、字面量（整数/浮点数/字符串/NULL）、函数调用（如聚合函数）。

## 构建方式

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

> 注意：当前所有函数体均为空/占位实现（标记 `TODO`），因此可以编译通过，
> 但运行时不会产生正确结果，需要你逐个模块补充实现。

## 建议的实现顺序

1. `lexer/Token.cpp`、`lexer/Lexer.cpp` —— 先能把SQL切成Token
2. `ast/AST.cpp` —— 补齐各节点的`ToString()`，方便调试打印AST
3. `parser/Parser.cpp` —— 递归下降解析出AST
4. `semantic/SymbolTable.cpp`、`semantic/SemanticAnalyzer.cpp` —— 表结构管理与语义检查
5. `plan/Plan.cpp`、`plan/Planner.cpp` —— AST转执行计划
6. `optimizer/Optimizer.cpp` —— 优化规则（可选，最后再做）
7. `codegen/CodeGenerator.cpp` —— 计划转指令
8. `main.cpp` —— 串联整个流程，做一个简单的命令行/REPL工具
