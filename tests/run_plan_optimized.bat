@echo off
REM ============================================================
REM  Phase 1.6 smoke harness: pre-/post-optimization plan split
REM  (test 92_plan_optimized). Verifies that:
REM    \.plan       => pre-optimization text (Filter stays above seqScan)
REM    \.optimized  => post-optimization text (predicate pushed down
REM                    into seqScanNode by Optimizer::PushDownPredicates)
REM  Usage: run_plan_optimized.bat
REM  Notes: This file MUST be saved as CRLF + ASCII (no UTF-8 BOM).
REM         Pure-LF line endings break cmd.exe parsing on Windows.
REM         findstr patterns avoid () / [] to dodge cmd.exe block-
REM         parser confusion inside if () blocks.
REM ============================================================

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%D in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fD"
set "BUILD_DIR=%ROOT_DIR%\build\Debug"
set "WORK_DIR=%SCRIPT_DIR%tmp"
set "EXEC=%BUILD_DIR%\sqlcompiler.exe"
set "DB=%WORK_DIR%\92_plan_optimized.db"
set "OUT=%WORK_DIR%\92_plan_optimized.out"
set "IN=%WORK_DIR%\92_plan_optimized.in"

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if exist "%DB%" del "%DB%"
if exist "%DB%.wal" del "%DB%.wal"
if exist "%OUT%" del "%OUT%"

REM ---- Build the heredoc-style input ------------------------------
(
    echo CREATE TABLE foo^(id INT, val INT^);
    echo INSERT INTO foo^(id,val^) VALUES ^(1,10^),^(2,20^),^(3,30^);
    echo SELECT id, val FROM foo WHERE id ^> 1;
    echo \.plan
    echo \.optimized
    echo exit;
) > "%IN%"

"%EXEC%" "%DB%" < "%IN%" > "%OUT%" 2>&1
set "EXITCODE=!errorlevel!"

echo ========================================== >> "%OUT%"
echo   Plan Optimized Smoke >> "%OUT%"
echo   Exit code: !EXITCODE! >> "%OUT%"
echo ========================================== >> "%OUT%"

type "%OUT%"

set "PASS=1"

REM ---- SQL itself ran correctly: '20' and '30' must appear -----
findstr /C:"20" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected row '20' in SELECT output
    set "PASS=0"
)
findstr /C:"30" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected row '30' in SELECT output
    set "PASS=0"
)

REM ---- \.plan: pre-optimization label must appear ----
findstr /C:"plan-before-opt" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected plan-before-opt header in \.plan output
    set "PASS=0"
)

REM ---- \.plan: standalone Filter node must be present ----
REM Use the leaf marker 'Filter' alone (no parens) to avoid
REM the cmd.exe block-parser trap with nested parens.
findstr /C:"Filter" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected standalone Filter node in pre-opt plan
    set "PASS=0"
)

REM ---- \.optimized: post-optimization label must appear ----
findstr /C:"plan-optimized" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected plan-optimized header in \.optimized output
    set "PASS=0"
)

REM ---- \.optimized: pushed-down predicate rendered inside seqScan's
REM      tuple. We can't match the literal '>' in findstr without
REM      escaping gymnastics; instead verify the line containing the
REM      table name + comma, which is unique to the post-opt form
REM      ('SeqScan(foo, ...)') and absent from the pre-opt 'SeqScan(foo)'.
findstr /I /C:"seqScan(foo, " "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected predicate pushed into seqScan^(foo, ...^)
    set "PASS=0"
)

REM ---- Both views must still show Project and seqScan(foo) ----
findstr /I /C:"Project" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected Project node in either view
    set "PASS=0"
)
findstr /I /C:"seqScan" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected seqScan node in either view
    set "PASS=0"
)
findstr /I /C:"foo" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected foo table name in either view
    set "PASS=0"
)

if !EXITCODE! neq 0 (
    echo [FAIL] sqlcompiler exited with code !EXITCODE!
    set "PASS=0"
)

if !PASS! equ 1 (
    echo [ OK ]
    endlocal
    exit /b 0
)
endlocal
exit /b 1
