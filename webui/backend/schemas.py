"""Pydantic models for the SQL-Compiler web UI API.

Design notes:
- Every endpoint accepts/returns a typed payload so the static frontend
  can rely on a stable JSON shape.
- `success` always reflects whether the C++ engine produced a usable
  result; transport-level errors (4xx/5xx) are reserved for protocol
  problems (missing file, bad path, etc.).
"""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, Field

# ── Database binding ────────────────────────────────────────────────────────


class OpenDatabaseRequest(BaseModel):
    """Bind the current session to a `.db` file.

    The file is created on disk if it does not exist (the C++ engine
    bootstraps it automatically).  The `db_path` is echoed back so the
    frontend can show a confirmation banner.
    """

    db_path: str = Field(..., min_length=1, description="Absolute path to the .db file")


class OpenDatabaseResponse(BaseModel):
    success: bool
    db_path: str
    is_new: bool
    message: str = ""


class CloseDatabaseResponse(BaseModel):
    """Returned by POST /api/db/close.

    `was_open` tells the caller whether there was actually a session
    (so the UI can avoid noisy "no database" warnings on a no-op).
    """

    success: bool = True
    was_open: bool = False
    previous_db_path: str = ""
    message: str = ""


class UnlinkFileResponse(BaseModel):
    """Returned by POST /api/db/unlink?path=…

    Reports whether the requested path was deleted and (in case the
    path was the open database) whether the session was cleared.
    `deleted` is False when the file did not exist; we treat that
    as success so a "reset on an unknown file" path is idempotent.

    `companions_deleted` lists the side-car files that were removed
    together with the requested path (the engine's `<db>.wal`, plus
    `<db>.shm` when present).  A `.db` delete always takes its WAL
    with it: leaving it behind lets the engine's ARIES recovery
    replay the old log into the next database the same path creates.
    """

    success: bool = True
    deleted: bool = False
    cleared_session: bool = False
    path: str = ""
    companions_deleted: list[str] = []
    message: str = ""


class ListDatabasesRequest(BaseModel):
    """Optional: scan a directory for `.db` candidates.

    Used by the file picker to populate a quick selector.  `directory`
    may be a folder or a parent of a `.db` file.
    """

    directory: str = Field(..., min_length=1)


class DatabaseFileInfo(BaseModel):
    name: str
    path: str
    size_bytes: int


class ListDatabasesResponse(BaseModel):
    items: list[DatabaseFileInfo]


class ValidatePathRequest(BaseModel):
    """Probe a path: does the parent dir exist? does the .db file exist?

    Used by the file picker to give the user real-time feedback as they
    type a path.  Three terminal states:

    - `parent_missing`  — the parent directory does not exist (typo)
    - `directory`       — the path itself is an existing directory
    - `new_file`        — the path is a .db file that does NOT exist yet
                          (a new database will be created on open)
    - `existing_file`   — the path is an existing .db file (will be opened)
    - `wrong_type`      — the path exists but is neither a .db file nor
                          a directory (e.g. user pointed at a folder/file
                          of an unexpected shape)
    """

    path: str = Field(..., min_length=1)


class ValidatePathResponse(BaseModel):
    status: Literal["parent_missing", "directory", "new_file", "existing_file", "wrong_type"]
    parent: str = ""
    message: str = ""


# ── Server-side filesystem browser ────────────────────────────────────────


class BrowseEntry(BaseModel):
    """One entry in a directory listing returned by /api/db/browse."""

    name: str
    path: str
    # kind discriminates directories from .db files from other files.
    # We only return directories + .db files; other files are filtered
    # out so the browser stays focused.
    kind: Literal["dir", "db"]


class BrowseRequest(BaseModel):
    path: str = Field(default="", description="Directory to list. Empty → user's home / system default.")


class BrowseResponse(BaseModel):
    """A directory listing with breadcrumbs.

    `parent` is the path of the parent directory (empty when at a
    filesystem root).  `entries` is sorted: directories first (alpha),
    then .db files (by mtime desc).
    """

    path: str
    parent: str = ""
    entries: list[BrowseEntry] = Field(default_factory=list)
    # system roots for the "Home" / quick-jump button (e.g. C:\ on Windows)
    roots: list[str] = Field(default_factory=list)
    error: str = ""


# ── Schema introspection ────────────────────────────────────────────────────


class TableSummary(BaseModel):
    name: str
    column_count: int = 0
    row_count: int | None = None  # may be expensive; None if not fetched


class ListTablesResponse(BaseModel):
    success: bool
    tables: list[TableSummary]
    message: str = ""


class ColumnInfo(BaseModel):
    name: str
    type: str
    nullable: bool = True
    pk: bool = False
    default: str | None = None


class TableSchemaResponse(BaseModel):
    success: bool
    table: str
    columns: list[ColumnInfo]
    create_sql: str = ""
    message: str = ""


# ── Query execution ─────────────────────────────────────────────────────────


class ExecuteRequest(BaseModel):
    """Run a SQL statement or a multi-statement script.

    `statement` may be DDL / DML / SELECT / SHOW / EXPLAIN, or a
    `;`-separated script.  The backend splits the script into statements,
    executes them in order, and returns one `StatementResult` per statement
    (an "error isolation" design — a failing statement does not corrupt the
    results of its neighbours).

    `on_error` selects the isolation strategy:
      - `"abort"`    (default) stop at the first failing statement;
      - `"continue"` keep executing the remaining statements, tagging each
                     independent failure.
    `transaction` wraps the whole batch in an implicit BEGIN … COMMIT in a
    single engine process, giving atomic commit when every statement
    succeeds.
    """

    statement: str = Field(..., min_length=1)
    # If true, run the statement even if it has no `;` terminator
    force: bool = False
    on_error: Literal["abort", "continue"] = "abort"
    transaction: bool = False


class StatementResult(BaseModel):
    """One row in the response — corresponds to one statement execution."""

    success: bool
    statement: str
    message: str = ""
    error: str = ""
    column_names: list[str] = Field(default_factory=list)
    rows: list[list[str]] = Field(default_factory=list)
    elapsed_ms: int = 0
    # For convenience: detected kind (DML/DDL/SELECT) so the UI can
    # pick the right success banner without re-parsing.
    kind: Literal["select", "ddl", "dml", "txn", "other", "error"] = "other"
    # Per-statement debug JSON (tokens / ast / plan / storage stats) when
    # the caller asked for visualization.  None when the engine did not
    # emit an envelope for this statement (e.g. the C++ binary only writes
    # envelopes for statements that compiled far enough to enter the
    # optimisation / planning pipeline).  Each block carries its own copy
    # so the UI can re-render any statement's visualization on click.
    debug: dict | None = None


class ExecuteResponse(BaseModel):
    success: bool
    results: list[StatementResult]
    db_path: str = ""
    total_elapsed_ms: int = 0
    # Number of statements the backend split from the input script.
    statement_count: int = 0
    # True when `on_error="abort"` stopped execution at the first failure
    # (so the UI can warn "remaining statements were not executed").
    aborted: bool = False


# ── Misc ────────────────────────────────────────────────────────────────────


class HealthResponse(BaseModel):
    status: Literal["ok"]
    version: str
    engine_path: str | None = None
    engine_exists: bool = False
    db_path: str | None = None
