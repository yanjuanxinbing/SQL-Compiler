@echo off
REM ============================================================================
REM  SQL-Compiler Web UI launcher (Windows)
REM
REM  Robustness goals:
REM   - Stay open on errors (so the user can see what failed).
REM   - Auto-build the C++ engine if missing, with generator fallback.
REM   - Detect port-in-use and fall back to the next free port.
REM   - Verify the Python environment before launching uvicorn.
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

REM ── 0) paths ──────────────────────────────────────────────────────────────
set "ROOT=%~dp0.."
pushd "%ROOT%" >nul
set "ROOT=%CD%"
popd >nul
set "WEBUI=%ROOT%\webui"
set "BUILD=%ROOT%\build"
set "PORT=8765"
set "BIN_DIR=%BUILD%\Release"
set "BIN=%BIN_DIR%\sqlcompiler.exe"

echo.
echo ============================================================
echo   SQL-Compiler Web UI
echo ============================================================
echo   Project : %ROOT%
echo   Engine  : %BIN%
echo   URL     : http://127.0.0.1:%PORT%
echo.

REM ── 1) ensure the engine binary exists; build if missing ─────────────────
if exist "%BIN%" goto :engine_ok

echo [INFO] Engine binary not found. Building C++ project...
echo.

REM 1a) locate CMake
where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake is not on PATH.
    echo         Install it from https://cmake.org/download/ and reopen this terminal.
    goto :fail
)

REM 1b) pick the best generator available; many users only have one of these
set "CMAKE_GEN="
for %%G in ("Visual Studio 17 2022" "Visual Studio 16 2019" "Ninja" "NMake Makefiles" "MinGW Makefiles" "Unix Makefiles") do (
    if not defined CMAKE_GEN (
        cmake -G "%%~G" --help 1>nul 2>nul
        if not errorlevel 1 set "CMAKE_GEN=%%~G"
    )
)
if not defined CMAKE_GEN (
    echo [ERROR] No supported CMake generator found.
    echo         Install one of: Visual Studio 2022/2019 (with C++ workload),
    echo         Ninja, or MinGW.  Then reopen this terminal and re-run.
    goto :fail
)
echo [INFO] Using CMake generator: %CMAKE_GEN%
echo.

REM 1c) configure + build
if not exist "%BUILD%" mkdir "%BUILD%"
pushd "%BUILD%" >nul
cmake -G "%CMAKE_GEN%" -DCMAKE_BUILD_TYPE=Debug ..
if errorlevel 1 (
    echo [ERROR] CMake configure failed.  See the output above.
    popd >nul
    goto :fail
)
cmake --build . --config Debug
set "RC=%ERRORLEVEL%"
popd >nul
if not "%RC%"=="0" (
    echo [ERROR] C++ build failed with code %RC%.
    goto :fail
)

REM 1d) re-check
if not exist "%BIN%" (
    echo [ERROR] Build finished but %BIN% was not produced.
    goto :fail
)

:engine_ok
echo [OK] Engine binary ready.
echo.

REM ── 2) ensure Python deps are present ────────────────────────────────────
where python >nul 2>nul
if errorlevel 1 (
    echo [ERROR] python is not on PATH.
    echo         Install Python 3.10+ from https://www.python.org/downloads/
    echo         and tick "Add python.exe to PATH" during install.
    goto :fail
)
echo [INFO] Python:
python --version
echo.

python -c "import fastapi, uvicorn, pydantic" 2>nul
if errorlevel 1 (
    echo [INFO] Installing Python dependencies...
    python -m pip install --disable-pip-version-check -r "%WEBUI%\requirements.txt"
    if errorlevel 1 (
        echo [ERROR] pip install failed.  Check the output above.
        goto :fail
    )
)
echo [OK] Python dependencies installed.
echo.

REM ── 3) pick a free port ──────────────────────────────────────────────────
set "TRY_PORT=%PORT%"
:port_check
netstat -an 1>nul 2>nul | findstr /R /C:":%TRY_PORT% .*LISTENING" >nul
if errorlevel 1 goto :port_ok
echo [WARN] Port %TRY_PORT% is in use; trying the next one.
set /a TRY_PORT+=1
if %TRY_PORT% gtr 8780 (
    echo [ERROR] Could not find a free port in [%PORT%..8780].
    goto :fail
)
goto :port_check
:port_ok
set "PORT=%TRY_PORT%"
echo [OK] Using port %PORT%.
echo.

REM ── 4) launch uvicorn ────────────────────────────────────────────────────
echo ============================================================
echo   Server starting on http://127.0.0.1:%PORT%
echo   API docs:  http://127.0.0.1:%PORT%/docs
echo   Press Ctrl+C to stop.
echo ============================================================
echo.

cd /d "%WEBUI%"
python -m uvicorn backend.main:app --host 127.0.0.1 --port %PORT% --reload
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
    echo Server stopped cleanly.
) else (
    echo [ERROR] uvicorn exited with code %RC%.
)
goto :end

:fail
echo.
echo ============================================================
echo   Startup failed.  See the [ERROR] lines above.
echo ============================================================
echo.

:end
echo.
echo Press any key to close this window...
pause >nul
endlocal
