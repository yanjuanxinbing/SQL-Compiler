@echo off
REM ============================================================
REM  Batch runner for all SQL test scripts (Windows)
REM  Usage: run_all_tests.bat
REM  Notes: This file MUST be saved as CRLF + ASCII (no UTF-8 BOM).
REM         Pure-LF line endings break cmd.exe parsing on Windows.
REM ============================================================

setlocal enabledelayedexpansion

REM ---- Locate paths --------------------------------------------------
set "SCRIPT_DIR=%~dp0"
for %%D in ("%SCRIPT_DIR%..") do set "ROOT_DIR=%%~fD"
set "BUILD_DIR=%ROOT_DIR%\build\Debug"
set "SQL_DIR=%SCRIPT_DIR%sql"
set "WORK_DIR=%SCRIPT_DIR%tmp"

REM ---- Sanity checks -------------------------------------------------
if not exist "%SQL_DIR%\*.sql" (
    echo [ERROR] No SQL test scripts found in: %SQL_DIR%
    pause
    exit /b 1
)

if not exist "%BUILD_DIR%\sqlcompiler.exe" (
    if not exist "%BUILD_DIR%\sqlcompiler" (
        echo [ERROR] sqlcompiler executable not found in: %BUILD_DIR%
        echo         Please build the project first.
        pause
        exit /b 1
    )
)
if exist "%BUILD_DIR%\sqlcompiler.exe" (
    set "EXEC=%BUILD_DIR%\sqlcompiler.exe"
) else (
    set "EXEC=%BUILD_DIR%\sqlcompiler"
)

if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
cd /d "%WORK_DIR%"

REM ---- Run all tests -------------------------------------------------
set "PASSED=0"
set "FAILED=0"
set "FAILED_TESTS="

echo ==========================================
echo   SQL Compiler Test Suite
echo   Exec:  %EXEC%
echo   CWD:   %WORK_DIR%
echo ==========================================
echo.

for %%f in ("%SQL_DIR%\*.sql") do (
    set "test=%%f"
    set "name=%%~nf"
    set "db_file=%WORK_DIR%\!name!.db"
    set "out_file=%WORK_DIR%\!name!.out"
    if exist "!db_file!" del "!db_file!"

    echo [ RUN  ] !name!

    "%EXEC%" "!db_file!" < "!test!" > "!out_file!" 2>&1
    set "EXITCODE=!errorlevel!"

    REM Heuristic: success iff exit code is 0 AND output has no "Error:" line.
    set "HAS_ERROR=0"
    findstr /B /C:"sqlcompiler> Error:" "!out_file!" >nul 2>&1
    if !errorlevel! == 0 set "HAS_ERROR=1"

    REM Decide pass/fail with a helper file so we don't trip on cmd's
    REM if/else nesting quirks inside for-loops. Writing a one-byte
    REM marker is much more robust than set /a on nested branches.
    set "RESULT_FILE=!WORK_DIR!\!name!.result"
    set "FAIL_REASON="
    if !EXITCODE! neq 0 (
        set "FAIL_REASON=exit=!EXITCODE!"
    ) else (
        if !HAS_ERROR! neq 0 set "FAIL_REASON=sql error"
    )

    if defined FAIL_REASON (
        echo  [FAIL] ^(!FAIL_REASON!^)
        set /a FAILED = FAILED + 1
        set "FAILED_TESTS=!FAILED_TESTS! !name!"
    ) else (
        echo  [ OK ]
        set /a PASSED = PASSED + 1
    )
)

echo.
echo ==========================================
echo   Summary:  !PASSED! passed,  !FAILED! failed
echo ==========================================

if !FAILED! gtr 0 (
    echo.
    echo [Failed tests]
    echo !FAILED_TESTS!
    echo.
    pause
    endlocal & exit /b 1
)
echo.
pause
endlocal & exit /b 0
