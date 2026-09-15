"""FastAPI routes for the SQL-Compiler web UI.

Endpoints
---------
GET  /api/health                     liveness + engine path discovery
POST /api/db/open                    bind a `.db` file path (creates if missing)
POST /api/db/close                   drop the session engine + path (idempotent)
POST /api/db/unlink?path=…           delete a `.db` (or companion WAL) file
GET  /api/db/ls?directory=…          list `.db` files in a directory
POST /api/schema/tables              list tables (SHOW TABLES + per-table count)
POST /api/schema/table               describe one table (SHOW COLUMNS + CREATE)

POST /api/query/execute              run a single SQL statement

Per-process state: an `AppSession` holds the currently bound database
path and the most recent engine instance.  Multi-user / multi-DB
concurrency is not a goal of this UI; the C++ engine is single-user.
"""

from __future__ import annotations

import asyncio
import sys
import time
from contextlib import asynccontextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, HTTPException, Query
from pydantic import BaseModel, Field

from backend.engine import (
    EngineError,
    SqlEngine,
    describe_table,
    find_engine_binary,
    list_db_files,
    list_tables,
)
from backend.schemas import (
    BrowseEntry,
    BrowseRequest,
    BrowseResponse,
    CloseDatabaseResponse,
    ColumnInfo,
    DatabaseFileInfo,
    DebugData,
    DebugRequest,
    DebugResponse,
    ExecuteRequest,
    ExecuteResponse,
    HealthResponse,
    ListDatabasesResponse,
    ListTablesResponse,
    OpenDatabaseRequest,
    OpenDatabaseResponse,
    PageInfo,
    ReplacementEntry,
    ResetStorageResponse,
    StatementResult,
    StoragePagesResponse,
    StorageStats,
    StorageStatsResponse,
    TableSchemaResponse,
    TableSummary,
    TokenInfo,
    UnlinkFileResponse,
    ValidatePathRequest,
    ValidatePathResponse,
)

router = APIRouter()


# ── App-level session ──────────────────────────────────────────────────────


@dataclass
class AppSession:
    """In-process state for the web UI.

    One Database at a time (matching the C++ engine's single-user
    design).  A new `SqlEngine` is built on `open_database`; the
    previous one (if any) is left to GC — each instance owns its own
    subprocess calls and there is no persistent state to release.
    """

    db_path: Optional[str] = None
    engine: Optional[SqlEngine] = None
    binary: Optional[Path] = None
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)
    # Latest storage_stats / replacement_log observed during an
    # `execute_debug` run.  The C++ engine keeps cumulative counters;
    # we deliberately do *not* run a probe `SELECT 1` to fetch them
    # because that would inflate the counters every time the user
    # opens the storage tab, defeating the "Reset View" feature.
    last_stats_raw: Optional[dict] = None
    last_repl_log_raw: Optional[list] = None


SESSION = AppSession()


# ── Helpers ─────────────────────────────────────────────────────────────────


def _engine_or_404() -> SqlEngine:
    if SESSION.engine is None or SESSION.db_path is None:
        raise HTTPException(status_code=409, detail="No database is currently open. Use /api/db/open first.")
    return SESSION.engine


@asynccontextmanager
async def _engine_call():
    """Acquire the engine lock and yield the live SqlEngine.

    The C++ engine is a single-process, single-threaded executable
    that opens the same `.db` file.  Two concurrent requests against
    it (e.g. a slow visualize run + a list_tables refresh) would each
    spawn a subprocess pointing at the same database, and the second
    one would race with the first one's writes — corrupting the
    buffer-pool stats and potentially the WAL.  Wrapping every engine
    call in this context manager serialises them and surfaces a clean
    503 when the queue is overloaded.
    """
    if SESSION.lock.locked():
        # If a previous request is still in flight, surface that
        # immediately rather than letting the user stare at a spinner
        # for the entire duration of the long-running query.
        raise HTTPException(
            status_code=503,
            detail="Engine is busy with another request. Please retry shortly.",
        )
    async with SESSION.lock:
        yield _engine_or_404()


def _to_statement_result(block, elapsed_ms: int) -> StatementResult:
    """Convert an engine `ParsedBlock` into the API's StatementResult.

    `block.statement` carries the per-statement text the engine parser
    associated with this block; fall back to the caller-provided string
    for empty blocks (e.g. trailing scripts that produced no output).
    On failure the raw diagnostic is surfaced in a dedicated `error`
    field so the UI can show the exact message independently of the
    human-friendly `message`.
    """
    return StatementResult(
        success=block.success,
        statement=block.statement or "",
        message=block.message if block.message else ("OK" if block.success else "Execution failed"),
        error=block.message if not block.success else "",
        column_names=block.column_names,
        rows=block.rows,
        elapsed_ms=elapsed_ms,
        kind=block.kind,  # type: ignore[arg-type]
        debug=getattr(block, "debug", None),
    )


# ── Health ─────────────────────────────────────────────────────────────────


@router.get("/api/health", response_model=HealthResponse)
async def health() -> HealthResponse:
    binary = SESSION.binary or find_engine_binary()
    return HealthResponse(
        status="ok",
        version="0.1.0",
        engine_path=str(binary) if binary else None,
        engine_exists=binary is not None,
        db_path=SESSION.db_path,
    )


# ── Database binding ───────────────────────────────────────────────────────


class _OpenBody(OpenDatabaseRequest):
    pass


@router.post("/api/db/open", response_model=OpenDatabaseResponse)
async def open_database(req: OpenDatabaseRequest) -> OpenDatabaseResponse:
    """Bind the session to a `.db` file (creating it if necessary)."""
    binary = SESSION.binary or find_engine_binary()
    if binary is None:
        raise HTTPException(
            status_code=500,
            detail=(
                "sqlcompiler engine binary not found. Build the C++ project "
                "(cmake --build build) or set SQLCOMPILER_BIN to the executable path."
            ),
        )
    SESSION.binary = binary  # cache the resolution for subsequent calls

    p = Path(req.db_path).expanduser()
    if not p.is_absolute():
        p = p.resolve()
    parent = p.parent
    if not parent.exists() or not parent.is_dir():
        raise HTTPException(
            status_code=400,
            detail=f"Parent directory does not exist or is not a directory: {parent}",
        )

    is_new = not p.exists()
    try:
        engine = SqlEngine(binary, p)
    except EngineError as e:
        raise HTTPException(status_code=500, detail=str(e)) from e
    except Exception as e:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=f"Failed to open engine: {e}") from e

    SESSION.engine = engine
    SESSION.db_path = str(p)
    # Reset the storage stats cache.  The cumulative counters in the
    # C++ engine are global across DB files (the engine process is
    # reused), so showing the previous DB's numbers on a freshly
    # opened file would be misleading.  The user must run a query on
    # the new DB to refresh them.
    SESSION.last_stats_raw = None
    SESSION.last_repl_log_raw = None
    return OpenDatabaseResponse(
        success=True,
        db_path=str(p),
        is_new=is_new,
        message="Database opened" if not is_new else "Database created",
    )


@router.post("/api/db/close", response_model=CloseDatabaseResponse)
async def close_database() -> CloseDatabaseResponse:
    """Drop the currently-bound engine + path.

    Idempotent: calling on an empty session returns `was_open=False`
    rather than 404, so the frontend can use it as part of a
    delete-then-reopen flow without first having to probe whether a
    database is open.
    """
    if SESSION.engine is None and SESSION.db_path is None:
        return CloseDatabaseResponse(success=True, was_open=False)
    previous = SESSION.db_path or ""
    SESSION.engine = None
    SESSION.db_path = None
    SESSION.last_stats_raw = None
    SESSION.last_repl_log_raw = None
    return CloseDatabaseResponse(
        success=True,
        was_open=True,
        previous_db_path=previous,
        message="Database closed",
    )


@router.post("/api/db/unlink", response_model=UnlinkFileResponse)
async def unlink_db_file(path: str = Query(..., min_length=1)) -> UnlinkFileResponse:
    """Delete a single `.db` (or `.wal` / `.shm`) file by absolute path.

    Safety constraints — this endpoint can destroy user data:

    1. Path must be absolute.
    2. Parent directory must exist.
    3. Only `.db`, `.wal`, `-wal`, `.shm`, `-shm` suffixes are allowed.
       Rejecting other extensions prevents callers from asking us to
       delete `.exe` / source code / arbitrary files.
    4. If the path matches the currently-open database, the session
       is cleared so the next read doesn't touch a deleted file.

    A `.db` delete also removes the engine's side-car files
    (`<db>.wal`, `<db>.shm`) — see `_delete_companions`.  Without that,
    the next `Database` open runs ARIES recovery on the orphaned WAL
    and replays the old catalog back into the freshly created file.

    A missing file returns `deleted=False, success=True` so the
    delete-then-reopen flow is idempotent.
    """
    p = Path(path).expanduser()
    if not p.is_absolute():
        raise HTTPException(status_code=400, detail=f"Path must be absolute: {path}")
    parent = p.parent
    if not parent.exists() or not parent.is_dir():
        raise HTTPException(
            status_code=400,
            detail=f"Parent directory does not exist: {parent}",
        )
    suffix = p.suffix.lower()
    # Strip a leading dash variant (e.g. ".db-wal" parsed by Path as suffix "-wal").
    raw_suffix = p.name.lower()
    allowed_suffixes = {".db", ".wal", "-wal", ".shm", "-shm", ".tmp"}
    if suffix not in allowed_suffixes and not any(
        raw_suffix.endswith(s) for s in (".db.wal", ".db-wal")
    ):
        raise HTTPException(
            status_code=400,
            detail=(
                f"Refusing to delete files with suffix {suffix!r}; "
                f"only .db / .wal / .shm are allowed"
            ),
        )

    cleared = False
    if SESSION.db_path and str(p).lower() == SESSION.db_path.lower():
        SESSION.engine = None
        SESSION.db_path = None
        SESSION.last_stats_raw = None
        SESSION.last_repl_log_raw = None
        cleared = True

    if not p.exists():
        # The `.db` may already be gone while its WAL survives from an
        # earlier partial reset — still take the companion with us, so
        # this call acts as a real recovery rather than a no-op.
        companions_deleted = _delete_companions(p, suffix)
        return UnlinkFileResponse(
            success=True,
            deleted=False,
            cleared_session=cleared,
            path=str(p),
            companions_deleted=companions_deleted,
            message="File does not exist (already gone or never created)",
        )
    try:
        p.unlink()
    except PermissionError as e:
        raise HTTPException(
            status_code=403,
            detail=f"Permission denied deleting {p}: {e}",
        ) from e
    except OSError as e:
        raise HTTPException(
            status_code=500,
            detail=f"Failed to delete {p}: {e}",
        ) from e
    companions_deleted = _delete_companions(p, suffix)
    return UnlinkFileResponse(
        success=True,
        deleted=True,
        cleared_session=cleared,
        path=str(p),
        companions_deleted=companions_deleted,
        message="File deleted",
    )


def _delete_companions(p: Path, suffix: str) -> list[str]:
    """Delete the engine's side-car files for a `.db` and return them.

    `Database` keeps its ARIES write-ahead log at `<db_file>.wal`
    (`src/db/Database.cpp`) and opens it on every startup, running
    analysis → redo → undo *even for a brand-new, empty database*
    (`src/db/Database.cpp`'s recovery block).  So a reset that deletes
    only `foo.db` leaves `foo.db.wal` behind, and the next open replays
    the old log into the recreated file — the tables the user just
    wiped come back.  Deleting the WAL together with the `.db` is what
    makes "重置库" actually stick.

    `<db>.shm` is removed opportunistically for forward compatibility
    (this engine does not currently create one).  A non-`.db` path has
    no companions, so the call is a no-op for direct `.wal` deletes.
    """
    if suffix != ".db":
        return []
    removed: list[str] = []
    for cp in (Path(str(p) + ".wal"), Path(str(p) + ".shm")):
        if not cp.exists():
            continue
        try:
            cp.unlink()
        except PermissionError as e:
            raise HTTPException(
                status_code=403,
                detail=f"Permission denied deleting {cp}: {e}",
            ) from e
        except OSError as e:
            raise HTTPException(
                status_code=500,
                detail=f"Failed to delete {cp}: {e}",
            ) from e
        removed.append(str(cp))
    return removed


@router.get("/api/db/ls", response_model=ListDatabasesResponse)
async def list_db_under(directory: str = Query(..., min_length=1)) -> ListDatabasesResponse:
    """List `.db` files in a directory (non-recursive, by mtime desc)."""
    p = Path(directory).expanduser()
    if not p.is_absolute():
        p = p.resolve()
    if not p.exists():
        raise HTTPException(status_code=400, detail=f"Path does not exist: {p}")
    if not p.is_dir():
        # accept a `.db` file's parent as the directory
        if p.suffix.lower() == ".db" and p.parent.exists():
            p = p.parent
        else:
            raise HTTPException(status_code=400, detail=f"Not a directory: {p}")
    items = await list_db_files(p)
    return ListDatabasesResponse(items=[DatabaseFileInfo(**it) for it in items])


@router.post("/api/db/validate", response_model=ValidatePathResponse)
async def validate_db_path(req: ValidatePathRequest) -> ValidatePathResponse:
    """Probe a typed path so the UI can show real-time feedback.

    Decision tree:
      1. If the path's parent doesn't exist → `parent_missing`.
      2. If the path itself is an existing directory → `directory`.
      3. If the path ends in `.db` and the file exists → `existing_file`.
      4. If the path ends in `.db` and the file does NOT exist → `new_file`.
      5. Otherwise → `wrong_type`.
    """
    p = Path(req.path).expanduser()
    if not p.is_absolute():
        p = p.resolve()
    parent = p.parent
    if not parent.exists() or not parent.is_dir():
        return ValidatePathResponse(
            status="parent_missing",
            parent=str(parent),
            message=f"Parent directory does not exist: {parent}",
        )
    if p.is_dir():
        return ValidatePathResponse(
            status="directory",
            parent=str(parent),
            message=f"This is a directory. Pick a `.db` file inside, or type a new file name.",
        )
    if p.suffix.lower() == ".db":
        if p.exists() and p.is_file():
            return ValidatePathResponse(
                status="existing_file",
                parent=str(parent),
                message=f"Existing database file ({p.stat().st_size} bytes).",
            )
        return ValidatePathResponse(
            status="new_file",
            parent=str(parent),
            message="A new database will be created at this path.",
        )
    return ValidatePathResponse(
        status="wrong_type",
        parent=str(parent),
        message=f"Path exists but is not a .db file. Use a .db extension or pick a directory.",
    )


# ── Server-side filesystem browser ────────────────────────────────────────


def _system_roots() -> list[str]:
    """Return the list of filesystem roots to show in the "Home" menu.

    Windows: drive letters C:\\, D:\\, …
    POSIX:   just `/`
    """
    if sys.platform == "win32":
        import string
        roots: list[str] = []
        for letter in string.ascii_uppercase:
            drive = f"{letter}:\\"
            if Path(drive).exists():
                roots.append(drive)
        return roots
    return ["/"]


def _user_home() -> Path:
    """Best-effort resolution of the current user's home directory."""
    return Path.home()


@router.post("/api/db/browse", response_model=BrowseResponse)
async def browse_directory(req: BrowseRequest) -> BrowseResponse:
    """Server-side filesystem browser.

    Lists the immediate children of `req.path`, returning:

    - all *subdirectories* (sorted alphabetically, case-insensitive);
    - all `*.db` files in the directory (sorted by mtime desc).

    Other files are filtered out so the picker stays focused on
    navigable targets.  The response also carries `parent` (for the
    "up" button) and `roots` (for the "drives" / filesystem-root jump
    menu) so the frontend can build a full file browser without any
    extra round-trips.
    """
    raw = (req.path or "").strip()
    # Default: user's home
    if not raw:
        target = _user_home()
    else:
        target = Path(raw).expanduser()
        if not target.is_absolute():
            target = target.resolve()
    # Auto-recover: if the typed path is an existing .db file, list
    # its parent (lets the user jump straight into "show me siblings
    # of this file").  If it looks like a .db filename in an existing
    # parent directory (e.g. for the "create new" flow), also list
    # the parent — the response carries the typed path so the UI can
    # pre-fill the "new file" name.
    if target.is_file():
        target = target.parent
    if (not target.exists() or not target.is_dir()) and target.suffix.lower() == ".db":
        candidate_parent = target.parent
        if candidate_parent.exists() and candidate_parent.is_dir():
            return BrowseResponse(
                path=str(candidate_parent),
                parent=str(candidate_parent.parent) if candidate_parent.parent != candidate_parent else "",
                entries=[],
                roots=_system_roots(),
                error="",
            )
    if not target.exists() or not target.is_dir():
        return BrowseResponse(
            path=str(target),
            parent="",
            entries=[],
            roots=_system_roots(),
            error=f"Path does not exist or is not a directory: {target}",
        )
    try:
        dirs: list[BrowseEntry] = []
        dbs: list[tuple[float, BrowseEntry]] = []
        for child in target.iterdir():
            try:
                if child.is_dir():
                    # Skip hidden / system dirs on Windows to keep the
                    # list clean.  On POSIX we leave that choice to the
                    # user (dotfiles are first-class there).
                    if sys.platform == "win32" and child.name.startswith("."):
                        continue
                    dirs.append(BrowseEntry(name=child.name, path=str(child), kind="dir"))
                elif child.is_file() and child.suffix.lower() == ".db":
                    stat = child.stat()
                    dbs.append((stat.st_mtime, BrowseEntry(name=child.name, path=str(child), kind="db")))
            except (PermissionError, OSError):
                # unreadable child — skip silently
                continue
        dirs.sort(key=lambda e: e.name.lower())
        dbs.sort(key=lambda t: t[0], reverse=True)
        entries = dirs + [entry for _, entry in dbs]
        return BrowseResponse(
            path=str(target),
            parent=str(target.parent) if target.parent != target else "",
            entries=entries,
            roots=_system_roots(),
            error="",
        )
    except (PermissionError, OSError) as e:
        return BrowseResponse(
            path=str(target),
            parent="",
            entries=[],
            roots=_system_roots(),
            error=f"Cannot read directory: {e}",
        )


# ── Schema ─────────────────────────────────────────────────────────────────


@router.post("/api/schema/tables", response_model=ListTablesResponse)
async def schema_tables() -> ListTablesResponse:
    async with _engine_call() as engine:
        try:
            ok, tables, msg = await list_tables(engine)
        except EngineError as e:
            raise HTTPException(status_code=500, detail=str(e)) from e
    if not ok:
        return ListTablesResponse(success=False, tables=[], message=msg or "SHOW TABLES failed")
    return ListTablesResponse(
        success=True,
        tables=[
            TableSummary(
                name=t["name"],
                column_count=t.get("column_count", 0),
                row_count=t.get("row_count"),
            )
            for t in tables
        ],
        message="",
    )


class _TableBody(BaseModel):
    table: str = Field(..., min_length=1)


@router.post("/api/schema/table", response_model=TableSchemaResponse)
async def schema_table(req: _TableBody) -> TableSchemaResponse:
    engine = _engine_or_404()
    # basic identifier sanity (no embedded whitespace / quotes)
    name = req.table.strip()
    if not name or any(ch in name for ch in " \t\n;'\"`"):
        raise HTTPException(status_code=400, detail=f"Invalid table name: {name!r}")
    try:
        ok, columns, create_sql, msg = await describe_table(engine, name)
    except EngineError as e:
        raise HTTPException(status_code=500, detail=str(e)) from e
    if not ok:
        return TableSchemaResponse(
            success=False,
            table=name,
            columns=[],
            create_sql="",
            message=msg or "SHOW COLUMNS failed",
        )
    return TableSchemaResponse(
        success=True,
        table=name,
        columns=[ColumnInfo(**c) for c in columns],
        create_sql=create_sql,
        message="",
    )


# ── Query execution ────────────────────────────────────────────────────────


@router.post("/api/query/execute", response_model=ExecuteResponse)
async def execute_query(req: ExecuteRequest) -> ExecuteResponse:
    statement = req.statement
    if not statement.strip():
        raise HTTPException(status_code=400, detail="Empty statement")
    t0 = time.monotonic()
    async with _engine_call() as engine:
        try:
            run = await engine.execute(
                statement, on_error=req.on_error, transaction=req.transaction
            )
        except EngineError as e:
            raise HTTPException(status_code=500, detail=str(e)) from e
    total_ms = int((time.monotonic() - t0) * 1000)
    per_stmt_ms = (total_ms // max(1, len(run.blocks))) if run.blocks else total_ms
    results = [_to_statement_result(blk, per_stmt_ms) for blk in run.blocks]
    overall_success = all(r.success for r in results) and run.success
    # In "abort" mode a failing block at the end means we stopped early.
    aborted = (
        req.on_error == "abort"
        and bool(run.blocks)
        and not run.blocks[-1].success
    )
    return ExecuteResponse(
        success=overall_success,
        results=results,
        db_path=SESSION.db_path or "",
        total_elapsed_ms=total_ms,
        statement_count=len(results),
        aborted=aborted,
    )


# ── Visualization / Debug ────────────────────────────────────────────────────


@router.post("/api/query/debug", response_model=DebugResponse)
async def query_debug(req: DebugRequest) -> DebugResponse:
    """Execute a SQL statement and return full visualization data.

    Runs with --debug-output to capture: token stream, AST text,
    plan JSON (before/after optimization), and storage stats.
    """
    statement = req.statement
    if not statement.strip():
        raise HTTPException(status_code=400, detail="Empty statement")
    t0 = time.monotonic()
    async with _engine_call() as engine:
        try:
            run, debug_raw = await engine.execute_debug(statement)
        except EngineError as e:
            raise HTTPException(status_code=500, detail=str(e)) from e
    total_ms = int((time.monotonic() - t0) * 1000)
    per_stmt_ms = (total_ms // max(1, len(run.blocks))) if run.blocks else total_ms
    results = [_to_statement_result(blk, per_stmt_ms) for blk in run.blocks]

    # Build DebugData from raw JSON
    debug_data = None
    malformed = 0
    if debug_raw:
        # Defensive parsing: the C++ engine emits well-formed tokens,
        # but a future schema drift could land us here with a missing
        # `type` or `lexeme`.  Pydantic's ValidationError would
        # otherwise propagate as an uncaught 500.  Skip malformed
        # entries and surface a flag in the message so the user can
        # tell the visualisation is incomplete.
        tokens_raw = debug_raw.get("tokens", []) or []
        parsed_tokens: list[TokenInfo] = []
        for t in tokens_raw:
            try:
                parsed_tokens.append(TokenInfo(**t))
            except Exception:
                malformed += 1
        stats_raw = debug_raw.get("storage_stats") or {}
        # Stash the raw counters on the session so /api/storage/stats
        # can return them without triggering a fresh probe SELECT that
        # would itself inflate the counters.
        SESSION.last_stats_raw = stats_raw
        SESSION.last_repl_log_raw = debug_raw.get("replacement_log") or []
        storage_stats = StorageStats(
            hit_count=stats_raw.get("hit_count", 0),
            miss_count=stats_raw.get("miss_count", 0),
            replacement_count=stats_raw.get("replacement_count", 0),
            hit_rate=stats_raw.get("hit_rate", 0.0),
            total_pages=stats_raw.get("total_pages", 0),
        )
        repl_log = [ReplacementEntry(**e) for e in SESSION.last_repl_log_raw]
        debug_data = DebugData(
            tokens=parsed_tokens,
            ast_text=debug_raw.get("ast_text") or "",
            plan_json=debug_raw.get("plan_json") or "",
            plan_before_opt=debug_raw.get("plan_before_opt") or "",
            storage_stats=storage_stats,
            replacement_log=repl_log,
        )

    overall_success = all(r.success for r in results) and run.success
    msg = "" if overall_success else run.stderr
    if malformed:
        # Surface the dropped-token count so the user knows the
        # visualisation may be incomplete.  Don't clobber the engine's
        # error message (it's more informative); append if both apply.
        suffix = f"({malformed} malformed token(s) skipped)"
        msg = f"{msg} {suffix}" if msg else suffix
    return DebugResponse(
        success=overall_success,
        results=results,
        debug=debug_data,
        db_path=SESSION.db_path or "",
        total_elapsed_ms=total_ms,
        message=msg,
    )


@router.get("/api/storage/stats", response_model=StorageStatsResponse)
async def storage_stats() -> StorageStatsResponse:
    """Return the latest buffer-pool counters from the last execute_debug run.

    We deliberately do *not* run a probe query here because every
    `SELECT 1;` would itself increment the hit/miss counters, which
    would silently inflate the numbers shown on the storage tab
    and break the user's "Reset View" baseline.  Instead, the
    counters come from the last `query_debug` invocation the user
    already triggered.  When no debug run has happened yet (the
    page was just opened) the response reports an empty snapshot
    so the UI can render the "run a statement first" empty state.
    """
    if SESSION.db_path is None:
        raise HTTPException(
            status_code=409,
            detail="No database is currently open. Use /api/db/open first.",
        )
    stats_raw = SESSION.last_stats_raw or {}
    repl_log_raw = SESSION.last_repl_log_raw or []
    stats = StorageStats(
        hit_count=stats_raw.get("hit_count", 0),
        miss_count=stats_raw.get("miss_count", 0),
        replacement_count=stats_raw.get("replacement_count", 0),
        hit_rate=stats_raw.get("hit_rate", 0.0),
        total_pages=stats_raw.get("total_pages", 0),
    )
    repl_log = [ReplacementEntry(**e) for e in repl_log_raw]
    return StorageStatsResponse(
        success=True,
        stats=stats,
        replacement_log=repl_log,
        message=(
            "Run a statement in the Visualize tab to refresh counters."
            if not stats_raw
            else ""
        ),
    )


@router.post("/api/storage/reset", response_model=ResetStorageResponse)
async def reset_storage_view() -> ResetStorageResponse:
    """Capture the *current* buffer-pool counters as a "baseline".

    The C++ engine keeps cumulative counters internally; we can't
    reset them without rebuilding the buffer pool.  Instead we
    expose this endpoint so the frontend can ask the backend for
    the canonical "before" snapshot of stats at reset time, then
    render deltas against it locally.  This keeps the wire format
    symmetric with `/api/storage/stats` while still giving the
    user a clean "fresh session" view of the counters.

    Internally no probe query runs — we just hand the current
    cached snapshot back as the baseline.  When no debug run has
    happened yet the baseline is all-zeros.
    """
    if SESSION.db_path is None:
        raise HTTPException(
            status_code=409,
            detail="No database is currently open. Use /api/db/open first.",
        )
    stats_raw = SESSION.last_stats_raw or {}
    baseline = StorageStats(
        hit_count=stats_raw.get("hit_count", 0),
        miss_count=stats_raw.get("miss_count", 0),
        replacement_count=stats_raw.get("replacement_count", 0),
        hit_rate=stats_raw.get("hit_rate", 0.0),
        total_pages=stats_raw.get("total_pages", 0),
    )
    return ResetStorageResponse(
        success=True,
        baseline=baseline,
        message="Baseline captured. Display now shows deltas against this snapshot.",
    )
