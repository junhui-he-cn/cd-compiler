#!/usr/bin/env python3

"""Run the versioned, stage-aware verification inventory.

This module owns orchestration only.  Fixture assertions remain in the existing
golden, artifact, and Rust VM runners and are imported here rather than
reimplemented.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
sys.path.insert(0, str(TESTS_DIR))

import bytecode_artifact_tests  # noqa: E402
import bytecode_module_cache_tests  # noqa: E402
import boundary_comparison  # noqa: E402
import run_golden_tests  # noqa: E402
import run_boundary_tests  # noqa: E402
import run_malformed_tests  # noqa: E402
import run_rust_vm_tests  # noqa: E402
import verification_inventory  # noqa: E402


@dataclass(frozen=True)
class RecordedResult:
    case_id: str
    passed: bool
    message: str = ""


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def command_display(command: Iterable[str]) -> str:
    return " ".join(str(part) for part in command)


def process_failure(command: list[str], completed: subprocess.CompletedProcess[str]) -> str:
    parts = [
        f"command exited with {completed.returncode}: {command_display(command)}",
    ]
    if completed.stdout:
        parts.extend(["STDOUT:", completed.stdout.rstrip()])
    if completed.stderr:
        parts.extend(["STDERR:", completed.stderr.rstrip()])
    return "\n".join(parts)


def run_command(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)


def result_name_map(
    checks: list[dict[str, object]],
    runner: str,
) -> dict[str, dict[str, object]]:
    selected = [check for check in checks if check.get("runner") == runner]
    mapping: dict[str, dict[str, object]] = {}
    for check in selected:
        result_name = str(check["result_name"])
        if result_name in mapping:
            raise ValueError(f"inventory has duplicate {runner} result_name: {result_name}")
        mapping[result_name] = check
    return mapping


def record_runner_results(
    checks: list[dict[str, object]],
    runner: str,
    raw_results: Iterable[Any],
) -> tuple[list[RecordedResult], list[str]]:
    expected = result_name_map(checks, runner)
    recorded: dict[str, RecordedResult] = {}
    extras: list[str] = []

    for raw_result in raw_results:
        result_name = str(raw_result.name)
        check = expected.get(result_name)
        if check is None:
            extras.append(f"{runner}: untracked result {result_name}")
            continue
        case_id = str(check["case_id"])
        if result_name in recorded:
            extras.append(f"{runner}: duplicate result {result_name}")
            continue
        recorded[result_name] = RecordedResult(case_id, bool(raw_result.passed), str(raw_result.message))

    results: list[RecordedResult] = []
    for result_name, check in expected.items():
        result = recorded.get(result_name)
        if result is None:
            results.append(
                RecordedResult(
                    str(check["case_id"]),
                    False,
                    f"{runner} did not report expected result: {result_name}",
                )
            )
        else:
            results.append(result)
    return results, extras


def run_imported_runner(
    checks: list[dict[str, object]],
    runner: str,
    callback,
) -> tuple[list[RecordedResult], list[str]]:
    try:
        raw_results = callback()
    except Exception as error:  # pragma: no cover - defensive boundary for external runners
        expected = result_name_map(checks, runner)
        results = [
            RecordedResult(
                str(check["case_id"]),
                False,
                f"{runner} raised {type(error).__name__}: {error}",
            )
            for check in expected.values()
        ]
        return results, []
    return record_runner_results(checks, runner, raw_results)


def run_golden_suite(compiler: Path, checks: list[dict[str, object]]) -> tuple[list[RecordedResult], list[str]]:
    return run_imported_runner(
        checks,
        "golden",
        lambda: run_golden_tests.run_all(compiler, TESTS_DIR / "golden", update=False),
    )


def run_boundary_token_suite(
    compiler: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    return run_imported_runner(
        checks,
        "boundary_tokens",
        lambda: run_boundary_tests.run_all(compiler),
    )


def run_malformed_suite(
    compiler: Path,
    vm_manifest: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    return run_imported_runner(
        checks,
        "malformed",
        lambda: run_malformed_tests.run_all(
            compiler,
            vm_manifest,
            report_path=compiler.parent / "malformed-report.json",
        ),
    )


def run_artifact_suite(
    compiler: Path,
    vm_manifest: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    def callback():
        raw_results = []
        for case_dir in bytecode_artifact_tests.discover_cases(TESTS_DIR / "bytecode_artifacts"):
            raw_results.extend(bytecode_artifact_tests.check_case(compiler, vm_manifest, case_dir))
        return raw_results

    return run_imported_runner(checks, "artifact", callback)


def run_module_cache_suite(
    compiler: Path,
    vm_manifest: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    return run_imported_runner(
        checks,
        "module_cache",
        lambda: bytecode_module_cache_tests.run_all(compiler, vm_manifest.parent),
    )


def run_rust_artifact_suite(
    compiler: Path,
    vm_manifest: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    def callback():
        raw_results = []
        for case_dir in run_rust_vm_tests.discover_artifact_cases(TESTS_DIR / "bytecode_artifacts"):
            raw_results.extend(run_rust_vm_tests.check_case(compiler, vm_manifest, case_dir))
        return raw_results

    return run_imported_runner(checks, "rust_vm_artifact", callback)


def run_rust_golden_suite(
    compiler: Path,
    vm_manifest: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    def callback():
        raw_results = []
        for case_dir in run_rust_vm_tests.discover_golden_cases(TESTS_DIR / "golden"):
            raw_results.extend(run_rust_vm_tests.check_case(compiler, vm_manifest, case_dir))
        return raw_results

    return run_imported_runner(checks, "rust_vm_golden", callback)


def run_rust_runtime_error_suite(
    compiler: Path,
    vm_manifest: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    def callback():
        raw_results = []
        for source in run_rust_vm_tests.discover_runtime_error_cases(TESTS_DIR / "golden" / "runtime_errors"):
            raw_results.extend(run_rust_vm_tests.check_runtime_error_case(compiler, vm_manifest, source))
        return raw_results

    return run_imported_runner(checks, "rust_vm_runtime_error", callback)


def run_named_process_suite(
    repo_root: Path,
    checks: list[dict[str, object]],
    runner: str,
    command: list[str],
) -> tuple[list[RecordedResult], list[str]]:
    expected = result_name_map(checks, runner)
    completed = run_command(command, repo_root)
    passed = completed.returncode == 0
    message = "" if passed else process_failure(command, completed)
    return [
        RecordedResult(str(check["case_id"]), passed, message)
        for check in expected.values()
    ], []


def run_ctest_suite(
    repo_root: Path,
    build_dir: Path,
    checks: list[dict[str, object]],
) -> tuple[list[RecordedResult], list[str]]:
    results: list[RecordedResult] = []
    for check in sorted(
        (check for check in checks if check.get("runner") == "ctest"),
        key=lambda value: str(value["result_name"]),
    ):
        test_name = str(check["result_name"])
        command = [
            "ctest",
            "--test-dir",
            str(build_dir),
            "--output-on-failure",
            "-R",
            f"^{re.escape(test_name)}$",
        ]
        completed = run_command(command, repo_root)
        no_test = "No tests were found" in completed.stdout
        passed = completed.returncode == 0 and not no_test
        message = "" if passed else process_failure(command, completed)
        if no_test:
            message = f"CTest did not discover test {test_name}\n{message}".rstrip()
        results.append(RecordedResult(str(check["case_id"]), passed, message))
    return results, []


def resolve_vm_manifest(vm_path: Path) -> Path:
    return vm_path / "Cargo.toml" if vm_path.is_dir() else vm_path


def run_canonical(
    compiler: Path,
    vm_manifest: Path,
    inventory: dict[str, object],
    repo_root: Path,
) -> tuple[list[RecordedResult], list[str]]:
    checks = [check for check in inventory["checks"] if isinstance(check, dict)]
    all_results: list[RecordedResult] = []
    extras: list[str] = []

    ctest_results, ctest_extras = run_ctest_suite(repo_root, compiler.parent, checks)
    all_results.extend(ctest_results)
    extras.extend(ctest_extras)

    direct_suites = (
        ("golden", lambda: run_golden_suite(compiler, checks)),
        ("boundary_tokens", lambda: run_boundary_token_suite(compiler, checks)),
        ("malformed", lambda: run_malformed_suite(compiler, vm_manifest, checks)),
        (
            "golden_selftest",
            lambda: run_named_process_suite(
                repo_root,
                checks,
                "golden_selftest",
                [sys.executable, str(TESTS_DIR / "run_golden_tests_selftest.py")],
            ),
        ),
        ("artifact", lambda: run_artifact_suite(compiler, vm_manifest, checks)),
        ("module_cache", lambda: run_module_cache_suite(compiler, vm_manifest, checks)),
        ("rust_vm_artifact", lambda: run_rust_artifact_suite(compiler, vm_manifest, checks)),
        ("rust_vm_golden", lambda: run_rust_golden_suite(compiler, vm_manifest, checks)),
        (
            "rust_vm_runtime_error",
            lambda: run_rust_runtime_error_suite(compiler, vm_manifest, checks),
        ),
        (
            "cargo_test",
            lambda: run_named_process_suite(
                repo_root,
                checks,
                "cargo_test",
                ["cargo", "test", "--manifest-path", str(vm_manifest)],
            ),
        ),
    )
    for runner, callback in direct_suites:
        runner_results, runner_extras = callback()
        all_results.extend(runner_results)
        extras.extend(runner_extras)

    return all_results, extras


def build_report(
    inventory: dict[str, object],
    results: list[RecordedResult],
    extras: list[str],
) -> dict[str, object]:
    by_case_id = {result.case_id: result for result in results}
    cases = []
    allowlist = boundary_comparison.load_allowlist()
    for check in inventory["checks"]:
        case_id = str(check["case_id"])
        result = by_case_id.get(case_id)
        if result is None:
            result = RecordedResult(case_id, False, "canonical runner did not produce a result")
        entry = {
            "case_id": case_id,
            "runner": check["runner"],
            "stage": check["stage"],
            "backend": check["backend"],
            "boundary_sequence": check["boundary_sequence"],
            "terminal_boundary": check["terminal_boundary"],
            "passed": result.passed,
        }
        if result.message:
            entry["message"] = boundary_comparison.canonicalize(result.message, allowlist)
        if not result.passed:
            entry["failure_boundary"] = boundary_comparison.failure_boundary(check)
        cases.append(entry)

    failed = [case for case in cases if not case["passed"]]
    supported_boundaries = sorted(
        {
            str(boundary)
            for check in inventory["checks"]
            for boundary in check["boundary_sequence"]
        },
        key=lambda boundary: boundary_comparison.boundary_rank(boundary),
    )
    report: dict[str, object] = {
        "schema_version": 1,
        "inventory_revision": inventory["inventory_revision"],
        "baseline_commit": inventory["baseline_commit"],
        "summary": {
            "total": len(cases),
            "passed": len(cases) - len(failed),
            "failed": len(failed),
            "untracked_results": len(extras),
        },
        "boundary_summary": {
            "supported_boundaries": supported_boundaries,
            "first_failure": boundary_comparison.first_failure(cases),
            "failed_by_boundary": boundary_comparison.failure_counts(cases),
        },
        "cases": cases,
        "untracked_results": extras,
    }
    return report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the canonical Compiler Design verification inventory.")
    parser.add_argument("compiler", nargs="?", type=Path, help="Path to compiler_design executable")
    parser.add_argument("vm", nargs="?", type=Path, help="Path to vm-rs directory or Cargo.toml")
    parser.add_argument(
        "--inventory",
        type=Path,
        default=TESTS_DIR / "verification_inventory.json",
        help="Path to the versioned verification inventory",
    )
    parser.add_argument("--report", type=Path, help="Write a machine-readable JSON result report")
    parser.add_argument("--list", action="store_true", help="List validated case IDs without running tests")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = REPO_ROOT.resolve()
    inventory_path = args.inventory.resolve()
    try:
        inventory = verification_inventory.load_inventory(inventory_path)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"verification inventory: {error}", file=sys.stderr)
        return 64

    errors = verification_inventory.validate_inventory(inventory, repo_root)
    if errors:
        for error in errors:
            print(f"FAIL {error}", file=sys.stderr)
        return 1

    if args.list:
        for check in inventory["checks"]:
            print(check["case_id"])
        return 0

    if args.compiler is None or args.vm is None:
        print("compiler and vm arguments are required unless --list is used", file=sys.stderr)
        return 64

    compiler = args.compiler.resolve()
    vm_manifest = resolve_vm_manifest(args.vm.resolve())
    if not compiler.is_file():
        print(f"compiler not found: {compiler}", file=sys.stderr)
        return 64
    if not vm_manifest.is_file():
        print(f"Rust VM manifest not found: {vm_manifest}", file=sys.stderr)
        return 64

    results, extras = run_canonical(compiler, vm_manifest, inventory, repo_root)
    report = build_report(inventory, results, extras)
    if args.report is not None:
        report_path = args.report.resolve()
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    summary = report["summary"]
    print(
        "verification: "
        f"{summary['passed']} passed, {summary['failed']} failed, "
        f"{summary['total']} total"
    )
    for case in report["cases"]:
        if not case["passed"]:
            print(f"FAIL {case['case_id']}: {case.get('message', 'no failure message')}", file=sys.stderr)
    for extra in extras:
        print(f"FAIL {extra}", file=sys.stderr)
    return 1 if summary["failed"] or extras else 0


if __name__ == "__main__":
    raise SystemExit(main())
