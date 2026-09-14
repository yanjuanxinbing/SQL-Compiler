# 项目快速启动与运行指南

## 1. 环境要求

- 操作系统：Windows 10/11（64 位）。(本项目页面管理模块的主力验证环境。)
- 编译器：MinGW-w64 **g++ 支持 C++17**（已验证：`D:/mingw64/bin/g++.exe`，gcc 15.1）。
- 构建工具：CMake（≥ 3.10）+ mingw32-make（MinGW Makefiles 生成器）。
- 可选调优：`SQLCOMPILER_BUFFER_MEMORY`（缓冲池内存上限，字节）、`SQLCOMPILER_BG_FLUSH_MS`（后台刷脏间隔）。

## 2. 依赖检查

在 PowerShell 中确认工具可用：

```powershell
g++ --version            # 需 ≥ 8（支持 C++17）
cmake --version          # 需 ≥ 3.10
D:/mingw64/bin/mingw32-make.exe --version
```

若 `g++`/`cmake` 不在 PATH，请记录其绝对路径，后续命令用到。

## 3. 配置与构建

首次配置会自动生成 `build/` 目录：

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
      -DCMAKE_CXX_COMPILER=D:/mingw64/bin/g++.exe `
      -DCMAKE_MAKE_PROGRAM=D:/mingw64/bin/mingw32-make.exe
```

> 说明：若不传 `-DCMAKE_CXX_COMPILER` 等项目可能探测失败，建议始终显式指定；生成器必须是 MinGW Makefiles 以匹配该工具链。

编译全部目标（主程序 + 存储单测）：

```powershell
cmake --build build
```

产物：
- `build/sqlcompiler.exe` — SQL 命令行入口
- `build/storage_ut.exe` — 存储子系统单元测试

> 新增 `.cpp` 后需重新执行一次 configure（`file(GLOB)` 在配置期求值）。

## 4. 运行

### 4.1 运行存储单元测试

```powershell
.\build\storage_ut.exe
```

期望输出：`checks: 5433   fails: 0` / `RESULT: PASS`。

### 4.2 运行 SQL 交互式 CLI

```powershell
.\build\sqlcompiler.exe <db_文件路径>
```

出现 `sqlcompiler>` 提示后逐条输入：

```text
CREATE TABLE t(a INT);
INSERT INTO t VALUES (1);
SELECT * FROM t;
\stats;
exit;
```

> `\stats` 等指令需带分号/回车；除 `exit;` 外也可直接用 `\q`。数据文件、`<db>.wal`、`<db>.crc`、`<db>.fpl` 会自动创建并持久化。

### 4.3 一次性执行脚本（逐行喂入最稳）

```powershell
Get-Content script.sql | .\build\sqlcompiler.exe .\data\mydb.bin
```

> 一次性把整段脚本写进 stdin 偶发丢失首条语句，逐行管道（如上）可复现真实 harness 行为。

### 4.4 调优（内存上限 / 后台刷脏）

```powershell
# 缓冲池内存上限设为 16 KB（= 4 帧）
$env:SQLCOMPILER_BUFFER_MEMORY = "16384"

# 每 50ms 后台刷脏（默认 0 关闭）
$env:SQLCOMPILER_BG_FLUSH_MS    = "50"

.\build\sqlcompiler.exe .\data\mydb.bin
# 进入后执行 \stats; 查看 buffer memory / background flush 一行生效
```

取消环境变量：`Remove-Item Env:SQLCOMPILER_BUFFER_MEMORY` / 置空即可回退默认（64 帧 = 256 KB）。

## 5. 目录结构（页面管理模块相关）

```
src/storage/          存储子系统实现
  DiskManager.cpp     页分配/回收、页 CRC、空闲页位图、块设备
  BufferPoolManager.cpp 缓冲池、替换、脏页写回、后台刷脏、页锁
  LRU/FIFO/Clock/LRUKReplacer.cpp  替换算法
  Page.cpp            页（帧）元数据与读写锁
  PageAllocator.cpp   页内内存分配器
  BlockDevice.cpp     文件/故障注入设备
include/index/PageGuard.h  RAII 页句柄（含页级读写锁）
tests/storage/storage_ut.cpp  单元测试
```

## 6. 常见问题

- **configure 报 `CMAKE_CXX_COMPILER not set` / 引用 `nmake`**：未显式给生成器与编译器，按 §3 补全参数。
- **新增源文件链接失败**：重新跑 §3 的 configure。
- **UBSan 缺 `-lubsan`**：MinGW 未装运行时，本机不支持 `-fsanitize=undefined`；开发期用 `-Wall -Wextra` 告警审计替代（页面管理模块当前零告警）。
- **用 `< 文件` 喂脚本报 `RedirectionNotSupported`**：PowerShell 不支持 `<` 输入重定向（cmd 才有）；且 CLI 的第一个参数是**数据库文件路径**、不是 SQL 脚本路径。请改用 §4.3 的管道方式：`Get-Content script.sql | .\build\sqlcompiler.exe <db路径>`。
- **不带参数启动后 `CREATE TABLE` 报 `table already exists`**：无参数时默认打开当前目录的 `sqlcompiler.db`，其中已有上次会话的表。属正常的语义检查与持久化行为；想用全新库就传一个新路径参数。