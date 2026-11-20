# Visualization Module — Testing Guide

This document describes how the visualization module is tested end-to-end.
It covers the four layers of the test pyramid (unit, integration, end-to-end,
static UI) plus a live smoke-test driver that exercises the full stack
against a real C++ engine.

## 1. Layout

```
webui/backend/tests/
├── test_engine.py        # Pure-Python parsing / splitting / classification
├── test_api.py           # FastAPI TestClient (no subprocess, monkey-patched engine)
├── test_e2e.py           # Live HTTP server + real C++ binary
├── test_ui_static.py     # HTML / CSS / JS structural assertions + JS behavior
└── smoke_against_server.py # Standalone smoke driver (run against `python -m webui.backend`)
```

The current count is **150 tests passing** (plus 1 skipped when
`fastapi.testclient` is missing).

## 2. Running the suite

From `webui/`:

```powershell
# Everything
py -m unittest discover -s backend/tests -t . -v

# A single module
py -m unittest backend.tests.test_engine -v
py -m unittest backend.tests.test_api    -v
py -m unittest backend.tests.test_e2e    -v
py -m unittest backend.tests.test_ui_static -v
```

The `e2e` tests spawn the C++ binary (`build/sqlcompiler.exe`) — make sure
the project has been built at least once:

```powershell
cd c:\Users\Lenovo\Desktop\SQL-Compiler-xu
cmake -S . -B build
cmake --build build --config Release
```

## 3. What each layer covers

### 3.1 `test_engine.py` — unit tests (no I/O)

* `_split_statements` edge cases: escaped quotes, line comments, unicode,
  unterminated strings, only-semicolons, etc.
* `_split_blocks` / `_parse_one_block`: per-statement block padding,
  stderr/error propagation, multi-statement scripts.
* `_classify_kind`: DDL / DML / SELECT / TXN / OTHER tagging used by the
  UI banner.
* `_strip_cell_padding`: formatting-edge preservation.
* Pydantic model round-trips (`TokenInfo`, `DebugData`, etc.).
* Engine binary discovery + missing-binary error path.

### 3.2 `test_api.py` — FastAPI integration (in-process)

Drives `create_app()` via `fastapi.testclient.TestClient` with a monkey-patched
`SqlEngine`.  No subprocess, no real binary — tests stay fast and deterministic.

Endpoints covered:
* `/api/health`
* `/api/db/open`, `/api/db/browse`, `/api/db/ls`
* `/api/schema/tables`, `/api/schema/table`
* `/api/query/execute`
* `/api/query/debug` — including malformed-token resilience and storage
  counters being captured as a side effect.
* `/api/storage/stats` and `/api/storage/reset` — including the
  "Reset View baseline" contract.
* Error / edge cases: empty statement (400), no-open-db (409), bad paths.

### 3.3 `test_e2e.py` — end-to-end

Starts uvicorn on a free port and exercises real HTTP requests through
the full stack — `FastAPI` → `engine.execute_debug` →
`build/sqlcompiler.exe --debug-output` → JSON debug payload → response.

This is the layer that catches integration regressions where the Python
wrapper and the C++ binary disagree on field names or types.

### 3.4 `test_ui_static.py` — static UI + behavioral checks

Two flavours:

1. **Static structure** — `index.html`, `style.css`, and `app.js` are
   parsed as text and asserted on:
   * The five mode tabs (Query / Tables / Schema / Storage / Visualize)
     and the Run button are present.
   * Required CSS classes (`.btn-ghost`, `.badge`, `.viz-content`, …)
     are defined.
   * `app.js` defines the required renderer functions
     (`renderTokens`, `renderPlan`, `renderStorage`, `tokenCategoryClass`,
     `makeCacheKey`, …).
   * No `eval()` or `new Function()` (XSS hardening).
   * `escapeHtml` neutralizes `<`, `>`, `&`, `"`, `'`.
2. **Behavioral** — the relevant portions of `app.js` are extracted and
   run inside a JS harness (`node --input-type=module` via subprocess)
   to verify:
   * `tokenCategoryClass` maps every C++ token prefix/suffix to a CSS class
     (literal, operator, keyword, identifier).
   * `makeCacheKey` normalises whitespace and hashes with FNV-1a.
   * The LRU cache enforces its 8-entry cap, promotes on hit, and returns
     `undefined` on miss.
   * `renderPlan` walks nested operator trees.
   * `renderStorage` formats hit-rate bars and computes deltas against
     a baseline.

The behavioral suite uses Node.js (>= 14).  If Node is missing those
specific tests are skipped automatically.

## 4. Smoke test against a running server

The unit/integration/e2e suites run inside their own process.  To verify
the **shipped** wire format end-to-end (and to demo the running stack
without opening a browser), use the live smoke driver:

### 4.1 Start the server

```powershell
cd c:\Users\Lenovo\Desktop\SQL-Compiler-xu
py -m webui.backend    # listens on http://127.0.0.1:8765
```

The server pre-resolves the C++ binary path at startup and serves the
static frontend at `/`.

### 4.2 Run the smoke driver

In another shell:

```powershell
cd c:\Users\Lenovo\Desktop\SQL-Compiler-xu\webui
py -m backend.tests.smoke_against_server
```

The driver issues real HTTP requests, asserts on the JSON shape, and
prints a one-line summary per check.  Exit code 0 = all passed; non-zero
= first failure (printed with the full body for triage).

What it covers (in order):

| # | Check                                                   |
|---|---------------------------------------------------------|
| 1 | `/api/health` reports engine path + exists               |
| 2 | Open a fresh `.db` file                                  |
| 3 | CREATE TABLE + INSERT (DDL / DML round-trip)            |
| 4 | `/api/query/debug` returns tokens / AST / plan JSON    |
| 5 | `/api/storage/stats` reports buffer-pool counters       |
| 6 | `/api/storage/reset` returns a baseline snapshot        |
| 7 | Subsequent debug run keeps counters monotonic           |
| 8 | Empty statement is rejected with HTTP 400               |
| 9 | Re-opening a different `.db` resets stats to empty       |

## 5. Performance checks

The visualization module ships two micro-optimizations:

* **LRU cache** keyed by a normalized + FNV-1a-hashed SQL string,
  capacity 8 (`vizState.vizCache`).  Behavioural tests verify cap,
  eviction, and promotion (`test_lru_cache_*`).
* **80ms debounce** on storage-mode refresh (`refreshStorageStats`) so
  tab-switching does not flood the network.

If you change either, update `test_ui_static.py` accordingly.

## 6. Adding a new visualization field

When the C++ engine grows a new debug field (e.g. `cost_estimates`):

1. Extend `schemas.DebugData` with the typed field.
2. Update `app.js`'s `renderXxx` function(s) and add a `test_*` in
   `test_ui_static.TestBehavioralRenderers`.
3. If the field is also fetched via `/api/storage/stats`, mirror it in
   `StorageStats` and assert in `test_api.py`.
4. Run `py -m unittest discover -s backend/tests -t .` — the suite
   exercises the new field at every layer.

## 7. Debugging tips

* `webui/backend/static/app.js` is the canonical source of the
  frontend; the behaviour tests run a *slice* of it, so edits there can
  silently break tests.  If a behavioural test starts failing on a
  marker mismatch, update `slice_app_js_for_node` in `test_ui_static.py`.
* The smoke driver exits with code 1 on the first failure and prints the
  raw response body — copy/paste it back into a curl invocation to
  reproduce.
* The FastAPI server logs `uvicorn` startup output to stdout; any
  ValidationError or EngineError from the C++ binary surfaces in the
  response `message` field and (when verbose) in the server logs.