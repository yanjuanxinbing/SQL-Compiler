# ============================================================
#  Phase 5 周期4：性能曲线图 + 运行截图生成
#  用法: powershell -ExecutionPolicy Bypass -File tests\make_sweep_charts.ps1
#  输入: docs\test_evidence\param_sweep.log 的 CSV 区
#  输出: docs\test_evidence\sweep_hitrate.png / sweep_fsync.png /
#        sweep_writeback.png / sweep_elapsed.png / demo_console.png
# ============================================================

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Windows.Forms.DataVisualization
Add-Type -AssemblyName System.Drawing

$root = "c:\Users\Lenovo\Desktop\SQL-Compiler"
$log  = Join-Path $root "docs\test_evidence\param_sweep.log"
$evid = Join-Path $root "docs\test_evidence"

# ---- 解析 CSV ----
$lines = Get-Content $log
$csvLines = $lines | Where-Object { $_ -match '^[a-z]' -and $_ -notmatch '^====' -and $_ -notmatch '^case,' }
$rows = @()
foreach ($ln in $csvLines) {
    $f = $ln.Split(',')
    if ($f.Count -lt 11) { continue }
    $rows += [pscustomobject]@{
        case = $f[0]; ms = [int]$f[2]; hit = [double]($f[3].TrimEnd('%'));
        w = [int]$f[4]; fsync = [int]$f[5]; cold = [int]$f[6]; hot = [int]$f[7]; ticks = [int]$f[8]
    }
}

# ---- 画图辅助 ----
function New-Chart {
    param([string]$title, [int]$w, [int]$h)
    $ch = New-Object System.Windows.Forms.DataVisualization.Charting.Chart
    $ch.Width = $w; $ch.Height = $h
    $area = New-Object System.Windows.Forms.DataVisualization.Charting.ChartArea
    $ch.ChartAreas.Add($area)
    $ch.Titles.Add($title) | Out-Null
    $ch.Titles[0].Font = New-Object System.Drawing.Font("Microsoft YaHei", 11, [System.Drawing.FontStyle]::Bold)
    $area.AxisX.LabelStyle.Font = New-Object System.Drawing.Font("Microsoft YaHei", 8)
    $area.AxisY.LabelStyle.Font = New-Object System.Drawing.Font("Microsoft YaHei", 8)
    $area.AxisX.LabelStyle.Angle = -45
    $area.AxisX.Interval = 1
    return $ch
}
function Add-Series {
    param($ch, [string]$name, [string]$chartType)
    $s = $ch.Series.Add($name)
    $s.ChartType = $chartType
    $s.Font = New-Object System.Drawing.Font("Microsoft YaHei", 8)
    $s.IsValueShownAsLabel = $true
    return $s
}
function Save-Chart { param($ch, [string]$path) $ch.SaveImage($path, [System.Windows.Forms.DataVisualization.Charting.ChartImageFormat]::Png); $ch.Dispose() }

# 1) 命中率 vs 缓冲池大小
$hit = New-Chart "参数扫描：命中率 vs 缓冲池大小" 700 360
$s = Add-Series $hit "hit%" "Column"
foreach ($c in $rows | Where-Object { $_.case -match '^buf-' -or $_.case -eq 'baseline-default' }) { [void]$s.Points.AddXY($c.case, $c.hit) }
$hit.ChartAreas[0].AxisY.Maximum = 100
Save-Chart $hit (Join-Path $evid "sweep_hitrate.png")

# 2) WAL fsyncs 对比（缓冲/刷脏/组提交/全开）
$f = New-Chart "参数扫描：WAL fsync 次数对比" 900 360
$s = Add-Series $f "fsyncs" "Column"
foreach ($c in $rows) { [void]$s.Points.AddXY($c.case, $c.fsync) }
Save-Chart $f (Join-Path $evid "sweep_fsync.png")

# 3) 写回 cold/hot 分流（温度刷盘效果）
$wb = New-Chart "参数扫描：脏页写回 cold/hot 分流" 900 360
$s1 = Add-Series $wb "cold" "StackedColumn"
$s2 = Add-Series $wb "hot" "StackedColumn"
foreach ($c in $rows) {
    [void]$s1.Points.AddXY($c.case, $c.cold)
    [void]$s2.Points.AddXY($c.case, $c.hot)
}
Save-Chart $wb (Join-Path $evid "sweep_writeback.png")

# 4) 耗时
$e = New-Chart "参数扫描：耗时(ms)" 900 360
$s = Add-Series $e "elapsed" "Column"
foreach ($c in $rows) { [void]$s.Points.AddXY($c.case, $c.ms) }
Save-Chart $e (Join-Path $evid "sweep_elapsed.png")

Write-Host "charts: 4 PNG saved to $evid"

# ============================================================
#  运行截图：用推荐配置跑一段演示，把控制台输出渲染成 PNG
# ============================================================
$exec = Join-Path $root "build\sqlcompiler.exe"
$demoDb = Join-Path $root "tests\tmp_sweep2\demo_shot.db"
foreach ($suffix in @("", ".wal", ".crc", ".fpl")) { if (Test-Path "$demoDb$suffix") { Remove-Item "$demoDb$suffix" } }
$demoSql = @"
CREATE TABLE student(id INT PRIMARY KEY, name VARCHAR(32), score INT);
CREATE INDEX idx_student_score ON student(score);
INSERT INTO student VALUES (1, 'Alice', 92), (2, 'Bob', 78), (3, 'Carol', 88), (4, 'Dave', 65);
SELECT id, name, score FROM student WHERE score >= 80 ORDER BY score;
UPDATE student SET score = score + 1 WHERE id = 2;
SELECT COUNT(*), AVG(score) FROM student;
SELECT id, name, score FROM student ORDER BY id;
\stats;
"@
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exec
$psi.Arguments = "`"$demoDb`""
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.EnvironmentVariables["SQLCOMPILER_BUFFER_MEMORY"] = "1048576"
$psi.EnvironmentVariables["SQLCOMPILER_TEMP_FLUSH"] = "1"
$psi.EnvironmentVariables["SQLCOMPILER_BG_FLUSH_MS"] = "10"
$psi.EnvironmentVariables["SQLCOMPILER_GROUPCOMMIT_WINDOW_MS"] = "2"
$psi.EnvironmentVariables["SQLCOMPILER_BG_VACUUM_MS"] = "5"
$p = [System.Diagnostics.Process]::Start($psi)
foreach ($line in $demoSql) {
    $b = [System.Text.Encoding]::UTF8.GetBytes($line + "`r`n")
    $p.StandardInput.BaseStream.Write($b, 0, $b.Length)
    $p.StandardInput.BaseStream.Flush()
}
$p.StandardInput.Close()
$outTask = $p.StandardOutput.ReadToEndAsync()
$errTask = $p.StandardError.ReadToEndAsync()
$p.WaitForExit()
$out = $outTask.GetAwaiter().GetResult()
$err = $errTask.GetAwaiter().GetResult()
$consoleText = "SQL-Compiler 演示会话（推荐配置演示）`r`n`r`n" + ($out + $err)

# 渲染为终端样式 PNG
$font = New-Object System.Drawing.Font("Consolas", 11)
$lines2 = $consoleText -split "`r?`n"
$cellH = 20; $pad = 14
$w2 = 980; $h2 = ($lines2.Count + 1) * $cellH + $pad * 2
$bmp = New-Object System.Drawing.Bitmap($w2, $h2)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(30, 30, 30))
$white = [System.Drawing.Brushes]::White
$green = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(80, 220, 120))
$y = $pad
for ($i = 0; $i -lt $lines2.Count; $i++) {
    $isPrompt = $lines2[$i] -match '^(sqlcompiler|SQL>|>|1>|2>)|^\\'
    $brush = if ($isPrompt) { $green } else { $white }
    $g.DrawString($lines2[$i], $font, $brush, $pad, $y)
    $y += $cellH
}
$g.Dispose()
$bmp.Save((Join-Path $evid "demo_console.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "console capture: demo_console.png saved"
Write-Host "CHART_DONE"
