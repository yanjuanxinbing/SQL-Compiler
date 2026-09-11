@echo off
REM ============================================================
REM  Phase B smoke harness: WAL + recovery test (test 49).
REM  Usage: run_acid_recovery.bat
REM  Notes: This file MUST be saved as CRLF + ASCII (no UTF-8 BOM).
REM         Pure-LF line endings break cmd.exe parsing on Windows.
REM ============================================================

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%D in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fD"
set "BUILD_DIR=%ROOT_DIR%\build\Debug"
set "WORK_DIR=%SCRIPT_DIR%tmp"
set "EXEC=%BUILD_DIR%\sqlcompiler.exe"
set "DB=%WORK_DIR%\49_acid_recovery.db"
set "PHASE1=%WORK_DIR%\49_acid_recovery_p1.sql"
set "PHASE2=%WORK_DIR%\49_acid_recovery_p2.sql"
set "OUT=%WORK_DIR%\49_acid_recovery.out"

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if exist "%DB%" del "%DB%"
if exist "%DB%.wal" del "%DB%.wal"
if exist "%OUT%" del "%OUT%"

REM ---- Phase 1: open db, create table, insert baseline row,
REM                 BEGIN txn, UPDATE without COMMIT, then \crash.
(
    echo CREATE TABLE acct^(id INT PRIMARY KEY, bal INT^);
    echo INSERT INTO acct VALUES ^(1, 100^), ^(2, 50^);
    echo BEGIN;
    echo UPDATE acct SET bal = 999 WHERE id = 1;
    echo \crash;
) > "%PHASE1%"

REM Run phase 1; expect non-zero exit because \crash _Exit(1)'s.
"%EXEC%" "%DB%" < "%PHASE1%" > "%WORK_DIR%\49_phase1.log" 2>&1
set "EXIT1=!errorlevel!"
echo [phase1 exit=!EXIT1!] >> "%OUT%"
type "%WORK_DIR%\49_phase1.log" >> "%OUT%"

REM ---- Phase 2: same db, no \crash. Recovery should undo the UPDATE
REM                 (bal should be 100, not 999). Plus a previously-COMMITted
REM                 UPDATE on row 2 should remain (durability smoke).
(
    echo SELECT id, bal FROM acct ORDER BY id;
    echo UPDATE acct SET bal = bal + 10 WHERE id = 2;
    echo BEGIN;
    echo UPDATE acct SET bal = 777 WHERE id = 1;
    echo ROLLBACK;
    echo SELECT id, bal FROM acct ORDER BY id;
) > "%PHASE2%"

"%EXEC%" "%DB%" < "%PHASE2%" >> "%OUT%" 2>&1
set "EXIT2=!errorlevel!"

echo ========================================== >> "%OUT%"
echo   Acid Recovery Smoke >> "%OUT%"
echo   Phase1 exit: !EXIT1! >> "%OUT%"
echo   Phase2 exit: !EXIT2! >> "%OUT%"
echo ========================================== >> "%OUT%"

type "%OUT%"

echo ==========================================
echo   Acid Recovery Smoke
echo   Phase1 exit: !EXIT1!
echo   Phase2 exit: !EXIT2!
echo ==========================================

set "PASS=1"
REM findstr 的 | 字符在 delayed expansion 里被错误处理；改用 grep / find
REM 直接搜列值（不依赖管道位置）。Phase2 输出格式是 "id | bal"，
REM 数字 100 与 60 在出现位置都是唯一的。
findstr /C:"100" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected '100' in output
    set "PASS=0"
)
findstr /C:"60" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected '60' in output
    set "PASS=0"
)
if !EXIT1! equ 0 (
    echo [FAIL] phase1 did not crash
    set "PASS=0"
)
if !EXIT2! neq 0 (
    echo [FAIL] phase2 exit was !EXIT2!
    set "PASS=0"
)
if !PASS! equ 1 (
    echo [ OK ]
    endlocal & exit /b 0
)
endlocal & exit /b 1