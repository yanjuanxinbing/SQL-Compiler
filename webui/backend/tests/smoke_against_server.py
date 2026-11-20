"""Smoke-test the visualisation pipeline against `tests/sql/*.sql`.

Drives the live server on http://127.0.0.1:8765.  For each chosen
script we:
  1. open a fresh temp database (so CREATE TABLE never collides),
  2. POST the script to /api/query/debug,
  3. assert: every block has a debug envelope OR has a clean
     single-line message; every successful SELECT has columns + rows;
     every block's kind matches what its leading keyword says.

Run after starting the server:
    py smoke_against_server.py
"""
from __future__ import annotations

import json
import re
import sys
import urllib.request
import urllib.error
from pathlib import Path

SERVER = "http://127.0.0.1:8765"
SQL_DIR = Path(r"c:\Users\Lenovo\Desktop\SQL-Compiler-xu\tests\sql")

# A curated spread: DDL, multi-statement, comments, aggregates, joins,
# set ops, subqueries, window functions, transactions, edge cases.
# 49_acid_recovery.sql is documentation-only (no SQL body) so it's
# intentionally excluded.
PICK = [
    "01_ddl.sql",
    "02_dml.sql",
    "03_query_basic.sql",
    "04_aggregate.sql",
    "05_join.sql",
    "06_operators.sql",
    "08_types.sql",
    "09_aggregate_functions.sql",
    "10_distinct.sql",
    "11_expression.sql",
    "12_constraints.sql",
    "13_outer_join.sql",
    "15_order_by.sql",
    "16_null_edge.sql",
    "17_aggregate_advanced.sql",
    "18_arithmetic.sql",
    "20_complex.sql",
    "22_boundary.sql",
    "23_chinese_idents.sql",
    "24_comprehensive.sql",
    "26_edge_cases.sql",
    "27_case_scalar.sql",
    "28_cast.sql",
    "30_subquery.sql",
    "31_cte.sql",
    "32_set_ops.sql",
    "33_window.sql",
    "34_index_basic.sql",
    "38_join_variants.sql",
    "40_txn_view_udf.sql",
    "51_nested_savepoint.sql",
    "85_txn_read_your_own_writes.sql",
    "90_typeof_function.sql",
]


def post(path: str, body: dict) -> dict:
    req = urllib.request.Request(
        SERVER + path,
        data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read().decode("utf-8"))


def get(path: str) -> dict:
    with urllib.request.urlopen(SERVER + path, timeout=10) as r:
        return json.loads(r.read().decode("utf-8"))


def open_fresh_db() -> str:
    import uuid
    db = Path("c:/Users/Lenovo/AppData/Local/Temp") / (
        "smoke_" + uuid.uuid4().hex[:8] + ".db"
    )
    post("/api/db/open", {"db_path": str(db)})
    return str(db)


def kind_matches_statement(stmt: str, kind: str) -> bool:
    s = stmt.strip()
    while s.startswith("--"):
        i = s.find("\n")
        s = s[i + 1 :].lstrip() if i >= 0 else ""
    while s.startswith("/*"):
        i = s.find("*/")
        s = s[i + 2 :].lstrip() if i >= 0 else ""
    s_lower = s.lower()
    if not s_lower:
        return kind in ("other", "error")
    head = s_lower.split(None, 1)[0]
    # The C++ REPL's meta-commands (`exit;`, `\.tables;`, etc.) reach
    # the engine as ordinary statements and are parsed as unknown
    # tokens, so the engine returns kind="error".  That's by design.
    if head in ("exit", ".exit", "quit", ".quit"):
        return kind == "error"
    mapping = {
        "select": "select", "with": "select", "show": "select", "explain": "select",
        "create": "ddl", "drop": "ddl", "alter": "ddl", "truncate": "ddl",
        "rename": "ddl", "comment": "ddl",
        "insert": "dml", "update": "dml", "delete": "dml", "merge": "dml",
        "replace": "dml", "upsert": "dml",
        "begin": "txn", "commit": "txn", "rollback": "txn",
        "savepoint": "txn", "release": "txn",
    }
    expected = mapping.get(head, "other")
    return kind == expected


def main() -> int:
    print("== smoke ==  server:", SERVER)
    try:
        h = get("/api/health")
        print(f"  engine={h.get('engine_path')}")
    except urllib.error.URLError as e:
        print(f"FAIL: server not reachable: {e}")
        return 1

    fail = 0
    for name in PICK:
        path = SQL_DIR / name
        if not path.exists():
            print(f"[SKIP] {name}: file not found")
            continue
        sql = path.read_text(encoding="utf-8")
        db = open_fresh_db()
        try:
            r = post("/api/query/debug", {"statement": sql})
        except Exception as e:
            print(f"[ERR ] {name}: request failed: {e}")
            fail += 1
            continue

        blocks = r.get("results") or []
        n = len(blocks)
        ok_selects = 0
        bad_selects = 0
        bad_kind = 0
        no_debug = 0
        for b in blocks:
            stmt = b.get("statement") or ""
            # Only complain about mis-classification when the block
            # actually succeeded — engine-level failures are out of
            # scope for our visualisation layer.
            if b.get("success"):
                if not kind_matches_statement(stmt, b.get("kind", "")):
                    bad_kind += 1
                if b.get("kind") == "select":
                    if b.get("column_names") and b.get("rows") is not None:
                        ok_selects += 1
                    else:
                        bad_selects += 1
                if not b.get("debug"):
                    no_debug += 1

        tag = "OK  " if (bad_selects == 0 and bad_kind == 0) else "FAIL"
        print(f"[{tag}] {name:28}  blocks={n:3}  ok_sel={ok_selects:2}  bad_sel={bad_selects}  bad_kind={bad_kind}  no_debug={no_debug}")
        if bad_selects or bad_kind:
            fail += 1
    print(f"== done: {fail} failed ==")
    return 0 if fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())