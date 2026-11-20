"""Integration tests for the FastAPI visualization endpoints.

Unlike `test_engine.py` (pure Python parsing) and `test_e2e.py` (live
HTTP server), this module exercises the API layer with FastAPI's
in-process `TestClient`.  It does NOT spawn the C++ engine binary;
instead it monkey-patches `SqlEngine.execute` and `execute_debug` to
return canned responses so the test stays fast and deterministic.

If you only want to verify the engine integration, see `test_e2e.py`.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve()
_PKG_PARENT = _HERE.parents[2]  # webui/
if str(_PKG_PARENT) not in sys.path:
    sys.path.insert(0, str(_PKG_PARENT))

try:
    from fastapi.testclient import TestClient
except ImportError:
    TestClient = None  # type: ignore

import backend.api as api_module  # noqa: E402
import backend.engine as engine_module  # noqa: E402
from backend.engine import EngineRunResult, ParsedBlock  # noqa: E402


def _block(success: bool, message: str = "", columns=None, rows=None,
           kind: str = "select", statement: str = "") -> ParsedBlock:
    return ParsedBlock(
        success=success,
        message=message,
        column_names=columns or [],
        rows=rows or [],
        kind=kind,
        statement=statement,
    )


@unittest.skipIf(TestClient is None, "fastapi.testclient not available")
class TestApiVisualization(unittest.TestCase):
    """Drive the FastAPI app in-process and assert on the wire format."""

    def setUp(self):
        # Reset the module-level SESSION so each test starts clean.
        api_module.SESSION.db_path = None
        api_module.SESSION.engine = None
        api_module.SESSION.binary = None

        # Build a fake engine whose execute / execute_debug return canned
        # results.  No subprocess is spawned.
        self.fake_engine = _FakeEngine()

        # Monkey-patch the engine constructor so /api/db/open succeeds
        # without touching the filesystem.
        self._orig_sqlengine_ctor = engine_module.SqlEngine
        engine_module.SqlEngine = lambda binary, db_path: self.fake_engine

        from backend.main import create_app
        self.client = TestClient(create_app())

    def tearDown(self):
        engine_module.SqlEngine = self._orig_sqlengine_ctor
        api_module.SESSION.db_path = None
        api_module.SESSION.engine = None

    # ── /api/query/debug ────────────────────────────────────────────────

    def test_query_debug_requires_open_db(self):
        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 409)

    def test_query_debug_returns_tokens_ast_plan(self):
        self._open_db()
        self.fake_engine.next_debug = {
            "tokens": [
                {"type": "KEYWORD_SELECT", "lexeme": "SELECT", "line": 1, "col": 1},
                {"type": "INTEGER_LITERAL", "lexeme": "1", "line": 1, "col": 8},
            ],
            "ast_text": "SELECT 1",
            "plan_json": '{"type":"Project","col_0":"1"}',
            "plan_before_opt": "Project(1)",
            "storage_stats": {
                "hit_count": 5,
                "miss_count": 2,
                "replacement_count": 1,
                "hit_rate": 5 / 7,
                "total_pages": 3,
            },
            "replacement_log": [
                {"evicted": 1, "loaded": 4, "dirty": False},
                {"evicted": 2, "loaded": 5, "dirty": True},
            ],
        }
        self.fake_engine.next_blocks = [_block(True, "(1 row)", ["?column?"], [["1"]], "select")]

        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        self.assertIsNotNone(data["debug"])
        self.assertEqual(len(data["debug"]["tokens"]), 2)
        self.assertEqual(data["debug"]["tokens"][0]["lexeme"], "SELECT")
        self.assertEqual(data["debug"]["ast_text"], "SELECT 1")
        self.assertEqual(data["debug"]["plan_before_opt"], "Project(1)")
        self.assertEqual(data["debug"]["storage_stats"]["hit_count"], 5)
        self.assertEqual(len(data["debug"]["replacement_log"]), 2)
        self.assertFalse(data["debug"]["replacement_log"][0]["dirty"])
        self.assertTrue(data["debug"]["replacement_log"][1]["dirty"])

    def test_query_debug_handles_missing_debug_data(self):
        # If the engine fails to emit a JSON block (e.g. it crashed),
        # the API should still return a 200 with debug=None rather than
        # 500'ing.  The frontend relies on this to display an error panel.
        self._open_db()
        self.fake_engine.next_debug = None
        self.fake_engine.next_blocks = [_block(False, "Error: engine crashed", kind="error")]
        self.fake_engine.next_stderr = "Error: engine crashed"

        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertFalse(data["success"])
        self.assertIsNone(data["debug"])
        self.assertIn("crashed", data["message"])

    def test_query_debug_rejects_empty_statement(self):
        self._open_db()
        # Pydantic v2 rejects empty strings with 422 (validation error)
        # before the handler runs.  Both 400 and 422 are acceptable
        # for this case — the contract is "reject before invoking
        # the engine".
        r = self.client.post("/api/query/debug", json={"statement": ""})
        self.assertIn(r.status_code, (400, 422))
        # And the engine was NOT called.
        self.assertEqual(len([c for c in self.fake_engine.calls if c[0] == "execute_debug"]), 0)

    # ── /api/storage/stats ──────────────────────────────────────────────

    def _seed_last_stats(self, stats=None, repl_log=None):
        """Inject a fake stats snapshot into the SESSION cache.

        The storage endpoints no longer probe the engine with
        `SELECT 1;` (which would inflate the counters); they read
        from `SESSION.last_stats_raw`, populated by the last
        `execute_debug` call.

        Pass `stats=None` (default) to simulate "no debug run yet" —
        the cache stays `None` and the storage endpoints respond
        with the "Run a statement" message.  Pass an explicit dict
        (including `{}`) to install that exact snapshot.
        """
        if stats is None:
            api_module.SESSION.last_stats_raw = None
        else:
            api_module.SESSION.last_stats_raw = stats
        api_module.SESSION.last_repl_log_raw = repl_log if repl_log is not None else []

    def test_storage_stats_returns_cached_snapshot(self):
        # Stats come from the SESSION cache (set by /api/query/debug),
        # NOT from a fresh probe `SELECT 1;` that would itself
        # increment the counters.
        self._open_db()
        self._seed_last_stats(
            stats={
                "hit_count": 10, "miss_count": 5,
                "replacement_count": 2, "hit_rate": 10 / 15,
                "total_pages": 8,
            },
            repl_log=[{"evicted": 7, "loaded": 9, "dirty": False}],
        )
        r = self.client.get("/api/storage/stats")
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        self.assertEqual(data["stats"]["hit_count"], 10)
        self.assertEqual(data["stats"]["miss_count"], 5)
        self.assertEqual(data["stats"]["total_pages"], 8)
        self.assertEqual(len(data["replacement_log"]), 1)
        # Critically: no probe query was executed.
        self.assertEqual(
            len([c for c in self.fake_engine.calls if c[0] == "execute_debug"]),
            0,
        )

    def test_storage_stats_empty_cache_says_run_a_statement(self):
        # No prior /api/query/debug → empty cache → helpful message,
        # not a confusing zero baseline.
        self._open_db()
        self._seed_last_stats()  # default: None cache
        r = self.client.get("/api/storage/stats")
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        self.assertEqual(data["stats"]["hit_count"], 0)
        self.assertIn("Run a statement", data["message"])

    def test_storage_stats_requires_open_db(self):
        r = self.client.get("/api/storage/stats")
        self.assertEqual(r.status_code, 409)

    # ── /api/storage/reset ──────────────────────────────────────────────

    def test_storage_reset_returns_cached_snapshot(self):
        self._open_db()
        self._seed_last_stats(
            stats={
                "hit_count": 12, "miss_count": 6,
                "replacement_count": 3, "hit_rate": 12 / 18,
                "total_pages": 4,
            }
        )
        r = self.client.post("/api/storage/reset", json={})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        self.assertEqual(data["baseline"]["hit_count"], 12)
        self.assertEqual(data["baseline"]["miss_count"], 6)
        self.assertEqual(data["baseline"]["replacement_count"], 3)
        self.assertIn("Baseline", data["message"])
        # The handler must NOT have invoked the engine (no probe).
        self.assertEqual(
            len([c for c in self.fake_engine.calls if c[0] == "execute_debug"]),
            0,
        )

    def test_storage_reset_requires_open_db(self):
        r = self.client.post("/api/storage/reset", json={})
        self.assertEqual(r.status_code, 409)

    # ── /api/db/close + /api/db/unlink (database reset affordance) ────

    def test_close_db_is_idempotent_on_empty_session(self):
        """Calling /api/db/close with no open DB returns was_open=False
        rather than raising — this lets the frontend use it as part of
        a delete-then-reopen flow without first having to probe."""
        api_module.SESSION.engine = None
        api_module.SESSION.db_path = None
        r = self.client.post("/api/db/close")
        self.assertEqual(r.status_code, 200)
        body = r.json()
        self.assertTrue(body["success"])
        self.assertFalse(body["was_open"])
        self.assertEqual(body["previous_db_path"], "")

    def test_close_db_drops_session_when_open(self):
        """When a DB is bound, /api/db/close clears the session and
        reports was_open=True so the frontend can refresh its UI."""
        self._open_db()
        r = self.client.post("/api/db/close")
        self.assertEqual(r.status_code, 200)
        body = r.json()
        self.assertTrue(body["success"])
        self.assertTrue(body["was_open"])
        self.assertEqual(body["previous_db_path"], "/tmp/fake.db")
        # The SESSION must be cleared, otherwise the next /api/query/debug
        # would still try to use a closed engine.
        self.assertIsNone(api_module.SESSION.engine)
        self.assertIsNone(api_module.SESSION.db_path)

    def test_unlink_refuses_non_db_extensions(self):
        """Safety: only `.db` / `.wal` / `.shm` (and variants) are
        deletable.  Anything else returns 400 to make it impossible to
        ask the API to delete user source / executables."""
        r = self.client.post(
            "/api/db/unlink?path=" + "%5C%5Cevil%5Cmalware.exe"
            if False else  # never encode a real malware path
            "/api/db/unlink?path=" + "C:%2FUsers%2FLenovo%2FAppData%2FLocal%2FTemp%2Fevil.exe"
        )
        self.assertEqual(r.status_code, 400, msg=r.text)
        self.assertIn("Refusing", r.json()["detail"])

    def test_unlink_refuses_relative_path(self):
        """The delete endpoint requires an absolute path.  Relative paths
        are rejected so the caller can't accidentally target the cwd."""
        r = self.client.post("/api/db/unlink?path=relative.db")
        self.assertEqual(r.status_code, 400)
        self.assertIn("absolute", r.json()["detail"])

    def test_unlink_missing_file_is_idempotent(self):
        """A missing file returns success=True, deleted=False so the
        delete-then-reopen flow doesn't need a pre-check."""
        import tempfile as _tf, os as _os
        path = Path(_tf.gettempdir()) / ("missing_unlink_" + _os.urandom(4).hex() + ".db")
        self.assertFalse(path.exists())
        r = self.client.post("/api/db/unlink?path=" + str(path))
        self.assertEqual(r.status_code, 200)
        body = r.json()
        self.assertTrue(body["success"])
        self.assertFalse(body["deleted"])
        self.assertFalse(body["cleared_session"])

    def test_unlink_existing_db_clears_open_session(self):
        """When the path being deleted matches the open DB, the
        session must be cleared so subsequent requests don't try to
        read from a now-deleted file."""
        import tempfile as _tf, os as _os
        path = Path(_tf.gettempdir()) / ("live_unlink_" + _os.urandom(4).hex() + ".db")
        path.write_bytes(b"placeholder")
        try:
            api_module.SESSION.db_path = str(path)
            api_module.SESSION.engine = self.fake_engine
            r = self.client.post("/api/db/unlink?path=" + str(path))
            self.assertEqual(r.status_code, 200)
            body = r.json()
            self.assertTrue(body["deleted"])
            self.assertTrue(body["cleared_session"])
            self.assertIsNone(api_module.SESSION.engine)
            self.assertIsNone(api_module.SESSION.db_path)
            self.assertFalse(path.exists(), msg="file should be removed")
        finally:
            try:
                path.unlink()
            except FileNotFoundError:
                pass

    def test_unlink_db_also_removes_wal_companion(self):
        """Regression: the engine keeps its ARIES WAL at `<db>.wal` and
        replays it on every open — even into a freshly created, empty
        database file.  Deleting only the `.db` therefore left the old
        catalog recoverable, so "重置库" appeared to succeed while the
        next run hit the exact same `DuplicateName` / `ArityMismatch` /
        `ColumnNotFound` errors.  The `.db` delete must take the WAL
        with it."""
        import tempfile as _tf, os as _os
        stem = Path(_tf.gettempdir()) / ("wal_unlink_" + _os.urandom(4).hex() + ".db")
        wal = Path(str(stem) + ".wal")
        stem.write_bytes(b"placeholder")
        wal.write_bytes(b"stale-wal-records")
        try:
            r = self.client.post("/api/db/unlink?path=" + str(stem))
            self.assertEqual(r.status_code, 200, msg=r.text)
            body = r.json()
            self.assertTrue(body["deleted"])
            self.assertEqual(body["companions_deleted"], [str(wal)])
            self.assertFalse(stem.exists())
            self.assertFalse(wal.exists(), msg="orphaned WAL must not survive a reset")
        finally:
            for f in (stem, wal):
                try:
                    f.unlink()
                except FileNotFoundError:
                    pass

    def test_unlink_recovers_orphaned_wal_when_db_already_gone(self):
        """The half-deleted state (`.db` removed, `.wal` left behind) is
        exactly what the buggy reset produced.  Calling unlink on the
        missing `.db` path must still clean the orphaned WAL so the user
        can recover without hunting for the file."""
        import tempfile as _tf, os as _os
        stem = Path(_tf.gettempdir()) / ("orphan_wal_" + _os.urandom(4).hex() + ".db")
        wal = Path(str(stem) + ".wal")
        wal.write_bytes(b"stale-wal-records")
        self.assertFalse(stem.exists())
        try:
            r = self.client.post("/api/db/unlink?path=" + str(stem))
            self.assertEqual(r.status_code, 200, msg=r.text)
            body = r.json()
            self.assertTrue(body["success"])
            self.assertFalse(body["deleted"])
            self.assertEqual(body["companions_deleted"], [str(wal)])
            self.assertFalse(wal.exists())
        finally:
            for f in (stem, wal):
                try:
                    f.unlink()
                except FileNotFoundError:
                    pass

    def test_unlink_direct_wal_delete_has_no_companions(self):
        """A direct `.wal` delete must not recurse into `<x>.wal.wal`."""
        import tempfile as _tf, os as _os
        base = Path(_tf.gettempdir()) / ("direct_wal_" + _os.urandom(4).hex() + ".db")
        wal = Path(str(base) + ".wal")
        wal.write_bytes(b"stale-wal-records")
        try:
            r = self.client.post("/api/db/unlink?path=" + str(wal))
            self.assertEqual(r.status_code, 200, msg=r.text)
            body = r.json()
            self.assertTrue(body["deleted"])
            self.assertEqual(body["companions_deleted"], [])
            self.assertFalse(wal.exists())
        finally:
            for f in (base, wal, Path(str(wal) + ".wal")):
                try:
                    f.unlink()
                except FileNotFoundError:
                    pass

    # ── /api/query/debug populates the stats cache ──────────────────────

    def test_query_debug_populates_session_storage_cache(self):
        """After /api/query/debug runs, the SESSION holds the latest
        stats so /api/storage/stats can return them without re-querying
        the engine."""
        self._open_db()
        self.fake_engine.next_debug = {
            "tokens": [],
            "ast_text": "",
            "plan_json": "",
            "plan_before_opt": "",
            "storage_stats": {
                "hit_count": 7, "miss_count": 3,
                "replacement_count": 1, "hit_rate": 7 / 10,
                "total_pages": 2,
            },
            "replacement_log": [{"evicted": 1, "loaded": 2, "dirty": True}],
        }
        # First, run a debug query to seed the cache.
        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 200)
        # Then the stats endpoint should return the same numbers
        # without invoking the engine again.
        self.fake_engine.calls.clear()
        r2 = self.client.get("/api/storage/stats")
        self.assertEqual(r2.status_code, 200)
        data = r2.json()
        self.assertEqual(data["stats"]["hit_count"], 7)
        self.assertEqual(data["stats"]["miss_count"], 3)
        self.assertEqual(data["stats"]["total_pages"], 2)
        self.assertEqual(data["replacement_log"][0]["evicted"], 1)
        # The second call must not have spawned a probe subprocess.
        self.assertEqual(len(self.fake_engine.calls), 0)

    # ── engine-call lock serialisation ──────────────────────────────────

    def test_engine_call_503_when_lock_held(self):
        """When SESSION.lock is already held, _engine_call must
        surface a 503 instead of blocking (and potentially racing
        on the underlying `.db` file)."""
        import asyncio
        from backend import api as api_module

        original_lock = api_module.SESSION.lock

        class _FakeLock:
            """Mimics asyncio.Lock's interface for SESSION.lock."""

            def __init__(self):
                self._locked = False

            def locked(self):
                return self._locked

            async def __aenter__(self):
                self._locked = True
                return self

            async def __aexit__(self, *_):
                self._locked = False
                return False

        fake_lock = _FakeLock()
        api_module.SESSION.lock = fake_lock
        try:

            async def run():
                # Manually mark the lock as held (simulating a
                # previous request still in flight).
                await fake_lock.__aenter__()
                self.assertTrue(fake_lock.locked())
                from fastapi import HTTPException
                try:
                    async with api_module._engine_call() as _:
                        return "should not reach"
                except HTTPException as e:
                    return e.status_code
                finally:
                    await fake_lock.__aexit__()

            status = asyncio.run(run())
            self.assertEqual(status, 503)
        finally:
            api_module.SESSION.lock = original_lock

    # ── /api/query/execute (sanity check; the visualization module
    #    piggy-backs on this endpoint's response shape for cross-tab
    #    sync) ─────────────────────────────────────────────────────────

    def test_execute_returns_statements_in_order(self):
        self._open_db()
        self.fake_engine.next_blocks = [
            _block(True, "", ["id", "name"], [["1", "Alice"]], "select"),
        ]
        r = self.client.post("/api/query/execute", json={"statement": "SELECT * FROM t"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        self.assertEqual(len(data["results"]), 1)
        self.assertEqual(data["results"][0]["column_names"], ["id", "name"])
        self.assertEqual(data["results"][0]["rows"], [["1", "Alice"]])

    def test_execute_propagates_engine_failure(self):
        self._open_db()
        self.fake_engine.next_blocks = [_block(False, "Error: table not found", kind="error")]
        r = self.client.post("/api/query/execute", json={"statement": "SELECT * FROM bad"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertFalse(data["success"])
        self.assertIn("not found", data["results"][0]["message"])

    def test_execute_forwards_isolation_knobs(self):
        # on_error / transaction must reach the engine unchanged.
        self._open_db()
        self.fake_engine.next_blocks = [_block(True, "OK", kind="dml")]
        r = self.client.post("/api/query/execute", json={
            "statement": "INSERT INTO t VALUES (1)",
            "on_error": "continue",
            "transaction": True,
        })
        self.assertEqual(r.status_code, 200)
        self.assertEqual(
            self.fake_engine.execute_kwargs,
            [{"on_error": "continue", "transaction": True}],
        )

    def test_execute_continue_reports_per_statement_results(self):
        # A mid-batch failure must not stop later statements, and the API
        # must expose exactly one result per statement with the diagnostic
        # on the failing one only.
        self._open_db()
        self.fake_engine.next_blocks = [
            _block(True, "OK", kind="dml"),
            _block(False, "Error: table not found", kind="error"),
            _block(True, "OK", kind="dml"),
        ]
        r = self.client.post("/api/query/execute", json={
            "statement": "INSERT INTO t VALUES (1); SELECT * FROM bad; INSERT INTO t VALUES (2);",
            "on_error": "continue",
        })
        data = r.json()
        self.assertEqual(data["statement_count"], 3)
        self.assertFalse(data["aborted"])
        results = data["results"]
        self.assertTrue(results[0]["success"])
        self.assertFalse(results[1]["success"])
        self.assertEqual(results[1]["error"], "Error: table not found")
        self.assertTrue(results[2]["success"])

    def test_execute_abort_flags_aborted(self):
        # In abort mode a trailing failure marks the batch as truncated.
        self._open_db()
        self.fake_engine.next_blocks = [
            _block(True, "OK", kind="dml"),
            _block(False, "Error: table not found", kind="error"),
        ]
        r = self.client.post("/api/query/execute", json={
            "statement": "INSERT INTO t VALUES (1); SELECT * FROM bad;",
            "on_error": "abort",
        })
        data = r.json()
        self.assertFalse(data["success"])
        self.assertTrue(data["aborted"])
        self.assertEqual(data["statement_count"], 2)

    # ── API edge cases (boundary / error paths) ────────────────────────

    def test_query_debug_with_no_blocks_and_no_debug(self):
        # An empty SELECT might legitimately produce no stdout and no
        # debug envelope (e.g. a query the engine short-circuits).
        # The API must return success=true with empty results + null debug.
        self._open_db()
        self.fake_engine.next_blocks = []
        self.fake_engine.next_debug = None
        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        self.assertEqual(data["results"], [])
        self.assertIsNone(data["debug"])

    def test_query_debug_partial_debug_payload(self):
        # Engine emitted SOME fields of the debug envelope but not all.
        # The Pydantic model still accepts the payload and the missing
        # fields default to safe empty values.
        self._open_db()
        self.fake_engine.next_debug = {
            "tokens": [{"type": "KEYWORD_SELECT", "lexeme": "SELECT", "line": 1, "col": 1}],
            "ast_text": "SELECT 1",
            # plan_json missing → defaults to ""
            # plan_before_opt missing → defaults to ""
            # storage_stats missing → defaults to {}
            # replacement_log missing → defaults to []
        }
        self.fake_engine.next_blocks = [_block(True, "(1 row)", ["?column?"], [["1"]], "select")]
        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        debug = data["debug"]
        self.assertEqual(debug["plan_json"], "")
        self.assertEqual(debug["plan_before_opt"], "")
        # storage_stats default is all-zero, not None, because the
        # model wraps the raw dict in a StorageStats() constructor.
        self.assertEqual(debug["storage_stats"]["hit_count"], 0)
        self.assertEqual(debug["replacement_log"], [])

    def test_query_debug_skips_malformed_tokens_instead_of_500(self):
        # The C++ engine emits well-formed tokens, but a future schema
        # drift could land us with a missing `type` / `lexeme`.  The
        # API must NOT 500 — it skips the malformed entries, returns
        # the well-formed ones, and surfaces a count in `message`.
        self._open_db()
        self.fake_engine.next_debug = {
            "tokens": [
                {"line": 1, "col": 1},                                 # malformed
                {"type": "KEYWORD_SELECT", "lexeme": "SELECT", "line": 1, "col": 1},  # ok
                {"type": "INTEGER_LITERAL"},                            # malformed (no lexeme)
                {"type": "INTEGER_LITERAL", "lexeme": "1", "line": 1, "col": 8},     # ok
            ],
            "ast_text": "SELECT 1",
            "plan_json": '{"type":"Project"}',
            "plan_before_opt": "Project(1)",
        }
        self.fake_engine.next_blocks = [_block(True, "(1 row)", ["?column?"], [["1"]], "select")]
        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        debug = data["debug"]
        # Two well-formed tokens survive.
        self.assertEqual(len(debug["tokens"]), 2)
        self.assertEqual(debug["tokens"][0]["lexeme"], "SELECT")
        self.assertEqual(debug["tokens"][1]["lexeme"], "1")
        # The message surfaces the dropped count.
        self.assertIn("2 malformed", data["message"])

    def test_storage_reset_returns_zero_baseline_when_no_debug_yet(self):
        # No prior /api/query/debug → no stats cached.  /api/storage/reset
        # must still return a clean baseline (all zeros), not crash.
        self._open_db()
        self._seed_last_stats()  # default: None cache
        r = self.client.post("/api/storage/reset", json={})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        baseline = data["baseline"]
        self.assertEqual(baseline["hit_count"], 0)
        self.assertEqual(baseline["miss_count"], 0)
        self.assertEqual(baseline["replacement_count"], 0)
        self.assertEqual(baseline["total_pages"], 0)

    def test_storage_stats_after_db_switch_is_fresh(self):
        """Regression guard for the fix that resets the stats cache
        whenever a different DB is opened.  We simulate the bug by
        manually populating the cache, opening a new DB, and checking
        the cache is reset to None."""
        self._seed_last_stats(
            stats={
                "hit_count": 99, "miss_count": 99,
                "replacement_count": 99, "hit_rate": 0.5,
                "total_pages": 99,
            }
        )
        # Now switch DB by calling the actual /api/db/open endpoint.
        # The fake engine is configured (via monkey-patch) to be
        # returned for every SqlEngine() construction.
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as f:
            tmp = Path(f.name)
        try:
            r = self.client.post("/api/db/open", json={"db_path": str(tmp)})
            self.assertEqual(r.status_code, 200)
            # After open, the storage cache MUST be cleared so the user
            # doesn't see counters from the previous DB.
            self.assertIsNone(api_module.SESSION.last_stats_raw)
            self.assertIsNone(api_module.SESSION.last_repl_log_raw)
        finally:
            tmp.unlink(missing_ok=True)

    def test_storage_stats_after_db_switch_returns_empty(self):
        """Companion to the previous test: after a DB switch, the
        /api/storage/stats endpoint must report the 'run a statement'
        empty-state message — NOT the previous DB's counters."""
        self._seed_last_stats(stats={"hit_count": 99, "miss_count": 99,
                                     "replacement_count": 99, "hit_rate": 0.5,
                                     "total_pages": 99})
        import tempfile
        with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as f:
            tmp = Path(f.name)
        try:
            r = self.client.post("/api/db/open", json={"db_path": str(tmp)})
            self.assertEqual(r.status_code, 200)
            r = self.client.get("/api/storage/stats")
            self.assertEqual(r.status_code, 200)
            data = r.json()
            self.assertEqual(data["stats"]["hit_count"], 0)
            self.assertIn("Run a statement", data["message"])
        finally:
            tmp.unlink(missing_ok=True)

    def test_schema_tables_requires_open_db(self):
        r = self.client.post("/api/schema/tables", json={})
        self.assertEqual(r.status_code, 409)

    def test_schema_table_invalid_name_rejected(self):
        self._open_db()
        r = self.client.post("/api/schema/table", json={"table": "evil; DROP TABLE x"})
        self.assertEqual(r.status_code, 400)

    def test_schema_table_empty_name_rejected(self):
        self._open_db()
        r = self.client.post("/api/schema/table", json={"table": ""})
        # Pydantic Field(min_length=1) → 422; the handler-level check
        # also accepts the empty string as 400.  Either is fine.
        self.assertIn(r.status_code, (400, 422))

    def test_health_endpoint_always_200(self):
        # /api/health must NOT depend on the engine binary being present.
        # (It may return engine_exists=false but the HTTP status stays 200.)
        r = self.client.get("/api/health")
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertEqual(data["status"], "ok")

    def test_db_validate_endpoint_variants(self):
        # Drive the validator with the five terminal states.
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            new_path = tmpdir / "new.db"
            existing_path = tmpdir / "exists.db"
            existing_path.write_bytes(b"x" * 100)
            # 1. existing file
            r = self.client.post("/api/db/validate", json={"path": str(existing_path)})
            self.assertEqual(r.status_code, 200)
            self.assertEqual(r.json()["status"], "existing_file")
            # 2. new file (parent exists, .db suffix, doesn't yet exist)
            r = self.client.post("/api/db/validate", json={"path": str(new_path)})
            self.assertEqual(r.json()["status"], "new_file")
            # 3. parent missing
            bogus = Path("C:/this/should/not/exist_xyz/db.db")
            r = self.client.post("/api/db/validate", json={"path": str(bogus)})
            self.assertEqual(r.json()["status"], "parent_missing")
            # 4. directory
            r = self.client.post("/api/db/validate", json={"path": str(tmpdir)})
            self.assertEqual(r.json()["status"], "directory")
            # 5. wrong_type: existing file with non-.db suffix
            txt = tmpdir / "notes.txt"
            txt.write_text("hello")
            r = self.client.post("/api/db/validate", json={"path": str(txt)})
            self.assertEqual(r.json()["status"], "wrong_type")

    def test_db_browse_lists_db_files_and_dirs(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            tmpdir = Path(tmp)
            # Create one subdirectory and two .db files with different mtimes
            (tmpdir / "sub").mkdir()
            (tmpdir / "a.db").write_bytes(b"x")
            (tmpdir / "b.db").write_bytes(b"yy")
            r = self.client.post("/api/db/browse", json={"path": str(tmpdir)})
            self.assertEqual(r.status_code, 200)
            data = r.json()
            entries = data["entries"]
            kinds = [e["kind"] for e in entries]
            names = [e["name"] for e in entries]
            # The dir is always listed before the .db files.
            self.assertIn("dir", kinds)
            self.assertIn("db", kinds)
            self.assertIn("sub", names)
            self.assertIn("a.db", names)
            self.assertIn("b.db", names)

    def test_db_browse_nonexistent_path_returns_error_in_payload(self):
        # The browse endpoint must NOT raise for a bad path; it returns
        # 200 with an `error` field set so the UI can show a banner.
        bogus = Path("C:/this/should/not/exist_zzz")
        r = self.client.post("/api/db/browse", json={"path": str(bogus)})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertNotEqual(data["error"], "")
        self.assertEqual(data["entries"], [])

    def test_execute_empty_statement_returns_400(self):
        self._open_db()
        # The request handler rejects whitespace-only with 400 (it
        # reaches the empty-statement guard).  A fully-empty string
        # `""` is caught by Pydantic's min_length=1 and returns 422.
        # Both are valid rejections; we just verify the engine is
        # NOT invoked.
        for stmt, expected_code in (
            ("", 422),
            ("   ", 400),
            ("\n\n\t", 400),
        ):
            with self.subTest(stmt=repr(stmt)):
                self.fake_engine.calls.clear()
                r = self.client.post("/api/query/execute", json={"statement": stmt})
                self.assertEqual(r.status_code, expected_code)
                # And the engine was NOT called.
                self.assertEqual(self.fake_engine.calls, [])

    def test_query_debug_timing_fields_present(self):
        # The response must carry `total_elapsed_ms` and `db_path` so
        # the frontend can render a footer / banner without an extra
        # round-trip.
        self._open_db()
        self.fake_engine.next_debug = {
            "tokens": [], "ast_text": "", "plan_json": "",
            "plan_before_opt": "", "storage_stats": {},
            "replacement_log": [],
        }
        self.fake_engine.next_blocks = [_block(True, "(1 row)", ["x"], [["1"]], "select")]
        r = self.client.post("/api/query/debug", json={"statement": "SELECT 1"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertIn("total_elapsed_ms", data)
        self.assertGreaterEqual(data["total_elapsed_ms"], 0)
        self.assertEqual(data["db_path"], "/tmp/fake.db")

    def test_query_debug_message_on_failure(self):
        # When success=false, the response must carry the engine's
        # error message in `message` so the UI can display it inline
        # without re-parsing the results array.
        self._open_db()
        self.fake_engine.next_debug = None
        self.fake_engine.next_blocks = [_block(False, "Error: bad syntax", kind="error")]
        self.fake_engine.next_stderr = "Error: bad syntax\n"
        r = self.client.post("/api/query/debug", json={"statement": "BAD SQL"})
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertFalse(data["success"])
        self.assertIn("bad syntax", data["message"])

    def test_session_db_path_not_set_then_request_returns_409(self):
        # Multiple endpoints share the "no DB open" 409 contract.
        for path, method in (
            ("/api/storage/stats", "GET"),
            ("/api/storage/reset", "POST"),
            ("/api/schema/tables", "POST"),
            ("/api/query/debug", "POST"),
            ("/api/query/execute", "POST"),
        ):
            with self.subTest(path=path, method=method):
                if method == "GET":
                    r = self.client.get(path)
                else:
                    r = self.client.post(path, json={"statement": "SELECT 1"})
                self.assertEqual(r.status_code, 409,
                                 msg=f"{method} {path} should 409 without open DB")

    # ── helpers ────────────────────────────────────────────────────────

    def _open_db(self):
        # We don't actually open a DB; we set the SESSION directly so
        # subsequent calls bypass /api/db/open.
        api_module.SESSION.engine = self.fake_engine
        api_module.SESSION.db_path = "/tmp/fake.db"


class _FakeEngine:
    """Stub for SqlEngine — returns whatever `next_blocks` / `next_debug` say."""

    def __init__(self):
        self.next_blocks: list[ParsedBlock] = []
        self.next_debug: dict | None = None
        self.next_stderr: str = ""
        self.calls: list[tuple[str, str]] = []  # (method, statement)
        self.execute_kwargs: list[dict] = []  # kwargs passed to execute()

    async def execute(self, statement, *, on_error="abort", transaction=False):
        self.calls.append(("execute", statement))
        self.execute_kwargs.append({"on_error": on_error, "transaction": transaction})
        return EngineRunResult(
            success=all(b.success for b in self.next_blocks),
            blocks=list(self.next_blocks),
            stderr=self.next_stderr,
        )

    async def execute_debug(self, statement):
        self.calls.append(("execute_debug", statement))
        return (
            EngineRunResult(
                success=all(b.success for b in self.next_blocks),
                blocks=list(self.next_blocks),
                stderr=self.next_stderr,
            ),
            self.next_debug,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)