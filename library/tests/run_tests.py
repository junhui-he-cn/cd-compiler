#!/usr/bin/env python3
"""Run the data-structure library fixtures without discovering project tests."""

import argparse
import sys
from pathlib import Path


LIBRARY_ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = LIBRARY_ROOT.parent
sys.path.insert(0, str(PROJECT_ROOT / "tests"))

from run_golden_tests import check_success_case  # noqa: E402
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
        "--update",
        action="store_true",
        help="Refresh existing compiler golden files for the library fixture",
    )
    args = parser.parse_args()

    case_dir = Path(__file__).resolve().parent / "data_structures_deque"
    compiler = args.compiler.resolve()
    vm = args.vm.resolve()
    vm_manifest = vm / "Cargo.toml" if vm.is_dir() else vm
    results = check_success_case(compiler, case_dir, args.update)
    results.extend(check_case(compiler, vm_manifest, case_dir))

    failed = [result for result in results if not result.passed]
    for failure in failed:
        print(failure.message, file=sys.stderr)
    passed = len(results) - len(failed)
    print(f"library tests: {passed} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
