@echo off
REM ============================================================
REM  60_view_trigger persistence smoke test
REM  Verifies that CREATE TRIGGER persists to __sys_triggers__ and
REM  is restored after a database reopen.
REM  Phase 1: open DB, create trigger, close.
REM  Phase 2: reopen DB, drop the trigger (proving it exists), close.
REM ============================================================

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%D in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fD"
set "BUILD_DIR=%ROOT_DIR%\build\Debug"
set "WORK_DIR=%SCRIPT_DIR%tmp"
set "EXEC=%BUILD_DIR%\sqlcompiler.exe"
set "DB=%WORK_DIR%\60_trigger_persist.db"
set "PHASE1=%WORK_DIR%\60_trigger_persist_p1.sql"
set "PHASE2=%WORK_DIR%\60_trigger_persist_p2.sql"
set "OUT=%WORK_DIR%\60_trigger_persist.out"

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if exist "%DB%" del "%DB%"
if exist "%DB%.wal" del "%DB%.wal"
if exist "%OUT%" del "%OUT%"

REM Phase 1: create table and trigger.
(
    echo CREATE TABLE p_acct^(id INT PRIMARY KEY, bal INT^);
    echo CREATE TRIGGER tr_p BEFORE INSERT ON p_acct FOR EACH ROW SET NEW.bal = NEW.bal + 1;
    echo exit;
) > "%PHASE1%"
"%EXEC%" "%DB%" < "%PHASE1%" > nul 2>&1

REM Phase 2: reopen DB and verify trigger still exists by attempting DROP.
(
    echo DROP TRIGGER tr_p;
    echo exit;
) > "%PHASE2%"
"%EXEC%" "%DB%" < "%PHASE2%" > "%OUT%" 2>&1

REM Phase 2 should report OK on DROP TRIGGER (meaning trigger was found).
findstr /B /C:"sqlcompiler> Error:" "%OUT%" >nul 2>&1
if %errorlevel% == 0 (
    echo [FAIL] trigger_persist: trigger was lost across restart
    del "%PHASE1%" "%PHASE2%"
    endlocal & exit /b 1
)

REM Now also verify that DROP TRIGGER IF EXISTS works on missing trigger.
(
    echo DROP TRIGGER IF EXISTS tr_nonexistent;
    echo exit;
) > "%PHASE2%"
"%EXEC%" "%DB%" < "%PHASE2%" > "%OUT%" 2>&1
findstr /B /C:"sqlcompiler> Error:" "%OUT%" >nul 2>&1
if %errorlevel% == 0 (
    echo [FAIL] trigger_persist: DROP TRIGGER IF EXISTS reported error
    del "%PHASE1%" "%PHASE2%"
    endlocal & exit /b 1
)

echo [ OK ] trigger_persist
del "%PHASE1%" "%PHASE2%"
endlocal & exit /b 0
