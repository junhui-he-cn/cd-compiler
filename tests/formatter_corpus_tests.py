#!/usr/bin/env python3

"""Run the first formatter gate against the existing successful source corpus."""

from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path


ERROR_CASES = {"runtime_errors", "parse_errors", "type_errors", "import_errors"}
EXPECTED_CASE_COUNT = 249


def compiler_inputs(case_dir: Path) -> list[Path]:
    args_path = case_dir / "args.txt"
    if args_path.is_file():
        return [case_dir / entry for entry in args_path.read_text(encoding="utf-8").split()]
    return [case_dir / "input.cd"]


def run(compiler: str, *args: str, source: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [compiler, *args],
        input=source,
        text=True,
        capture_output=True,
        check=False,
    )


def comments(source: str) -> list[str]:
    result: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(source):
        character = source[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            index += 1
            continue
        if character == '"':
            in_string = True
            index += 1
            continue
        if character == "/" and index + 1 < len(source) and source[index + 1] == "/":
            end = source.find("\n", index)
            if end == -1:
                end = len(source)
            result.append(source[index:end])
            index = end
            continue
        index += 1
    return result


def has_import(source: str) -> bool:
    return bool(re.search(r"(?:^|\s)import\s+\"|export\s+.*?from\s+\"", source))


def fail(case_dir: Path, message: str, completed: subprocess.CompletedProcess[str] | None = None) -> None:
    print(f"FAIL {case_dir.name}: {message}", file=sys.stderr)
    if completed is not None:
        print(f"stdout={completed.stdout!r}", file=sys.stderr)
        print(f"stderr={completed.stderr!r}", file=sys.stderr)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: formatter_corpus_tests.py COMPILER", file=sys.stderr)
        return 2

    compiler = sys.argv[1]
    repo_root = Path(__file__).resolve().parents[1]
    case_dirs = sorted(
        case_dir
        for case_dir in (repo_root / "tests" / "golden").iterdir()
        if case_dir.is_dir()
        and case_dir.name not in ERROR_CASES
        and ((case_dir / "input.cd").is_file() or (case_dir / "args.txt").is_file())
    )
    if len(case_dirs) != EXPECTED_CASE_COUNT:
        print(
            f"formatter corpus count changed: expected {EXPECTED_CASE_COUNT}, got {len(case_dirs)}; update the named gate",
            file=sys.stderr,
        )
        return 1

    checked = 0
    for case_dir in case_dirs:
        source_paths = compiler_inputs(case_dir)
        source_texts = [path.read_text(encoding="utf-8") for path in source_paths]
        import_case = any(has_import(source) for source in source_texts)

        for source_path, source_text in zip(source_paths, source_texts):
            formatted_result = run(compiler, "--format", str(source_path))
            if formatted_result.returncode != 0 or formatted_result.stderr:
                fail(case_dir, f"formatting {source_path.name} failed", formatted_result)
                return 1
            formatted = formatted_result.stdout
            if comments(source_text) != comments(formatted):
                fail(case_dir, f"comments changed for {source_path.name}")
                return 1

            if import_case:
                # Relative imports need the original directory context.  A
                # sibling temporary file preserves that context while keeping
                # the checkout untouched.
                temporary_path: Path | None = None
                try:
                    with tempfile.NamedTemporaryFile(
                        mode="w",
                        encoding="utf-8",
                        suffix=".cd",
                        prefix=".formatted_",
                        dir=source_path.parent,
                        delete=False,
                    ) as temporary:
                        temporary.write(formatted)
                        temporary_path = Path(temporary.name)
                    reformatted_result = run(compiler, "--format", str(temporary_path))
                finally:
                    if temporary_path is not None:
                        temporary_path.unlink(missing_ok=True)
            else:
                reformatted_result = run(compiler, "--format", source=formatted)

            if reformatted_result.returncode != 0 or reformatted_result.stderr:
                fail(case_dir, f"formatted {source_path.name} could not be parsed", reformatted_result)
                return 1
            if reformatted_result.stdout != formatted:
                fail(case_dir, f"formatting {source_path.name} is not idempotent")
                return 1

        if not import_case:
            with tempfile.TemporaryDirectory(prefix="compiler_formatter_corpus_") as directory:
                temporary_sources: list[Path] = []
                for index, source_text in enumerate(source_texts):
                    temporary_source = Path(directory) / f"source_{index}.cd"
                    formatted_result = run(compiler, "--format", source=source_text)
                    if formatted_result.returncode != 0:
                        fail(case_dir, "could not format source for AST comparison", formatted_result)
                        return 1
                    temporary_source.write_text(formatted_result.stdout, encoding="utf-8")
                    temporary_sources.append(temporary_source)

                original_ast = run(compiler, *(str(path) for path in source_paths))
                formatted_ast = run(compiler, *(str(path) for path in temporary_sources))
                if (
                    original_ast.returncode != 0
                    or formatted_ast.returncode != 0
                    or original_ast.stderr
                    or formatted_ast.stderr
                    or original_ast.stdout != formatted_ast.stdout
                ):
                    fail(case_dir, "formatted sources changed AST/semantic output", formatted_ast)
                    return 1

        checked += 1

    print(f"formatter corpus: {checked}/{len(case_dirs)} cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
