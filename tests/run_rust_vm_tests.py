#!/usr/bin/env python3
import argparse
import difflib
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class CheckResult:
    name: str
    passed: bool
    message: str = ""


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


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


def discover_artifact_cases(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    return sorted(
        path for path in root.iterdir()
        if path.is_dir()
        and ((path / "input.cd").is_file() or (path / "args.txt").is_file())
        and (path / "run.out").is_file()
    )


def discover_golden_cases(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    return sorted(
        path for path in root.iterdir()
        if path.is_dir()
        and ((path / "input.cd").is_file() or (path / "args.txt").is_file())
        and (path / "run.out").is_file()
    )


def discover_runtime_error_cases(root: Path) -> list[Path]:
    if not root.is_dir():
        return []
    return sorted(root.glob("*.cd"))


def expected_output(case_dir: Path) -> str:
    return read_text(case_dir / "run.out")


def expected_runtime_error(source: Path) -> tuple[str, str]:
    stem = source.with_suffix("")
    return read_text(stem.with_suffix(".run.err")), read_text(stem.with_suffix(".exit")).strip()


def emit_bytecode(compiler: Path, sources: list[Path], artifact: Path, name: str) -> tuple[list[CheckResult], bool]:
    compile_command = [str(compiler), "--emit-bytecode", str(artifact), *(str(source) for source in sources)]
    compiled = run_command(compile_command)
    results: list[CheckResult] = []
    compile_name = f"{name} emit"
    if compiled.returncode != 0:
        results.append(CheckResult(
            compile_name,
            False,
            f"FAIL {compile_name} exited with {compiled.returncode}\n\nSTDOUT:\n{compiled.stdout}\nSTDERR:\n{compiled.stderr}",
        ))
        return results, False
    if compiled.stdout:
        results.append(CheckResult(compile_name, False, f"FAIL {compile_name} produced unexpected stdout\n\n{compiled.stdout}"))
    if compiled.stderr:
        results.append(CheckResult(compile_name, False, f"FAIL {compile_name} produced unexpected stderr\n\n{compiled.stderr}"))
    if not artifact.is_file():
        results.append(CheckResult(compile_name, False, f"FAIL {compile_name} did not create {artifact}"))
        return results, False
    if not any(result.name == compile_name and not result.passed for result in results):
        results.append(CheckResult(compile_name, True))
    return results, True


def check_case(
    compiler: Path,
    vm_manifest: Path,
    case_dir: Path,
    include_emit_result: bool = True,
) -> list[CheckResult]:
    sources = compiler_inputs(case_dir)
    expected = expected_output(case_dir)
    results: list[CheckResult] = []
    run_name = f"{case_dir.name} rust-run"

    with tempfile.TemporaryDirectory() as temp_dir:
        artifact = Path(temp_dir) / "program.cdbc"
        emit_results, can_run = emit_bytecode(compiler, sources, artifact, case_dir.name)
        if include_emit_result:
            results.extend(emit_results)
        if not can_run:
            if not include_emit_result:
                failure = next((result for result in emit_results if not result.passed), None)
                message = failure.message if failure is not None else f"FAIL {run_name} could not prepare VM execution"
                results.append(CheckResult(run_name, False, message))
            return results

        run_command_line = ["cargo", "run", "--quiet", "--manifest-path", str(vm_manifest), "--", "run", str(artifact)]
        executed = run_command(run_command_line)
        if executed.returncode != 0:
            results.append(CheckResult(
                run_name,
                False,
                f"FAIL {run_name} exited with {executed.returncode}\n\nSTDOUT:\n{executed.stdout}\nSTDERR:\n{executed.stderr}",
            ))
            return results
        if executed.stderr:
            results.append(CheckResult(run_name, False, f"FAIL {run_name} produced unexpected stderr\n\n{executed.stderr}"))
        if executed.stdout != expected:
            results.append(CheckResult(
                run_name,
                False,
                f"FAIL {run_name} stdout mismatch\n\n" + unified_diff(expected, executed.stdout, "expected", "actual"),
            ))
        else:
            results.append(CheckResult(run_name, True))

    return results


def check_runtime_error_case(compiler: Path, vm_manifest: Path, source: Path) -> list[CheckResult]:
    expected_err, expected_exit = expected_runtime_error(source)
    results: list[CheckResult] = []

    with tempfile.TemporaryDirectory() as temp_dir:
        artifact = Path(temp_dir) / "program.cdbc"
        case_name = f"runtime_errors/{source.stem}"
        emit_results, can_run = emit_bytecode(compiler, [source], artifact, case_name)
        results.extend(emit_results)
        if not can_run:
            return results

        run_command_line = ["cargo", "run", "--quiet", "--manifest-path", str(vm_manifest), "--", "run", str(artifact)]
        executed = run_command(run_command_line)
        run_name = f"runtime_errors/{source.stem} rust-run"
        if executed.stdout:
            results.append(CheckResult(run_name, False, f"FAIL {run_name} produced unexpected stdout\n\n{executed.stdout}"))
        if executed.stderr != expected_err:
            results.append(CheckResult(
                run_name,
                False,
                f"FAIL {run_name} stderr mismatch\n\n" + unified_diff(expected_err, executed.stderr, "expected stderr", "actual stderr"),
            ))
        if str(executed.returncode) != expected_exit:
            results.append(CheckResult(
                run_name,
                False,
                f"FAIL {run_name} exit code mismatch\nexpected: {expected_exit}\nactual: {executed.returncode}",
            ))
        if not any(result.name == run_name and not result.passed for result in results):
            results.append(CheckResult(run_name, True))

    return results


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Compiler Design Rust VM integration tests.")
    parser.add_argument("compiler", type=Path, help="Path to compiler_design executable")
    parser.add_argument("vm", type=Path, help="Path to vm-rs directory or vm-rs/Cargo.toml")
    parser.add_argument("--case", action="append", dest="cases", help="Run only a case directory name. Can be repeated.")
    parser.add_argument("--goldens", action="store_true", help="Also run selected tests/golden fixtures with run outputs.")
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

    tests_root = Path(__file__).resolve().parent
    case_dirs = discover_artifact_cases(tests_root / "bytecode_artifacts")
    if args.goldens:
        case_dirs.extend(discover_golden_cases(tests_root / "golden"))

    if args.cases:
        wanted = set(args.cases)
        case_dirs = [path for path in case_dirs if path.name in wanted]

    results: list[CheckResult] = []
    for case_dir in case_dirs:
        results.extend(check_case(compiler, vm_manifest, case_dir))

    if args.goldens:
        runtime_sources = discover_runtime_error_cases(tests_root / "golden" / "runtime_errors")
        if args.cases:
            wanted = set(args.cases)
            runtime_sources = [source for source in runtime_sources if source.stem in wanted or f"runtime_errors/{source.stem}" in wanted]
        for source in runtime_sources:
            results.extend(check_runtime_error_case(compiler, vm_manifest, source))

    if not results:
        print("no Rust VM fixtures selected", file=sys.stderr)
        return 1

    failed = [result for result in results if not result.passed]
    for failure in failed:
        print(failure.message, file=sys.stderr)

    passed_count = len(results) - len(failed)
    print(f"rust vm tests: {passed_count} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
