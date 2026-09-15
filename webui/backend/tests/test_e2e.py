"""End-to-end smoke test for the visualization module.

This script exercises the full pipeline:

    browser  →  FastAPI backend  →  C++ sqlcompiler.exe --debug-output

It is designed to be run from the `webui/` directory as:

    python -m backend.tests.test_e2e

It starts a short-lived FastAPI server on a free port, opens a database,
runs a representative SQL statement via `/api/query/debug`, and asserts
that the response contains the expected debug data (tokens, AST, plan,
storage stats).  The server is torn down at the end.

If the C++ engine binary is missing (e.g. CI without a build), the
test is skipped rather than failing — the unit tests still cover the
Python-side parsing logic.
"""

from __future__ import annotations

import asyncio
import os
import socket
import sys
import time
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve()
_PKG_PARENT = _HERE.parents[2]  # webui/
if str(_PKG_PARENT) not in sys.path:
    sys.path.insert(0, str(_PKG_PARENT))

try:
    import httpx
    import uvicorn
except ImportError:
    httpx = None  # type: ignore
    uvicorn = None  # type: ignore

from backend.engine import find_engine_binary  # noqa: E402


def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _run_server(port: int) -> "uvicorn.Server":
    from backend.main import create_app
    config = uvicorn.Config(
        create_app(),
        host="127.0.0.1",
        port=port,
        log_level="warning",
        lifespan="off",
    )
    server = uvicorn.Server(config)
    return server


@unittest.skipIf(httpx is None or uvicorn is None, "httpx/uvicorn not installed")
@unittest.skipIf(find_engine_binary() is None, "sqlcompiler engine binary not found")
class TestE2EVisualization(unittest.TestCase):
    """Full-stack smoke test for /api/query/debug."""

    port: int
    server: "uvicorn.Server"
    client: "httpx.AsyncClient"
    db_path: Path

    @classmethod
    def setUpClass(cls):
        cls.port = _free_port()
        cls.server = _run_server(cls.port)
        cls.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(cls.loop)
        cls.loop.run_until_complete(cls._start_server())
        cls.db_path = Path(__file__).parent / f"_e2e_test_{cls.port}.db"
        if cls.db_path.exists():
            cls.db_path.unlink()

    @classmethod
    def tearDownClass(cls):
        if cls.server is not None:
            cls.server.should_exit = True
            try:
                cls.loop.run_until_complete(cls._wait_for_shutdown())
            except Exception:
                pass
        if cls.db_path.exists():
            cls.db_path.unlink()

    @classmethod
    async def _start_server(cls):
        task = asyncio.create_task(cls.server.serve())
        # Wait for the server to be ready (up to 5s).
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                async with httpx.AsyncClient() as c:
                    r = await c.get(f"http://127.0.0.1:{cls.port}/api/health")
                    if r.status_code == 200:
                        break
            except (httpx.ConnectError, OSError):
                pass
            await asyncio.sleep(0.1)
        else:
            raise RuntimeError("Server did not start in time")
        cls.client = httpx.AsyncClient(base_url=f"http://127.0.0.1:{cls.port}")

    @classmethod
    async def _wait_for_shutdown(cls):
        # Give the server a moment to drain.
        await asyncio.sleep(0.2)

    def _run(self, coro):
        return self.loop.run_until_complete(coro)

    def test_health(self):
        r = self._run(self.client.get("/api/health"))
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertEqual(data["status"], "ok")
        self.assertTrue(data["engine_exists"])

    def test_open_database(self):
        # Just verify the open call succeeds; is_new may be True or
        # False depending on whether a previous test run left the file
        # behind (the cleanup path is best-effort).
        r = self._run(self.client.post("/api/db/open", json={"db_path": str(self.db_path)}))
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        self.assertEqual(data["db_path"], str(self.db_path))

    def test_debug_query_returns_tokens_and_plan(self):
        # 1) open DB
        self._run(self.client.post("/api/db/open", json={"db_path": str(self.db_path)}))
        # 2) create a table via the normal execute endpoint
        self._run(self.client.post(
            "/api/query/execute",
            json={"statement": "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(50))"},
        ))
        self._run(self.client.post(
            "/api/query/execute",
            json={"statement": "INSERT INTO t VALUES (1, 'Alice')"},
        ))
        # 3) run a SELECT via /api/query/debug
        r = self._run(self.client.post(
            "/api/query/debug",
            json={"statement": "SELECT id, name FROM t WHERE id = 1"},
        ))
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"], msg=data.get("message"))
        debug = data["debug"]
        self.assertIsNotNone(debug)
        # tokens must be non-empty
        self.assertGreater(len(debug["tokens"]), 0)
        # first token should be SELECT
        self.assertEqual(debug["tokens"][0]["type"], "KEYWORD_SELECT")
        # AST text must mention the table
        self.assertIn("SELECT", debug["ast_text"])
        # plan_json must parse as JSON
        import json
        plan = json.loads(debug["plan_json"])
        self.assertIn("type", plan)
        # storage_stats must be present
        self.assertIsNotNone(debug["storage_stats"])
        self.assertGreaterEqual(debug["storage_stats"]["hit_count"], 0)
        # plan_before_opt must be non-empty (even for trivial queries)
        self.assertTrue(debug["plan_before_opt"])

    def test_debug_query_with_semantic_error(self):
        # Open DB and run a query that references a non-existent table.
        self._run(self.client.post("/api/db/open", json={"db_path": str(self.db_path)}))
        r = self._run(self.client.post(
            "/api/query/debug",
            json={"statement": "SELECT * FROM nonexistent_table"},
        ))
        self.assertEqual(r.status_code, 200)
        data = r.json()
        # The engine should report failure (semantic error) but still
        # return a parseable response.
        self.assertFalse(data["success"])
        self.assertTrue(data["message"])  # error message must be non-empty

    def test_storage_stats_endpoint(self):
        self._run(self.client.post("/api/db/open", json={"db_path": str(self.db_path)}))
        r = self._run(self.client.get("/api/storage/stats"))
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"])
        stats = data["stats"]
        self.assertIn("hit_count", stats)
        self.assertIn("miss_count", stats)
        self.assertIn("replacement_count", stats)
        self.assertIn("hit_rate", stats)
        self.assertIn("total_pages", stats)
        # hit_rate must be in [0, 1]
        self.assertGreaterEqual(stats["hit_rate"], 0)
        self.assertLessEqual(stats["hit_rate"], 1)

    def test_debug_query_with_join(self):
        # Exercise a more complex plan (JOIN) to ensure the plan JSON
        # serializes correctly for multi-node trees.
        self._run(self.client.post("/api/db/open", json={"db_path": str(self.db_path)}))
        self._run(self.client.post(
            "/api/query/execute",
            json={"statement": "CREATE TABLE a (id INT PRIMARY KEY, x INT)"},
        ))
        self._run(self.client.post(
            "/api/query/execute",
            json={"statement": "CREATE TABLE b (id INT PRIMARY KEY, y INT)"},
        ))
        self._run(self.client.post(
            "/api/query/execute",
            json={"statement": "INSERT INTO a VALUES (1, 10), (2, 20)"},
        ))
        self._run(self.client.post(
            "/api/query/execute",
            json={"statement": "INSERT INTO b VALUES (1, 100), (2, 200)"},
        ))
        r = self._run(self.client.post(
            "/api/query/debug",
            json={"statement": "SELECT a.id, a.x, b.y FROM a JOIN b ON a.id = b.id"},
        ))
        self.assertEqual(r.status_code, 200)
        data = r.json()
        self.assertTrue(data["success"], msg=data.get("message"))
        import json
        plan = json.loads(data["debug"]["plan_json"])
        # The plan must be a multi-node tree (Project over a join
        # variant over two SeqScans).  Walk it and verify both the
        # scan tables are referenced somewhere in the tree.
        plan_text = json.dumps(plan)
        self.assertIn("SeqScan", plan_text)
        self.assertIn('"a"', plan_text)
        self.assertIn('"b"', plan_text)
        # Join-type nodes can be reported as INNER / LEFT / RIGHT / CROSS
        # depending on the SQL; just confirm the tree has at least one
        # node whose type is one of those.
        def _find_join(node):
            if isinstance(node, dict):
                t = node.get("type", "")
                if t in ("INNER", "LEFT", "RIGHT", "FULL", "CROSS", "JOIN", "HashJoin", "NestedLoopJoin"):
                    return True
                for v in node.values():
                    if _find_join(v):
                        return True
            elif isinstance(node, list):
                return any(_find_join(x) for x in node)
            return False
        self.assertTrue(_find_join(plan), msg=f"No join node found in plan: {plan}")


if __name__ == "__main__":
    unittest.main(verbosity=2)