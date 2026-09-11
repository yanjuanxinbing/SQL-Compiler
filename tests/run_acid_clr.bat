@echo off
REM ============================================================
REM  Phase C smoke harness: CLR-correct rollback test (test 50).
REM  Usage: run_acid_clr.bat
REM  Notes: This file MUST be saved as CRLF + ASCII (no UTF-8 BOM).
REM         Pure-LF line endings break cmd.exe parsing on Windows.
REM ============================================================

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%D in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fD"
set "BUILD_DIR=%ROOT_DIR%\build\Debug"
set "WORK_DIR=%SCRIPT_DIR%tmp"
set "EXEC=%BUILD_DIR%\sqlcompiler.exe"
set "DB=%WORK_DIR%\50_undo_clr.db"
set "PHASE1=%WORK_DIR%\50_undo_clr_p1.sql"
set "PHASE2=%WORK_DIR%\50_undo_clr_p2.sql"
set "OUT=%WORK_DIR%\50_undo_clr.out"

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if exist "%DB%" del "%DB%"
if exist "%DB%.wal" del "%DB%.wal"
if exist "%OUT%" del "%OUT%"

REM ---- Phase 1: CREATE + 3 baseline INSERT; BEGIN; arm crash; 3 more INSERT; ROLLBACK.
(
    echo CREATE TABLE acct^(id INT PRIMARY KEY, bal INT^);
    echo INSERT INTO acct VALUES ^(1, 100^), ^(2, 200^), ^(3, 300^);
    echo BEGIN;
    echo \crash_after_undo_steps 1;
    echo INSERT INTO acct VALUES ^(4, 400^);
    echo INSERT INTO acct VALUES ^(5, 500^);
    echo INSERT INTO acct VALUES ^(6, 600^);
    echo ROLLBACK;
) > "%PHASE1%"

"%EXEC%" "%DB%" < "%PHASE1%" > "%WORK_DIR%\50_phase1.log" 2>&1
set "EXIT1=!errorlevel!"
echo [phase1 exit=!EXIT1!] >> "%OUT%"
type "%WORK_DIR%\50_phase1.log" >> "%OUT%"

REM ---- Phase 2: same db restarted.
(
    echo SELECT id, bal FROM acct ORDER BY id;
) > "%PHASE2%"

"%EXEC%" "%DB%" < "%PHASE2%" >> "%OUT%" 2>&1
set "EXIT2=!errorlevel!"

echo ========================================== >> "%OUT%"
echo   Acid CLR Rollback Smoke >> "%OUT%"
echo   Phase1 exit: !EXIT1! >> "%OUT%"
echo   Phase2 exit: !EXIT2! >> "%OUT%"
echo ========================================== >> "%OUT%"

type "%OUT%"

echo ==========================================
echo   Acid CLR Rollback Smoke
echo   Phase1 exit: !EXIT1!
echo   Phase2 exit: !EXIT2!
echo ==========================================

set "PASS=1"
if !EXIT1! equ 0 (
    echo [FAIL] phase1 did not crash
    set "PASS=0"
)
if !EXIT2! neq 0 (
    echo [FAIL] phase2 exit was !EXIT2!
    set "PASS=0"
)
findstr /C:"(3 rows)" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected 3 rows
    set "PASS=0"
)
findstr /C:"4 | 400" "%OUT%" >nul
if !errorlevel! equ 0 (
    echo [FAIL] row 4 should be rolled back
    set "PASS=0"
)
findstr /C:"5 | 500" "%OUT%" >nul
if !errorlevel! equ 0 (
    echo [FAIL] row 5 should be rolled back
    set "PASS=0"
)
findstr /C:"6 | 600" "%OUT%" >nul
if !errorlevel! equ 0 (
    echo [FAIL] row 6 should be rolled back
    set "PASS=0"
)
if !PASS! equ 1 (
    echo [ OK ]
    endlocal & exit /b 0
)
endlocal & exit /b 1