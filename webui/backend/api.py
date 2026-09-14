"""FastAPI routes for the SQL-Compiler web UI.

Endpoints
---------
GET  /api/health                     liveness + engine path discovery
POST /api/db/open                    bind a `.db` file path (creates if missing)
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
    ColumnInfo,
    DatabaseFileInfo,
    ExecuteRequest,
    ExecuteResponse,
    HealthResponse,
    ListDatabasesResponse,
    ListTablesResponse,
    OpenDatabaseRequest,
    OpenDatabaseResponse,
    StatementResult,
    TableSchemaResponse,
    TableSummary,
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


SESSION = AppSession()


# ── Helpers ─────────────────────────────────────────────────────────────────


def _engine_or_404() -> SqlEngine:
    if SESSION.engine is None or SESSION.db_path is None:
        raise HTTPException(status_code=409, detail="No database is currently open. Use /api/db/open first.")
    return SESSION.engine


def _to_statement_result(block, elapsed_ms: int) -> StatementResult:
    """Convert an engine `ParsedBlock` into the API's StatementResult.

    `block.statement` carries the per-statement text the engine parser
    associated with this block; fall back to the caller-provided string
    for empty blocks (e.g. trailing scripts that produced no output).
    """
    return StatementResult(
        success=block.success,
        statement=block.statement or "",
        message=block.message if block.message else ("OK" if block.success else ""),
        column_names=block.column_names,
        rows=block.rows,
        elapsed_ms=elapsed_ms,
        kind=block.kind,  # type: ignore[arg-type]
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
    return OpenDatabaseResponse(
        success=True,
        db_path=str(p),
        is_new=is_new,
        message="Database opened" if not is_new else "Database created",
    )


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
    engine = _engine_or_404()
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
    engine = _engine_or_404()
    statement = req.statement
    if not statement.strip():
        raise HTTPException(status_code=400, detail="Empty statement")
    t0 = time.monotonic()
    try:
        run = await engine.execute(statement)
    except EngineError as e:
        raise HTTPException(status_code=500, detail=str(e)) from e
    total_ms = int((time.monotonic() - t0) * 1000)
    per_stmt_ms = (total_ms // max(1, len(run.blocks))) if run.blocks else total_ms
    results = [_to_statement_result(blk, per_stmt_ms) for blk in run.blocks]
    overall_success = all(r.success for r in results) and run.success
    return ExecuteResponse(
        success=overall_success,
        results=results,
        db_path=SESSION.db_path or "",
        total_elapsed_ms=total_ms,
    )
