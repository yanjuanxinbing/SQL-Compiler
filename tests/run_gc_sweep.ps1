# ============================================================
#  Phase 6 U2-3：组提交窗口调参曲线（fsync 次数 / 延迟 / 吞吐 权衡）
#  用法: powershell -ExecutionPolicy Bypass -File tests\run_gc_sweep.ps1
#  固定高并发混合负载 \bench 8 300（2400 op），扫描
#  SQLCOMPILER_GROUPCOMMIT_WINDOW_MS = 0/1/2/5/10/20/50ms，从 bench report 提取
#  吞吐 / avg / p95 与 ok/err，从 \stats 提取 wal fsyncs / disk writes。
#  输出 CSV 权衡曲线至 docs\test_evidence\gc_sweep.log，据此给推荐窗口值。
#  判定：全部组合 exit 0 且 ok 全覆盖（err=0）——窗口调大不得丢提交。
# ============================================================

$ErrorActionPreference = "Continue"

$root   = "c:\Users\Lenovo\Desktop\SQL-Compiler"
$exec   = Join-Path $root "build\sqlcompiler.exe"
$work   = Join-Path $root "tests\tmp_gc2"
$evid   = Join-Path $root "docs\test_evidence"
$log    = Join-Path $evid "gc_sweep.log"

if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work | Out-Null }
if (-not (Test-Path $evid)) { New-Item -ItemType Directory -Path $evid | Out-Null }
if (Test-Path $log) { Remove-Item $log }

# 固定高并发混合负载：8 线程 × 300 op/线程（2400 op），组提交窗口仅在并发下摊薄。
$benchCmd = "\bench 8 300;"
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

function Get-BenchVal {   # 从 bench report 提取主数值：throughput / avg / p95 / ok / err
    param([string]$text, [string]$key)
    switch ($key) {
        "thr"  { $m = [regex]::Match($text, "throughput\s*:\s*([\d\.]+)\s*ops/s") }
        "avg"  { $m = [regex]::Match($text, "overall\s*:.*avg=([\d\.]+)us") }
        "p95"  { $m = [regex]::Match($text, "overall\s*:.*p95=([\d\.]+)us") }
        "ok"   { $m = [regex]::Match($text, "ok=(\d+)\s+err=(\d+)") }
        "err"  { $m = [regex]::Match($text, "ok=(\d+)\s+err=(\d+)") }
        default { $m = $null }
    }
    if ($m -and $m.Success) {
        if ($key -eq "ok") { return $m.Groups[1].Value }
        if ($key -eq "err") { return $m.Groups[2].Value }
        if ($key -eq "avg") { return $m.Groups[1].Value }
        if ($key -eq "p95") { return $m.Groups[1].Value }
        return $m.Groups[1].Value
    }
    return ""
}

function Get-DiskWrites {
    param([string]$text)
    $m = [regex]::Match($text, "disk reads / writes\s*:\s*\d+\s*/\s*(\d+)")
    if ($m.Success) { return $m.Groups[1].Value }
    return ""
}

# ---- 扫描 SQLCOMPILER_GROUPCOMMIT_WINDOW_MS ----
$windows = @(0, 1, 2, 5, 10, 20, 50)   # 0 = 关闭（仍聚合，见 LogManager 语义）

Add-Content $log "==== Phase 6 U2-3 组提交窗口调参曲线 ===="
Add-Content $log ("time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Content $log ("workload(高并发): " + $benchCmd)
Add-Content $log ""

$rows = @()
foreach ($w in $windows) {
    $name = "gc-" + $w + "ms"
    $db = Join-Path $work ($name + ".db")
    foreach ($suffix in @("", ".wal", ".crc", ".fpl")) { if (Test-Path "$db$suffix") { Remove-Item "$db$suffix" } }
    $envs = @{}
    if ($w -gt 0) { $envs["SQLCOMPILER_GROUPCOMMIT_WINDOW_MS"] = [string]$w }
    $r = Invoke-BenchCase -db $db -envs $envs
    $out = $r.Out
    $row = [ordered]@{
        window_ms = $w
        exit      = $r.Exit
        throughput= (Get-BenchVal $out "thr")
        avg_us    = (Get-BenchVal $out "avg")
        p95_us    = (Get-BenchVal $out "p95")
        ok_ops    = (Get-BenchVal $out "ok")
        err_ops   = (Get-BenchVal $out "err")
        fsyncs    = (Get-StatVal $out "wal fsyncs")
        disk_w    = (Get-DiskWrites $out)
    }
    $rows += [pscustomobject]$row
    $status = if ($r.Exit -eq 0 -and $row.err_ops -eq "0") { "OK" } else { "FAIL(exit=$($r.Exit) err=$($row.err_ops))" }
    Write-Host ("[{0}] window={1}ms  thr={2} ops/s  avg={3}us p95={4}us  ok={5} err={6}  fsync={7} diskW={8}" -f `
        $status, $w, $row.throughput, $row.avg_us, $row.p95_us, $row.ok_ops, $row.err_ops, $row.fsyncs, $row.disk_w)
}

# ---- 汇总曲线表（CSV） ----
Add-Content $log "window_ms,exit,throughput_ops,avg_us,p95_us,ok_ops,err_ops,wal_fsyncs,disk_writes"
foreach ($r in $rows) {
    Add-Content $log ("{0},{1},{2},{3},{4},{5},{6},{7},{8}" -f `
        $r.window_ms, $r.exit, $r.throughput, $r.avg_us, $r.p95_us, $r.ok_ops, $r.err_ops, $r.fsyncs, $r.disk_w)
}
Add-Content $log ""
Add-Content $log ("fsync 节约 vs 0ms: 窗口调大应显著降 fsync（组提交批量），但 avg/p95 随之上升（等待窗口摊薄）；ok=2400 全绿为正确性保底")
Add-Content $log ("done: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Write-Host "results -> $log"