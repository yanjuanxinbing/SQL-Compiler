"""Smoke test: drive the running FastAPI server with a real C++ engine.

Hits the visualization endpoints (/api/query/debug, /api/storage/*) end-to-end
so we exercise the full pipeline:

    HTTP request -> FastAPI -> backend.engine.execute_debug
        -> C++ sqlcompiler.exe --debug-output -> JSON debug data
        -> parses -> returns to test -> asserts shape

Usage (server already running on 127.0.0.1:8765):

    py -m backend.tests.smoke_against_server
"""

from __future__ import annotations

import json
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

BASE = "http://127.0.0.1:8765"


def _post(path: str, payload: dict | None = None) -> tuple[int, dict | str]:
    body = json.dumps(payload or {}).encode("utf-8")
    req = urllib.request.Request(
        BASE + path,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read().decode("utf-8") or "{}")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")
    except urllib.error.URLError as e:
        return 0, str(e)


def _get(path: str) -> tuple[int, dict | str]:
    try:
        with urllib.request.urlopen(BASE + path, timeout=30) as r:
            return r.status, json.loads(r.read().decode("utf-8") or "{}")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", errors="replace")
    except urllib.error.URLError as e:
        return 0, str(e)


def assert_eq(actual, expected, label: str) -> None:
    if actual != expected:
        raise AssertionError(f"{label}: expected {expected!r}, got {actual!r}")
    print(f"  [OK] {label}: {actual}")


def main() -> int:
    print(f"[smoke] targeting {BASE}")

    # 1) /api/health
    code, body = _get("/api/health")
    assert_eq(code, 200, "health status")
    assert body["status"] == "ok", body
    assert body["engine_exists"] is True, body
    print(f"  engine: {body['engine_path']}")

    # 2) Open a fresh db
    tmpdir = Path(tempfile.mkdtemp(prefix="smoke-db-"))
    db_path = tmpdir / "smoke.db"
    code, body = _post("/api/db/open", {"db_path": str(db_path)})
    assert_eq(code, 200, "db open status")
    assert body["db_path"].endswith("smoke.db"), body
    print(f"  opened: {db_path}")

    # 3) Create a table, insert rows
    code, body = _post("/api/query/execute", {"statement": "CREATE TABLE t (id INT, name TEXT)"})
    assert_eq(code, 200, "CREATE status")
    assert body["results"][0]["success"] is True, body

    code, body = _post("/api/query/execute", {"statement": "INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')"})
    assert_eq(code, 200, "INSERT status")
    assert body["results"][0]["success"] is True, body

    # 4) Run /api/query/debug for a SELECT
    code, body = _post("/api/query/debug", {"statement": "SELECT id, name FROM t WHERE id > 1"})
    assert_eq(code, 200, "debug status")
    assert body["success"] is True, body
    debug = body.get("debug") or {}
    tokens = debug.get("tokens") or []
    assert len(tokens) > 0, body
    assert debug.get("ast_text"), body
    assert debug.get("plan_json"), body
    print(f"  tokens: {len(tokens)}, AST len: {len(debug['ast_text'])}, plan len: {len(debug['plan_json'])}")

    # 5) /api/storage/stats
    code, body = _get("/api/storage/stats")
    assert_eq(code, 200, "storage/stats status")
    assert "stats" in body, body
    stats = body["stats"]
    assert "hit_count" in stats and "miss_count" in stats, body
    print(f"  storage: hits={stats['hit_count']} misses={stats['miss_count']} pages={stats.get('total_pages')}")

    # 6) /api/storage/reset
    code, body = _post("/api/storage/reset")
    assert_eq(code, 200, "storage/reset status")
    assert "baseline" in body, body
    baseline = body["baseline"]
    assert "hit_count" in baseline, body
    print(f"  baseline: hits={baseline['hit_count']} misses={baseline['miss_count']}")

    # 7) Run another SELECT to potentially produce delta
    code, body = _post("/api/query/debug", {"statement": "SELECT * FROM t"})
    assert_eq(code, 200, "debug2 status")

    code, body = _get("/api/storage/stats")
    assert_eq(code, 200, "storage/stats after reset status")
    print(f"  after-reset stats: {body['stats']}")

    # 8) Validation: empty statement is rejected at the protocol level (HTTP 400)
    code, body = _post("/api/query/execute", {"statement": "   "})
    assert_eq(code, 400, "empty-statement rejected")
    print(f"  empty-statement rejected at HTTP layer: {body!r}")

    # 9) Re-open a fresh DB; confirm stats reset to empty (no probe runs)
    new_db = tmpdir / "other.db"
    code, body = _post("/api/db/open", {"db_path": str(new_db)})
    assert_eq(code, 200, "db re-open status")

    # Storage stats should now be empty / "Run a statement" placeholder
    code, body = _get("/api/storage/stats")
    assert_eq(code, 200, "storage/stats after switch")
    s = body["stats"]
    assert s["hit_count"] == 0 and s["miss_count"] == 0, f"expected reset, got {s}"
    assert "Run a statement" in body.get("message", ""), body
    print(f"  storage reset on db switch: {s}")

    # Run a debug SELECT on the fresh db — this WILL produce counters,
    # but they're the new DB's counters (no carry-over from the old one).
    code, body = _post("/api/query/debug", {"statement": "SELECT 1"})
    assert_eq(code, 200, "debug on fresh db status")
    assert body["success"] is True, body

    print("\n[smoke] all checks passed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as e:
        print(f"\n[smoke] FAILED: {e}", file=sys.stderr)
        sys.exit(1)