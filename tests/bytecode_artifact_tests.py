#!/usr/bin/env python3
import argparse
import difflib
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import boundary_comparison


@dataclass(frozen=True)
class CheckResult:
    name: str
    passed: bool
    message: str = ""


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def normalize_checkout_paths(text: str) -> str:
    return boundary_comparison.canonicalize(text)


def compiler_inputs(case_dir: Path) -> list[Path]:
    args_path = case_dir / "args.txt"
    if args_path.is_file():
        entries = read_text(args_path).split()
        return [case_dir / entry for entry in entries]
    return [case_dir / "input.cd"]


def unified_diff(expected: str, actual: str, fromfile: str, tofile: str) -> str:
    return "".join(
        difflib.unified_diff(
            expected.splitlines(keepends=True),
            actual.splitlines(keepends=True),
            fromfile=fromfile,
            tofile=tofile,
        )
    )


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def discover_cases(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    return sorted(
        path for path in root.iterdir()
        if path.is_dir()
        and ((path / "input.cd").is_file() or (path / "args.txt").is_file())
        and (path / "expected.cdbc").is_file()
    )


def check_case(compiler: Path, vm_manifest: Path, case_dir: Path) -> list[CheckResult]:
    sources = compiler_inputs(case_dir)
    expected_path = case_dir / "expected.cdbc"
    expected = read_text(expected_path)
    results: list[CheckResult] = []

    with tempfile.TemporaryDirectory() as temp_dir:
        actual_path = Path(temp_dir) / "actual.cdbc"
        compile_command = [str(compiler), "--emit-bytecode", str(actual_path), *(str(source) for source in sources)]
        compiled = run_command(compile_command)
        compile_name = f"{case_dir.name} emit"
        if compiled.returncode != 0:
            results.append(CheckResult(
                compile_name,
                False,
                f"FAIL {compile_name} exited with {compiled.returncode}\n\nSTDOUT:\n{compiled.stdout}\nSTDERR:\n{compiled.stderr}",
            ))
            return results
        if compiled.stdout:
            results.append(CheckResult(compile_name, False, f"FAIL {compile_name} produced unexpected stdout\n\n{compiled.stdout}"))
        if compiled.stderr:
            results.append(CheckResult(compile_name, False, f"FAIL {compile_name} produced unexpected stderr\n\n{compiled.stderr}"))
        if not actual_path.is_file():
            results.append(CheckResult(compile_name, False, f"FAIL {compile_name} did not create {actual_path}"))
            return results

        actual = normalize_checkout_paths(read_text(actual_path))
        canonical_expected = normalize_checkout_paths(expected)
        if actual != canonical_expected:
            results.append(CheckResult(
                compile_name,
                False,
                f"FAIL {compile_name} artifact mismatch\n\n"
                + unified_diff(canonical_expected, actual, "expected", "actual"),
            ))
        else:
            results.append(CheckResult(compile_name, True))

        dump_command = ["cargo", "run", "--quiet", "--manifest-path", str(vm_manifest), "--", "dump", str(actual_path)]
        dumped = run_command(dump_command)
        dump_name = f"{case_dir.name} rust-dump"
        if dumped.returncode != 0:
            results.append(CheckResult(
                dump_name,
                False,
                f"FAIL {dump_name} exited with {dumped.returncode}\n\nSTDOUT:\n{dumped.stdout}\nSTDERR:\n{dumped.stderr}",
            ))
            return results
        if dumped.stderr:
            results.append(CheckResult(dump_name, False, f"FAIL {dump_name} produced unexpected stderr\n\n{dumped.stderr}"))
        dumped_stdout = normalize_checkout_paths(dumped.stdout)
        if dumped_stdout != canonical_expected:
            results.append(CheckResult(
                dump_name,
                False,
                f"FAIL {dump_name} stdout mismatch\n\n"
                + unified_diff(canonical_expected, dumped_stdout, "expected", "actual"),
            ))
        else:
            results.append(CheckResult(dump_name, True))

    return results


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Compiler Design bytecode artifact integration tests.")
    parser.add_argument("compiler", type=Path, help="Path to compiler_design executable")
    parser.add_argument("vm", type=Path, help="Path to vm-rs directory or vm-rs/Cargo.toml")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    compiler = args.compiler.resolve()
    if not compiler.is_file():
        print(f"compiler not found: {compiler}", file=sys.stderr)
        return 64

    vm_path = args.vm.resolve()
    vm_manifest = vm_path / "Cargo.toml" if vm_path.is_dir() else vm_path
    if not vm_manifest.is_file():
        print(f"Rust VM manifest not found: {vm_manifest}", file=sys.stderr)
        return 64

    root = Path(__file__).resolve().parent / "bytecode_artifacts"
    cases = discover_cases(root)
    if not cases:
        print(f"no bytecode artifact fixtures found under {root}", file=sys.stderr)
        return 1

    results: list[CheckResult] = []
    for case_dir in cases:
        results.extend(check_case(compiler, vm_manifest, case_dir))

    failed = [result for result in results if not result.passed]
    for failure in failed:
        print(failure.message, file=sys.stderr)

    passed_count = len(results) - len(failed)
    print(f"bytecode artifact tests: {passed_count} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
