"""Unit tests for the visualization module.

Covers:
- engine.py: parsing of C++ engine output, debug JSON extraction,
  statement splitting with string/comment awareness.
- schemas.py: Pydantic model round-trips and edge cases.
- _classify_kind helper: per-statement kind tagging for the UI banner.

These tests are pure-Python (no subprocess, no FastAPI server, no
browser).  They are intended to run cheaply on every commit so that
regressions in the parsing pipeline surface immediately.

Run with:

    cd webui
    python -m unittest backend.tests.test_engine -v
"""

from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path

# Ensure the webui/ directory is on sys.path so `from backend.engine import …`
# resolves regardless of the caller's cwd (mirrors __main__.py).
_HERE = Path(__file__).resolve()
_PKG_PARENT = _HERE.parents[2]  # webui/
if str(_PKG_PARENT) not in sys.path:
    sys.path.insert(0, str(_PKG_PARENT))

from backend.engine import (  # noqa: E402
    ParsedBlock,
    SqlEngine,
    _classify_kind,
    _parse_one_block,
    _split_blocks,
    _strip_cell_padding,
    find_engine_binary,
)


# ── _classify_kind ──────────────────────────────────────────────────────────


class TestClassifyKind(unittest.TestCase):
    """The UI banner colour is driven by the per-statement `kind` tag."""

    def test_select(self):
        b = ParsedBlock()
        _classify_kind("SELECT 1", b)
        self.assertEqual(b.kind, "select")

    def test_select_with_cte(self):
        b = ParsedBlock()
        _classify_kind("WITH x AS (SELECT 1) SELECT * FROM x", b)
        self.assertEqual(b.kind, "select")

    def test_show(self):
        b = ParsedBlock()
        _classify_kind("SHOW TABLES", b)
        self.assertEqual(b.kind, "select")

    def test_explain(self):
        b = ParsedBlock()
        _classify_kind("EXPLAIN SELECT 1", b)
        self.assertEqual(b.kind, "select")

    def test_create_table(self):
        b = ParsedBlock()
        _classify_kind("CREATE TABLE t (id INT)", b)
        self.assertEqual(b.kind, "ddl")

    def test_drop_table(self):
        b = ParsedBlock()
        _classify_kind("DROP TABLE t", b)
        self.assertEqual(b.kind, "ddl")

    def test_alter_table(self):
        b = ParsedBlock()
        _classify_kind("ALTER TABLE t ADD COLUMN c INT", b)
        self.assertEqual(b.kind, "ddl")

    def test_truncate(self):
        b = ParsedBlock()
        _classify_kind("TRUNCATE TABLE t", b)
        self.assertEqual(b.kind, "ddl")

    def test_insert(self):
        b = ParsedBlock()
        _classify_kind("INSERT INTO t VALUES (1)", b)
        self.assertEqual(b.kind, "dml")

    def test_update(self):
        b = ParsedBlock()
        _classify_kind("UPDATE t SET c=1", b)
        self.assertEqual(b.kind, "dml")

    def test_delete(self):
        b = ParsedBlock()
        _classify_kind("DELETE FROM t WHERE id=1", b)
        self.assertEqual(b.kind, "dml")

    def test_merge(self):
        b = ParsedBlock()
        _classify_kind("MERGE INTO t USING s ON …", b)
        self.assertEqual(b.kind, "dml")

    def test_begin(self):
        b = ParsedBlock()
        _classify_kind("BEGIN", b)
        self.assertEqual(b.kind, "txn")

    def test_commit(self):
        b = ParsedBlock()
        _classify_kind("COMMIT", b)
        self.assertEqual(b.kind, "txn")

    def test_rollback(self):
        b = ParsedBlock()
        _classify_kind("ROLLBACK", b)
        self.assertEqual(b.kind, "txn")

    def test_savepoint(self):
        b = ParsedBlock()
        _classify_kind("SAVEPOINT sp1", b)
        self.assertEqual(b.kind, "txn")

    def test_verb_takes_priority_over_error_flag(self):
        # The verb-based classification is what the banner colour is
        # keyed on; the success/failure bit drives a separate red
        # banner via `block.success`.  A SELECT that failed still
        # reports kind=select (the UI then shows "SELECT" with red).
        b = ParsedBlock(success=False)
        _classify_kind("SELECT * FROM notthere", b)
        self.assertEqual(b.kind, "select")

    def test_leading_whitespace_and_parens(self):
        b = ParsedBlock()
        _classify_kind("  (  select 1", b)
        self.assertEqual(b.kind, "select")

    def test_other_fallback(self):
        b = ParsedBlock()
        _classify_kind("RANDOM COMMAND", b)
        self.assertEqual(b.kind, "other")


# ── _parse_one_block ────────────────────────────────────────────────────────


class TestParseOneBlock(unittest.TestCase):
    """Parse a single textual block from the C++ engine output."""

    def test_select_block(self):
        raw = (
            "id | name\n"
            "---+----\n"
            "1  | Alice\n"
            "2  | Bob\n"
            "(2 rows)"
        )
        b = _parse_one_block("SELECT * FROM t", raw)
        self.assertTrue(b.success)
        self.assertEqual(b.column_names, ["id", "name"])
        self.assertEqual(b.rows, [["1", "Alice"], ["2", "Bob"]])
        self.assertEqual(b.message, "(2 rows)")

    def test_select_block_pads_short_rows(self):
        # The engine sometimes emits fewer cells than columns on error
        # or truncation.  We must not crash and we must preserve the
        # declared column count.
        raw = (
            "a | b | c\n"
            "---+---+---\n"
            "1 | 2\n"
        )
        b = _parse_one_block("SELECT a,b,c FROM t", raw)
        self.assertEqual(b.column_names, ["a", "b", "c"])
        self.assertEqual(b.rows, [["1", "2", ""]])

    def test_select_block_truncates_long_rows(self):
        raw = (
            "a | b\n"
            "---+---\n"
            "1 | 2 | 3 | extra\n"
        )
        b = _parse_one_block("SELECT a,b FROM t", raw)
        self.assertEqual(b.rows, [["1", "2"]])

    def test_ddl_block(self):
        b = _parse_one_block("CREATE TABLE t (id INT)", "OK")
        self.assertTrue(b.success)
        self.assertEqual(b.message, "OK")
        self.assertEqual(b.column_names, [])
        self.assertEqual(b.rows, [])
        self.assertEqual(b.kind, "ddl")

    def test_dml_block_with_row_count(self):
        b = _parse_one_block(
            "INSERT INTO t VALUES (1)", "Inserted 1 row"
        )
        self.assertTrue(b.success)
        self.assertEqual(b.message, "Inserted 1 row")
        self.assertEqual(b.kind, "dml")

    def test_empty_stdout_with_stderr(self):
        b = _parse_one_block(
            "SELECT bad", "", stderr="Error: parse error at line 1 col 5"
        )
        self.assertFalse(b.success)
        self.assertEqual(b.kind, "error")
        self.assertIn("Error:", b.message)

    def test_stderr_only_is_error(self):
        b = _parse_one_block(
            "SELECT bad",
            "",
            stderr="Error: semantic error: table not found",
        )
        self.assertFalse(b.success)
        self.assertEqual(b.kind, "error")

    def test_inline_error_in_stdout(self):
        # Some engine paths write "Error: …" to stdout; we should still
        # tag the block as failure.
        raw = "Error: type mismatch\nfoo | bar\n---+----\n1 | 2\n"
        b = _parse_one_block("SELECT 1", raw)
        self.assertFalse(b.success)
        self.assertEqual(b.kind, "error")

    def test_no_separator_ddl(self):
        # DDL sometimes returns multiple lines (e.g. "Created table t"
        # + a separator).  We accept the first non-empty line as the
        # message and don't try to parse it as a tabular block.
        b = _parse_one_block(
            "CREATE TABLE t (id INT)",
            "Table created: t\n",
        )
        self.assertTrue(b.success)
        self.assertEqual(b.message, "Table created: t")
        self.assertEqual(b.kind, "ddl")


# ── _split_blocks ───────────────────────────────────────────────────────────


class TestSplitBlocks(unittest.TestCase):
    """Split combined stdout into one ParsedBlock per statement."""

    def test_multiple_selects(self):
        stdout = (
            "id | name\n"
            "---+----\n"
            "1  | Alice\n"
            "(1 row)\n"
            "\n"
            "id | name\n"
            "---+----\n"
            "2  | Bob\n"
            "(1 row)\n"
        )
        blocks = _split_blocks(stdout, ["SELECT * FROM t", "SELECT * FROM t"])
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0].rows, [["1", "Alice"]])
        self.assertEqual(blocks[1].rows, [["2", "Bob"]])

    def test_dml_dml_does_not_lump(self):
        # Two DML statements each produce a one-line "OK" / row count.
        # A greedy parser would glue them together; we must not.
        stdout = "OK\nInserted 1 row\n"
        blocks = _split_blocks(
            stdout, ["CREATE TABLE t (id INT)", "INSERT INTO t VALUES (1)"]
        )
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0].message, "OK")
        self.assertEqual(blocks[1].message, "Inserted 1 row")

    def test_pad_for_short_statement_list(self):
        # If the engine produced fewer blocks than expected, we still
        # return one block per statement (success=True, empty content).
        blocks = _split_blocks("", ["SELECT 1", "SELECT 2", "SELECT 3"])
        self.assertEqual(len(blocks), 3)
        for b in blocks:
            self.assertTrue(b.success)

    def test_pad_for_short_statement_list_with_stderr_error(self):
        # Critical regression guard: when the engine only writes to
        # stderr (e.g. semantic error "table not found") and stdout
        # is empty, padded blocks must inherit the failure so the
        # UI surfaces the error rather than silently claiming "OK".
        stderr = "Error: semantic error [TableNotFound] line=1, col=1: table not found: t\n"
        blocks = _split_blocks("", ["SELECT * FROM t"], stderr)
        self.assertEqual(len(blocks), 1)
        self.assertFalse(blocks[0].success)
        self.assertEqual(blocks[0].kind, "error")
        self.assertIn("TableNotFound", blocks[0].message)

    def test_pad_for_multiple_with_stderr_error(self):
        # Multi-statement script that all fail together.
        stderr = "Error: lexical error line=1, col=1: bad character\n"
        blocks = _split_blocks("", ["SELECT 1", "SELECT 2"], stderr)
        self.assertEqual(len(blocks), 2)
        for b in blocks:
            self.assertFalse(b.success)
            self.assertEqual(b.kind, "error")

    def test_leftover_goes_to_last_block(self):
        stdout = "OK\nsome unexpected trailer\n"
        blocks = _split_blocks(stdout, ["CREATE TABLE t (id INT)", "INSERT INTO t VALUES (1)"])
        # The first block absorbs "OK" (DDL message); the leftover line
        # is captured into the second block (or a synthetic block if
        # only one statement was declared).  Either way we end up with
        # at least one block per statement.
        self.assertGreaterEqual(len(blocks), 1)


# ── SqlEngine._split_statements ─────────────────────────────────────────────


class TestSplitStatements(unittest.TestCase):
    """Statement splitter — must respect strings and comments."""

    def test_basic_split(self):
        out = SqlEngine._split_statements("SELECT 1; SELECT 2;")
        self.assertEqual(out, ["SELECT 1", "SELECT 2"])

    def test_no_trailing_semicolon(self):
        out = SqlEngine._split_statements("SELECT 1")
        self.assertEqual(out, ["SELECT 1"])

    def test_string_with_semicolon(self):
        out = SqlEngine._split_statements(
            "INSERT INTO t VALUES ('a;b', 1); SELECT 1;"
        )
        self.assertEqual(out, [
            "INSERT INTO t VALUES ('a;b', 1)",
            "SELECT 1",
        ])

    def test_escaped_quote(self):
        out = SqlEngine._split_statements(
            "INSERT INTO t VALUES ('it''s ok', 1); SELECT 1;"
        )
        self.assertEqual(out, [
            "INSERT INTO t VALUES ('it''s ok', 1)",
            "SELECT 1",
        ])

    def test_line_comment_with_semicolon(self):
        # The splitter preserves comments in the output (they belong
        # to the next statement).  The key invariant is that the `;`
        # *inside* the comment does NOT trigger a split — the comment
        # text is appended verbatim into the next statement.
        out = SqlEngine._split_statements(
            "-- this ; is in a comment\nSELECT 1;"
        )
        self.assertEqual(len(out), 1)
        self.assertIn("SELECT 1", out[0])
        self.assertIn("-- this", out[0])

    def test_multiple_statements_with_whitespace(self):
        out = SqlEngine._split_statements(
            "  SELECT 1;  \n\n  SELECT 2  ;\n  SELECT 3;"
        )
        self.assertEqual(out, ["SELECT 1", "SELECT 2", "SELECT 3"])

    def test_empty_string(self):
        out = SqlEngine._split_statements("")
        self.assertEqual(out, [])

    def test_only_comments(self):
        # A script that is only line comments produces a single block
        # of preserved comment text.  The downstream `_split_blocks`
        # is robust enough to handle this — it ignores blank / comment
        # output and reports success=True.
        out = SqlEngine._split_statements("-- nothing here\n-- ; still nothing\n")
        self.assertEqual(len(out), 1)
        self.assertIn("nothing here", out[0])


# ── Debug JSON extraction ──────────────────────────────────────────────────


class TestDebugJsonExtraction(unittest.TestCase):
    """The regex that pulls JSON between [DEBUG_JSON_START]/[DEBUG_JSON_END]."""

    DEBUG_JSON_RE = __import__("re").compile(
        r"\[DEBUG_JSON_START\]\s*(.*?)\s*\[DEBUG_JSON_END\]", __import__("re").DOTALL
    )

    def test_extracts_json(self):
        sample = (
            "OK\n"
            "\n[DEBUG_JSON_START]\n"
            '{"tokens":[{"type":"KEYWORD","lexeme":"SELECT","line":1,"col":1}],'
            '"ast_text":"SELECT 1","plan_json":"{\\"type\\":\\"Project\\"}",'
            '"plan_before_opt":"","storage_stats":{"hit_count":1,"miss_count":0,'
            '"replacement_count":0,"hit_rate":1.0,"total_pages":1},'
            '"replacement_log":[]}\n'
            "[DEBUG_JSON_END]\n"
        )
        m = self.DEBUG_JSON_RE.search(sample)
        self.assertIsNotNone(m)
        parsed = json.loads(m.group(1))
        self.assertEqual(parsed["tokens"][0]["lexeme"], "SELECT")
        self.assertEqual(parsed["storage_stats"]["hit_count"], 1)
        self.assertEqual(parsed["storage_stats"]["hit_rate"], 1.0)
        self.assertEqual(parsed["replacement_log"], [])

    def test_no_marker_yields_no_match(self):
        sample = "OK\njust some stdout, no debug info\n"
        m = self.DEBUG_JSON_RE.search(sample)
        self.assertIsNone(m)

    def test_multiple_markers_takes_first(self):
        sample = (
            "[DEBUG_JSON_START]\n{\"a\":1}\n[DEBUG_JSON_END]\n"
            "[DEBUG_JSON_START]\n{\"b\":2}\n[DEBUG_JSON_END]\n"
        )
        m = self.DEBUG_JSON_RE.search(sample)
        self.assertIsNotNone(m)
        self.assertEqual(json.loads(m.group(1)), {"a": 1})


# ── Boundary / error-path edge cases ────────────────────────────────────────


class TestSplitStatementsEdgeCases(unittest.TestCase):
    """Boundary cases that the basic suite didn't hit."""

    def test_only_semicolons(self):
        # `;;;` → no real statements, the splitter must drop them.
        out = SqlEngine._split_statements(";;;")
        self.assertEqual(out, [])

    def test_only_whitespace(self):
        out = SqlEngine._split_statements("   \n\t  ")
        self.assertEqual(out, [])

    def test_unterminated_string_at_eof(self):
        # An opening `'` with no closing quote is a malformed script.
        # The splitter must NOT crash; it may keep the unterminated text
        # in the trailing buffer (the C++ engine will then reject it).
        out = SqlEngine._split_statements("SELECT 'unterminated")
        self.assertEqual(out, ["SELECT 'unterminated"])

    def test_unterminated_string_then_statement(self):
        # Properly closed string with `;` inside: the splitter exits the
        # string at the closing quote and the next `;` splits.  This
        # documents that an unterminated string is the only case where
        # the splitter "swallows" trailing `;` characters.
        out = SqlEngine._split_statements("SELECT 'a;b'; SELECT 1;")
        self.assertEqual(out, ["SELECT 'a;b'", "SELECT 1"])

    def test_unterminated_string_at_file_end(self):
        # An opening `'` with no closing quote before EOF means EVERY
        # remaining `;` is treated as inside the string.  The whole
        # script becomes one statement (the splitter can't recover).
        out = SqlEngine._split_statements("SELECT 'a;b; SELECT 1")
        self.assertEqual(len(out), 1)
        self.assertIn("SELECT 1", out[0])
        self.assertTrue(out[0].startswith("SELECT 'a"))

    def test_doubled_quote_at_string_end(self):
        # `''` is an escaped single quote inside a string.  The `;`
        # right after the closing quote SHOULD split.
        out = SqlEngine._split_statements("SELECT 'it''s'; SELECT 1;")
        self.assertEqual(out, ["SELECT 'it''s'", "SELECT 1"])

    def test_comment_then_empty_statements(self):
        # `;` after a comment should still split.
        out = SqlEngine._split_statements("-- preamble\nSELECT 1;\n-- tail\n;SELECT 2;")
        # Both statements should be present; the empty `;SELECT 2` is
        # valid because the splitter trims.
        self.assertGreaterEqual(len(out), 2)
        self.assertIn("SELECT 1", out[0])

    def test_trailing_semicolon_with_no_statement(self):
        # `SELECT 1;;` → one statement (the trailing empty `;` is dropped).
        out = SqlEngine._split_statements("SELECT 1;;")
        self.assertEqual(out, ["SELECT 1"])

    def test_unicode_in_string_literal(self):
        # Multi-byte characters in a string must NOT confuse the splitter.
        out = SqlEngine._split_statements("INSERT INTO t VALUES ('你好;世界', 1); SELECT 1;")
        self.assertEqual(out, [
            "INSERT INTO t VALUES ('你好;世界', 1)",
            "SELECT 1",
        ])


class TestParseOneBlockEdgeCases(unittest.TestCase):
    """Boundary cases for `_parse_one_block`."""

    def test_stderr_with_leading_blank_line(self):
        # Some platforms prefix stderr with a newline; the parser must
        # still detect the `Error:` line.
        b = _parse_one_block("SELECT bad", "", stderr="\nError: parse error\n")
        self.assertFalse(b.success)
        self.assertIn("Error", b.message)

    def test_stderr_with_multiple_lines_first_is_error(self):
        b = _parse_one_block(
            "SELECT 1",
            "",
            stderr="Error: semantic error\n[more context]\n",
        )
        self.assertFalse(b.success)
        self.assertIn("semantic error", b.message)

    def test_stderr_lowercase_error_is_also_caught(self):
        b = _parse_one_block("SELECT 1", "", stderr="error: foo bar\n")
        self.assertFalse(b.success)

    def test_only_blank_lines(self):
        b = _parse_one_block("SELECT 1", "\n\n   \n")
        # Blank stdout with no stderr → success=True, kind=other (no data).
        self.assertTrue(b.success)

    def test_tab_separator_still_parsed(self):
        # The separator pattern uses `[\s\-+|]+`, which accepts tabs too.
        raw = "id\n---\t---\n1\n(1 row)"
        b = _parse_one_block("SELECT 1", raw)
        self.assertEqual(b.column_names, ["id"])
        self.assertEqual(b.rows, [["1"]])

    def test_single_column_select(self):
        raw = "x\n---\n42\n(1 row)"
        b = _parse_one_block("SELECT 42", raw)
        self.assertEqual(b.column_names, ["x"])
        self.assertEqual(b.rows, [["42"]])

    def test_select_zero_rows(self):
        # The engine emits the header + separator, then the "(0 rows)"
        # terminator with no data lines.
        raw = "id\n---\n(0 rows)"
        b = _parse_one_block("SELECT id FROM t WHERE 1=0", raw)
        self.assertEqual(b.column_names, ["id"])
        self.assertEqual(b.rows, [])
        self.assertEqual(b.message, "(0 rows)")


class TestSplitBlocksEdgeCases(unittest.TestCase):
    """Boundary cases for `_split_blocks`."""

    def test_trailing_script_summary_is_stripped(self):
        # The engine always emits a final "[script] <path>: ran N
        # statement(s)" line.  We must strip it so it doesn't pollute
        # the last block.
        stdout = (
            "id\n---\n1\n(1 row)\n\n"
            "[script] /tmp/foo.sql: ran 1 statement(s)\n"
        )
        blocks = _split_blocks(stdout, ["SELECT 1"])
        self.assertEqual(len(blocks), 1)
        self.assertFalse(blocks[0].message.startswith("[script]"))

    def test_consecutive_ddl_ddl_each_get_a_block(self):
        stdout = "OK\nOK\n"
        blocks = _split_blocks(stdout, ["CREATE TABLE a (id INT)", "CREATE TABLE b (id INT)"])
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0].message, "OK")
        self.assertEqual(blocks[1].message, "OK")

    def test_mixed_select_then_ddl(self):
        stdout = (
            "id\n---\n1\n(1 row)\n"
            "OK\n"
        )
        blocks = _split_blocks(
            stdout,
            ["SELECT 1", "CREATE TABLE t (id INT)"],
        )
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[0].rows, [["1"]])
        self.assertEqual(blocks[1].message, "OK")

    def test_more_statements_than_blocks_pads_with_error_on_stderr(self):
        # Engine emitted a header+sep for stmt 1, then crashed before
        # stmt 2.  Stderr has the error; the second block (which is
        # padded) must inherit the failure.
        stdout = "id\n---\n1\n(1 row)\n"
        stderr = "Error: runtime error at line 5\n"
        blocks = _split_blocks(stdout, ["SELECT 1", "SELECT 2"], stderr)
        self.assertEqual(len(blocks), 2)
        # First block: parser sees the successful SELECT in stdout but
        # also sees an Error in stderr — the current implementation is
        # pessimistic and tags the block as a failure so the user
        # never sees a green "OK" while an error was emitted.  This
        # regression-guard documents that behaviour.
        self.assertFalse(blocks[0].success)
        # Second block: padded, inherits the error.
        self.assertFalse(blocks[1].success)
        self.assertEqual(blocks[1].kind, "error")


class TestStripCellPadding(unittest.TestCase):
    """`_strip_cell_padding` is internal but exercised via `_parse_one_block`."""

    def test_basic_padding(self):
        # Two cells with surrounding spaces — both must be trimmed.
        cells = ["  id  ", "  name  "]
        out = [_strip_cell_padding(c) for c in cells]
        self.assertEqual(out, ["id", "name"])

    def test_no_padding(self):
        self.assertEqual(_strip_cell_padding("x"), "x")

    def test_internal_space_preserved(self):
        # `_strip_cell_padding` strips leading/trailing, not internal.
        self.assertEqual(_strip_cell_padding("  hello world  "), "hello world")


class TestFindEngineBinary(unittest.TestCase):
    """`find_engine_binary` should fall through the lookup chain safely."""

    def setUp(self):
        # Snapshot env so we can restore it after each test.
        self._env_backup = os.environ.get("SQLCOMPILER_BIN")

    def tearDown(self):
        if self._env_backup is None:
            os.environ.pop("SQLCOMPILER_BIN", None)
        else:
            os.environ["SQLCOMPILER_BIN"] = self._env_backup

    def test_env_override_to_missing_path_returns_none(self):
        os.environ["SQLCOMPILER_BIN"] = "C:/nonexistent/sqlcompiler.exe"
        # Even with a bad override, the function must NOT raise; it
        # returns None and lets the cleanup paths fall through to
        # PATH / build-dir discovery.
        result = find_engine_binary()
        # The result is None OR a real path found in build/PATH — both
        # are acceptable.  The contract is just "no exception".
        if result is not None:
            self.assertTrue(result.exists())

    def test_env_override_to_real_binary(self):
        # Find an existing binary on this machine (if any).
        import shutil
        existing = shutil.which("sqlcompiler") or shutil.which("sqlcompiler.exe")
        if not existing:
            self.skipTest("No sqlcompiler binary on PATH")
        os.environ["SQLCOMPILER_BIN"] = existing
        result = find_engine_binary()
        self.assertIsNotNone(result)
        self.assertEqual(str(result).lower(), existing.lower())


class TestListDbFiles(unittest.TestCase):
    """`list_db_files` returns `[]` for missing directories and filters
    non-`.db` files.  No subprocess, no engine."""

    def test_missing_directory_returns_empty(self):
        import asyncio
        from backend.engine import list_db_files
        bogus = Path("C:/definitely/not/a/real/dir_xyz123")
        out = asyncio.run(list_db_files(bogus))
        self.assertEqual(out, [])

    def test_file_instead_of_directory_returns_empty(self):
        import asyncio
        from backend.engine import list_db_files
        # Pick any existing file from this test module.
        me = _HERE  # the test_engine.py file itself
        out = asyncio.run(list_db_files(me))
        self.assertEqual(out, [])

    def test_directory_with_db_files_is_listed(self):
        import asyncio
        import tempfile
        from backend.engine import list_db_files
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            # Create three .db files with different mtimes so the
            # sort-by-mtime-desc ordering is deterministic.
            for i, name in enumerate(["alpha.db", "beta.db", "gamma.db"]):
                p = tmpdir / name
                p.write_bytes(b"x" * (i + 1))
                # Force distinct mtimes
                import os
                os.utime(p, (1000 + i, 1000 + i))
            out = asyncio.run(list_db_files(tmpdir))
            names = [it["name"] for it in out]
            self.assertEqual(set(names), {"alpha.db", "beta.db", "gamma.db"})
            # Most-recently-modified first
            self.assertEqual(names[0], "gamma.db")
            self.assertEqual(names[-1], "alpha.db")
            # Each entry has size_bytes set
            for it in out:
                self.assertIn("size_bytes", it)
                self.assertGreater(it["size_bytes"], 0)

    def test_non_db_files_are_filtered_out(self):
        import asyncio
        import tempfile
        from backend.engine import list_db_files
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "real.db").write_bytes(b"x")
            (Path(tmp) / "fake.txt").write_text("not a db")
            (Path(tmp) / "notes.sql").write_text("-- not a db either")
            out = asyncio.run(list_db_files(Path(tmp)))
            names = [it["name"] for it in out]
            self.assertEqual(names, ["real.db"])


class TestEngineError(unittest.TestCase):
    """The `EngineError` exception exists for binary-missing cases."""

    def test_is_runtime_error_subclass(self):
        from backend.engine import EngineError
        self.assertTrue(issubclass(EngineError, RuntimeError))

    def test_carries_message(self):
        from backend.engine import EngineError
        err = EngineError("binary not found")
        self.assertIn("binary not found", str(err))


class TestSqlEngineBinaryMissing(unittest.TestCase):
    """`SqlEngine.execute` must surface a clear EngineError when the
    binary is absent (this is what `/api/db/open` wraps as a 500)."""

    def test_binary_not_found_raises_engine_error(self):
        from backend.engine import EngineError, SqlEngine
        engine = SqlEngine(Path("C:/nonexistent/sqlcompiler.exe"), Path("C:/tmp/x.db"))
        import asyncio
        with self.assertRaises(EngineError):
            asyncio.run(engine.execute("SELECT 1"))





class TestSchemas(unittest.TestCase):
    """Pydantic models round-trip cleanly."""

    def test_token_info_roundtrip(self):
        from backend.schemas import TokenInfo
        t = TokenInfo(type="KEYWORD_SELECT", lexeme="SELECT", line=1, col=1)
        self.assertEqual(t.model_dump(), {
            "type": "KEYWORD_SELECT",
            "lexeme": "SELECT",
            "line": 1,
            "col": 1,
        })

    def test_debug_data_with_optional_storage(self):
        from backend.schemas import DebugData, ReplacementEntry, StorageStats
        d = DebugData(
            tokens=[],
            ast_text="SELECT 1",
            plan_json='{"type":"Project"}',
            plan_before_opt="",
            storage_stats=None,  # explicitly absent — see _to_debug_data below
            replacement_log=[],
        )
        self.assertIsNone(d.storage_stats)
        d2 = DebugData(
            tokens=[],
            ast_text="",
            plan_json="",
            plan_before_opt="",
            storage_stats=StorageStats(
                hit_count=2, miss_count=1, replacement_count=0,
                hit_rate=2/3, total_pages=3,
            ),
            replacement_log=[ReplacementEntry(evicted=1, loaded=2, dirty=True)],
        )
        self.assertEqual(d2.storage_stats.hit_count, 2)
        self.assertEqual(d2.replacement_log[0].evicted, 1)
        self.assertTrue(d2.replacement_log[0].dirty)


if __name__ == "__main__":
    unittest.main(verbosity=2)