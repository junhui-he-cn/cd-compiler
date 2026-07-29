#!/usr/bin/env python3

"""Build and validate the repository's versioned verification inventory.

The inventory is intentionally generated from the checked-in fixture layout and
CTest declarations.  The generated case IDs are stable because they are based
on fixture paths, expected-output roles, and named suites rather than discovery
order.  Updating a fixture or adding a test therefore requires an explicit
inventory refresh.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Optional

import malformed_corpus


SCHEMA_VERSION = 1
INVENTORY_REVISION = "m0d-2026-07-22-r1"
BASELINE_COMMIT = "79fbfa0"
BOUNDARY_CASES_PATH = Path(__file__).resolve().with_name("boundary_cases.json")
MODULE_CACHE_CASES_PATH = Path(__file__).resolve().with_name("module_cache_cases.json")

CANONICAL_COMMAND = [
    "python3",
    "tests/run_verification.py",
    "./build/compiler_design",
    "vm-rs",
    "--report",
    "build/verification-report.json",
]

LEGACY_COMMANDS = [
    ["ctest", "--test-dir", "build", "--output-on-failure"],
    ["python3", "tests/run_golden_tests.py", "./build/compiler_design"],
    ["python3", "tests/run_boundary_tests.py", "./build/compiler_design"],
    [
        "python3",
        "tests/run_malformed_tests.py",
        "./build/compiler_design",
        "vm-rs",
        "--report",
        "build/malformed-report.json",
    ],
    ["python3", "tests/run_golden_tests_selftest.py"],
    ["python3", "tests/bytecode_artifact_tests.py", "./build/compiler_design", "vm-rs"],
    ["python3", "tests/run_rust_vm_tests.py", "./build/compiler_design", "vm-rs", "--goldens"],
    ["cargo", "test", "--manifest-path", "vm-rs/Cargo.toml"],
]

SUCCESS_OUTPUTS = (
    ("ast", "ast.out", "default(ast)", "ast-output"),
    ("ir", "ir.out", "--ir", "ir-output"),
    ("bytecode", "bytecode.out", "--bytecode", "bytecode-output"),
    ("module_interface", "module-interface.out", "--module-interface", "module-interface-output"),
)

CTEST_SOURCE_OVERRIDES = {
    "source_map": ["tests/source_map_tests.cpp"],
    "ir_source_location": ["tests/ir_source_location_tests.cpp"],
    "golden_runner_selftest": ["tests/run_golden_tests_selftest.py"],
    "cmake_config": ["tests/cmake_config_tests.py"],
    "ci_workflow": ["tests/ci_workflow_tests.py", ".github/workflows/ci.yml"],
    "cli_multi_source": ["tests/cli_multi_source_tests.py"],
    "formatter_cli": ["tests/formatter_cli_tests.py"],
    "formatter_corpus": ["tests/formatter_corpus_tests.py"],
    "language_server": ["tests/lsp_tests.py"],
    "golden": ["tests/run_golden_tests.py"],
    "bytecode_artifacts": ["tests/bytecode_artifact_tests.py"],
    "bytecode_module_artifacts": ["tests/bytecode_module_artifact_tests.py", "vm-rs"],
    "rust_vm": ["tests/run_rust_vm_tests.py", "vm-rs"],
    "frontend_session": ["tests/frontend_session_tests.cpp"],
    "flow_facts": ["tests/flow_facts_tests.cpp"],
    "type_utils": ["tests/type_utils_tests.cpp"],
    "module_symbols": ["tests/module_symbols_tests.cpp"],
    "module_interface_emitter": ["tests/module_interface_emitter_tests.cpp"],
    "debugger": ["tests/debugger_tests.py", "vm-rs"],
    "module_debug_metadata": ["tests/module_debug_metadata_tests.py"],
    "verification_inventory": ["tests/verification_inventory.py", "tests/verification_inventory.json"],
    "verification_runner_selftest": ["tests/run_verification_selftest.py"],
    "boundary_comparison_selftest": [
        "tests/boundary_comparison_selftest.py",
        "tests/boundary_comparison.py",
        "tests/boundary_allowlist.json",
    ],
    "malformed_harness_selftest": [
        "tests/malformed_tests_selftest.py",
        "tests/malformed_corpus.py",
        "tests/run_malformed_tests.py",
    ],
    "verification_matrix_selftest": [
        "tests/verification_matrix_selftest.py",
        "tests/verification_matrix.py",
        "tests/run_verification_matrix.py",
        "tests/verification_matrix.json",
    ],
}


def infer_boundary_metadata(
    runner: str,
    result_name: str,
    expected_result_kind: str,
    stage: str,
) -> tuple[list[str], str]:
    parsed = ["tokens", "ast"]
    lowered = [*parsed, "semantic", "ir", "bytecode", "cdbc"]

    if runner in {"ctest", "golden_selftest", "cargo_test"}:
        return ["verification"], "verification"
    if runner == "boundary_tokens":
        return ["tokens"], "tokens"
    if runner == "golden":
        if stage == "parse-error":
            return [*parsed, "parse_diagnostic"], "parse_diagnostic"
        if stage == "type-error":
            return [*parsed, "semantic_diagnostic"], "semantic_diagnostic"
        if stage == "import-error":
            return [*parsed, "import_diagnostic"], "import_diagnostic"
        if "default(ast)" in result_name:
            return parsed, "ast"
        if result_name.endswith(" --ir"):
            return [*parsed, "semantic", "ir"], "ir"
        if result_name.endswith(" --bytecode"):
            return lowered[:-1], "bytecode"
        if result_name.endswith(" --module-interface"):
            return [*parsed, "semantic", "module_interface"], "module_interface"
    if runner in {"artifact", "rust_vm_artifact", "rust_vm_golden", "rust_vm_runtime_error"}:
        if result_name.endswith(" rust-dump"):
            return [*lowered, "rust_decode"], "rust_decode"
        if result_name.endswith(" rust-run"):
            return [*lowered, "rust_decode", "vm_output"], "vm_output"
        return lowered, "cdbc"
    if expected_result_kind == "diagnostic-and-exit":
        return [*parsed, "semantic_diagnostic"], "semantic_diagnostic"
    return ["verification"], "verification"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def relative_path(repo_root: Path, path: Path) -> str:
    return path.resolve().relative_to(repo_root.resolve()).as_posix()


def fixture_sources(repo_root: Path, case_dir: Path) -> list[str]:
    args_path = case_dir / "args.txt"
    if args_path.is_file():
        entries = read_text(args_path).split()
        return [relative_path(repo_root, case_dir / entry) for entry in entries]
    return [relative_path(repo_root, case_dir / "input.cd")]


def fixture_metadata(repo_dir: Path, case_dir: Path) -> dict[str, object]:
    return {
        "fixture": relative_path(repo_dir, case_dir),
        "sources": fixture_sources(repo_dir, case_dir),
    }


def make_case(
    *,
    case_id: str,
    runner: str,
    result_name: str,
    fixture: Optional[str],
    sources: list[str],
    expected_files: list[str],
    stage: str,
    capability_tags: list[str],
    backend: str,
    expected_result_kind: str,
    boundary_sequence: Optional[list[str]] = None,
    terminal_boundary: Optional[str] = None,
) -> dict[str, object]:
    if boundary_sequence is None or terminal_boundary is None:
        inferred_sequence, inferred_terminal = infer_boundary_metadata(
            runner,
            result_name,
            expected_result_kind,
            stage,
        )
        if boundary_sequence is None:
            boundary_sequence = inferred_sequence
        if terminal_boundary is None:
            terminal_boundary = inferred_terminal
    case: dict[str, object] = {
        "case_id": case_id,
        "runner": runner,
        "result_name": result_name,
        "stage": stage,
        "capability_tags": capability_tags,
        "backend": backend,
        "expected_result_kind": expected_result_kind,
        "boundary_sequence": boundary_sequence,
        "terminal_boundary": terminal_boundary,
        "sources": sources,
        "expected_files": expected_files,
    }
    if fixture is not None:
        case["fixture"] = fixture
    return case


def discover_ctest_names(repo_root: Path) -> list[str]:
    cmake_path = repo_root / "CMakeLists.txt"
    names = re.findall(r"\bNAME\s+([A-Za-z0-9_.-]+)", read_text(cmake_path))
    return sorted(set(names))


def discover_success_cases(repo_root: Path) -> list[Path]:
    golden_dir = repo_root / "tests" / "golden"
    excluded = {"runtime_errors", "parse_errors", "type_errors", "import_errors"}
    return sorted(
        case_dir
        for case_dir in golden_dir.iterdir()
        if case_dir.is_dir()
        and case_dir.name not in excluded
        and ((case_dir / "input.cd").is_file() or (case_dir / "args.txt").is_file())
    )


def discover_error_sources(repo_root: Path, category: str) -> list[Path]:
    return sorted((repo_root / "tests" / "golden" / category).glob("*.cd"))


def discover_artifact_cases(repo_root: Path) -> list[Path]:
    artifact_dir = repo_root / "tests" / "bytecode_artifacts"
    return sorted(
        case_dir
        for case_dir in artifact_dir.iterdir()
        if case_dir.is_dir()
        and ((case_dir / "input.cd").is_file() or (case_dir / "args.txt").is_file())
        and (case_dir / "expected.cdbc").is_file()
    )


def discover_run_cases(repo_root: Path, root: Path) -> list[Path]:
    return sorted(
        case_dir
        for case_dir in root.iterdir()
        if case_dir.is_dir()
        and ((case_dir / "input.cd").is_file() or (case_dir / "args.txt").is_file())
        and (case_dir / "run.out").is_file()
    )


def add_ctest_cases(repo_root: Path, cases: list[dict[str, object]]) -> None:
    for name in discover_ctest_names(repo_root):
        sources = ["CMakeLists.txt", *CTEST_SOURCE_OVERRIDES.get(name, [])]
        backend = "python" if any(path.endswith(".py") for path in sources) else "cpp"
        cases.append(
            make_case(
                case_id=f"ctest.{name}",
                runner="ctest",
                result_name=name,
                fixture=None,
                sources=sources,
                expected_files=[],
                stage="verification",
                capability_tags=["ctest", name],
                backend=backend,
                expected_result_kind="pass",
            )
        )


def add_golden_cases(repo_root: Path, cases: list[dict[str, object]]) -> None:
    for case_dir in discover_success_cases(repo_root):
        metadata = fixture_metadata(repo_root, case_dir)
        fixture = str(metadata["fixture"])
        sources = list(metadata["sources"])
        for mode, filename, result_mode, tag in SUCCESS_OUTPUTS:
            expected_path = case_dir / filename
            if not expected_path.is_file():
                continue
            cases.append(
                make_case(
                    case_id=f"golden.success.{case_dir.name}.{mode}",
                    runner="golden",
                    result_name=f"{case_dir.name} {result_mode}",
                    fixture=fixture,
                    sources=sources,
                    expected_files=[relative_path(repo_root, expected_path)],
                    stage="semantic" if mode != "bytecode" else "bytecode",
                    capability_tags=["golden", "success", tag, f"fixture:{case_dir.name}"],
                    backend="cpp",
                    expected_result_kind="stdout-golden",
                )
            )

    categories = (
        ("parse_errors", "parse-error"),
        ("type_errors", "type-error"),
        ("import_errors", "import-error"),
    )
    for category, stage in categories:
        for source in discover_error_sources(repo_root, category):
            stem = source.with_suffix("")
            expected_files = [
                relative_path(repo_root, source),
                relative_path(repo_root, stem.with_suffix(".err")),
                relative_path(repo_root, stem.with_suffix(".exit")),
            ]
            cases.append(
                make_case(
                    case_id=f"golden.{category}.{source.stem}",
                    runner="golden",
                    result_name=f"{category}/{source.stem} default(ast)",
                    fixture=relative_path(repo_root, source.parent),
                    sources=[relative_path(repo_root, source)],
                    expected_files=expected_files,
                    stage=stage,
                    capability_tags=["golden", category, f"fixture:{source.stem}"],
                    backend="cpp",
                    expected_result_kind="diagnostic-and-exit",
                )
            )


def add_artifact_cases(repo_root: Path, cases: list[dict[str, object]]) -> None:
    for case_dir in discover_artifact_cases(repo_root):
        metadata = fixture_metadata(repo_root, case_dir)
        fixture = str(metadata["fixture"])
        sources = list(metadata["sources"])
        expected = relative_path(repo_root, case_dir / "expected.cdbc")
        cases.extend(
            [
                make_case(
                    case_id=f"artifact.{case_dir.name}.emit",
                    runner="artifact",
                    result_name=f"{case_dir.name} emit",
                    fixture=fixture,
                    sources=sources,
                    expected_files=[expected],
                    stage="artifact-emission",
                    capability_tags=["artifact", "cdbc", "emit", f"fixture:{case_dir.name}"],
                    backend="cpp",
                    expected_result_kind="cdbc-golden",
                ),
                make_case(
                    case_id=f"artifact.{case_dir.name}.rust_dump",
                    runner="artifact",
                    result_name=f"{case_dir.name} rust-dump",
                    fixture=fixture,
                    sources=sources,
                    expected_files=[expected],
                    stage="artifact-parse",
                    capability_tags=["artifact", "cdbc", "rust-parser", f"fixture:{case_dir.name}"],
                    backend="rust-vm",
                    expected_result_kind="cdbc-golden",
                ),
            ]
        )


def add_rust_vm_cases(repo_root: Path, cases: list[dict[str, object]]) -> None:
    artifact_root = repo_root / "tests" / "bytecode_artifacts"
    for case_dir in discover_run_cases(repo_root, artifact_root):
        metadata = fixture_metadata(repo_root, case_dir)
        fixture = str(metadata["fixture"])
        sources = list(metadata["sources"])
        expected = relative_path(repo_root, case_dir / "run.out")
        cases.extend(
            [
                make_case(
                    case_id=f"rust_vm.artifact.{case_dir.name}.emit",
                    runner="rust_vm_artifact",
                    result_name=f"{case_dir.name} emit",
                    fixture=fixture,
                    sources=sources,
                    expected_files=[expected],
                    stage="artifact-emission",
                    capability_tags=["rust-vm", "artifact", "emit", f"fixture:{case_dir.name}"],
                    backend="cpp",
                    expected_result_kind="artifact-exists",
                ),
                make_case(
                    case_id=f"rust_vm.artifact.{case_dir.name}.run",
                    runner="rust_vm_artifact",
                    result_name=f"{case_dir.name} rust-run",
                    fixture=fixture,
                    sources=sources,
                    expected_files=[expected],
                    stage="runtime",
                    capability_tags=["rust-vm", "artifact", "runtime", f"fixture:{case_dir.name}"],
                    backend="rust-vm",
                    expected_result_kind="runtime-stdout",
                ),
            ]
        )

    golden_root = repo_root / "tests" / "golden"
    for case_dir in discover_run_cases(repo_root, golden_root):
        metadata = fixture_metadata(repo_root, case_dir)
        fixture = str(metadata["fixture"])
        sources = list(metadata["sources"])
        expected = relative_path(repo_root, case_dir / "run.out")
        cases.extend(
            [
                make_case(
                    case_id=f"rust_vm.golden.{case_dir.name}.emit",
                    runner="rust_vm_golden",
                    result_name=f"{case_dir.name} emit",
                    fixture=fixture,
                    sources=sources,
                    expected_files=[expected],
                    stage="artifact-emission",
                    capability_tags=["rust-vm", "golden", "emit", f"fixture:{case_dir.name}"],
                    backend="cpp",
                    expected_result_kind="artifact-exists",
                ),
                make_case(
                    case_id=f"rust_vm.golden.{case_dir.name}.run",
                    runner="rust_vm_golden",
                    result_name=f"{case_dir.name} rust-run",
                    fixture=fixture,
                    sources=sources,
                    expected_files=[expected],
                    stage="runtime",
                    capability_tags=["rust-vm", "golden", "runtime", f"fixture:{case_dir.name}"],
                    backend="rust-vm",
                    expected_result_kind="runtime-stdout",
                ),
            ]
        )

    runtime_root = repo_root / "tests" / "golden" / "runtime_errors"
    for source in sorted(runtime_root.glob("*.cd")):
        stem = source.with_suffix("")
        expected_files = [
            relative_path(repo_root, source),
            relative_path(repo_root, stem.with_suffix(".run.err")),
            relative_path(repo_root, stem.with_suffix(".exit")),
        ]
        fixture = relative_path(repo_root, runtime_root)
        source_path = relative_path(repo_root, source)
        cases.extend(
            [
                make_case(
                    case_id=f"rust_vm.runtime_errors.{source.stem}.emit",
                    runner="rust_vm_runtime_error",
                    result_name=f"runtime_errors/{source.stem} emit",
                    fixture=fixture,
                    sources=[source_path],
                    expected_files=expected_files,
                    stage="artifact-emission",
                    capability_tags=["rust-vm", "runtime-error", "emit", f"fixture:{source.stem}"],
                    backend="cpp",
                    expected_result_kind="artifact-exists",
                ),
                make_case(
                    case_id=f"rust_vm.runtime_errors.{source.stem}.run",
                    runner="rust_vm_runtime_error",
                    result_name=f"runtime_errors/{source.stem} rust-run",
                    fixture=fixture,
                    sources=[source_path],
                    expected_files=expected_files,
                    stage="runtime",
                    capability_tags=["rust-vm", "runtime-error", "stderr-and-exit", f"fixture:{source.stem}"],
                    backend="rust-vm",
                    expected_result_kind="runtime-diagnostic-and-exit",
                ),
            ]
        )


def load_boundary_cases(repo_root: Path) -> list[dict[str, object]]:
    value = json.loads(read_text(BOUNDARY_CASES_PATH))
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError(f"invalid boundary case manifest: {BOUNDARY_CASES_PATH}")
    manifest_cases = value.get("cases")
    if not isinstance(manifest_cases, list):
        raise ValueError(f"boundary case manifest cases must be a list: {BOUNDARY_CASES_PATH}")
    cases = [case for case in manifest_cases if isinstance(case, dict)]
    for case in cases:
        for field in (
            "case_id",
            "result_name",
            "fixture",
            "sources",
            "expected_files",
            "stage",
            "capability_tags",
            "backend",
            "expected_result_kind",
        ):
            if field not in case:
                raise ValueError(f"boundary case is missing {field}: {case!r}")
    return sorted(cases, key=lambda case: str(case["case_id"]))


def load_module_cache_cases(repo_root: Path) -> list[dict[str, object]]:
    manifest_path = repo_root / "tests" / MODULE_CACHE_CASES_PATH.name
    value = json.loads(read_text(manifest_path))
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError(f"invalid module-cache case manifest: {manifest_path}")
    if value.get("inventory_revision") != INVENTORY_REVISION:
        raise ValueError(
            "module-cache case manifest inventory revision does not match inventory: "
            f"{value.get('inventory_revision')} != {INVENTORY_REVISION}"
        )
    manifest_cases = value.get("cases")
    if not isinstance(manifest_cases, list):
        raise ValueError(f"module-cache case manifest cases must be a list: {manifest_path}")
    required_fields = (
        "case_id",
        "result_name",
        "stage",
        "capability_tags",
        "backend",
        "expected_result_kind",
        "boundary_sequence",
        "terminal_boundary",
        "sources",
        "expected_files",
    )
    cases = [case for case in manifest_cases if isinstance(case, dict)]
    for case in cases:
        for field in required_fields:
            if field not in case:
                raise ValueError(f"module-cache case is missing {field}: {case!r}")
    return sorted(cases, key=lambda case: str(case["case_id"]))


def add_boundary_token_cases(repo_root: Path, cases: list[dict[str, object]]) -> None:
    for boundary_case in load_boundary_cases(repo_root):
        cases.append(
            make_case(
                case_id=str(boundary_case["case_id"]),
                runner="boundary_tokens",
                result_name=str(boundary_case["result_name"]),
                fixture=str(boundary_case["fixture"]),
                sources=[str(source) for source in boundary_case["sources"]],
                expected_files=[str(path) for path in boundary_case["expected_files"]],
                stage=str(boundary_case["stage"]),
                capability_tags=[str(tag) for tag in boundary_case["capability_tags"]],
                backend=str(boundary_case["backend"]),
                expected_result_kind=str(boundary_case["expected_result_kind"]),
                boundary_sequence=["tokens"],
                terminal_boundary="tokens",
            )
        )


def add_malformed_cases(repo_root: Path, cases: list[dict[str, object]]) -> None:
    for malformed_case in malformed_corpus.expand_cases(repo_root=repo_root):
        cases.append(
            make_case(
                case_id=str(malformed_case["case_id"]),
                runner="malformed",
                result_name=str(malformed_case["result_name"]),
                fixture=str(malformed_case["fixture"]),
                sources=[str(source) for source in malformed_case["sources"]],
                expected_files=[str(path) for path in malformed_case["expected_files"]],
                stage=str(malformed_case["stage"]),
                capability_tags=[str(tag) for tag in malformed_case["capability_tags"]],
                backend=str(malformed_case["backend"]),
                expected_result_kind=str(malformed_case["expected_result_kind"]),
                boundary_sequence=[str(boundary) for boundary in malformed_case["boundary_sequence"]],
                terminal_boundary=str(malformed_case["terminal_boundary"]),
            )
        )


def add_named_suite_cases(repo_root: Path, cases: list[dict[str, object]]) -> None:
    cases.append(
        make_case(
            case_id="runner.golden_selftest",
            runner="golden_selftest",
            result_name="golden_runner_selftest",
            fixture=None,
            sources=["tests/run_golden_tests_selftest.py"],
            expected_files=[],
            stage="verification",
            capability_tags=["runner", "golden", "selftest"],
            backend="python",
            expected_result_kind="pass",
        )
    )
    for module_cache_case in load_module_cache_cases(repo_root):
        cases.append(
            make_case(
                case_id=str(module_cache_case["case_id"]),
                runner="module_cache",
                result_name=str(module_cache_case["result_name"]),
                fixture=None,
                sources=[str(source) for source in module_cache_case["sources"]],
                expected_files=[str(path) for path in module_cache_case["expected_files"]],
                stage=str(module_cache_case["stage"]),
                capability_tags=[str(tag) for tag in module_cache_case["capability_tags"]],
                backend=str(module_cache_case["backend"]),
                expected_result_kind=str(module_cache_case["expected_result_kind"]),
                boundary_sequence=[str(boundary) for boundary in module_cache_case["boundary_sequence"]],
                terminal_boundary=str(module_cache_case["terminal_boundary"]),
            )
        )
    cases.append(
        make_case(
            case_id="runner.cargo_test",
            runner="cargo_test",
            result_name="cargo test",
            fixture=None,
            sources=["vm-rs/Cargo.toml"],
            expected_files=[],
            stage="verification",
            capability_tags=["runner", "rust", "unit-tests"],
            backend="rust",
            expected_result_kind="pass",
        )
    )


def build_cases(repo_root: Path) -> list[dict[str, object]]:
    cases: list[dict[str, object]] = []
    add_ctest_cases(repo_root, cases)
    add_golden_cases(repo_root, cases)
    add_artifact_cases(repo_root, cases)
    add_rust_vm_cases(repo_root, cases)
    add_boundary_token_cases(repo_root, cases)
    add_malformed_cases(repo_root, cases)
    add_named_suite_cases(repo_root, cases)
    return sorted(cases, key=lambda case: str(case["case_id"]))


def build_inventory(repo_root: Path, baseline_commit: str = BASELINE_COMMIT) -> dict[str, object]:
    return {
        "schema_version": SCHEMA_VERSION,
        "inventory_revision": INVENTORY_REVISION,
        "baseline_commit": baseline_commit,
        "canonical_command": CANONICAL_COMMAND,
        "legacy_commands": LEGACY_COMMANDS,
        "checks": build_cases(repo_root),
    }


def load_inventory(path: Path) -> dict[str, object]:
    value = json.loads(read_text(path))
    if not isinstance(value, dict):
        raise ValueError(f"inventory root must be an object: {path}")
    return value


def validate_inventory(inventory: dict[str, object], repo_root: Path) -> list[str]:
    errors: list[str] = []
    if inventory.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}")
    if not inventory.get("inventory_revision"):
        errors.append("inventory_revision is required")
    if not inventory.get("baseline_commit"):
        errors.append("baseline_commit is required")
    for field in ("canonical_command", "legacy_commands"):
        if not isinstance(inventory.get(field), list) or not inventory[field]:
            errors.append(f"{field} must be a non-empty list")

    actual_cases = inventory.get("checks")
    if not isinstance(actual_cases, list):
        return [*errors, "checks must be a list"]

    case_ids = [case.get("case_id") for case in actual_cases if isinstance(case, dict)]
    duplicates = sorted({case_id for case_id in case_ids if case_ids.count(case_id) > 1})
    for case_id in duplicates:
        errors.append(f"duplicate case_id: {case_id}")

    expected_cases = build_cases(repo_root)
    expected_by_id = {str(case["case_id"]): case for case in expected_cases}
    actual_by_id = {
        str(case.get("case_id")): case
        for case in actual_cases
        if isinstance(case, dict) and case.get("case_id") is not None
    }
    missing = sorted(set(expected_by_id) - set(actual_by_id))
    unexpected = sorted(set(actual_by_id) - set(expected_by_id))
    errors.extend(f"missing case_id: {case_id}" for case_id in missing)
    errors.extend(f"unexpected case_id: {case_id}" for case_id in unexpected)
    for case_id in sorted(set(expected_by_id) & set(actual_by_id)):
        if actual_by_id[case_id] != expected_by_id[case_id]:
            errors.append(f"case metadata mismatch: {case_id}")

    for case in actual_cases:
        if not isinstance(case, dict):
            errors.append("every check must be an object")
            continue
        for field in (
            "case_id",
            "runner",
            "result_name",
            "stage",
            "capability_tags",
            "backend",
            "expected_result_kind",
            "boundary_sequence",
            "terminal_boundary",
            "sources",
            "expected_files",
        ):
            if field not in case:
                errors.append(f"{case.get('case_id', '<unknown>')} missing field: {field}")
        for field in ("sources", "expected_files"):
            values = case.get(field, [])
            if not isinstance(values, list):
                errors.append(f"{case.get('case_id', '<unknown>')} {field} must be a list")
                continue
            for value in values:
                path = repo_root / str(value)
                if not path.exists():
                    errors.append(f"{case.get('case_id', '<unknown>')} missing path: {value}")
        sequence = case.get("boundary_sequence")
        terminal = case.get("terminal_boundary")
        if not isinstance(sequence, list) or not sequence:
            errors.append(f"{case.get('case_id', '<unknown>')} boundary_sequence must be a non-empty list")
        elif terminal not in sequence:
            errors.append(f"{case.get('case_id', '<unknown>')} terminal_boundary is not in boundary_sequence")

    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build or validate the verification inventory.")
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument(
        "--inventory",
        type=Path,
        default=Path(__file__).resolve().with_name("verification_inventory.json"),
    )
    parser.add_argument("--write", action="store_true", help="Regenerate the checked-in inventory.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    inventory_path = args.inventory.resolve()

    if args.write:
        baseline_commit = BASELINE_COMMIT
        if inventory_path.is_file():
            try:
                existing = load_inventory(inventory_path)
                if existing.get("inventory_revision") == INVENTORY_REVISION:
                    baseline_commit = str(existing.get("baseline_commit", baseline_commit))
            except (OSError, ValueError, json.JSONDecodeError):
                pass
        inventory = build_inventory(repo_root, baseline_commit)
        inventory_path.write_text(json.dumps(inventory, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {len(inventory['checks'])} verification cases to {inventory_path}")
        return 0

    try:
        inventory = load_inventory(inventory_path)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"verification inventory: {error}", file=sys.stderr)
        return 1
    errors = validate_inventory(inventory, repo_root)
    if errors:
        for error in errors:
            print(f"FAIL {error}", file=sys.stderr)
        return 1
    print(f"verification inventory: {len(inventory['checks'])} cases validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
