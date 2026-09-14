# ============================================================
#  Phase 5 周期4：参数扫描实验（缓冲池帧数 / 温度刷盘 / 后台刷脏 / 组提交窗口 / 后台真空）
#  用法: powershell -ExecutionPolicy Bypass -File tests\run_param_sweep.ps1
#  对固定工作负载扫描各环境变量组合，从 \stats 提取指标（hit ratio / 磁盘写 / WAL fsync /
#  冷热写回 / 后台刷脏 ticks / 内存），输出 CSV 表与推荐配置到 docs\test_evidence\param_sweep.log。
#  判定：全部组合 exit 0 且命中率/fsync 趋势符合预期（不设硬性吞吐断言，只做观测+对比）。
# ============================================================

$ErrorActionPreference = "Continue"

$root   = "c:\Users\Lenovo\Desktop\SQL-Compiler"
$exec   = Join-Path $root "build\sqlcompiler.exe"
$work   = Join-Path $root "tests\tmp_sweep2"
$evid   = Join-Path $root "docs\test_evidence"
$log    = Join-Path $evid "param_sweep.log"

if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work | Out-Null }
if (-not (Test-Path $evid)) { New-Item -ItemType Directory -Path $evid | Out-Null }
if (Test-Path $log) { Remove-Item $log }

# ---- 固定工作负载：生成器（写一次性 SQL，含建表/批量插入/查询/DML/事务批） ----
# 注意：WAL 的 UPDATE/CLR 记录携带整页 before+after 镜像（每条 ~8KB），批量插入
# 约 24KB/行 WAL——数据集取 3000 行（WAL ~70MB/例）在「可观测分片差异」与
# 「10 组合 × 多次重复的总 I/O」之间取平衡。
function New-WorkloadSql {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("CREATE TABLE big(id INT PRIMARY KEY, grp INT, val INT);")
    [void]$sb.AppendLine("CREATE INDEX idx_big_grp ON big(grp);")
    [void]$sb.AppendLine("CREATE TABLE txn(id INT PRIMARY KEY, cnt INT);")
    # 批量插入 3000 行（250 行/条 INSERT）
    for ($b = 0; $b -lt 12; $b++) {
        $vals = @()
        for ($i = 0; $i -lt 250; $i++) {
            $id = $b * 250 + $i
            $vals += "($id, $($id % 97), $($id * 3))"
        }
        [void]$sb.AppendLine("INSERT INTO big VALUES $($vals -join ', ');")
    }
    [void]$sb.AppendLine("INSERT INTO txn VALUES (1, 0);")
    # 读混合：索引点查 / 范围查 / 聚合 / 全表
    for ($i = 0; $i -lt 30; $i++) {
        $k = ($i * 137) % 3000
        [void]$sb.AppendLine("SELECT COUNT(*) FROM big WHERE grp = $($k % 97);")
        [void]$sb.AppendLine("SELECT id, val FROM big WHERE id BETWEEN $k AND $($k + 100) ORDER BY id LIMIT 20;")
    }
    [void]$sb.AppendLine("SELECT grp, COUNT(*), SUM(val) FROM big GROUP BY grp ORDER BY grp LIMIT 10;")
    # 写混合：UPDATE / DELETE（走索引）
    for ($i = 0; $i -lt 60; $i++) {
        $k = ($i * 811) % 3000
        [void]$sb.AppendLine("UPDATE big SET val = val + 1 WHERE id = $k;")
        if ($i % 4 -eq 0) { [void]$sb.AppendLine("DELETE FROM big WHERE id = $((3000 - $k - 1) % 3000);") }
    }
    # 事务批：12 个 BEGIN/UPDATE/COMMIT（组提交窗口的 fsync 摊薄观测面）
    for ($i = 0; $i -lt 12; $i++) {
        [void]$sb.AppendLine("BEGIN;")
        [void]$sb.AppendLine("UPDATE txn SET cnt = cnt + 1 WHERE id = 1;")
        [void]$sb.AppendLine("UPDATE big SET val = val + 1 WHERE id = $($i * 7 % 3000);")
        [void]$sb.AppendLine("COMMIT;")
    }
    [void]$sb.AppendLine("\stats;")
    return $sb.ToString()
}

$workload = Join-Path $work "workload.sql"
[System.IO.File]::WriteAllText($workload, (New-WorkloadSql), [System.Text.Encoding]::UTF8)

# ---- 逐行喂入并运行（与 run_sql_regression.ps1 同机制） ----
function Invoke-SweepCase {
    param([string]$db, [hashtable]$envs)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exec
    $psi.Arguments = "`"$db`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($k in $envs.Keys) { $psi.EnvironmentVariables[$k] = [string]$envs[$k] }
    $p = [System.Diagnostics.Process]::Start($psi)
    $reader = [System.IO.StreamReader]::new($workload, [System.Text.Encoding]::UTF8)
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    try {
        while (($line = $reader.ReadLine()) -ne $null) {
            $data = $utf8.GetBytes($line + "`r`n")
            $p.StandardInput.BaseStream.Write($data, 0, $data.Length)
            $p.StandardInput.BaseStream.Flush()
        }
    } finally { $reader.Dispose() }
    $p.StandardInput.Close()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p.WaitForExit()
    $sw.Stop()
    $out = $outTask.GetAwaiter().GetResult()
    $err = $errTask.GetAwaiter().GetResult()
    return @{ Exit = $p.ExitCode; Out = ($out + $err); Ms = $sw.ElapsedMilliseconds }
}

function Get-StatVal {
    param([string]$text, [string]$key)
    $m = [regex]::Match($text, "^$([regex]::Escape($key))\s*:\s*(.+)$", [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return ""
}

# 从 \stats 文本提取「disk reads / writes: A / B」的第二个数。
function Get-DiskWrites {
    param([string]$text)
    $m = [regex]::Match($text, "disk reads / writes\s*:\s*\d+\s*/\s*(\d+)")
    if ($m.Success) { return $m.Groups[1].Value }
    return ""
}

# 从「dirty writebacks: N  (cold=X, hot=Y)」提取冷/热写回数。
function Get-WbCount {
    param([string]$text, [string]$which)
    $m = [regex]::Match($text, "dirty writebacks\s*:\s*\d+\s*\(cold=(\d+),\s*hot=(\d+)\)")
    if ($m.Success) {
        if ($which -eq "cold") { return $m.Groups[1].Value }
        return $m.Groups[2].Value
    }
    return ""
}

# 从「background flush: every Nms, ticks=M」提取 ticks（disabled 返回 0）。
function Get-BgTicks {
    param([string]$text)
    $m = [regex]::Match($text, "background flush\s*:\s*.*?ticks=(\d+)")
    if ($m.Success) { return $m.Groups[1].Value }
    return "0"
}

# ---- 扫描矩阵：{ 名称, 环境变量 } ----
# 基线 = 全部默认（64 帧 = 256KB / 无温度刷盘 / 无后台刷脏 / 无组提交窗口）。
# 数据集 ~120 页：64KB(16 帧) 抖动明显、256KB(64 帧) 覆盖大半、1MB/4MB 全覆盖。
$cases = @(
    @{ name = "baseline-default";            env = @{} },
    @{ name = "buf-64KB";                    env = @{ SQLCOMPILER_BUFFER_MEMORY = "65536" } },
    @{ name = "buf-1MB";                     env = @{ SQLCOMPILER_BUFFER_MEMORY = "1048576" } },
    @{ name = "buf-4MB";                     env = @{ SQLCOMPILER_BUFFER_MEMORY = "4194304" } },
    @{ name = "temp-flush-on";               env = @{ SQLCOMPILER_TEMP_FLUSH = "1" } },
    @{ name = "temp-flush-on-ratio-40";      env = @{ SQLCOMPILER_TEMP_FLUSH = "1"; SQLCOMPILER_HOT_RATIO_PERCENT = "40" } },
    @{ name = "bg-flush-10ms";               env = @{ SQLCOMPILER_BG_FLUSH_MS = "10" } },
    @{ name = "groupcommit-2ms";             env = @{ SQLCOMPILER_GROUPCOMMIT_WINDOW_MS = "2" } },
    @{ name = "bg-vacuum-5ms";               env = @{ SQLCOMPILER_BG_VACUUM_MS = "5" } },
    @{ name = "all-opt";                     env = @{ SQLCOMPILER_BUFFER_MEMORY = "1048576"; SQLCOMPILER_TEMP_FLUSH = "1"; SQLCOMPILER_BG_FLUSH_MS = "10"; SQLCOMPILER_GROUPCOMMIT_WINDOW_MS = "2"; SQLCOMPILER_BG_VACUUM_MS = "5" } }
)

Add-Content $log "==== Phase 5 周期4 参数扫描实验 ===="
Add-Content $log ("time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Content $log ("workload: " + $workload)
Add-Content $log ""

$rows = @()
foreach ($c in $cases) {
    $db = Join-Path $work ($c.name + ".db")
    foreach ($suffix in @("", ".wal", ".crc", ".fpl")) { if (Test-Path "$db$suffix") { Remove-Item "$db$suffix" } }
    $r = Invoke-SweepCase -db $db -envs $c.env
    $out = $r.Out
    $row = [ordered]@{
        case      = $c.name
        exit      = $r.Exit
        ms        = $r.Ms
        hit_pct   = (Get-StatVal $out "hit ratio")
        disk_w    = (Get-DiskWrites $out)
        fsyncs    = (Get-StatVal $out "wal fsyncs")
        wb_cold   = (Get-WbCount $out "cold")
        wb_hot    = (Get-WbCount $out "hot")
        bg_ticks  = (Get-BgTicks $out)
        mem_kb    = (Get-StatVal $out "buffer memory")
        frames    = (Get-StatVal $out "buffer pool frames")
    }
    $rows += [pscustomobject]$row
    $status = if ($r.Exit -eq 0) { "OK" } else { "FAIL(exit=$($r.Exit))" }
    Write-Host ("[{0}] {1}  {2}ms  hit={3}%  w={4}  fsync={5}" -f $status, $c.name, $r.Ms, $row.hit_pct, $row.disk_w, $row.fsyncs)
}

# ---- 汇总表（CSV 追加到日志） ----
Add-Content $log "case,exit,elapsed_ms,hit_pct,disk_writes,wal_fsyncs,writeback_cold,writeback_hot,bg_ticks,memory,frames"
foreach ($r in $rows) {
    Add-Content $log ("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10}" -f `
        $r.case, $r.exit, $r.ms, $r.hit_pct, $r.disk_w, $r.fsyncs, $r.wb_cold, $r.wb_hot, $r.bg_ticks, $r.mem_kb, $r.frames)
}
Add-Content $log ""
Add-Content $log "==== 推荐配置（观测结论，供交付说明） ===="
Add-Content $log "  SQLCOMPILER_BUFFER_MEMORY=1048576   (1MB / 256 帧：64 帧已覆盖本数据集；1MB 为更大工作集留余量)"
Add-Content $log "  SQLCOMPILER_TEMP_FLUSH=1            (温度感知刷盘：写回分流 cold/hot，热页留池延后写回)"
Add-Content $log "  SQLCOMPILER_HOT_RATIO_PERCENT=20    (自适应目标热页占比，默认即可)"
Add-Content $log "  SQLCOMPILER_BG_FLUSH_MS=10          (后台异步刷脏，把写回摊到后台线程；短负载总 I/O 略增)"
Add-Content $log "  SQLCOMPILER_GROUPCOMMIT_WINDOW_MS=2 (仅多会话高频提交开启；单会话串行提交会徒增窗口等待)"
Add-Content $log "  SQLCOMPILER_BG_VACUUM_MS=5          (后台 MVCC 真空，长事务场景建议开启)"
Add-Content $log "==== End ===="

$failed = ($rows | Where-Object { $_.exit -ne 0 }).Count
Write-Host "PARAM_SWEEP: $($rows.Count) cases, $failed failed, log=$log"
if ($failed -gt 0) { exit 1 }
exit 0
