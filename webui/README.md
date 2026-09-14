# SQL-Compiler Web UI

A browser-based management console for the C++17 `SQL-Compiler` engine.

The backend is a thin FastAPI layer that talks to the compiled
`sqlcompiler.exe` binary in batch mode (`-f <script.sql>`), parses the
tabular output, and returns it as JSON.  The frontend is a single-page
vanilla-JS app that mirrors the navy design system used by the
companion EFRI web app in this workspace.

```
SQL-Compiler/
├── build/Debug/sqlcompiler.exe   ← the C++ engine (must be built first)
└── webui/                        ← this directory
    ├── backend/
    │   ├── engine.py             ← subprocess wrapper + output parser
    │   ├── api.py                ← FastAPI routes
    │   ├── schemas.py            ← Pydantic models
    │   ├── main.py               ← FastAPI app factory
    │   ├── __main__.py           ← `python -m webui.backend` entry
    │   └── static/
    │       ├── index.html        ← the page
    │       ├── style.css         ← navy design system
    │       └── app.js            ← single-file controller
    ├── requirements.txt
    ├── run.bat                   ← Windows launcher
    └── run.sh                    ← git-bash / WSL launcher
```

## Quick start (Windows)

```bat
cd webui
run.bat
```

`run.bat` will:
1. Build the C++ engine if `build\Debug\sqlcompiler.exe` is missing.
2. Install `fastapi` / `uvicorn` if not already present.
3. Launch the app on <http://127.0.0.1:8765>.

Open the URL in your browser, click the **No database open** pill in
the top-right, then either:
- type/paste a `.db` path (a new one is created if it does not exist), or
- click any file from the **Recent `.db` files** list (the directory of
  the current path is scanned).

## Quick start (manual)

```bash
# 1) build the C++ engine (one time)
cd ..
mkdir -p build && cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Debug

# 2) install Python deps
cd ../../webui
python -m pip install -r requirements.txt

# 3) launch
python -m uvicorn backend.main:app --host 127.0.0.1 --port 8765 --reload
```

## Features

- **Database binding** — open any `.db` file; new files are auto-created
  on first open.
- **Table list** — auto-refreshes after every DDL statement
  (`CREATE` / `DROP` / `ALTER` / `TRUNCATE`); click a table to open its
  schema tab.
- **Schema viewer** — columns, types, nullability, PK markers, defaults,
  plus the `SHOW CREATE TABLE` SQL, plus a sample `SELECT * LIMIT 100`
  preview.
- **SQL editor** — multi-line textarea, `Ctrl+Enter` to run, `Tab`
  inserts two spaces, "Format" button uppercases keywords.
- **Result rendering** — tabular results with sticky headers, NULL
  styling, numeric right-alignment, per-block kind badge
  (`SELECT`/`DDL`/`DML`/`TXN`/`error`), elapsed time, and a "SQL"
  toggle to see the original statement.
- **EXPLAIN / EXPLAIN ANALYZE** — the engine's plan text is rendered
  as a result table (one row per plan node).
- **Query history** — last 50 statements kept in `localStorage`; click
  any entry to load it back into the editor.

## API surface

| Method | Path                       | Description                                |
| ------ | -------------------------- | ------------------------------------------ |
| GET    | `/api/health`              | Liveness + engine binary path              |
| POST   | `/api/db/open`             | Bind the session to a `.db` file           |
| GET    | `/api/db/ls?directory=…`   | List `.db` files in a directory            |
| POST   | `/api/schema/tables`       | `SHOW TABLES` + per-table column count     |
| POST   | `/api/schema/table`        | `SHOW COLUMNS` + `SHOW CREATE TABLE`       |
| POST   | `/api/query/execute`       | Run a single SQL statement                 |

Interactive API docs (Swagger UI) are served at `/docs`.

## Design notes

- **Engine integration is process-isolated.** Each `execute` call
  spawns `sqlcompiler.exe <db_file> -f <temp.sql>`, captures stdout /
  stderr, and parses the output.  The C++ engine remains a CLI
  REPL — no source changes were needed.
- **Multi-statement scripts are supported on the wire but split on the
  server.** A user-supplied string like
  `"CREATE TABLE t (…); INSERT …; SELECT …;"` is split on `;` and
  written to the temp file with one statement per line so the engine's
  `HasCompleteStatement` line-buffer sees each terminator.
- **Output parsing is line-oriented.** The C++ engine's REPL emits
  tabular blocks for `SELECT` / `SHOW` / `EXPLAIN` and a single-line
  message ("OK", "(N rows)", or an `Error:` line on stderr) for DDL /
  DML / TXN.  The parser walks the line stream, peeling off one block
  per statement.
- **Single-user, single-database, single-thread.** The C++ engine was
  not designed for concurrent access; the web UI matches that
  contract.  Open the database you want to work on, and only one DB
  is bound per browser tab.

## Troubleshooting

- **"engine binary not found"** — the backend looks at
  `build/Debug/sqlcompiler.exe`, then `build/Release/`, then
  `$SQLCOMPILER_BIN`.  Build the C++ project (`cmake --build build`)
  or set the env var.
- **"table not found" right after CREATE** — usually means the
  previous statement errored.  Check the result banner; the parser
  attaches the engine's stderr error message to the failing block.
- **Port 8765 in use** — edit `run.bat` / `run.sh` / `__main__.py` and
  pick another port.  Update the URL accordingly.
