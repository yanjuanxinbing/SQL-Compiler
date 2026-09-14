# ============================================================
#  Phase 6 U2-1：\bench 参数扫描（进程内多会话并发读写混合负载）
#  用法: powershell -ExecutionPolicy Bypass -File tests\run_bench.ps1
#  对同一 \bench 工作负载扫描各环境变量组合（缓冲池内存 / 温度刷盘 / 后台刷脏 /
#  组提交窗口），从 bench report 提取吞吐 / avg / p95 延迟，从 \stats 提取
#  wal fsyncs / disk writes，输出 CSV 表到 docs\test_evidence\bench_sweep.log。
#  判定：全部组合 exit 0，观测并对比不同配置下的吞吐 / 延迟 / fsync 差异。
# ============================================================

$ErrorActionPreference = "Continue"

$root   = "c:\Users\Lenovo\Desktop\SQL-Compiler"
$exec   = Join-Path $root "build\sqlcompiler.exe"
$work   = Join-Path $root "tests\tmp_bench2"
$evid   = Join-Path $root "docs\test_evidence"
$log    = Join-Path $evid "bench_sweep.log"

if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work | Out-Null }
if (-not (Test-Path $evid)) { New-Item -ItemType Directory -Path $evid | Out-Null }
if (Test-Path $log) { Remove-Item $log }

# ---- 固定 \bench 工作负载：4 线程 × 400 op/线程（2000 op） ----
$benchCmd = "\bench 4 400;"
$statsCmd = "\stats;"
$utf8 = [System.Text.UTF8Encoding]::new($false)

function Invoke-BenchCase {
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
    $data = $utf8.GetBytes($benchCmd + "`r`n" + $statsCmd + "`r`n")
    $p.StandardInput.BaseStream.Write($data, 0, $data.Length)
    $p.StandardInput.BaseStream.Flush()
    $p.StandardInput.Close()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    $out = $outTask.GetAwaiter().GetResult()
    $err = $errTask.GetAwaiter().GetResult()
    return @{ Exit = $p.ExitCode; Out = ($out + $err) }
}

function Get-StatVal {
    param([string]$text, [string]$key)
    $m = [regex]::Match($text, "^$([regex]::Escape($key))\s*:\s*(.+)$", [System.Text.RegularExpressions.RegexOptions]::Multiline)
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return ""
}

# 从 bench report 提取 "throughput : NNN.x ops/s" 的主数值。
function Get-BenchThrpt {
    param([string]$text)
    $m = [regex]::Match($text, "throughput\s*:\s*([\d\.]+)\s*ops/s")
    if ($m.Success) { return $m.Groups[1].Value }
    return ""
}

# 从 "overall : n=N  avg=XXX.XXus  p95=YYYus" 提取 avg / p95（微秒）。
function Get-BenchOverall {
    param([string]$text, [string]$which)
    $m = [regex]::Match($text, "overall\s*:.*avg=([\d\.]+)us\s*p95=([\d\.]+)us")
    if ($m.Success) {
        if ($which -eq "avg") { return $m.Groups[1].Value }
        return $m.Groups[2].Value
    }
    return ""
}

function Get-DiskWrites {
    param([string]$text)
    $m = [regex]::Match($text, "disk reads / writes\s*:\s*\d+\s*/\s*(\d+)")
    if ($m.Success) { return $m.Groups[1].Value }
    return ""
}

# ---- 扫描矩阵：{ 名称, 环境变量 } ----
$cases = @(
    @{ name = "baseline-default";        env = @{} },
    @{ name = "buf-64KB";                env = @{ SQLCOMPILER_BUFFER_MEMORY = "65536" } },
    @{ name = "buf-1MB";                 env = @{ SQLCOMPILER_BUFFER_MEMORY = "1048576" } },
    @{ name = "temp-flush-on";           env = @{ SQLCOMPILER_TEMP_FLUSH = "1" } },
    @{ name = "bg-flush-10ms";           env = @{ SQLCOMPILER_BG_FLUSH_MS = "10" } },
    @{ name = "groupcommit-2ms";         env = @{ SQLCOMPILER_GROUPCOMMIT_WINDOW_MS = "2" } },
    @{ name = "all-opt";                 env = @{ SQLCOMPILER_BUFFER_MEMORY = "1048576"; SQLCOMPILER_TEMP_FLUSH = "1"; SQLCOMPILER_BG_FLUSH_MS = "10"; SQLCOMPILER_GROUPCOMMIT_WINDOW_MS = "2" } }
)

Add-Content $log "==== Phase 6 U2-1 \bench 参数扫描 ===="
Add-Content $log ("time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Content $log ("workload: " + $benchCmd)
Add-Content $log ""

$rows = @()
foreach ($c in $cases) {
    $db = Join-Path $work ($c.name + ".db")
    foreach ($suffix in @("", ".wal", ".crc", ".fpl")) { if (Test-Path "$db$suffix") { Remove-Item "$db$suffix" } }
    $r = Invoke-BenchCase -db $db -envs $c.env
    $out = $r.Out
    $row = [ordered]@{
        case      = $c.name
        exit      = $r.Exit
        thruput   = (Get-BenchThrpt $out)
        avg_us    = (Get-BenchOverall $out "avg")
        p95_us    = (Get-BenchOverall $out "p95")
        hit_pct   = (Get-StatVal $out "hit ratio")
        fsyncs    = (Get-StatVal $out "wal fsyncs")
        disk_w    = (Get-DiskWrites $out)
        frames    = (Get-StatVal $out "buffer pool frames")
    }
    $rows += [pscustomobject]$row
    $status = if ($r.Exit -eq 0) { "OK" } else { "FAIL(exit=$($r.Exit))" }
    Write-Host ("[{0}] {1}  thr={2} ops/s  avg={3}us p95={4}us  hit={5}%  fsync={6}" -f `
        $status, $c.name, $row.thruput, $row.avg_us, $row.p95_us, $row.hit_pct, $row.fsyncs)
}

# ---- 汇总表（CSV 追加到日志） ----
Add-Content $log "case,exit,throughput_ops,avg_us,p95_us,hit_pct,wal_fsyncs,disk_writes,frames"
foreach ($r in $rows) {
    Add-Content $log ("{0},{1},{2},{3},{4},{5},{6},{7},{8}" -f `
        $r.case, $r.exit, $r.thruput, $r.avg_us, $r.p95_us, $r.hit_pct, $r.fsyncs, $r.disk_w, $r.frames)
}
Add-Content $log ""
Add-Content $log ("done: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Write-Host "results -> $log"