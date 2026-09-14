#!/usr/bin/env python3
"""verify_outputs.py - Batch verification of SQL compiler tests.

For every ``tests/sql/<name>.sql`` (with a matching ``tests/expected/<name>.out``),
this script runs the ``sqlcompiler`` binary in REPL mode — the SQL file is piped
into the binary's stdin and a fresh per-test database file is provided via
argv — captures the output **in real time**, and compares it byte-for-byte
against the expected reference.

Unlike the previous version, no intermediate ``tests/tmp/<name>.out`` files are
read or produced by default; capture happens entirely in memory. Pass
``--keep-output`` to additionally write the actual stdout of failing tests to
``tests/verify_tmp/<name>.actual`` for debugging.

Discovery of the binary (first match wins):
    1. ``--bin <path>`` CLI argument
    2. ``SQLCOMPILER_BIN`` environment variable
    3. Platform-specific defaults under ``build/``

Usage (from project root):
    python tests/verify_outputs.py                  # run all 85 tests
    python tests/verify_outputs.py 00_smoke 01_ddl  # subset
    python tests/verify_outputs.py -j 8             # parallel
    python tests/verify_outputs.py --bin path/to/sqlcompiler.exe
    python tests/verify_outputs.py -k               # keep actuals on FAIL
    python tests/verify_outputs.py --filter '*cte*' # glob filter
    python tests/verify_outputs.py --list           # list test names
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent
SQL_DIR = HERE / "sql"
EXP_DIR = HERE / "expected"
# Persistent location used only when --keep-output is passed (debug aid).
# Always cleaned at the start of each run so stale outputs don't linger.
KEEP_DIR = HERE / "verify_tmp"


# ---------------------------------------------------------------------------
# Binary discovery
# ---------------------------------------------------------------------------

def find_binary(explicit: str | None) -> Path:
    """Locate the sqlcompiler binary using CLI > env > platform defaults."""
    if explicit:
        p = Path(explicit).expanduser().resolve()
        if not p.is_file():
            sys.exit(f"[ERROR] binary not found: {p}")
        return p
    env = os.environ.get("SQLCOMPILER_BIN")
    if env:
        p = Path(env).expanduser().resolve()
        if not p.is_file():
            sys.exit(f"[ERROR] SQLCOMPILER_BIN points to missing file: {p}")
        return p
    candidates: list[Path] = []
    if os.name == "nt":
        candidates.append(PROJECT_ROOT / "build" / "Debug" / "sqlcompiler.exe")
        candidates.append(PROJECT_ROOT / "build" / "Release" / "sqlcompiler.exe")
        candidates.append(PROJECT_ROOT / "build" / "sqlcompiler.exe")
    else:
        candidates.append(PROJECT_ROOT / "build" / "sqlcompiler")
        candidates.append(PROJECT_ROOT / "sqlcompiler")
    for c in candidates:
        if c.is_file():
            return c
    sys.exit(
        "[ERROR] sqlcompiler binary not found. Tried:\n  "
        + "\n  ".join(str(c) for c in candidates)
        + "\nPass --bin <path> or set SQLCOMPILER_BIN."
    )


# ---------------------------------------------------------------------------
# Output normalization & comparison
# ---------------------------------------------------------------------------

def normalize(data: bytes) -> bytes:
    """Normalize line endings (CRLF → LF) and strip trailing newlines.

    Trailing newline stripping matches the previous implementation so that
    the very last line of expected files (which may or may not end with \n)
    compares equal to whatever the binary produced before EOF.
    """
    return data.replace(b"\r\n", b"\n").rstrip(b"\n")


# ---------------------------------------------------------------------------
# Single test execution
# ---------------------------------------------------------------------------

@dataclass
class Result:
    name: str
    status: str  # "PASS" | "FAIL" | "SKIP" | "ERROR"
    detail: str = ""
    duration_ms: int = 0


def run_one(binary: Path, sql_path: Path, db_file: Path,
            timeout: int) -> tuple[bytes, int, str]:
    """Pipe ``sql_path`` into ``binary``'s stdin and capture combined output.

    The binary writes both prompts / results to ``stdout`` and error /
    diagnostic messages to ``stderr`` (see main.cpp's ``PrintResult`` path).
    The legacy ``run_all_tests.bat`` driver captured them as a single stream
    via ``2>&1``, so the reference ``tests/expected/*.out`` files contain the
    interleaved result. We mirror that by routing ``stderr`` into ``stdout``
    via ``subprocess.STDOUT`` — the OS pipe preserves the write order, so the
    captured bytes match what the shell driver produced.

    Returns ``(merged_stdout, returncode, error_message)``. ``error_message``
    is non-empty only when the subprocess itself failed (timeout, missing
    binary, etc.); an abnormal returncode from the binary is reported through
    ``returncode``.
    """
    start = time.monotonic()
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        with open(sql_path, "rb") as stdin_f:
            completed = subprocess.run(
                [str(binary), str(db_file)],
                stdin=stdin_f,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,  # merge: 2>&1, matches legacy driver
                timeout=timeout,
                creationflags=creationflags,
            )
        return completed.stdout, completed.returncode, ""
    except subprocess.TimeoutExpired:
        return (b"", -1,
                f"timeout after {timeout}s "
                f"(elapsed={int((time.monotonic() - start) * 1000)}ms)")
    except FileNotFoundError as e:
        return b"", -1, f"exec error: {e}"
    except Exception as e:  # pragma: no cover - defensive
        return b"", -1, f"{type(e).__name__}: {e}"


def verify(name: str, binary: Path, tmpdir: Path, timeout: int,
           keep_dir: Path | None) -> Result:
    sql_path = SQL_DIR / f"{name}.sql"
    exp_path = EXP_DIR / f"{name}.out"
    if not sql_path.is_file():
        return Result(name, "SKIP", f"missing sql/{name}.sql")
    if not exp_path.is_file():
        return Result(name, "SKIP", f"missing expected/{name}.out")

    db_file = tmpdir / f"{name}.db"
    actual, rc, err = run_one(binary, sql_path, db_file, timeout)
    if err:
        return Result(name, "ERROR", err)

    actual_n = normalize(actual)
    expected_n = normalize(exp_path.read_bytes())

    if actual_n == expected_n:
        return Result(name, "PASS")

    # Build a one-line diagnostic for FAIL. Order: most informative first.
    detail_parts: list[str] = []
    if rc != 0:
        detail_parts.append(f"exit={rc}")
    if keep_dir is not None:
        out_path = keep_dir / f"{name}.actual"
        out_path.write_bytes(actual)
        detail_parts.append(f"actual->{out_path.relative_to(HERE)}")
    return Result(name, "FAIL", "; ".join(detail_parts) or "output mismatch")


# ---------------------------------------------------------------------------
# Test selection & summary
# ---------------------------------------------------------------------------

def select_tests(args: argparse.Namespace) -> list[str]:
    all_names = sorted(p.stem for p in EXP_DIR.glob("*.out"))
    if not all_names:
        sys.exit(f"[ERROR] no expected files in {EXP_DIR}")
    if args.tests:
        names = list(args.tests)
    else:
        names = list(all_names)
    if args.filter:
        globs = args.filter
        names = [n for n in names
                 if any(fnmatch.fnmatch(n, g) for g in globs)]
    return names


def summarize(results: Iterable[Result]
              ) -> tuple[int, int, int, int, list[str], list[str]]:
    passed = failed = skipped = errored = 0
    failed_names: list[str] = []
    errored_names: list[str] = []
    for r in results:
        if r.status == "PASS":
            passed += 1
        elif r.status == "FAIL":
            failed += 1
            failed_names.append(r.name)
        elif r.status == "SKIP":
            skipped += 1
        else:
            errored += 1
            errored_names.append(r.name)
    return passed, failed, skipped, errored, failed_names, errored_names


def print_line(r: Result) -> None:
    suffix = f"  ({r.detail})" if r.detail else ""
    print(f"[{r.status}] {r.name}{suffix}")


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("tests", nargs="*",
                    help="Specific test names (without .sql/.out); default: all")
    ap.add_argument("-b", "--bin", help="Path to sqlcompiler binary")
    ap.add_argument("-j", "--jobs", type=int, default=1,
                    help="Parallel workers (default: 1 = sequential)")
    ap.add_argument("-k", "--keep-output", action="store_true",
                    help="Write actual stdout to tests/verify_tmp/<name>.actual "
                         "on FAIL (debug aid)")
    ap.add_argument("-f", "--filter", action="append", default=[],
                    metavar="GLOB",
                    help="Glob pattern(s) to filter test names (repeatable)")
    ap.add_argument("--timeout", type=int, default=60,
                    help="Per-test timeout in seconds (default: 60)")
    ap.add_argument("--list", action="store_true",
                    help="List test names and exit")
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)

    if args.list:
        for n in sorted(p.stem for p in EXP_DIR.glob("*.out")):
            print(n)
        return 0

    binary = find_binary(args.bin)
    names = select_tests(args)
    if not names:
        print("[ERROR] no tests selected", file=sys.stderr)
        return 2

    keep_dir: Path | None = None
    if args.keep_output:
        keep_dir = KEEP_DIR
        if keep_dir.exists():
            shutil.rmtree(keep_dir)
        keep_dir.mkdir(parents=True, exist_ok=True)

    print(f"[run] binary={binary}")
    print(f"[run] jobs={args.jobs} timeout={args.timeout}s "
          f"tests={len(names)}"
          f"{'  keep_output=' + str(keep_dir) if keep_dir else ''}")

    with tempfile.TemporaryDirectory(prefix="sqlcompiler-verify-") as tmpstr:
        tmpdir = Path(tmpstr)
        results: list[Result] = []

        if args.jobs > 1 and len(names) > 1:
            with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
                futs = [ex.submit(verify, n, binary, tmpdir,
                                  args.timeout, keep_dir) for n in names]
                for fut in cf.as_completed(futs):
                    results.append(fut.result())
        else:
            for n in names:
                results.append(verify(n, binary, tmpdir,
                                      args.timeout, keep_dir))

        # Restore stable, alphabetical (== expected file) ordering for output.
        order = {n: i for i, n in enumerate(names)}
        results.sort(key=lambda r: order.get(r.name, 1_000_000))

    print()
    for r in results:
        print_line(r)

    passed, failed, skipped, errored, failed_names, errored_names = summarize(results)
    total = passed + failed + skipped + errored
    print()
    print("=" * 60)
    print(f"  total={total}  passed={passed}  failed={failed}  "
          f"skipped={skipped}  errored={errored}")
    print("=" * 60)
    if failed_names:
        print("\nFailed tests:")
        for n in failed_names:
            print(f"  - {n}")
    if errored_names:
        print("\nErrored tests:")
        for n in errored_names:
            print(f"  - {n}")
    if keep_dir and (failed_names or errored_names):
        print(f"\n[info] actual outputs retained in {keep_dir}/")
    return 1 if (failed or errored) else 0


if __name__ == "__main__":
    sys.exit(main())