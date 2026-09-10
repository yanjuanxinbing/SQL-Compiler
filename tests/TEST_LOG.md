# 测试记录（TEST_LOG）

用于追踪测试问题原因、解决方案与测试结果。

---

## 2026-09-10 测试08（08_types.sql）失败修复

### 问题原因
1. **词法器不支持科学记数法**：`Lexer::ScanNumber()` 只处理整数/小数，`08_types.sql` 中
   `-2.5e-3` 的 `e` 被当作标识符 → 语法错误 `expected ')' after VALUES row (got 'e')`。
2. **NULL 序列化宽度不一致（崩溃根因）**：`Value::SerializeTo` 对 NULL 统一写 4 字节，
   而 `Value::DeserializeFrom` 对 FLOAT 列按 8 字节读取。INSERT 未指定 FLOAT 列时该列存
   NULL（4 字节），扫描读取时多消费 4 字节 → 同记录后续字段及后续记录全部错位 →
   产生野值/越界 → 进程 0xC0000005 崩溃。
3. **SystemCatalog 类型编码有损**：`DataTypeId()` 把 BIGINT/DOUBLE/TEXT 全部编码为 ID 3，
   `DataTypeName(3)` 解码为 VARCHAR → 数据库重载后表结构（列类型）被改变。
4. **连带缺陷（测试08"类型混合运算"段暴露）**：`ExpressionEvaluator` 混合运算对
   INTEGER 操作数直接调 `AsFloat()`（内部 `float_val_` 对整数值恒为 0）→ `f * 2` 恒为 0。

### 解决方案
- `src/lexer/Lexer.cpp`：`ScanNumber()` 增加 `e/E [+/-] 数字` 指数段（仅当 e 后是
  可选符号+数字时才消费，避免影响 `1 error` 类标识符扫描）。
- `include/src storage_engine/Value.h|Value.cpp`：新增
  `Value::SerializeNullTo(buf, column_type)` / `NullSerializedSize(column_type)`，
  NULL 按列类型等宽写入：FLOAT=8 字节全 1 位模式（信号 NaN payload）、VARCHAR=-1 长度前缀、
  INTEGER=-1 标记；FLOAT 反序列化识别该标记；`ValueTypeFromString` 补 BIGINT→INTEGER、
  DECIMAL→FLOAT 映射。
- `storage_engine/Tuple.h|Tuple.cpp`：新增 `Serialize(column_types)` 重载，NULL 按列类型
  等宽序列化，与 `Deserialize(column_types)` 消费宽度严格一致。
- `storage_engine/TableHeap.h|TableHeap.cpp`：`InsertTuple/UpdateTuple/InsertIntoPage`
  增加 `column_types` 参数，写库路径改为 schema 驱动序列化。
- 调用方同步更新：`InsertExecutor`（构建列类型 schema，并复用于自增扫描）、
  `UpdateExecutor`、`SystemCatalog`（元数据 VARCHAR 列）。
- `src/catalog/SystemCatalog.cpp`：`DataTypeId/DataTypeName` 扩展为
  0=INT/1=FLOAT/2=VARCHAR/3=BIGINT/4=DOUBLE/5=TEXT/6=CHAR/7=STRING，编码前先归一化大写。
- `src/execution/ExpressionEvaluator.cpp`：ADD/SUB/MUL/DIV 增加 `ToDouble`，
  将 INTEGER 操作数提升为 double 后再运算。

### 测试结果
- 测试08 单独运行：退出码 0、无 Error 行；12 行数据全部对齐正确，
  `-2.5e-3 → -0.002500`、`2.5`/科学记数法解析正常、`f * 2` 结果正确。
- 全量回归（run_all_tests.bat）：21 passed / 6 failed（失败项为既有引擎能力缺口，见下节）。

---

## 2026-09-10 新增测试 26_edge_cases.sql

- 命名遵循 `NN_主题.sql` 规范；与 run_all_tests.bat 自动集成（放入 tests/sql 即被执行，
  判定标准：退出码 0 且无 `Error:` 行）。
- 覆盖内容：
  - **数值边界**：INT 极值 ±2147483647、科学记数法 `1e15`/`2E3`/`1.5e-3`/`-2.5e-3`、负零。
  - **字符串边界**：空串、`\t` 与 `''` 转义、中英混排+特殊符号、80 字符长串、LIKE 前后缀。
  - **NULL 与空集**：部分列插入、COUNT(*) vs COUNT(col)、空集聚合、LIMIT 0/超行数、
    DISTINCT 含 NULL、NULL 参与排序。
  - **异常处理（优雅路径）**：整型/浮点除零→NULL、反向 BETWEEN→空集、0 行 UPDATE/DELETE。
    注：预期报错类语句（表不存在/语法错误）不纳入自动化文件，否则会被 runner 判失败；
    该类路径由 tests/*_test.cpp 单元测试覆盖。
  - **性能冒烟**：150 行（3 条多行 VALUES）跨多个 4KB 页 + 全表 COUNT、GROUP BY/HAVING、
    ORDER BY DESC + LIMIT、范围过滤。
- 结果：退出码 0、无 Error 行；关键结果手工验算一致
  （如 `val=7 ORDER BY score DESC LIMIT 5 → 147/137/127/117/107`，分组 AVG=37.25/37.75/38.25）。

---

## 2026-09-10 测试22（22_boundary.sql）失败修复

### 问题原因
1. **词法器不支持 `||` 字符串连接运算符**：`'line1' || 'line2'` 触发
   `Error: [Lexical] unexpected character: '|'`。（注：`LIMIT offset, count` 偏移语法
   经排查实际已正确支持，最初误报与其无关。）
2. **修复过程中发现代码库曾被外部还原**（非 git 仓库，应为 IDE 撤销/丢弃修改）：
   测试08 修复中的 `ExpressionEvaluator` 整型提升（ToDouble）、SystemCatalog
   BIGINT/DOUBLE/TEXT 类型映射、`ValueTypeFromString` 的 BIGINT/DECIMAL 映射丢失；
   NULL 等宽序列化则被以另一设计重写（`Value::SerializeTo(buf, column_type)` 重载，
   FLOAT 列 NULL 标记为 `-1`+4 零字节）。丢失部分已补回，两套设计不冲突。

### 解决方案
- `include/lexer/Token.h` + `src/lexer/Token.cpp`：新增 `OP_CONCAT`（`||`）。
- `src/lexer/Lexer.cpp`：`ScanOperatorOrSymbol()` 识别双字符 `||`。
- `include/ast/AST.h` + `src/ast/AST.cpp`：`BinaryOperator` 新增 `CONCAT`。
- `src/parser/Parser.cpp`：`ParseAdditiveExpr()` 接受 `||`（与 +/- 同级）。
- `src/execution/ExpressionEvaluator.cpp`：新增 CONCAT 求值（任一操作数为 NULL → NULL，
  非字符串操作数按文本形式连接）；并重新应用丢失的 ADD/SUB/MUL/DIV 整型提升 `ToDouble`。
- `src/catalog/SystemCatalog.cpp`：重新应用 BIGINT/DOUBLE/TEXT/CHAR/STRING 类型 ID 映射
  （编码前归一化大写）。
- `src/storage_engine/Value.cpp`：`ValueTypeFromString` 重新应用 BIGINT→INTEGER、
  DECIMAL→FLOAT。

### 测试结果
- 测试22：退出码 0、无 Error 行，`'line1' || 'line2'` → `line1line2`。
- 测试08 / 26 复跑通过，无回归。
- 全量：**22 passed / 5 failed**（剩余 5 项为既有引擎缺口，见下节）。

---

## 既有引擎能力缺口（导致 5 个历史测试失败，待后续修复）

以下失败与上述修复无关（相关测试文件此前从未全量运行过）：

| 测试 | 原因 |
| --- | --- |
| 10_distinct | 解析器不支持表达式中的 DISTINCT（如聚合函数参数内） |
| 12_constraints | 不支持表级 `PRIMARY KEY (col)` 约束语法 |
| 14_insert_select | 不支持 `INSERT INTO ... SELECT` |
| 21_ddl_variants | 同 12（表级主键）；另测试期望重建表语义，触发 "table already exists" |
| 25_truncate | TRUNCATE 执行路径导致后续 "table not found" |
