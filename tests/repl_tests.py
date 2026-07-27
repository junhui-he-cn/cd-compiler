#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def run_repl(compiler: Path, vm_manifest: Path, transcript: str) -> subprocess.CompletedProcess[str]:
    repl = Path(__file__).resolve().parents[1] / "tools" / "repl.py"
    return subprocess.run(
        [sys.executable, str(repl), str(compiler), str(vm_manifest)],
        input=transcript,
        text=True,
        capture_output=True,
        check=False,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} COMPILER VM_MANIFEST", file=sys.stderr)
        return 2

    compiler = Path(sys.argv[1]).resolve()
    vm_manifest = Path(sys.argv[2]).resolve()

    session = run_repl(
        compiler,
        vm_manifest,
        """let value = 1;

print value;

value = 2;

print value;

let broken = missing;

print value;

:reset

print value;

let value = 7;

print value;

:quit
""",
    )
    require(session.returncode == 0, f"session returned {session.returncode}: {session.stderr}")
    require(session.stdout == "1\n2\n2\n7\n", f"unexpected session stdout: {session.stdout!r}")
    require(session.stderr.count("Type error") == 2, f"unexpected session diagnostics: {session.stderr!r}")
    require("compiler-repl-" not in session.stderr, f"session diagnostics leaked temp path: {session.stderr!r}")

    runtime = run_repl(
        compiler,
        vm_manifest,
        """let values = [1];

print values[0];

print values[1];

print values[0];

:quit
""",
    )
    require(runtime.returncode == 0, f"runtime session returned {runtime.returncode}: {runtime.stderr}")
    require(runtime.stdout == "1\n1\n", f"runtime rollback leaked output: {runtime.stdout!r}")
    require(
        "array index out of range" in runtime.stderr,
        f"runtime diagnostic missing: {runtime.stderr!r}",
    )
    require("compiler-repl-" not in runtime.stderr, f"runtime diagnostic leaked temp path: {runtime.stderr!r}")

    multiline = run_repl(
        compiler,
        vm_manifest,
        """fun add(a: number, b: number): number {
  return a + b;
}

print add(2, 3);

:quit
""",
    )
    require(multiline.returncode == 0, f"multiline session returned {multiline.returncode}: {multiline.stderr}")
    require(multiline.stdout == "5\n", f"unexpected multiline stdout: {multiline.stdout!r}")
    require(multiline.stderr == "", f"unexpected multiline stderr: {multiline.stderr!r}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
