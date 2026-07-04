#!/usr/bin/env python3
# runner.py — pgcpp SQL regression test runner.
#
# Mirrors PostgreSQL's `make installcheck`-style regression framework:
# for each <name>.sql in the test directory, boot a FRESH pgcpp cluster,
# run the .sql via pgcpp_psql, capture the output, and diff it against
# <name>.expected.out. A test passes if the diff is empty.
#
# Each test runs against its own freshly-initialized data directory so that
# DDL/DML side effects from one test cannot pollute another (pgcpp's
# DROP TABLE does not yet unlink the underlying file, so reusing a cluster
# would cause spurious "relation file already exists" errors).
#
# Usage:
#   python3 runner.py \
#       --build-dir <build> \
#       --source-dir <source> \
#       [--test-dir <test/regression>] \
#       [--port 5433] \
#       [--keep-data] \
#       [-- <test-name> ...]
#
# If no test names are given, all *.sql files in the test directory are run.
# --keep-data preserves the data directories after the run for debugging.
#
# Exit code: 0 if all tests pass, 1 if any test fails or setup errors occur.

import argparse
import difflib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def find_binary(build_dir: Path, name: str) -> Path:
    """Locate a pgcpp binary under the build tree."""
    candidates = [
        build_dir / "tools" / f"pgcpp_{name}",
        build_dir / "src" / f"pgcpp_{name}",
        build_dir / f"pgcpp_{name}",
    ]
    for c in candidates:
        if c.exists() and os.access(c, os.X_OK):
            return c
    raise FileNotFoundError(f"cannot find pgcpp_{name} under {build_dir}")


def init_cluster(initdb: Path, data_dir: Path) -> None:
    """Run initdb to create a fresh cluster."""
    if data_dir.exists():
        shutil.rmtree(data_dir)
    data_dir.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(initdb), "-D", str(data_dir)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(
            f"initdb failed (rc={result.returncode}):\n"
            f"stdout: {result.stdout}\n"
            f"stderr: {result.stderr}\n"
        )
        sys.exit(1)


def start_server(server: Path, data_dir: Path, port: int,
                 log_file: Path, isready: Path) -> subprocess.Popen:
    """Start pgcpp_server in the background. Returns the Popen handle."""
    proc = subprocess.Popen(
        [str(server), "-D", str(data_dir), "-p", str(port)],
        stdout=open(log_file, "w"), stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    # Wait for the server to accept connections (max ~10 seconds).
    for _ in range(100):
        time.sleep(0.1)
        if proc.poll() is not None:
            sys.stderr.write(
                f"server exited prematurely with code {proc.returncode}; "
                f"see {log_file}\n"
            )
            sys.exit(1)
        isready_result = subprocess.run(
            [str(isready), "-h", "127.0.0.1", "-p", str(port)],
            capture_output=True, text=True,
        )
        if "accepting connections" in isready_result.stdout:
            return proc
    sys.stderr.write(f"server did not become ready within 10s; see {log_file}\n")
    sys.exit(1)


def stop_server(proc: subprocess.Popen) -> None:
    """Stop the server process gracefully."""
    if proc.poll() is not None:
        return
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        proc.wait()


def run_sql_file(psql: Path, port: int, sql_file: Path) -> str:
    """Execute a .sql file via pgcpp_psql -f and return combined stdout+stderr."""
    result = subprocess.run(
        [str(psql), "-h", "127.0.0.1", "-p", str(port), "-f", str(sql_file)],
        capture_output=True, text=True, timeout=60,
    )
    # psql prints query results to stdout; errors to stderr. We combine both
    # so error messages appear in the diff alongside any partial output.
    return result.stdout + result.stderr


def run_single_test(name: str, sql_path: Path, expected_path: Path,
                    binaries: dict, port: int, keep_data: bool) -> bool:
    """Boot a fresh cluster, run one .sql, diff against expected. Return True on pass."""
    data_dir = Path(tempfile.gettempdir()) / f"pgcpp_reg_{os.getpid()}_{name}"
    log_file = data_dir.with_suffix(f".{name}.log")

    print(f"  [{name}] initdb {data_dir}")
    init_cluster(binaries["initdb"], data_dir)

    print(f"  [{name}] starting server on port {port}")
    proc = start_server(binaries["server"], data_dir, port,
                        log_file, binaries["pg_isready"])
    try:
        actual = run_sql_file(binaries["psql"], port, sql_path)
    finally:
        stop_server(proc)
        if not keep_data:
            shutil.rmtree(data_dir, ignore_errors=True)
            log_file.unlink(missing_ok=True)

    if not expected_path.exists():
        sys.stderr.write(
            f"  [SKIP] {name}: expected file not found: {expected_path}\n"
        )
        return False

    expected = expected_path.read_text()
    if actual == expected:
        print(f"  [PASS] {name}")
        return True

    print(f"  [FAIL] {name}")
    diff = difflib.unified_diff(
        expected.splitlines(keepends=True),
        actual.splitlines(keepends=True),
        fromfile=f"{name}.expected.out",
        tofile=f"{name}.actual.out",
    )
    sys.stderr.write("".join(diff))
    if keep_data:
        sys.stderr.write(f"  [{name}] data dir preserved at {data_dir}\n")
        sys.stderr.write(f"  [{name}] server log at {log_file}\n")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="pgcpp SQL regression test runner")
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="Path to the CMake build directory")
    parser.add_argument("--source-dir", required=True, type=Path,
                        help="Path to the pgcpp source root (unused, kept for parity)")
    parser.add_argument("--test-dir", type=Path, default=None,
                        help="Directory containing *.sql and *.expected.out (default: <source-dir>/test/regression)")
    parser.add_argument("--port", type=int, default=5433,
                        help="Server listen port (default: 5433)")
    parser.add_argument("--keep-data", action="store_true",
                        help="Keep the data directories after the run for debugging")
    parser.add_argument("tests", nargs="*",
                        help="Specific test names to run (default: all)")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    if args.test_dir is not None:
        test_dir = args.test_dir.resolve()
    else:
        test_dir = Path(__file__).resolve().parent

    if not build_dir.exists():
        sys.stderr.write(f"build directory not found: {build_dir}\n")
        return 1

    binaries = {
        "initdb": find_binary(build_dir, "initdb"),
        "server": find_binary(build_dir, "server"),
        "psql": find_binary(build_dir, "psql"),
        "pg_isready": find_binary(build_dir, "pg_isready"),
    }

    if args.tests:
        test_names = args.tests
    else:
        test_names = sorted(p.stem for p in test_dir.glob("*.sql"))

    print(f"regression: test_dir={test_dir}")
    print(f"regression: build_dir={build_dir}")
    print(f"regression: port={args.port}")
    print(f"regression: {len(test_names)} test(s) to run")

    passed = 0
    failed = 0
    for name in test_names:
        sql_path = test_dir / f"{name}.sql"
        expected_path = test_dir / f"{name}.expected.out"
        if not sql_path.exists():
            sys.stderr.write(f"  [SKIP] {name}: .sql file not found\n")
            failed += 1
            continue
        if run_single_test(name, sql_path, expected_path,
                           binaries, args.port, args.keep_data):
            passed += 1
        else:
            failed += 1

    print(f"regression: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
