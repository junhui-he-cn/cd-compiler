#!/usr/bin/env python3
"""Run library fixtures and validate only their Rust VM output."""

import argparse
import sys
from pathlib import Path


LIBRARY_ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = LIBRARY_ROOT.parent
sys.path.insert(0, str(PROJECT_ROOT / "tests"))

from run_rust_vm_tests import check_case  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Run Compiler Design data-structure library tests.")
    parser.add_argument("compiler", type=Path, help="Path to the compiler_design executable")
    parser.add_argument(
        "vm",
        type=Path,
        nargs="?",
        default=PROJECT_ROOT / "vm-rs",
        help="Path to vm-rs (defaults to the project VM)",
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="Run only library fixture directories containing this substring; may be repeated",
    )
    args = parser.parse_args()

    tests_root = Path(__file__).resolve().parent
    case_dirs = sorted(
        path
        for path in tests_root.iterdir()
        if path.is_dir()
        and (path / "input.cd").is_file()
        and (path / "run.out").is_file()
        and (not args.case or any(pattern in path.name for pattern in args.case))
    )
    if not case_dirs:
        print("no library fixtures selected", file=sys.stderr)
        return 1

    compiler = args.compiler.resolve()
    vm = args.vm.resolve()
    vm_manifest = vm / "Cargo.toml" if vm.is_dir() else vm
    results = []
    for case_dir in case_dirs:
        results.extend(check_case(compiler, vm_manifest, case_dir, include_emit_result=False))

    failed = [result for result in results if not result.passed]
    for failure in failed:
        print(failure.message, file=sys.stderr)
    passed = len(results) - len(failed)
    print(f"library tests: {passed} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
