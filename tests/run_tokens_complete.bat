@echo off
REM ============================================================
REM  Phase 1.6 smoke harness: token-name completeness
REM  (test 80_tokens_complete). Verifies that TokenTypeToString
REM  has a case for every keyword registered in the lexer's
REM  keyword table, so .tokens never falls through to UNKNOWN.
REM
REM  Usage: run_tokens_complete.bat
REM  Notes: This file MUST be saved as CRLF + ASCII (no UTF-8 BOM).
REM         Pure-LF line endings break cmd.exe parsing on Windows.
REM ============================================================

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
for %%D in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fD"
set "BUILD_DIR=%ROOT_DIR%\build\Debug"
set "WORK_DIR=%SCRIPT_DIR%tmp"
set "EXEC=%BUILD_DIR%\sqlcompiler.exe"
set "DB=%WORK_DIR%\80_tokens_complete.db"
set "OUT=%WORK_DIR%\80_tokens_complete.out"
set "IN=%WORK_DIR%\80_tokens_complete.in"

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
if exist "%DB%" del "%DB%"
if exist "%DB%.wal" del "%DB%.wal"
if exist "%OUT%" del "%OUT%"

REM ---- Build the heredoc-style input ------------------------------
(
    echo CREATE TABLE t^(id INT, gpa FLOAT, name VARCHAR^);
    echo INSERT INTO t VALUES ^(1, 3.5, 'a'^), ^(2, 3.9, 'b'^);
    echo SELECT * FROM t ORDER BY gpa DESC;
    echo \.tokens
    echo exit;
) > "%IN%"

"%EXEC%" "%DB%" < "%IN%" > "%OUT%" 2>&1
set "EXITCODE=!errorlevel!"

echo ========================================== >> "%OUT%"
echo   Tokens Complete Smoke
echo   Exit code: !EXITCODE!
echo ========================================== >> "%OUT%"

type "%OUT%"

echo ==========================================
echo   Tokens Complete Smoke
echo   Exit code: !EXITCODE!
echo ==========================================

set "PASS=1"

REM ---- Semantic: sort by DESC puts 3.9 first ---------------------
findstr /C:"3.9" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected '3.9' (top of DESC sort) in SELECT output
    set "PASS=0"
)

REM ---- Primary fix: .tokens must print [KEYWORD_DESC], not fall
REM      through to [UNKNOWN]. Previously TokenTypeToString was
REM      missing cases for 130+ keywords.
findstr /C:"[KEYWORD_DESC]" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected '[KEYWORD_DESC]' token entry in \.tokens output
    set "PASS=0"
)

REM ---- Defensive: no [UNKNOWN] line should appear in this output -
findstr /C:"[UNKNOWN]" "%OUT%" >nul
if !errorlevel! == 0 (
    echo [FAIL] found '[UNKNOWN]' in \.tokens output - TokenTypeToString missing a case
    set "PASS=0"
)

REM ---- ASC neighbour stays OK (was already fine; regression check) -
findstr /C:"[KEYWORD_ASC]" "%OUT%" >nul
if !errorlevel! neq 0 (
    echo [FAIL] expected '[KEYWORD_ASC]' in \.tokens output
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
