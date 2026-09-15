"""C++ engine wrapper for the SQL-Compiler web UI.

The C++ binary (`sqlcompiler.exe`) is a CLI REPL.  In batch mode (`-f
<script.sql>`) it reads a SQL script, executes each statement, prints
tabular results, and exits.  This module:

1. locates the binary (env override → build/Debug/sqlcompiler.exe → PATH);
2. writes the incoming SQL into a temp file, invokes the binary, and
   captures stdout/stderr;
3. parses the tabular output back into structured `StatementResult`
   objects;
4. exposes a small async surface so FastAPI can run it via
   `asyncio.to_thread` (the binary is synchronous).

Output format we parse (one block per SELECT/SHOW/etc.):

    <col1> | <col2> | ...         ← column header
    ---+---+...                   ← separator
    <v1>  | <v2>  | ...           ← data row(s)
    (N rows)                      ← terminator
    <bare message for DDL/DML>    ← optional "OK" or row count message
    [script] <path>: ran N statement(s)   ← final summary line

DDL/DML/TXN results do not have a column header; instead they print
either the message returned by the engine ("OK", "Inserted 1 row", …)
or nothing.  We detect this by the absence of the "---" separator
line.

Threading note: the C++ engine is single-threaded; we never invoke it
in parallel for the same database.  The frontend is also single-user.
"""

from __future__ import annotations

import asyncio
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# ── Engine discovery ────────────────────────────────────────────────────────


def find_engine_binary() -> Path | None:
    """Resolve the path to `sqlcompiler.exe`.

    Lookup order:
        1. `SQLCOMPILER_BIN` environment variable (absolute path).
        2. `<repo>/build/Debug/sqlcompiler.exe` — the CMake Debug
           default on Windows.
        3. `<repo>/build/Release/sqlcompiler.exe` — Release variant.
        4. `sqlcompiler` / `sqlcompiler.exe` on PATH.
    """
    # 1) explicit env override
    env = os.environ.get("SQLCOMPILER_BIN")
    if env:
        p = Path(env)
        if p.exists():
            return p

    # 2/3) walk up from this file to find the repo root
    #    webui/backend/engine.py  → ../../..  = repo root
    here = Path(__file__).resolve()
    for ancestor in [here.parent, *here.parents]:
        if (ancestor / "CMakeLists.txt").exists():
            for variant in ("Debug", "Release", ""):
                candidate = ancestor / "build" / variant / "sqlcompiler.exe"
                if candidate.exists():
                    return candidate
                candidate = ancestor / "build" / variant / "sqlcompiler"
                if candidate.exists():
                    return candidate
            break

    # 4) PATH lookup
    which = shutil.which("sqlcompiler") or shutil.which("sqlcompiler.exe")
    if which:
        return Path(which)
    return None


# ── Output block parser ────────────────────────────────────────────────────


@dataclass
class ParsedBlock:
    """One parsed output block from the engine."""

    success: bool = True
    message: str = ""  # engine-level message (e.g. "OK", "(3 rows)")
    column_names: list[str] = field(default_factory=list)
    rows: list[list[str]] = field(default_factory=list)
    kind: str = "other"  # select | ddl | dml | txn | other | error
    raw: str = ""  # original block text (for debugging / display)
    statement: str = ""  # the SQL statement that produced this block


def _classify_kind(statement: str, block: ParsedBlock) -> None:
    """Best-effort classification for the UI banner color."""
    s = statement.strip().lstrip("(").lstrip().lower()
    if s.startswith("select") or s.startswith("with") or s.startswith("show") or s.startswith("explain"):
        block.kind = "select"
    elif s.startswith(("create", "drop", "alter", "truncate", "rename", "comment")):
        block.kind = "ddl"
    elif s.startswith(("insert", "update", "delete", "merge", "replace", "upsert")):
        block.kind = "dml"
    elif s.startswith(("begin", "commit", "rollback", "savepoint", "release")):
        block.kind = "txn"
    elif not block.success:
        block.kind = "error"
    else:
        block.kind = "other"


def _strip_cell_padding(cell: str) -> str:
    """The engine pads each cell to its display column width.

    Cells are split on ` | ` so they keep at most one leading + one
    trailing space from the separator.  We strip both ends; for the
    textual data this engine produces, no cell ever has meaningful
    leading or trailing whitespace.
    """
    return cell.strip()


def _parse_one_block(statement: str, raw: str, stderr: str = "") -> ParsedBlock:
    """Parse a single textual output block into a `ParsedBlock`.

    Layout (SELECT / SHOW):
        <col headers joined by " | ">
        ---+---+---
        <data rows joined by " | ">
        (N rows)
    Layout (DDL / DML / TXN):
        <optional one-line message; empty if engine prints nothing>

    The C++ engine writes lexical / syntax / semantic / runtime errors
    to *stderr*; successful runs write only to stdout.  We inspect both
    streams so a single bad statement in a multi-statement script is
    reflected in its block.
    """
    block = ParsedBlock(raw=raw)

    # ── Detect error: the C++ engine writes "Error: …" to stderr ─────
    stderr_clean = (stderr or "").strip()
    if stderr_clean:
        # strip a leading newline some platforms add
        for line in stderr_clean.splitlines():
            stripped = line.lstrip()
            if stripped.startswith("Error:") or stripped.startswith("error:"):
                block.success = False
                block.message = stripped
                block.kind = "error"
                return block
        # also check stdout (parser fallback in case engine mixes them)
    stdout_lines = [ln for ln in raw.splitlines() if ln.strip() != ""]
    if not stdout_lines:
        if stderr_clean:
            block.success = False
            block.message = stderr_clean
            block.kind = "error"
            return block
        _classify_kind(statement, block)
        return block

    # Detect error in stdout as a fallback
    err_idx = next(
        (i for i, ln in enumerate(stdout_lines) if ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")), None
    )
    if err_idx is not None:
        block.success = False
        block.message = " ".join(ln.strip() for ln in stdout_lines[err_idx:])
        block.kind = "error"
        return block

    # ── Find the separator line (---+---) anywhere in the block
    sep_idx = next((i for i, ln in enumerate(stdout_lines) if re.fullmatch(r"[\s\-+|]+", ln) and "-" in ln), None)
    if sep_idx is not None and sep_idx >= 1:
        header = stdout_lines[sep_idx - 1]
        block.column_names = [_strip_cell_padding(c) for c in header.split("|")]
        data_lines = stdout_lines[sep_idx + 1 :]
        # last line is "(N rows)" — peel it off as the message
        if data_lines and re.fullmatch(r"\(\d+ rows?\)", data_lines[-1].strip()):
            block.message = data_lines[-1].strip()
            data_lines = data_lines[:-1]
        for dl in data_lines:
            cells = [_strip_cell_padding(c) for c in dl.split("|")]
            # pad/truncate to header length
            if len(cells) < len(block.column_names):
                cells = cells + [""] * (len(block.column_names) - len(cells))
            elif len(cells) > len(block.column_names):
                cells = cells[: len(block.column_names)]
            block.rows.append(cells)
    else:
        # DDL/DML/TXN: take the first non-empty line as the engine message
        block.message = stdout_lines[0].strip()

    _classify_kind(statement, block)
    return block


# ── Splitting the combined stdout into per-statement blocks ────────────────


def _split_blocks(stdout: str, statements: list[str], stderr: str = "") -> list[ParsedBlock]:
    """Split the engine's combined stdout into one block per statement.

    The engine prints blocks back-to-back without an explicit delimiter.
    We use the *order* of statements and the heuristic that:

    - each SELECT/SHOW block contains a `---` separator line;
    - DDL/DML/TXN blocks are a single short line ("OK" or "Inserted N row(s)").
    - the final line is always `[script] <path>: ran N statement(s)`,
      which we drop.

    `stderr` is forwarded to every block so that a C++ engine error
    (which always goes to stderr) shows up on the right statement even
    when stdout is silent.

    Each returned `ParsedBlock` carries the per-statement text in
    `block.statement` so the API layer can echo it back to the UI
    without ambiguity.
    """
    # strip the trailing [script] ... summary
    cleaned = re.sub(r"\n\[script\][^\n]*\s*$", "", stdout, flags=re.MULTILINE)
    blocks: list[ParsedBlock] = []
    lines = cleaned.splitlines()
    i = 0
    n = len(lines)

    while i < n and len(blocks) < len(statements):
        # skip blank lines between blocks
        while i < n and lines[i].strip() == "":
            i += 1
        if i >= n:
            break

        # does this block start with a header followed by a `---` line?
        if i + 1 < n and re.fullmatch(r"[\s\-+|]+", lines[i + 1]) and "-" in lines[i + 1]:
            # SELECT/SHOW block: header + sep + 0..N data rows + optional "(N rows)"
            j = i + 2
            while j < n:
                ln = lines[j].strip()
                if ln == "" or ln.startswith("[script]"):
                    break
                if re.fullmatch(r"\(\d+ rows?\)", ln):
                    j += 1
                    break
                # if this line itself looks like a header for the next block
                if j + 1 < n and re.fullmatch(r"[\s\-+|]+", lines[j + 1]) and "-" in lines[j + 1]:
                    break
                j += 1
            block_text = "\n".join(lines[i:j])
        else:
            # DDL/DML/TXN block: a single line of output.  We deliberately
            # do NOT consume further lines here — consecutive DDL/DML
            # statements each produce exactly one line ("OK" etc.) and a
            # greedy loop would lump them together.
            j = i + 1
            block_text = lines[i]

        stmt = statements[len(blocks)]
        blk = _parse_one_block(stmt, block_text, stderr)
        blk.statement = stmt
        blocks.append(blk)
        i = j

    # any leftover stdout goes into a final synthetic block so the user sees it
    if i < n and len(blocks) < len(statements):
        leftover = "\n".join(lines[i:]).strip()
        if leftover:
            stmt = statements[len(blocks)]
            blk = _parse_one_block(stmt, leftover, stderr)
            blk.statement = stmt
            blocks.append(blk)
    # pad with empty blocks if we ended up with fewer blocks than statements.
    # When stderr carries an "Error:" line, every padded statement is
    # marked as a failure so the UI surfaces the engine's diagnostic
    # instead of silently showing "OK" for an unparseable block.
    has_stderr_error = bool(
        stderr
        and any(
            ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")
            for ln in stderr.splitlines()
        )
    )
    while len(blocks) < len(statements):
        pad = ParsedBlock(success=not has_stderr_error, kind="other",
                          statement=statements[len(blocks)])
        if has_stderr_error:
            pad.message = stderr.strip().splitlines()[0]
            pad.kind = "error"
        blocks.append(pad)
    return blocks


# ── Engine executor ────────────────────────────────────────────────────────


@dataclass
class EngineRunResult:
    success: bool
    blocks: list[ParsedBlock]
    stderr: str = ""
    elapsed_ms: int = 0
    returncode: int = 0


class EngineError(RuntimeError):
    """Raised when the engine itself cannot be invoked (binary missing, etc.)."""


class SqlEngine:
    """Thin async-friendly wrapper around the compiled `sqlcompiler` binary.

    Not thread-safe across multiple database files; one instance per
    session (i.e. one per HTTP request when the user switches DB).
    """

    def __init__(self, binary: Path, db_path: Path) -> None:
        self.binary = binary
        self.db_path = db_path
        # ensure parent directory exists (engine will throw if it doesn't)
        self.db_path.parent.mkdir(parents=True, exist_ok=True)

    # ── one-shot script run (used for DDL/DML/SELECT batches) ────────────
    async def run_script(self, sql: str) -> EngineRunResult:
        """Write `sql` to a temp file and run the engine in batch mode."""
        return await asyncio.to_thread(self._run_script_sync, sql)

    def _run_script_sync(self, sql: str) -> EngineRunResult:
        t0 = time.monotonic()
        # Normalize the script so that each statement is on its own line.
        # The C++ binary's `RunScriptFile` flushes the accumulated buffer
        # only when `HasCompleteStatement` returns true, but it tracks
        # `;` only on the *current* line — a multi-statement script
        # written on a single line would only have the first statement
        # dispatched, with the remainder dropped into a final
        # "flush on EOF" call (which then fails to parse as a single
        # SQL statement).  We split on `;` here, append the terminator
        # back, and put a newline between statements.
        stmts = self._split_statements(sql)
        script_text = "\n".join(s.rstrip(";").strip() + ";" for s in stmts) + "\n"
        with tempfile.NamedTemporaryFile(
            mode="w",
            suffix=".sql",
            delete=False,
            encoding="utf-8",
            dir=str(tempfile.gettempdir()),
        ) as f:
            f.write(script_text)
            tmp_path = f.name
        try:
            proc = subprocess.run(
                [str(self.binary), str(self.db_path), "-f", tmp_path],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=60,  # generous bound; CLI tests run in <2s
            )
        except subprocess.TimeoutExpired as e:
            return EngineRunResult(
                success=False,
                blocks=[],
                stderr=f"engine timeout after {e.timeout}s",
                elapsed_ms=int((time.monotonic() - t0) * 1000),
                returncode=-1,
            )
        except FileNotFoundError as e:
            raise EngineError(f"engine binary not found: {e}") from e
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

        # split script into individual statements for per-statement parsing
        statements = self._split_statements(sql)
        blocks = _split_blocks(proc.stdout or "", statements, proc.stderr or "")
        return EngineRunResult(
            success=proc.returncode == 0,
            blocks=blocks,
            stderr=proc.stderr or "",
            elapsed_ms=int((time.monotonic() - t0) * 1000),
            returncode=proc.returncode,
        )

    # ── public: execute a single statement (auto-terminate with `;`) ─────
    async def execute(self, statement: str) -> EngineRunResult:
        s = statement.strip()
        if not s:
            return EngineRunResult(success=True, blocks=[])
        # engine expects `;` terminator
        if not s.endswith(";"):
            s = s + ";"
        return await self.run_script(s)

    # ── debug: execute + return JSON visualization data ───────────────────
    async def execute_debug(self, statement: str) -> tuple[EngineRunResult, dict | None]:
        """Run a statement with --debug-output and parse the JSON debug block.

        Returns (EngineRunResult, debug_dict).  debug_dict is None if parsing
        failed or the engine crashed.
        """
        s = statement.strip()
        if not s:
            return EngineRunResult(success=True, blocks=[]), None
        if not s.endswith(";"):
            s = s + ";"

        # build script: statement + newline to flush
        script_text = s + "\n"
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".sql", delete=False, encoding="utf-8", dir=tempfile.gettempdir()
        ) as f:
            f.write(script_text)
            tmp_path = f.name
        try:
            proc = subprocess.run(
                [str(self.binary), str(self.db_path), "-f", tmp_path, "--debug-output"],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
            )
        except subprocess.TimeoutExpired:
            return EngineRunResult(success=False, blocks=[], stderr="engine timeout", returncode=-1), None
        except FileNotFoundError as e:
            raise EngineError(f"engine binary not found: {e}") from e
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

        # Strip the debug JSON envelope from stdout BEFORE we hand it to
        # `_split_blocks`, otherwise a `{...}` line and the surrounding
        # bracket markers get mis-classified as a DML "OK" line for the
        # previous statement.  The engine writes the envelope *after*
        # the statement output, so a single non-greedy capture here is
        # sufficient and leaves the regular block layout untouched.
        raw = proc.stdout or ""
        m = re.search(r"\[DEBUG_JSON_START\]\s*(.*?)\s*\[DEBUG_JSON_END\]", raw, re.DOTALL)
        debug_data = None
        if m:
            import json
            try:
                debug_data = json.loads(m.group(1))
            except json.JSONDecodeError:
                pass
            # Replace the envelope with a blank placeholder so the
            # block splitter sees a clean blank line between this
            # statement and the next one (defensive even when there is
            # only one statement in the script).
            raw = raw[: m.start()] + raw[m.end():]

        statements = self._split_statements(statement)
        blocks = _split_blocks(raw, statements, proc.stderr or "")
        # The C++ engine returns exit code 0 even for semantic errors
        # (e.g. table not found).  Detect those via the stderr
        # "Error:" prefix and propagate them as overall failures so
        # the API layer can report `success=false` instead of silently
        # claiming the compile succeeded.
        stderr_clean = proc.stderr or ""
        has_engine_error = any(
            ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")
            for ln in stderr_clean.splitlines()
        )
        overall_success = proc.returncode == 0 and not has_engine_error
        result = EngineRunResult(
            success=overall_success,
            blocks=blocks,
            stderr=stderr_clean,
            returncode=proc.returncode,
        )
        return result, debug_data

    # ── helper: split a script into per-statement strings ────────────────
    @staticmethod
    def _split_statements(sql: str) -> list[str]:
        """Split SQL on `;` while respecting string literals and comments.

        Mirrors the C++ REPL's `HasCompleteStatement` rules at a coarse
        level (we don't need the full BEGIN/END body tracking for the
        web UI because each ExecuteRequest is one statement).
        """
        out: list[str] = []
        buf: list[str] = []
        in_string = False
        i = 0
        n = len(sql)
        while i < n:
            c = sql[i]
            if in_string:
                buf.append(c)
                if c == "'":
                    if i + 1 < n and sql[i + 1] == "'":
                        buf.append("'")
                        i += 2
                        continue
                    in_string = False
                i += 1
                continue
            if c == "'":
                in_string = True
                buf.append(c)
                i += 1
                continue
            if c == "-" and i + 1 < n and sql[i + 1] == "-":
                # line comment
                while i < n and sql[i] != "\n":
                    buf.append(sql[i])
                    i += 1
                continue
            if c == ";":
                stmt = "".join(buf).strip()
                if stmt:
                    out.append(stmt)
                buf = []
                i += 1
                continue
            buf.append(c)
            i += 1
        tail = "".join(buf).strip()
        if tail:
            out.append(tail)
        return out


# ── Schema helpers (run on-demand SQL) ─────────────────────────────────────


async def list_tables(engine: SqlEngine) -> tuple[bool, list[dict[str, Any]], str]:
    """Return `[{name, column_count}]` via `SHOW TABLES` + per-table introspection."""
    # SHOW TABLES returns a single column `name`
    res = await engine.execute("SHOW TABLES")
    if not res.success or not res.blocks:
        return False, [], res.stderr or "SHOW TABLES failed"
    block = res.blocks[0]
    if not block.column_names or "name" not in [c.lower() for c in block.column_names]:
        return True, [], ""
    name_idx = next(i for i, c in enumerate(block.column_names) if c.lower() == "name")
    names = [row[name_idx] for row in block.rows]
    tables: list[dict[str, Any]] = []
    for n in names:
        # `SHOW COLUMNS FROM <name>` gives column count
        col_res = await engine.execute(f"SHOW COLUMNS FROM {n}")
        col_count = 0
        if col_res.success and col_res.blocks and col_res.blocks[0].rows:
            col_count = len(col_res.blocks[0].rows)
        tables.append({"name": n, "column_count": col_count, "row_count": None})
    return True, tables, ""


async def describe_table(engine: SqlEngine, name: str) -> tuple[bool, list[dict[str, Any]], str, str]:
    """Return (success, columns, create_sql, message) for a table.

    `success` reflects whether the *SQL* succeeded (i.e. the table
    actually exists).  This is the per-`ParsedBlock.success` flag set
    by the engine wrapper, NOT the `EngineRunResult.success` flag
    which only says "the C++ binary didn't crash".
    """
    # 1) SHOW COLUMNS FROM <name>
    col_res = await engine.execute(f"SHOW COLUMNS FROM {name}")
    if not col_res.blocks:
        return False, [], "", "SHOW COLUMNS returned no output"
    block = col_res.blocks[0]
    if not block.success:
        # The C++ engine reported a semantic / syntax error (typically
        # "table not found").  Bubble the message up so the UI can show
        # it instead of rendering an empty schema.
        return False, [], "", block.message or "SHOW COLUMNS failed"
    if not block.column_names:
        return True, [], "", ""
    # columns: name | type | nullable | key | default | extra  (engine-specific)
    headers_lower = [c.lower() for c in block.column_names]
    name_i = headers_lower.index("name") if "name" in headers_lower else 0
    type_i = headers_lower.index("type") if "type" in headers_lower else 1
    try:
        null_i = headers_lower.index("null") if "null" in headers_lower else headers_lower.index("nullable")
    except ValueError:
        null_i = -1
    pk_i = headers_lower.index("key") if "key" in headers_lower else -1
    default_i = headers_lower.index("default") if "default" in headers_lower else -1

    columns: list[dict[str, Any]] = []
    for row in block.rows:
        col_name = row[name_i] if name_i < len(row) else ""
        col_type = row[type_i] if type_i < len(row) else ""
        nullable = True
        if null_i >= 0 and null_i < len(row):
            v = row[null_i].strip().lower()
            nullable = v in ("yes", "y", "true", "1", "")
        is_pk = False
        if pk_i >= 0 and pk_i < len(row):
            v = row[pk_i].strip().upper()
            is_pk = "PRI" in v or "PK" in v
        default = row[default_i] if 0 <= default_i < len(row) else None
        columns.append(
            {
                "name": col_name,
                "type": col_type,
                "nullable": nullable,
                "pk": is_pk,
                "default": default,
            }
        )

    # 2) SHOW CREATE TABLE <name> for the canonical SQL
    cr_res = await engine.execute(f"SHOW CREATE TABLE {name}")
    create_sql = ""
    if cr_res.blocks:
        for blk in cr_res.blocks:
            if not blk.success:
                continue
            for row in blk.rows:
                if row:
                    create_sql = row[-1]  # last column tends to hold the SQL
                    break
            if create_sql:
                break
        if not create_sql and cr_res.blocks[0].message:
            create_sql = cr_res.blocks[0].message

    return True, columns, create_sql, ""


async def list_db_files(directory: Path) -> list[dict[str, Any]]:
    """Scan a directory for `.db` files (non-recursive, sorted by mtime desc).

    Implemented as a regular function and offloaded to a thread so
    `iterdir()` / `stat()` don't block the event loop.  The async
    wrapper is preserved for API symmetry with the other schema
    helpers, but it never suspends on real I/O.
    """
    return await asyncio.to_thread(_list_db_files_sync, directory)


def _list_db_files_sync(directory: Path) -> list[dict[str, Any]]:
    if not directory.exists() or not directory.is_dir():
        return []
    items: list[tuple[float, dict[str, Any]]] = []
    for p in directory.iterdir():
        if p.is_file() and p.suffix.lower() == ".db":
            try:
                stat = p.stat()
                items.append((stat.st_mtime, {"name": p.name, "path": str(p), "size_bytes": stat.st_size}))
            except OSError:
                continue
    items.sort(key=lambda x: x[0], reverse=True)
    return [info for _, info in items]
