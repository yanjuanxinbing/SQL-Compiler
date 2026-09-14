#!/usr/bin/env python3
"""verify_outputs.py - Batch verification of SQL compiler test outputs.

Compares tests/tmp/<name>.out against tests/expected/<name>.out for every
test case, prints a per-test pass/fail/skip line, a summary table, and
returns a non-zero exit code if any comparison fails.

Usage (from project root):
    python tests/verify_outputs.py
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TMP_DIR = HERE / "tmp"
EXP_DIR = HERE / "expected"


def main() -> int:
    expected = sorted(EXP_DIR.glob("*.out"))
    if not expected:
        print(f"[ERROR] no expected files in {EXP_DIR}", file=sys.stderr)
        return 2

    passed = failed = skipped = 0
    failed_names: list[str] = []

    for exp in expected:
        name = exp.stem
        actual = TMP_DIR / f"{name}.out"
        if not actual.exists():
            print(f"[SKIP] {name}  (missing tmp/{name}.out)")
            skipped += 1
            continue
        # byte-equal compare, ignoring trailing CR/LF noise
        if actual.read_bytes().rstrip(b"\r\n") == exp.read_bytes().rstrip(b"\r\n"):
            print(f"[PASS] {name}")
            passed += 1
        else:
            print(f"[FAIL] {name}")
            failed += 1
            failed_names.append(name)

    total = passed + failed + skipped
    print()
    print("=" * 56)
    print(f"  total={total}  passed={passed}  failed={failed}  skipped={skipped}")
    print("=" * 56)
    if failed:
        print("\nFailed tests:")
        for n in failed_names:
            print(f"  - {n}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
