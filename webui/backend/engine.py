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
    debug: dict | None = None  # per-statement debug JSON (tokens/ast/plan) when available


def _classify_kind(statement: str, block: ParsedBlock) -> None:
    """Best-effort classification for the UI banner color.

    The statement text may start with `--` / `/* … */` comments (the
    splitter preserves leading comments so the engine still sees them
    in the temp script).  We strip those before matching so the UI
    banner reflects the **operation**, not the comment prefix.
    """
    s = statement.strip().lstrip("(").lstrip()
    # strip leading line comments
    while s.startswith("--"):
        nl = s.find("\n")
        if nl == -1:
            s = ""
            break
        s = s[nl + 1 :].lstrip()
    # strip a leading block comment
    while s.startswith("/*"):
        end = s.find("*/")
        if end == -1:
            s = ""
            break
        s = s[end + 2 :].lstrip()
    s_lower = s.lower()
    if s_lower.startswith("select") or s_lower.startswith("with") or s_lower.startswith("show") or s_lower.startswith("explain"):
        block.kind = "select"
    elif s_lower.startswith(("create", "drop", "alter", "truncate", "rename", "comment")):
        block.kind = "ddl"
    elif s_lower.startswith(("insert", "update", "delete", "merge", "replace", "upsert")):
        block.kind = "dml"
    elif s_lower.startswith(("begin", "commit", "rollback", "savepoint", "release")):
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


def _norm_statement(s: str) -> str:
    """Normalize a statement's terminator to exactly one trailing `;`.

    The C++ `RunScriptFile` only dispatches on a `;` it sees (flushed at
    the following newline), so a missing terminator leaves a statement
    buffered until EOF.  We preserve the statement's *interior* verbatim
    (including line breaks and string literals) and only fix up the tail:
    strip trailing whitespace/semicolons, then re-append a single `;`.
    """
    t = s.strip()
    return t.rstrip(";").rstrip() + ";"


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
    stdout_lines = [ln for ln in raw.splitlines() if ln.strip() != ""]
    stderr_clean = (stderr or "").strip()

    # Detect error in stdout first — if our block's stdout itself is
    # an engine error (some builds print to stdout as a fallback),
    # trust it.
    err_idx = next(
        (i for i, ln in enumerate(stdout_lines)
         if ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")),
        None,
    )
    if err_idx is not None:
        block.success = False
        block.message = " ".join(ln.strip() for ln in stdout_lines[err_idx:])
        block.kind = "error"
        return block

    # No stdout → the engine signalled failure via stderr (the only
    # case where stderr is the sole error source).  This is the ONLY
    # place we trust stderr: when stdout is empty.  In multi-file
    # mode stderr is shared across all files, so a downstream block
    # that has its own stdout must NOT be marked as failing just
    # because some upstream file errored.
    if not stdout_lines:
        if stderr_clean:
            for line in stderr_clean.splitlines():
                stripped = line.lstrip()
                if stripped.startswith("Error:") or stripped.startswith("error:"):
                    block.success = False
                    block.message = stripped
                    block.kind = "error"
                    return block
        _classify_kind(statement, block)
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
    # strip [script] summary lines (the engine appends one after each
    # `-f` file's content; it can be either trailing or the only stdout
    # line when the file's statement failed and produced no rows).
    cleaned = re.sub(r"^\[script\][^\n]*\n?", "", stdout, flags=re.MULTILINE)
    cleaned = re.sub(r"\n\[script\][^\n]*\s*$", "", cleaned, flags=re.MULTILINE)
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

    async def run_script_text(self, script_text: str, statements: list[str]) -> EngineRunResult:
        """Run a pre-built script against a known statement list.

        Used by the transaction wrapper where the synthesized `script_text`
        (e.g. `BEGIN; … COMMIT;`) does not equal the statement list that
        should be *reported* back.  `statements` drives block-alignment.
        """
        return await asyncio.to_thread(self._invoke_engine, script_text, statements)

    def _run_script_sync(self, sql: str) -> EngineRunResult:
        """Normalize one script and run it in a single subprocess."""
        stmts = self._split_statements(sql)
        script_text = self._build_script_text(stmts)
        return self._invoke_engine(script_text, stmts)

    @staticmethod
    def _build_script_text(stmts: list[str]) -> str:
        """Put each statement on its own line, terminated with `;`.

        The C++ binary's `RunScriptFile` only dispatches a statement once
        `HasCompleteStatement` sees a `;`; it also line-buffers.  Splitting
        on `;` here, then re-terminating with `;` + newline guarantees every
        statement is flushed and executed in order.
        """
        return "".join(_norm_statement(s) + "\n" for s in stmts)

    def _invoke_engine(self, script_text: str, statements: list[str]) -> EngineRunResult:
        """Run `script_text` in one subprocess and split output by statement."""
        t0 = time.monotonic()
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

        # Detect engine-level "Error:" lines (the C++ binary exits 0 even
        # for semantic errors like "table not found", so a non-zero exit
        # code alone is not a reliable failure signal).
        stderr_clean = proc.stderr or ""
        has_engine_error = any(
            ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")
            for ln in stderr_clean.splitlines()
        )
        blocks = _split_blocks(proc.stdout or "", statements, stderr_clean)
        return EngineRunResult(
            success=(proc.returncode == 0) and not has_engine_error,
            blocks=blocks,
            stderr=stderr_clean,
            elapsed_ms=int((time.monotonic() - t0) * 1000),
            returncode=proc.returncode,
        )

    # ── public entry point for the query executor ────────────────────────
    async def execute(
        self,
        statement: str,
        *,
        on_error: str = "abort",   # "abort" | "continue"
        transaction: bool = False,
    ) -> EngineRunResult:
        """Execute a SQL script, returning one result block per statement.

        Execution model (chosen for robustness):

        - **Single statement** → a single engine subprocess call.
        - **Multi-statement with explicit BEGIN/COMMIT/ROLLBACK** → a single
          subprocess so the user-authored transaction stays in one session
          (the C++ engine commits/rolls back in-process only).
        - **Multi-statement autocommit** → each statement runs in its *own*
          subprocess.  This is the key isolation fix: when statement *k*
          fails, the engine writes its error to stderr and keeps running,
          so the combined-stdout splitter could never attribute the
          following statements' output correctly.  Per-statement execution
          gives exact stdout/stderr correlation for every statement.

        `on_error` selects the error-isolation strategy:
          - `"abort"`    (default) stop at the first failing statement;
          - `"continue"` keep executing the remaining statements, isolating
                         each failure so a bad statement doesn't corrupt
                         the others.

        `transaction` wraps the whole batch in an implicit
        `BEGIN … COMMIT` (or `ROLLBACK`) in a single subprocess, giving
        atomic commit when every statement succeeds.
        """
        s = statement.strip()
        if not s:
            return EngineRunResult(success=True, blocks=[])
        stmts = [
            x for x in self._split_statements(s)
            if not self._is_empty_or_comment(x) and not self._is_repl_meta(x)
        ]
        if not stmts:
            return EngineRunResult(success=True, blocks=[])

        user_txn = any(self._is_txn_control(x) for x in stmts)

        if transaction and not user_txn:
            # implicit all-or-nothing wrapper
            return await asyncio.to_thread(self._run_transaction_batch_sync, stmts)
        if len(stmts) == 1 or user_txn:
            # single statement, or an explicit user-led transaction that
            # must stay in one engine process
            return await self.run_script(s)

        # multi-statement autocommit → precise per-statement isolation
        return await self._run_each_statement(stmts, on_error)

    async def execute_batch(
        self,
        statement: str,
        *,
        on_error: str = "abort",
        transaction: bool = False,
    ) -> EngineRunResult:
        """Alias for `execute` that makes the batch semantics explicit.

        Used by the executor API so callers don't have to reason about the
        internal routing; all knobs are surfaced on one method.
        """
        return await self.execute(statement, on_error=on_error, transaction=transaction)

    # ── multi-statement autocommit: one subprocess per statement ──────────
    async def _run_each_statement(
        self, stmts: list[str], on_error: str
    ) -> EngineRunResult:
        blocks: list[ParsedBlock] = []
        total_start = time.monotonic()
        for stmt in stmts:
            # Each statement is its own script → its subprocess stdout is
            # exactly that statement's output and its stderr exactly its
            # error.  No cross-statement mis-attribution is possible.
            run = await self.run_script(stmt)
            if run.blocks:
                blk = run.blocks[0]
                blk.statement = stmt
            else:
                fail = run.stderr.strip() != "" or not run.success
                blk = ParsedBlock(
                    success=not fail,
                    kind="error" if fail else "other",
                    statement=stmt,
                    message=run.stderr.strip() or ("OK" if not fail else "engine returned no output"),
                )
            blocks.append(blk)
            if not blk.success and on_error == "abort":
                break
        success = bool(blocks) and all(b.success for b in blocks)
        return EngineRunResult(
            success=success,
            blocks=blocks,
            elapsed_ms=int((time.monotonic() - total_start) * 1000),
        )

    # ── implicit (automatic) transaction wrapper ──────────────────────────
    def _run_transaction_batch_sync(self, stmts: list[str]) -> EngineRunResult:
        """Run `BEGIN; …; COMMIT;` in a single process for atomic commit."""
        script_text = (
            "BEGIN;\n" + "".join(_norm_statement(s) + "\n" for s in stmts) + "COMMIT;\n"
        )
        # Align output against BEGIN + user stmts + COMMIT, then drop the
        # two synthetic boundary blocks so `blocks` mirrors the user's
        # statements only.
        full = ["BEGIN"] + list(stmts) + ["COMMIT"]
        run = self._invoke_engine(script_text, full)
        blocks = run.blocks[1:-1] if len(run.blocks) >= 2 else []
        return EngineRunResult(
            success=run.success and all(b.success for b in blocks),
            blocks=blocks,
            stderr=run.stderr,
            elapsed_ms=run.elapsed_ms,
            returncode=run.returncode,
        )

    # ── debug: execute + return JSON visualization data ───────────────────
    async def execute_debug(self, statement: str) -> tuple[EngineRunResult, dict | None]:
        """Run a SQL script with `--debug-output` and capture per-statement data.

        Returns `(EngineRunResult, focus_debug)`.  The returned
        `EngineRunResult.blocks` contains one `ParsedBlock` per input
        statement; each block carries its own `debug` dict (tokens /
        ast / plan / storage stats) so the UI can render any selected
        statement's visualization.  `focus_debug` is a convenience
        pointer to the **last statement** that produced a debug envelope
        (typically the last SELECT, since only successfully-compiled
        statements emit one).

        Execution strategy (chosen after observing the engine's actual
        output):

        - **Single statement** or **explicit user transaction** →
          `execute_debug_single`: one `-f` file, one envelope.
        - **Multi-statement autocommit** → write one temp file per
          statement and invoke the engine with **multiple `-f` flags** in
          a single subprocess.  This:

          1. Keeps every statement's side effects in the same DB process
             so DDL/DML order is preserved (a CREATE in file 1 is visible
             to an INSERT in file 2 — required for scripts like
             `06_operators.sql` where a CREATE must precede its INSERTs).
          2. Yields **one stdout + one `[DEBUG_JSON_START]…END]` block
             per `-f` file**, so we can map envelopes to statements in
             order.
          3. Gives exact per-statement error isolation: a file whose
             statement fails still emits a debug envelope (with an empty
             plan_json), and its neighbour's output is not corrupted.

        The previous per-statement-subprocess approach was incorrect for
        debug: it (a) lost DDL/DML side effects across statements and
        (b) only retained the *last* statement's debug envelope
        (`focus_debug`), so any SELECT after a CREATE/INSERT had no
        tokens/AST available in the visualization.
        """
        s = statement.strip()
        if not s:
            return EngineRunResult(success=True, blocks=[]), None
        stmts = [
            x for x in self._split_statements(s)
            if not self._is_empty_or_comment(x) and not self._is_repl_meta(x)
        ]
        if not stmts:
            return EngineRunResult(success=True, blocks=[]), None
        if len(stmts) == 1 or any(self._is_txn_control(x) for x in stmts):
            return await self._execute_debug_single(s)

        # multi-statement autocommit → one -f per statement, single subprocess
        return await asyncio.to_thread(self._execute_debug_multistatement_sync, stmts)

    def _execute_debug_multistatement_sync(
        self, stmts: list[str]
    ) -> tuple[EngineRunResult, dict | None]:
        """Run N statements via N `-f` files in one subprocess and parse all.

        Each -f file contains exactly one statement.  The engine writes
        each file's stdout and debug envelope back-to-back; we capture
        them in order and zip with the input statements.
        """
        tmp_paths: list[str] = []
        t0 = time.monotonic()
        try:
            for stmt in stmts:
                # Each statement gets its own temp file so the engine's
                # line-buffered parser sees clean, isolated input.  We
                # re-emit the comment-only block-leading content by
                # writing `stmt` verbatim (it may include leading
                # comment text the splitter preserved).  The C++ lexer
                # happily skips `-- …` and `/* … */` lines.
                norm = _norm_statement(stmt) + "\n"
                with tempfile.NamedTemporaryFile(
                    mode="w", suffix=".sql", delete=False,
                    encoding="utf-8", dir=tempfile.gettempdir(),
                ) as f:
                    f.write(norm)
                    tmp_paths.append(f.name)
            cmd = [str(self.binary), str(self.db_path)]
            for p in tmp_paths:
                cmd.extend(["-f", p])
            cmd.append("--debug-output")
            try:
                proc = subprocess.run(
                    cmd, capture_output=True, text=True,
                    encoding="utf-8", errors="replace", timeout=120,
                )
            except subprocess.TimeoutExpired:
                return (
                    EngineRunResult(
                        success=False, blocks=[],
                        stderr="engine timeout", returncode=-1,
                    ),
                    None,
                )
            except FileNotFoundError as e:
                raise EngineError(f"engine binary not found: {e}") from e
        finally:
            for p in tmp_paths:
                try:
                    os.unlink(p)
                except OSError:
                    pass

        raw = proc.stdout or ""
        stderr_clean = proc.stderr or ""
        return self._assemble_multistatement_debug(stmts, raw, stderr_clean, proc.returncode, t0)

    def _assemble_multistatement_debug(
        self,
        stmts: list[str],
        raw: str,
        stderr_clean: str,
        returncode: int,
        t0: float,
    ) -> tuple[EngineRunResult, dict | None]:
        """Parse per-statement stdout + per-statement debug envelopes.

        The engine layout for N files is: for each file, the file's
        stdout lines, then its `[DEBUG_JSON_START]…[DEBUG_JSON_END]`
        envelope, then optionally a `[script] file: ran N statement(s)`
        trailer.  We split on the envelopes, then for each segment
        strip its own envelope and hand the remainder to
        `_split_blocks([stmt], raw, stderr)`.

        When a file's statement fails (e.g. `SELECT * FROM missing_table`)
        the engine still emits its envelope but the file's stdout is
        empty (the error went to stderr).  We then fabricate a
        failure block carrying the engine's error message.
        """
        import json as _json

        # 1. collect every envelope (in order) and strip them out of `raw`.
        # `finditer` on the start marker gives us positions to slice; we
        # then locate the matching END on each segment.
        starts = [m.start() for m in re.finditer(r"\[DEBUG_JSON_START\]", raw)]
        ends = [m.end() for m in re.finditer(r"\[DEBUG_JSON_END\]", raw)]
        debug_dicts: list[dict | None] = []
        prev_end = 0
        new_raw_parts: list[str] = []
        for i, s_pos in enumerate(starts):
            e_pos = ends[i] if i < len(ends) else len(raw)
            envelope = raw[s_pos:e_pos]
            new_raw_parts.append(raw[prev_end:s_pos])
            prev_end = e_pos
            m = re.search(r"\[DEBUG_JSON_START\]\s*(.*?)\s*\[DEBUG_JSON_END\]", envelope, re.DOTALL)
            if m:
                try:
                    debug_dicts.append(_json.loads(m.group(1)))
                except _json.JSONDecodeError:
                    debug_dicts.append(None)
            else:
                debug_dicts.append(None)
        new_raw_parts.append(raw[prev_end:])
        raw_no_envelope = "".join(new_raw_parts)

        # 2. split the remaining stdout by file-trailer boundaries so
        # each file's stdout is isolated.  The trailer is a line
        # `[script] <path>: ran N statement(s)` that the engine appends
        # after each -f file's content.
        file_segments = re.split(r"^\[script\][^\n]*\n?", raw_no_envelope, flags=re.MULTILINE)

        # 3. zip statements with file segments and debug envelopes.
        #
        # CRITICAL: every empty slot in `file_segments` corresponds to a
        # statement whose `-f` produced no stdout (CREATE / DDL / failed
        # statements fall here).  We must NOT drop those slots — they
        # carry the per-statement "fingerprint" that block ordering
        # depends on.  Previously this routine popped all leading and
        # trailing empty entries, which silently collapsed all failing
        # statements' empty slots into one and associated the FIRST
        # non-empty stdout (a successful SELECT) with the FIRST
        # statement (a failed CREATE).  The fix is to strip ONLY the
        # trailing empty after the last `[script]` trailer (the engine
        # always emits an extra empty segment after the final file),
        # then pad / trim to exactly len(stmts).
        cleaned_segments = list(file_segments)
        # Strip trailing empties that exist after the last `[script]`
        # line — those are post-script artifacts that don't correspond
        # to any input statement.  We POP all consecutive trailing
        # empties (there's typically just one, but engine edge cases
        # can produce several) and never touch leading empties, because
        # a leading empty IS the first statement's stdout slot.
        while cleaned_segments and cleaned_segments[-1].strip() == "":
            cleaned_segments.pop()
        # Defensive: align with statement count.  If fewer segments
        # than statements, pad with empty strings (engine emitted fewer
        # `[script]` trailers than we expect — fall back to a "no stdout"
        # attribution for the missing ones).  If more, trim (very rare;
        # usually means an unexpected blank inside stdout that survived
        # the `[script]` split).
        if len(cleaned_segments) > len(stmts):
            cleaned_segments = cleaned_segments[: len(stmts)]
        elif len(cleaned_segments) < len(stmts):
            while len(cleaned_segments) < len(stmts):
                cleaned_segments.append("")

        # 4. Pair up stderr errors with empty-stdout blocks IN ORDER.
        # The C++ engine writes one stderr line per failed `-f` file (in
        # the same order as the inputs), so we dequeue one error per
        # empty-stdout block.  This is the only reliable way to
        # attribute errors to the right statement when stdout and
        # stderr are interleaved across `-f` invocations.
        err_lines = [
            ln.strip()
            for ln in stderr_clean.splitlines()
            if ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")
        ]
        err_idx = 0

        # 5. build blocks one per input statement
        blocks: list[ParsedBlock] = []
        focus_debug: dict | None = None
        for idx, stmt in enumerate(stmts):
            seg = cleaned_segments[idx]
            dbg = debug_dicts[idx] if idx < len(debug_dicts) else None
            # Pass `stderr_clean` only to `_split_blocks` for blocks with
            # actual stdout — that path catches errors written into the
            # output stream.  For empty-stdout blocks we attribute
            # errors ourselves below (one stderr line per failing `-f`)
            # because `_split_blocks`'s pad branch would otherwise
            # over-attribute the shared stderr to every empty block.
            blk_list = _split_blocks(seg, [stmt], "" if seg.strip() == "" else stderr_clean)
            if blk_list:
                blk = blk_list[0]
                blk.statement = stmt
            else:
                blk = ParsedBlock(success=True, statement=stmt)
            blk.debug = dbg
            # Failed-statement attribution: a successful-looking block
            # whose `-f` produced no stdout is almost certainly the
            # statement the engine just failed on.  Dequeue the next
            # stderr error to surface the real diagnostic.  Statements
            # that already reported failure via their own stdout
            # (`blk.success == False`) are left alone.
            if (
                blk.success
                and seg.strip() == ""
                and err_idx < len(err_lines)
            ):
                blk.success = False
                blk.message = err_lines[err_idx]
                blk.kind = "error"
                err_idx += 1
            blocks.append(blk)
            if dbg is not None:
                focus_debug = dbg

        has_engine_error = bool(
            stderr_clean
            and any(
                ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")
                for ln in stderr_clean.splitlines()
            )
        )
        # `has_engine_error` is a coarse signal: at least one statement
        # in the batch failed.  We DO NOT use it to mark every block as
        # a failure — each block has already been classified by
        # `_split_blocks` based on its own stdout + the same stderr,
        # so its `success` is accurate.  The flag only controls the
        # top-level `EngineRunResult.success` so the API layer can
        # surface "this batch had at least one failure" without
        # overruling per-statement outcomes.
        block_success = all(b.success for b in blocks)
        overall_success = returncode == 0 and not has_engine_error and block_success
        return (
            EngineRunResult(
                success=overall_success,
                blocks=blocks,
                stderr=stderr_clean,
                elapsed_ms=int((time.monotonic() - t0) * 1000),
                returncode=returncode,
            ),
            focus_debug,
        )

    async def _execute_debug_single(self, statement: str) -> tuple[EngineRunResult, dict | None]:
        """Run a multi-statement script with `--debug-output` in one subprocess.

        The engine writes one debug envelope per statement even when
        they all live in the same `-f` file (we confirmed this by
        inspecting the engine's stdout layout: each successful
        statement is followed by its own `[DEBUG_JSON_START]…END]`).
        We extract every envelope and zip it to the corresponding block
        so the UI can render any statement's visualisation.

        Falls back to "single envelope for the last block" when
        exactly one envelope is found (the common single-statement case).
        """
        s = statement.strip()
        if not s:
            return EngineRunResult(success=True, blocks=[]), None

        # build script (preserve the user's original, including
        # `--` / `/* */` comments — the splitter already separated
        # them as part of their adjacent statements).
        if not s.endswith(";"):
            s = s + ";"
        script_text = s + "\n"

        # Determine the statement list the engine actually saw.
        statements = self._split_statements(s)

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".sql", delete=False, encoding="utf-8", dir=tempfile.gettempdir()
        ) as f:
            f.write(script_text)
            tmp_path = f.name
        try:
            proc = subprocess.run(
                [str(self.binary), str(self.db_path), "-f", tmp_path, "--debug-output"],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
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

        # Capture every envelope (in order) and remove them from raw.
        raw = proc.stdout or ""
        import json as _json
        envelope_matches = list(
            re.finditer(r"\[DEBUG_JSON_START\]\s*(.*?)\s*\[DEBUG_JSON_END\]", raw, re.DOTALL)
        )
        debug_dicts: list[dict | None] = []
        if envelope_matches:
            for em in envelope_matches:
                try:
                    debug_dicts.append(_json.loads(em.group(1)))
                except _json.JSONDecodeError:
                    debug_dicts.append(None)
            # Rebuild stdout without the envelope regions so the
            # block splitter sees a clean per-statement layout.
            rebuilt: list[str] = []
            cursor = 0
            for em in envelope_matches:
                rebuilt.append(raw[cursor : em.start()])
                cursor = em.end()
            rebuilt.append(raw[cursor:])
            raw = "".join(rebuilt)

        blocks = _split_blocks(raw, statements, proc.stderr or "")
        # Attach each debug dict to the matching block.  When fewer
        # envelopes than statements (the engine short-circuited on a
        # parse error before emitting some envelopes), we pad with
        # None so every block gets a slot.
        for i, blk in enumerate(blocks):
            blk.debug = debug_dicts[i] if i < len(debug_dicts) else None
        # The "focus" envelope is the last non-None one — that's the
        # one the API layer surfaces in the response's `debug` field
        # for back-compat with single-statement callers.
        focus_debug = next((d for d in reversed(debug_dicts) if d is not None), None)

        stderr_clean = proc.stderr or ""
        has_engine_error = any(
            ln.lstrip().startswith("Error:") or ln.lstrip().startswith("error:")
            for ln in stderr_clean.splitlines()
        )
        block_success = all(b.success for b in blocks)
        overall_success = proc.returncode == 0 and not has_engine_error and block_success
        result = EngineRunResult(
            success=overall_success,
            blocks=blocks,
            stderr=stderr_clean,
            returncode=proc.returncode,
        )
        return result, focus_debug

    # ── statement classification helpers ──────────────────────────────
    @staticmethod
    def _is_empty_or_comment(s: str) -> bool:
        """A statement that carries no executable SQL (blank / comments only).

        The splitter preserves comment text into the *next* statement so
        it doesn't lose the leading comment; but a statement that is only
        comments (e.g. a trailing `/* … */` with no SQL after it) would
        otherwise surface as a phantom "statement" that the engine chokes
        on.  Downstream code calls this to drop such noise before
        executing.
        """
        t = s.strip()
        if not t:
            return True
        # strip line comments and whitespace
        import re as _re
        t = _re.sub(r"--[^\n]*", "", t)
        t = _re.sub(r"/\*.*?\*/", "", t, flags=_re.DOTALL)
        return not t.strip()

    @staticmethod
    def _is_repl_meta(s: str) -> bool:
        """True if `s` is a C++ REPL meta-command, not SQL.

        The CLI accepts a handful of dot/back-slash commands at the
        prompt (the engine banner says: `\.tokens`, `\.ast`, `\.plan`,
        `\.optimized`, `.source`, `.read`) plus the `exit` / `quit`
        family.  All of them raise `[Syntax] unexpected token` when
        `RunScriptFile` parses them as SQL — every test script in
        `tests/sql/` ends with a `exit;` line and used to surface as
        one extra red error block.  Drop them before dispatch so they
        don't pollute the per-statement result list.
        """
        t = s.strip()
        if not t:
            return False
        # skip past a leading line / block comment so `/* … */\\.tokens;`
        # is correctly classified.
        import re as _re
        while True:
            if t.startswith("--"):
                nl = t.find("\n")
                if nl == -1:
                    return True
                t = t[nl + 1 :].lstrip()
                continue
            if t.startswith("/*"):
                end = t.find("*/")
                if end == -1:
                    return True
                t = t[end + 2 :].lstrip()
                continue
            break
        if not t:
            return True
        # strip a single trailing `;`
        if t.endswith(";"):
            t = t[:-1].rstrip()
        head = t.split(None, 1)[0].lower() if t else ""
        if head in {"exit", "quit"}:
            return True
        # backslash / dot commands: `\.tokens`, `.source`, `.read`, etc.
        return bool(head) and head[0] in {"\\", "."}

    @staticmethod
    def _is_txn_control(s: str) -> bool:
        """True if `s` is a transaction control statement
        (BEGIN / COMMIT / ROLLBACK / SAVEPOINT / RELEASE)."""
        t = s.strip().lstrip("(").strip()
        if t.startswith("/*") or t.startswith("--"):
            # skip a leading comment, then re-inspect the first keyword
            import re as _re
            t = _re.sub(r"^/\*.*?\*/", " ", t, flags=_re.DOTALL)
            t = _re.sub(r"^--[^\n]*\n?", " ", t)
            t = t.strip()
        if not t:
            return False
        kw = t.split(None, 1)[0].upper().rstrip(";")
        return kw in {"BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT", "RELEASE"}

    # ── helper: split a script into per-statement strings ────────────────
    @staticmethod
    def _split_statements(sql: str) -> list[str]:
        """Split SQL on `;` while respecting string literals and comments.

        Unlike the C++ REPL's per-line `HasCompleteStatement`, this is a
        single pass over the *whole* script so multi-line statements and
        inline `;` inside one script split correctly.  Handles:

        - `'…'` single-quoted string literals (with `''` escaping);
        - `--` line comments;
        - `/* … */` block comments (a `;` inside any of these is NOT a
          statement terminator).

        Only a top-level `;` terminates a statement.  Comments are
        preserved verbatim into the following statement (the C++ engine
        ignores them at parse time).
        """
        out: list[str] = []
        buf: list[str] = []
        in_string = False
        in_line_comment = False
        in_block_comment = False
        i = 0
        n = len(sql)
        while i < n:
            c = sql[i]
            nxt = sql[i + 1] if i + 1 < n else ""
            if in_string:
                buf.append(c)
                if c == "'":
                    if nxt == "'":
                        buf.append("'")
                        i += 2
                        continue
                    in_string = False
                i += 1
                continue
            if in_line_comment:
                buf.append(c)
                if c == "\n":
                    in_line_comment = False
                i += 1
                continue
            if in_block_comment:
                buf.append(c)
                if c == "*" and nxt == "/":
                    buf.append("/")
                    i += 2
                    in_block_comment = False
                    continue
                i += 1
                continue
            if c == "'":
                in_string = True
                buf.append(c)
                i += 1
                continue
            if c == "-" and nxt == "-":
                in_line_comment = True
                buf.append(c)
                i += 1
                continue
            if c == "/" and nxt == "*":
                in_block_comment = True
                buf.append(c)
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
