# 全量 SQL 回归 + 崩溃注入复刻脚本（PowerShell，Windows）
# 用法: powershell -ExecutionPolicy Bypass -File tests\run_sql_regression.ps1
# 输入注入: System.Diagnostics.Process + UTF-8 字节级 stdin（逐行 Flush，避免首条丢失）
# 判定规则（与 run_all_tests.bat 一致）：exit 0 且输出无 "sqlcompiler> Error:" 行 → 通过

$ErrorActionPreference = "Continue"

$root   = "c:\Users\Lenovo\Desktop\SQL-Compiler"
$exec   = Join-Path $root "build\sqlcompiler.exe"
$sqlDir = Join-Path $root "tests\sql"
$work   = Join-Path $root "tests\tmp"
$log    = Join-Path $root "docs\test_evidence\sql_regression_run.log"

if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work | Out-Null }
if (Test-Path $log) { Remove-Item $log }

function Invoke-SqlFile {
    param([string]$exe, [string]$db, [string]$sqlFile)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = "`"$db`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    # 逐行写入 UTF-8 字节并 Flush（复刻 bat 的 < 重定向，逐字节保真）
    $reader = [System.IO.StreamReader]::new($sqlFile, [System.Text.Encoding]::UTF8)
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    try {
        while (($line = $reader.ReadLine()) -ne $null) {
            $data = $utf8.GetBytes($line + "`r`n")
            $p.StandardInput.BaseStream.Write($data, 0, $data.Length)
            $p.StandardInput.BaseStream.Flush()
        }
    } finally { $reader.Dispose() }
    $p.StandardInput.Close()
    $outTask  = $p.StandardOutput.ReadToEndAsync()
    $errTask  = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    $out = $outTask.GetAwaiter().GetResult()
    $err = $errTask.GetAwaiter().GetResult()
    return @{ Exit = $p.ExitCode; Out = ($out + $err) }
}

$passed = 0; $failed = 0; $failedList = @()
$start = Get-Date

# ---- Phase 1: 53 条 SQL 脚本 ----
Get-ChildItem -Path $sqlDir -Filter *.sql | Sort-Object Name | ForEach-Object {
    $name = $_.BaseName
    $db   = Join-Path $work "$name.db"
    $out  = Join-Path $work "$name.out"
    foreach ($suffix in @("", ".wal", ".crc", ".fpl")) { if (Test-Path "$db$suffix") { Remove-Item "$db$suffix" } }
    if (Test-Path $out) { Remove-Item $out }

    $r = Invoke-SqlFile -exe $exec -db $db -sqlFile $_.FullName
    [System.IO.File]::WriteAllText($out, $r.Out, [System.Text.Encoding]::UTF8)
    $hasErr = $r.Out -match "(?m)^sqlcompiler> Error:"

    if ($r.Exit -ne 0 -or $hasErr) {
        $failed++; $failedList += $name
        Add-Content $log "[FAIL] $name (exit=$($r.Exit) hasError=$hasErr)"
    } else {
        $passed++
        Add-Content $log "[ OK ] $name"
    }
}

# ---- Phase 2: 崩溃注入 49_acid_recovery（两阶段） ----
function Invoke-CrashRecovery49 {
    $db = Join-Path $work "49_acid_recovery.db"
    $p1 = Join-Path $work "49_acid_recovery_p1.sql"
    $p2 = Join-Path $work "49_acid_recovery_p2.sql"
    foreach ($suffix in @("", ".wal", ".crc", ".fpl")) { if (Test-Path "$db$suffix") { Remove-Item "$db$suffix" } }
    @(
      "CREATE TABLE acct(id INT PRIMARY KEY, bal INT);",
      "INSERT INTO acct VALUES (1, 100), (2, 50);",
      "BEGIN;",
      "UPDATE acct SET bal = 999 WHERE id = 1;",
      "\crash;"
    ) | Set-Content $p1 -Encoding UTF8
    $r1 = Invoke-SqlFile -exe $exec -db $db -sqlFile $p1
    @(
      "SELECT id, bal FROM acct ORDER BY id;",
      "UPDATE acct SET bal = bal + 10 WHERE id = 2;",
      "BEGIN;",
      "UPDATE acct SET bal = 777 WHERE id = 1;",
      "ROLLBACK;",
      "SELECT id, bal FROM acct ORDER BY id;"
    ) | Set-Content $p2 -Encoding UTF8
    $r2 = Invoke-SqlFile -exe $exec -db $db -sqlFile $p2
    [System.IO.File]::WriteAllText((Join-Path $work "49_acid_recovery.out"), "[phase1 exit=$($r1.Exit)]`r`n$($r1.Out)`r`n$($r2.Out)", [System.Text.Encoding]::UTF8)
    return ($r1.Exit -ne 0 -and $r2.Exit -eq 0 -and $r2.Out -match "100" -and $r2.Out -match "60")
}

# ---- Phase 2b: 崩溃注入 50_undo_clr（两阶段） ----
function Invoke-CrashClr50 {
    $db = Join-Path $work "50_undo_clr.db"
    $p1 = Join-Path $work "50_undo_clr_p1.sql"
    $p2 = Join-Path $work "50_undo_clr_p2.sql"
    foreach ($suffix in @("", ".wal", ".crc", ".fpl")) { if (Test-Path "$db$suffix") { Remove-Item "$db$suffix" } }
    @(
      "CREATE TABLE acct(id INT PRIMARY KEY, bal INT);",
      "INSERT INTO acct VALUES (1, 100), (2, 200), (3, 300);",
      "BEGIN;",
      "\crash_after_undo_steps 1;",
      "INSERT INTO acct VALUES (4, 400);",
      "INSERT INTO acct VALUES (5, 500);",
      "INSERT INTO acct VALUES (6, 600);",
      "ROLLBACK;"
    ) | Set-Content $p1 -Encoding UTF8
    $r1 = Invoke-SqlFile -exe $exec -db $db -sqlFile $p1
    @("SELECT id, bal FROM acct ORDER BY id;") | Set-Content $p2 -Encoding UTF8
    $r2 = Invoke-SqlFile -exe $exec -db $db -sqlFile $p2
    [System.IO.File]::WriteAllText((Join-Path $work "50_undo_clr.out"), "[phase1 exit=$($r1.Exit)]`r`n$($r1.Out)`r`n$($r2.Out)", [System.Text.Encoding]::UTF8)
    return ($r1.Exit -ne 0 -and $r2.Exit -eq 0 -and $r2.Out -match "\(3 rows\)" -and $r2.Out -notmatch "4 \| 400" -and $r2.Out -notmatch "5 \| 500" -and $r2.Out -notmatch "6 \| 600")
}

if (Invoke-CrashRecovery49) { $passed++; Add-Content $log "[ OK ] 49_acid_recovery" } else { $failed++; $failedList += "49_acid_recovery"; Add-Content $log "[FAIL] 49_acid_recovery" }
if (Invoke-CrashClr50)       { $passed++; Add-Content $log "[ OK ] 50_undo_clr" }         else { $failed++; $failedList += "50_undo_clr";         Add-Content $log "[FAIL] 50_undo_clr" }

$elapsed = (Get-Date) - $start
Add-Content $log ""
Add-Content $log "==== Summary ===="
Add-Content $log "passed: $passed   failed: $failed   elapsed: $($elapsed.TotalSeconds)s"
if ($failed -gt 0) { Add-Content $log "failed tests: $($failedList -join ', ')" }
Add-Content $log "==== End ===="

Write-Host "PASSED=$passed FAILED=$failed ELAPSED=$($elapsed.TotalSeconds)s"
