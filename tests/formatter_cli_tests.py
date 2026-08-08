#!/usr/bin/env python3

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


def run(compiler: str, *args: str, source: str | None = None) -> subprocess.CompletedProcess[str]:
    command = [compiler, *args]
    if source is None:
        return subprocess.run(
            command,
            text=True,
            capture_output=True,
            check=False,
        )
    with tempfile.TemporaryDirectory(prefix="compiler_formatter_source_") as directory:
        source_path = Path(directory) / "input.cd"
        source_path.write_text(source, encoding="utf-8")
        return subprocess.run(
            [*command, str(source_path)],
            text=True,
            capture_output=True,
            check=False,
        )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: formatter_cli_tests.py COMPILER", file=sys.stderr)
        return 2

    compiler = sys.argv[1]
    source = "let data={\"a\":1,\"b\":[2,3]}; // data\nprint data[\"a\"];\n"
    expected = (
        "let data = {\n"
        "  \"a\": 1,\n"
        "  \"b\": [2, 3]\n"
        "}; // data\n"
        "print data[\"a\"];\n"
    )

    source_result = run(compiler, "--format", source=source)
    if source_result.returncode != 0 or source_result.stdout != expected or source_result.stderr:
        print("source file formatter result mismatch", file=sys.stderr)
        print(source_result.stdout, file=sys.stderr)
        print(source_result.stderr, file=sys.stderr)
        return 1

    wide_result = run(compiler, "--format", "--format-indent-width", "4", source=source)
    wide_expected = expected.replace("  \"a\"", "    \"a\"").replace(
        "  \"b\"", "    \"b\"")
    if wide_result.returncode != 0 or wide_result.stdout != wide_expected or wide_result.stderr:
        print("custom formatter indentation result mismatch", file=sys.stderr)
        print(wide_result.stdout, file=sys.stderr)
        print(wide_result.stderr, file=sys.stderr)
        return 1

    check_noncanonical = run(compiler, "--format-check", source=source)
    if check_noncanonical.returncode != 1 or check_noncanonical.stdout or "format check failed:" not in check_noncanonical.stderr:
        print("noncanonical formatter check was not reported", file=sys.stderr)
        return 1

    check_canonical = run(compiler, "--format-check", source=expected)
    if check_canonical.returncode != 0 or check_canonical.stdout or check_canonical.stderr:
        print("canonical formatter check did not pass", file=sys.stderr)
        return 1

    missing_input = run(compiler, "--format")
    if (
        missing_input.returncode != 64
        or missing_input.stdout
        or "All non-LSP modes require at least one source file." not in missing_input.stderr
    ):
        print("formatter accepted missing source input", file=sys.stderr)
        return 1

    blank_lines = run(
        compiler,
        "--format",
        source="\n\nlet first=1;\n\n\n// between\n\n\nlet second=2;\n",
    )
    blank_expected = "let first = 1;\n\n// between\n\nlet second = 2;\n"
    if blank_lines.returncode != 0 or blank_lines.stdout != blank_expected or blank_lines.stderr:
        print("top-level blank-line formatter result mismatch", file=sys.stderr)
        print(blank_lines.stdout, file=sys.stderr)
        print(blank_lines.stderr, file=sys.stderr)
        return 1

    trailing_commas = run(
        compiler,
        "--format",
        source="enum Choice{First,Second,}\nlet value=match 1{1=>2,_=>0,};\n",
    )
    trailing_expected = (
        "enum Choice {\n"
        "  First,\n"
        "  Second,\n"
        "}\n"
        "let value = match 1 {\n"
        "  1 => 2,\n"
        "  _ => 0,\n"
        "};\n"
    )
    if trailing_commas.returncode != 0 or trailing_commas.stdout != trailing_expected or trailing_commas.stderr:
        print("trailing-comma formatter result mismatch", file=sys.stderr)
        print(trailing_commas.stdout, file=sys.stderr)
        print(trailing_commas.stderr, file=sys.stderr)
        return 1

    long_list = run(
        compiler,
        "--format",
        source=(
            "let values=[123456789012345678901234567890123456789012345,"
            "234567890123456789012345678901234567890123456,"
            "345678901234567890123456789012345678901234567];\n"
        ),
    )
    long_expected = (
        "let values = [\n"
        "  123456789012345678901234567890123456789012345,\n"
        "  234567890123456789012345678901234567890123456,\n"
        "  345678901234567890123456789012345678901234567\n"
        "];\n"
    )
    if long_list.returncode != 0 or long_list.stdout != long_expected or long_list.stderr:
        print("line-width formatter result mismatch", file=sys.stderr)
        print(long_list.stdout, file=sys.stderr)
        print(long_list.stderr, file=sys.stderr)
        return 1
    long_check = run(compiler, "--format-check", source=long_expected)
    if long_check.returncode != 0 or long_check.stdout or long_check.stderr:
        print("line-width canonical formatter check mismatch", file=sys.stderr)
        print(long_check.stdout, file=sys.stderr)
        print(long_check.stderr, file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="compiler_formatter_") as directory:
        root = Path(directory)
        entry = root / "entry.cd"
        dependency = root / "lib.cd"
        dependency.write_text("let value=1; export value;\n", encoding="utf-8")
        entry.write_text('import "./lib.cd";\nprint value;\n', encoding="utf-8")

        file_result = run(compiler, "--format", str(entry))
        expected_entry = 'import "./lib.cd";\nprint value;\n'
        if file_result.returncode != 0 or file_result.stdout != expected_entry or file_result.stderr:
            print("import entry formatter result mismatch", file=sys.stderr)
            print(file_result.stdout, file=sys.stderr)
            print(file_result.stderr, file=sys.stderr)
            return 1
        check_entry = run(compiler, "--format-check", str(entry))
        if check_entry.returncode != 0 or check_entry.stdout or check_entry.stderr:
            print("canonical import entry formatter check did not pass", file=sys.stderr)
            print(check_entry.stdout, file=sys.stderr)
            print(check_entry.stderr, file=sys.stderr)
            return 1

        first = root / "first.cd"
        second = root / "second.cd"
        first.write_text("let first = 1;\n", encoding="utf-8")
        second.write_text("print first;\n", encoding="utf-8")
        multi_result = run(compiler, "--format", str(first), str(second))
        if multi_result.returncode != 0 or multi_result.stdout != "let first = 1;\n\nprint first;\n" or multi_result.stderr:
            print("direct multi-file formatter result mismatch", file=sys.stderr)
            print(multi_result.stdout, file=sys.stderr)
            print(multi_result.stderr, file=sys.stderr)
            return 1
        check_multi = run(compiler, "--format-check", str(first), str(second))
        if check_multi.returncode != 0 or check_multi.stdout or check_multi.stderr:
            print("canonical direct multi-file formatter check did not pass", file=sys.stderr)
            print(check_multi.stdout, file=sys.stderr)
            print(check_multi.stderr, file=sys.stderr)
            return 1

    invalid = run(compiler, "--format", source="let =;")
    if invalid.returncode != 1 or invalid.stdout or not invalid.stderr.startswith("Parse error"):
        print("invalid formatter input did not use parser diagnostics", file=sys.stderr)
        return 1

    incomplete_cases = (
        ("let values=[1,2", "Parse error"),
        ("if (true) {", "Parse error"),
        ('let text="unfinished', "Lex error"),
    )
    for incomplete_source, diagnostic_prefix in incomplete_cases:
        incomplete = run(compiler, "--format", source=incomplete_source)
        if (
            incomplete.returncode != 1
            or incomplete.stdout
            or not incomplete.stderr.startswith(diagnostic_prefix)
        ):
            print("incomplete formatter input was not rejected without output", file=sys.stderr)
            print(incomplete.stdout, file=sys.stderr)
            print(incomplete.stderr, file=sys.stderr)
            return 1

    incomplete_check = run(compiler, "--format-check", source="let values=[1,2")
    if incomplete_check.returncode != 1 or incomplete_check.stdout or not incomplete_check.stderr.startswith("Parse error"):
        print("incomplete formatter check did not use parser diagnostics", file=sys.stderr)
        print(incomplete_check.stdout, file=sys.stderr)
        print(incomplete_check.stderr, file=sys.stderr)
        return 1

    conflict = run(compiler, "--format", "--tokens", source="let x=1;")
    if conflict.returncode != 64 or not conflict.stderr.startswith("Usage:"):
        print("formatter mode conflict was not rejected", file=sys.stderr)
        return 1

    invalid_width = run(compiler, "--format", "--format-indent-width", "0", source="let x=1;")
    if invalid_width.returncode != 64 or not invalid_width.stderr.startswith("--format-indent-width"):
        print("invalid formatter indentation width was not rejected", file=sys.stderr)
        return 1

    missing_format = run(compiler, "--format-indent-width", "4", source="let x=1;")
    if missing_format.returncode != 64 or not missing_format.stderr.startswith("--format-indent-width"):
        print("indentation width without formatter mode was not rejected", file=sys.stderr)
        return 1

    format_conflict = run(compiler, "--format", "--format-check", source="let x=1;")
    if format_conflict.returncode != 64 or not format_conflict.stderr.startswith("Usage:"):
        print("format and format-check conflict was not rejected", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
