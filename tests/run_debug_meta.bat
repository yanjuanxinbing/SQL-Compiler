@echo off
REM ============================================================
REM  Phase 1.5 smoke harness: REPL debug meta-commands
REM  (test 62_debug_meta). Exercises \.tokens / \.ast / \.plan
REM  after a normal SQL statement, verifying the cache wiring.
REM  Usage: run_debug_meta.bat
REM  Notes: This file MUST be saved as CRLF + ASCII (no UTF-8 BOM).
REM         Pure-LF line endings break cmd.exe parsing on Windows.
REM ============================================================

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%D in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fD"
set "BUILD_DIR=%ROOT_DIR%\build\Debug"
set "WORK_DIR=%SCRIPT_DIR%tmp"
set "EXEC=%BUILD_DIR%\sqlcompiler.exe"
set "DB=%WORK_DIR%\62_debug_meta.db"
set "OUT=%WORK_DIR%\62_debug_meta.out"
set "IN=%WORK_DIR%\62_debug_meta.in"

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if exist "%DB%" del "%DB%"
if exist "%DB%.wal" del "%DB%.wal"
if exist "%OUT%" del "%OUT%"

REM ---- Build the heredoc-style input --------------------------------
(
    echo CREATE TABLE foo^(id INT, val INT^);
    echo INSERT INTO foo^(id,val^) VALUES ^(1,10^),^(2,20^);
    echo SELECT id, val FROM foo WHERE id ^> 1;
    echo \.tokens
    echo \.ast
    echo \.plan
    echo exit;
) > "%IN%"

"%EXEC%" "%DB%" < "%IN%" > "%OUT%" 2>&1
set "EXITCODE=!errorlevel!"

echo ========================================== >> "%OUT%"
echo   Debug Meta Smoke
echo   Exit code: !EXITCODE!
echo ========================================== >> "%OUT%"

type "%OUT%"

echo ==========================================
echo   Debug Meta Smoke
echo   Exit code: !EXITCODE!
echo ==========================================

set "PASS=1"

REM ---- Check the SQL itself ran correctly: row "2 | 20" must appear --
findstr /C:"20" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected row '20' in SELECT output
    set "PASS=0"
)

REM ---- meta: tokens command should print "[tokens]" header
REM      and at least one KEYWORD_SELECT line.
findstr /C:"tokens" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected tokens header
    set "PASS=0"
)
findstr /C:"KEYWORD_SELECT" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected KEYWORD_SELECT token entry
    set "PASS=0"
)

REM ---- meta: ast command should print the SELECT text --------
findstr /C:"SELECT id, val FROM foo" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected SELECT AST text
    set "PASS=0"
)

REM ---- meta: plan command should print "Project(...)" and "SeqScan(...)"
findstr /C:"Project" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected Project plan node
    set "PASS=0"
)
findstr /C:"SeqScan" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected SeqScan plan node
    set "PASS=0"
)

if !EXITCODE! neq 0 (
    echo [FAIL] sqlcompiler exited with code !EXITCODE!
    set "PASS=0"
)

if !PASS! equ 1 (
    echo [ OK ]
    endlocal & exit /b 0
)
endlocal & exit /b 1
