#!/usr/bin/env bash
# ============================================================================
#  SQL-Compiler Web UI launcher (Unix-style; for git-bash on Windows / WSL)
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEBUI="$(cd "$(dirname "$0")" && pwd)"
BIN="$ROOT/build/Debug/sqlcompiler.exe"
BIN_NIX="$ROOT/build/sqlcompiler"

echo "=== SQL-Compiler Web UI =========================================="
echo

# 1) engine binary
if [[ ! -f "$BIN" && ! -f "$BIN_NIX" ]]; then
    echo "[WARN] Engine binary not found; building C++ project..."
    mkdir -p "$ROOT/build"
    (cd "$ROOT/build" && cmake .. && cmake --build .)
fi
[[ -f "$BIN" ]] && echo "[OK] Engine binary: $BIN"
[[ -f "$BIN_NIX" && ! -f "$BIN" ]] && echo "[OK] Engine binary: $BIN_NIX"
echo

# 2) python deps
if ! python -c "import fastapi, uvicorn" >/dev/null 2>&1; then
    echo "Installing Python dependencies..."
    python -m pip install -r "$WEBUI/requirements.txt"
fi
echo "[OK] Python dependencies installed."
echo

# 3) launch
echo "Starting FastAPI on http://127.0.0.1:8765"
echo "Press Ctrl+C to stop."
echo
cd "$WEBUI"
exec python -m uvicorn backend.main:app --host 127.0.0.1 --port 8765 --reload
