# ============================================================
#  Phase 5 周期4：一键复现脚本（统一证据包）
#  用法: powershell -ExecutionPolicy Bypass -File tests\run_repro_all.ps1
#  步骤：1) 缺构建时自动 cmake 构建 → 2) storage_ut → 3) SQL 回归(含崩溃注入)
#        → 4) 参数扫描实验 → 5) 汇总证据包到 docs\test_evidence\repro_summary.log
#  退出码：全部通过 0；任一失败 1。
# ============================================================

$ErrorActionPreference = "Continue"

$root = "c:\Users\Lenovo\Desktop\SQL-Compiler"
$build = Join-Path $root "build"
$evid  = Join-Path $root "docs\test_evidence"
if (-not (Test-Path $evid)) { New-Item -ItemType Directory -Path $evid | Out-Null }

$summary = Join-Path $evid "repro_summary.log"
if (Test-Path $summary) { Remove-Item $summary }

function Add-Line { param([string]$s) Add-Content $summary $s; Write-Host $s }

Add-Line "==== Phase 5 周期4 一键复现证据包 ===="
Add-Line ("time: " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
Add-Line ""

$allPass = $true

# ---- 1) 构建 ----
Add-Line "---- [1/4] build ----"
# 始终增量构建：确保 storage_ut/sqlcompiler 链接到最新 sqlcompiler_lib。
if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
    cmake -S $root -B $build 2>&1 | Out-File (Join-Path $evid "build.log") -Append
}
cmake --build $build -j 8 2>&1 | Out-File (Join-Path $evid "build.log") -Append
if ((Test-Path (Join-Path $build "sqlcompiler.exe")) -and
    (Test-Path (Join-Path $build "storage_ut.exe"))) {
    Add-Line "build: OK"
} else {
    Add-Line "build: FAIL"
    $allPass = $false
}

# ---- 2) storage_ut ----
Add-Line ""
Add-Line "---- [2/4] storage_ut ----"
& (Join-Path $build "storage_ut.exe") 2>&1 | Out-File (Join-Path $evid "storage_ut.log") -Encoding utf8
$utExit = $LASTEXITCODE
$utChecks = 0; $utFails = 1
$utLog = Get-Content (Join-Path $evid "storage_ut.log") -Raw
$m = [regex]::Match($utLog, "checks:\s*(\d+)\s+fails:\s*(\d+)")
if ($m.Success) { $utChecks = [int]$m.Groups[1].Value; $utFails = [int]$m.Groups[2].Value }
if ($utExit -eq 0 -and $utFails -eq 0) {
    Add-Line ("storage_ut: PASS ($utChecks checks / 0 fails)")
} else {
    Add-Line ("storage_ut: FAIL (exit=$utExit, $utChecks checks / $utFails fails)")
    $allPass = $false
}

# ---- 3) SQL 回归（含崩溃注入）----
Add-Line ""
Add-Line "---- [3/4] sql regression (incl. crash injection) ----"
& powershell -ExecutionPolicy Bypass -File (Join-Path $root "tests\run_sql_regression.ps1") | Out-File (Join-Path $evid "sql_regression_console.log") -Encoding utf8
$regExit = $LASTEXITCODE
$regLog = Get-Content (Join-Path $evid "sql_regression_run.log") -Raw
$rm = [regex]::Match($regLog, "passed:\s*(\d+)\s+failed:\s*(\d+)")
$regPassed = -1; $regFailed = -1
if ($rm.Success) { $regPassed = [int]$rm.Groups[1].Value; $regFailed = [int]$rm.Groups[2].Value }
if ($regExit -eq 0 -and $regFailed -eq 0) {
    Add-Line ("sql regression: PASS ($regPassed passed / 0 failed)")
} else {
    Add-Line ("sql regression: FAIL (exit=$regExit, passed=$regPassed failed=$regFailed)")
    $allPass = $false
}

# ---- 4) 参数扫描实验 ----
Add-Line ""
Add-Line "---- [4/4] param sweep ----"
& powershell -ExecutionPolicy Bypass -File (Join-Path $root "tests\run_param_sweep.ps1") | Out-File (Join-Path $evid "param_sweep_console.log") -Encoding utf8
$sweepExit = $LASTEXITCODE
if ($sweepExit -eq 0) {
    Add-Line "param sweep: PASS (log: param_sweep.log)"
} else {
    Add-Line "param sweep: FAIL (exit=$sweepExit)"
    $allPass = $false
}

# ---- 5) 性能曲线图 + 运行截图 ----
Add-Line ""
Add-Line "---- [5/5] charts & console capture ----"
& powershell -ExecutionPolicy Bypass -File (Join-Path $root "tests\make_sweep_charts.ps1") | Out-File (Join-Path $evid "charts_console.log") -Encoding utf8
$chartExit = $LASTEXITCODE
$pngs = (Get-ChildItem (Join-Path $evid "*.png") -ErrorAction SilentlyContinue).Count
if ($chartExit -eq 0 -and $pngs -ge 5) {
    Add-Line "charts: PASS ($pngs PNGs)"
} else {
    Add-Line "charts: FAIL (exit=$chartExit, $pngs PNGs)"
    $allPass = $false
}

# ---- 汇总 ----
Add-Line ""
Add-Line "==== Summary ===="
Add-Line ("storage_ut   : " + $(if ($utFails -eq 0) { "PASS ($utChecks checks)" } else { "FAIL" }))
Add-Line ("sql regression: " + $(if ($regFailed -eq 0) { "PASS ($regPassed/55)" } else { "FAIL" }))
Add-Line ("param sweep  : " + $(if ($sweepExit -eq 0) { "PASS" } else { "FAIL" }))
Add-Line ("charts/shot  : " + $(if ($pngs -ge 5) { "PASS ($pngs PNGs)" } else { "FAIL" }))
Add-Line ("overall      : " + $(if ($allPass) { "ALL GREEN" } else { "HAS FAILURES" }))
Add-Line "evidence: build.log / storage_ut.log / sql_regression_run.log / param_sweep.log / 5x PNG / repro_summary.log"
Add-Line "==== End ===="

Write-Host "REPRO_ALL: overall=$(if ($allPass) {'GREEN'} else {'FAIL'}) summary=$summary"
if ($allPass) { exit 0 }
exit 1
