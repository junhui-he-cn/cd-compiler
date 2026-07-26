#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


def run(compiler: Path, args: list[str], source: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(compiler), *args],
        input=source,
        text=True,
        capture_output=True,
        check=False,
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} COMPILER", file=sys.stderr)
        return 2

    compiler = Path(sys.argv[1])
    result = run(compiler, [], "print 1 @ 2 & 3;\n")
    expected = (
        "Lex error at 1:9: unexpected character `@`\n"
        "  print 1 @ 2 & 3;\n"
        "          ^\n"
        "Lex error at 1:13: unexpected character `&`\n"
        "  print 1 @ 2 & 3;\n"
        "              ^\n"
    )
    require(result.returncode == 1, f"unexpected stdin exit: {result.returncode}")
    require(result.stdout == "", f"unexpected stdin stdout: {result.stdout!r}")
    require(result.stderr == expected, f"unexpected stdin stderr:\n{result.stderr}")

    with tempfile.TemporaryDirectory(prefix="compiler-lexer-recovery-") as directory:
        root = Path(directory)
        first = root / "first.cd"
        second = root / "second.cd"
        first.write_text("print 1 @ 2;\n", encoding="utf-8")
        second.write_text("print 3 # 4;\n", encoding="utf-8")
        result = run(compiler, [str(first), str(second)])
        require(result.returncode == 1, f"unexpected multi-file exit: {result.returncode}")
        require(result.stdout == "", f"unexpected multi-file stdout: {result.stdout!r}")
        require(
            result.stderr
            == (
                f"Lex error at {first}:1:9: unexpected character `@`\n"
                "  print 1 @ 2;\n"
                "          ^\n"
                f"Lex error at {second}:1:9: unexpected character `#`\n"
                "  print 3 # 4;\n"
                "          ^\n"
            ),
            f"unexpected multi-file stderr:\n{result.stderr}",
        )

    result = run(compiler, [], 'print "unterminated;\n')
    require(result.returncode == 1, f"unexpected unterminated-string exit: {result.returncode}")
    require(result.stdout == "", f"unexpected unterminated-string stdout: {result.stdout!r}")
    require(result.stderr.count("Lex error") == 1, f"expected one unterminated-string error: {result.stderr!r}")
    require("unterminated string" in result.stderr, f"missing unterminated-string diagnostic: {result.stderr!r}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL {error}", file=sys.stderr)
        raise SystemExit(1)
