#!/usr/bin/env python3

"""Audit the existing cdbc 0.1 contract without changing its implementation."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
DEFAULT_DECISION_LOG = REPO_ROOT / "docs" / "decisions" / "m05b-cdbc-contract.json"
M4A_DECISION_LOG = REPO_ROOT / "docs" / "decisions" / "m4a-artifact-validation.json"
sys.path.insert(0, str(TESTS_DIR))
import bytecode_artifact_tests  # noqa: E402
import malformed_corpus  # noqa: E402
import verification_inventory  # noqa: E402


ALLOWED_CLASSIFICATIONS = {
    "already present",
    "compatible-extension candidate",
    "successor-version candidate",
    "not currently required",
    "deferred",
    "resolved",
}
FUNCTION_HEADER = re.compile(r"^function f\d+ name=.* arity=\d+ registers=\d+:$")
MAIN_HEADER = re.compile(r"^main registers=\d+:$")


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def load_decisions(path: Path = DEFAULT_DECISION_LOG) -> dict[str, Any]:
    return read_json(path)


def load_m4a_decision(path: Path = M4A_DECISION_LOG) -> dict[str, Any]:
    return read_json(path)


def validate_m4a_decision(decision: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if decision.get("decision_id") != "M4A-VALIDATION-001":
        errors.append("M4A decision id must be M4A-VALIDATION-001")
    if decision.get("status") != "resolved":
        errors.append("M4A validation decision must be resolved")
    matrix = decision.get("compatibility_matrix")
    if not isinstance(matrix, list) or not matrix:
        errors.append("M4A compatibility_matrix must be non-empty")
    else:
        for index, entry in enumerate(matrix):
            if not isinstance(entry, dict):
                errors.append(f"M4A compatibility matrix entry {index} must be an object")
                continue
            for field in ("header", "kind", "dump", "run", "link"):
                if not isinstance(entry.get(field), str) or not entry[field]:
                    errors.append(f"M4A compatibility matrix entry {index} missing {field}")
    successor = decision.get("successor_decision")
    if not isinstance(successor, dict) or successor.get("selected") is not False:
        errors.append("M4A validation must not select a successor version")
    validation_contract = decision.get("validation_contract")
    if not isinstance(validation_contract, list) or not validation_contract:
        errors.append("M4A validation_contract must be non-empty")
    return errors


def load_inventory(path: Path = TESTS_DIR / "verification_inventory.json") -> dict[str, Any]:
    return read_json(path)


def validate_evolution_decisions(decisions: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    contract = decisions.get("contract")
    if not isinstance(contract, dict):
        errors.append("contract must be an object")
    else:
        if contract.get("family") != "cdbc" or contract.get("version") != "0.1":
            errors.append("current contract must be cdbc 0.1")
        for field in ("transport", "current_promise"):
            if not isinstance(contract.get(field), str) or not contract[field]:
                errors.append(f"contract.{field} is required")

    decisions_list = decisions.get("evolution_decisions")
    if not isinstance(decisions_list, list) or not decisions_list:
        errors.append("evolution_decisions must be a non-empty list")
        decisions_list = []
    field_ids: list[str] = []
    for decision in decisions_list:
        if not isinstance(decision, dict):
            errors.append("every evolution decision must be an object")
            continue
        field_id = str(decision.get("field_id", "<unknown>"))
        field_ids.append(field_id)
        for field in (
            "field_id",
            "field",
            "classification",
            "current_state",
            "named_consumer",
            "planned_fixtures",
            "rationale",
            "revisit",
        ):
            if field not in decision:
                errors.append(f"evolution decision {field_id} missing field: {field}")
        classification = decision.get("classification")
        if classification not in ALLOWED_CLASSIFICATIONS:
            errors.append(f"evolution decision {field_id} has invalid classification: {classification}")
        if classification in {"deferred", "not currently required", "successor-version candidate"}:
            revisit = decision.get("revisit")
            if not isinstance(revisit, dict) or not revisit.get("milestone") or not revisit.get("condition"):
                errors.append(f"evolution decision {field_id} requires a revisit milestone and condition")
            if not decision.get("rationale"):
                errors.append(f"evolution decision {field_id} requires a rationale")
        if classification in {"compatible-extension candidate", "successor-version candidate"}:
            if not decision.get("named_consumer"):
                errors.append(f"selected candidate {field_id} requires a named consumer")
            fixtures = decision.get("planned_fixtures")
            if not isinstance(fixtures, list) or not fixtures:
                errors.append(f"selected candidate {field_id} requires a planned fixture")
    if len(field_ids) != len(set(field_ids)):
        errors.append("evolution field IDs must be unique")

    successor = decisions.get("successor_decision")
    if not isinstance(successor, dict):
        errors.append("successor_decision must be an object")
    else:
        for field in ("selected", "decision", "rationale", "m4_owner", "triggers"):
            if field not in successor:
                errors.append(f"successor_decision missing field: {field}")
        if successor.get("selected") is not False:
            errors.append("M0.5B baseline must not select a successor version")
        if not isinstance(successor.get("triggers"), list) or not successor["triggers"]:
            errors.append("successor_decision.triggers must be non-empty")

    invalid_policy = decisions.get("invalid_input_policy")
    if not isinstance(invalid_policy, dict):
        errors.append("invalid_input_policy must be an object")
    else:
        for field in ("family", "version", "unknown_section", "reference_corpus", "audit_probes"):
            if field not in invalid_policy:
                errors.append(f"invalid_input_policy missing field: {field}")
    return errors


def classify_section(line: str) -> str | None:
    if line == "constants:":
        return "constants"
    if line == "names:":
        return "names"
    if MAIN_HEADER.match(line):
        return "main"
    if FUNCTION_HEADER.match(line):
        return "function"
    if line == "debug_sources:":
        return "debug_sources"
    if line == "debug_locations:":
        return "debug_locations"
    if line == "debug_ranges:":
        return "debug_ranges"
    return None


def inspect_artifact(path: Path, repo_root: Path = REPO_ROOT) -> tuple[dict[str, Any], list[str]]:
    text = path.read_text(encoding="utf-8")
    non_empty = [(index, line) for index, line in enumerate(text.splitlines(), start=1) if line.strip()]
    errors: list[str] = []
    header = non_empty[0][1].strip() if non_empty else ""
    if header != "cdbc 0.1":
        errors.append(f"{path}: expected cdbc 0.1 header, found {header!r}")

    sections: list[str] = []
    unknown_top_level: list[str] = []
    for line_number, raw_line in non_empty[1:]:
        line = raw_line.strip()
        if raw_line[:1].isspace():
            continue
        section = classify_section(line)
        if section is None:
            unknown_top_level.append(f"line {line_number}: {line}")
        else:
            sections.append(section)
    if unknown_top_level:
        errors.append(f"{path}: unknown top-level lines: {unknown_top_level}")

    if sections[:3] != ["constants", "names", "main"]:
        errors.append(f"{path}: core sections must start constants, names, main: {sections}")
    debug_seen = False
    for section in sections:
        if section in {"debug_sources", "debug_locations", "debug_ranges"}:
            debug_seen = True
        elif debug_seen:
            errors.append(f"{path}: core section appears after debug metadata: {section}")
    if "debug_locations" in sections and "debug_sources" not in sections:
        errors.append(f"{path}: debug_locations requires debug_sources in the reference envelope")
    if "debug_ranges" in sections and "debug_sources" not in sections:
        errors.append(f"{path}: debug_ranges requires debug_sources in the reference envelope")
    if "debug_ranges" in sections and "debug_locations" not in sections:
        errors.append(f"{path}: debug_ranges requires debug_locations in the reference envelope")
    if sections.count("constants") != 1 or sections.count("names") != 1 or sections.count("main") != 1:
        errors.append(f"{path}: core sections must occur exactly once: {sections}")

    debug_source_lines: list[tuple[int, str]] = []
    in_debug_sources = False
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if line == "debug_sources:":
            in_debug_sources = True
            continue
        if line == "debug_locations:":
            in_debug_sources = False
            continue
        if in_debug_sources and line:
            debug_source_lines.append((line_number, line))

    has_debug_source_modules = False
    for expected_index, (line_number, line) in enumerate(debug_source_lines):
        prefix = f"s{expected_index} "
        if not line.startswith(prefix):
            errors.append(f"{path}: line {line_number}: expected debug source {prefix.strip()}")
            continue
        fields = line[len(prefix):]
        if fields.startswith("module="):
            module_text, separator, fields = fields.partition(" path=")
            if not separator or module_text == 'module=""' or not (
                module_text.startswith('module="') and module_text.endswith('"')
            ):
                errors.append(f"{path}: line {line_number}: invalid debug source module identity")
            else:
                has_debug_source_modules = True
        elif not fields.startswith("path="):
            errors.append(f"{path}: line {line_number}: expected debug source path")
        if " text=" not in fields:
            errors.append(f"{path}: line {line_number}: expected debug source text")

    relative = path.resolve().relative_to(repo_root.resolve()).as_posix()
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    info = {
        "path": relative,
        "bytes": len(text.encode("utf-8")),
        "sha256": digest,
        "header": header,
        "format_family": header.split(" ", 1)[0] if " " in header else header,
        "format_version": header.split(" ", 1)[1] if " " in header else "",
        "section_order": sections,
        "functions": sections.count("function"),
        "has_native_call": "native_call " in text,
        "has_debug_sources": "debug_sources" in sections,
        "has_debug_locations": "debug_locations" in sections,
        "has_debug_ranges": "debug_ranges" in sections,
        "has_debug_source_modules": has_debug_source_modules,
        "envelope_capabilities": [
            "header_family_version",
            "canonical_core_sections",
            "strict_reference_indices",
            *(["function_sections"] if "function" in sections else []),
            *(["native_call"] if "native_call " in text else []),
            *(["debug_sources"] if "debug_sources" in sections else []),
            *(["debug_locations"] if "debug_locations" in sections else []),
            *(["debug_ranges"] if "debug_ranges" in sections else []),
            *(["debug_source_module"] if has_debug_source_modules else []),
        ],
    }
    return info, errors


def artifact_checks(inventory: dict[str, Any]) -> list[dict[str, Any]]:
    return sorted(
        [check for check in inventory.get("checks", []) if check.get("runner") == "artifact"],
        key=lambda check: str(check.get("case_id")),
    )


def artifact_paths(cases: list[dict[str, Any]], repo_root: Path = REPO_ROOT) -> list[Path]:
    values: set[str] = set()
    for case in cases:
        for expected in case.get("expected_files", []):
            expected_text = str(expected)
            if expected_text.endswith("expected.cdbc"):
                values.add(expected_text)
    return [repo_root / value for value in sorted(values)]


def build_static_report(
    decisions: dict[str, Any],
    inventory: dict[str, Any],
    repo_root: Path = REPO_ROOT,
) -> dict[str, Any]:
    errors = validate_evolution_decisions(decisions)
    m4a_decision = load_m4a_decision(repo_root / "docs" / "decisions" / "m4a-artifact-validation.json")
    errors.extend(validate_m4a_decision(m4a_decision))
    cases = artifact_checks(inventory)
    paths = artifact_paths(cases, repo_root)
    artifacts: list[dict[str, Any]] = []
    for path in paths:
        if not path.is_file():
            errors.append(f"missing artifact reference: {path}")
            continue
        info, artifact_errors = inspect_artifact(path, repo_root)
        artifacts.append(info)
        errors.extend(artifact_errors)

    if len(cases) != int(decisions["corpus_scope"]["artifact_case_count"]):
        errors.append(
            "artifact case count mismatch: "
            f"{len(cases)} != {decisions['corpus_scope']['artifact_case_count']}"
        )
    if len(paths) != int(decisions["corpus_scope"]["artifact_fixture_count"]):
        errors.append(
            "artifact fixture count mismatch: "
            f"{len(paths)} != {decisions['corpus_scope']['artifact_fixture_count']}"
        )
    if not artifacts:
        errors.append("artifact reference corpus is empty")

    capabilities_by_path = {artifact["path"]: artifact for artifact in artifacts}
    case_capabilities: list[dict[str, Any]] = []
    for case in cases:
        expected = next(
            (str(path) for path in case.get("expected_files", []) if str(path).endswith("expected.cdbc")),
            None,
        )
        if expected is None or expected not in capabilities_by_path:
            errors.append(f"artifact case has no inspected expected.cdbc: {case.get('case_id')}")
            continue
        artifact = capabilities_by_path[expected]
        case_capabilities.append(
            {
                "case_id": case["case_id"],
                "result_name": case["result_name"],
                "fixture": case.get("fixture"),
                "artifact": expected,
                "format_family": artifact["format_family"],
                "format_version": artifact["format_version"],
                "section_order": artifact["section_order"],
                "envelope_capabilities": artifact["envelope_capabilities"],
            }
        )

    capability_counts = {
        "with_functions": sum(artifact["functions"] > 0 for artifact in artifacts),
        "with_native_call": sum(artifact["has_native_call"] for artifact in artifacts),
        "with_debug_sources": sum(artifact["has_debug_sources"] for artifact in artifacts),
        "with_debug_locations": sum(artifact["has_debug_locations"] for artifact in artifacts),
        "with_debug_ranges": sum(artifact["has_debug_ranges"] for artifact in artifacts),
        "with_debug_source_modules": sum(
            artifact["has_debug_source_modules"] for artifact in artifacts
        ),
    }
    return {
        "schema_version": 1,
        "milestone": "M0.5B",
        "audit_revision": decisions["audit_revision"],
        "baseline_commit": decisions["baseline_commit"],
        "verification_inventory_revision": decisions["inventory_revision"],
        "contract": decisions["contract"],
        "corpus": {
            "inventory_path": "tests/verification_inventory.json",
            "artifact_runner": "artifact",
            "artifact_case_count": len(cases),
            "artifact_fixture_count": len(artifacts),
            "artifact_case_ids": [str(case["case_id"]) for case in cases],
        },
        "envelope": {
            "current": decisions["current_envelope"],
            "observed_header_versions": sorted({artifact["header"] for artifact in artifacts}),
            "observed_capability_counts": capability_counts,
            "artifacts": artifacts,
            "case_capabilities": case_capabilities,
        },
        "evolution_decisions": decisions["evolution_decisions"],
        "successor_decision": decisions["successor_decision"],
        "invalid_input_policy": decisions["invalid_input_policy"],
        "m4a_compatibility": m4a_decision,
        "errors": errors,
    }


def run_process(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
    )


def run_dynamic_audit(
    report: dict[str, Any],
    compiler: Path,
    vm_manifest: Path,
    repo_root: Path = REPO_ROOT,
) -> dict[str, Any]:
    errors = list(report.get("errors", []))
    case_dirs = bytecode_artifact_tests.discover_cases(TESTS_DIR / "bytecode_artifacts")
    assertion_results = []
    for case_dir in case_dirs:
        assertion_results.extend(bytecode_artifact_tests.check_case(compiler, vm_manifest, case_dir))
    assertion_failures = [result for result in assertion_results if not result.passed]
    assertion_report = {
        "total": len(assertion_results),
        "passed": len(assertion_results) - len(assertion_failures),
        "failed": len(assertion_failures),
        "failure_names": [result.name for result in assertion_failures],
    }
    if assertion_failures:
        errors.extend(result.message for result in assertion_failures)

    reference_dumps: list[dict[str, Any]] = []
    for artifact in report["envelope"]["artifacts"]:
        artifact_path = repo_root / str(artifact["path"])
        dumped = run_process(
            [
                "cargo",
                "run",
                "--quiet",
                "--manifest-path",
                str(vm_manifest),
                "--",
                "dump",
                str(artifact_path),
            ],
            repo_root,
        )
        expected = artifact_path.read_text(encoding="utf-8")
        passed = dumped.returncode == 0 and not dumped.stderr and dumped.stdout == expected
        reference_dumps.append(
            {
                "artifact": artifact["path"],
                "passed": passed,
                "returncode": dumped.returncode,
                "stdout_bytes": len(dumped.stdout.encode("utf-8")),
                "stderr_empty": not dumped.stderr,
            }
        )
        if not passed:
            errors.append(f"reference dump mismatch: {artifact['path']}")

    invalid_probes: list[dict[str, Any]] = []
    invalid_artifact_probes: list[dict[str, Any]] = []
    seed_path = repo_root / str(report["envelope"]["artifacts"][0]["path"])
    seed = seed_path.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="compiler-design-cdbc-audit-") as temp_dir:
        temp_root = Path(temp_dir)
        for probe_id, header in (
            ("invalid-family", "not_cdbc 0.1"),
            ("invalid-version", "cdbc 9.9"),
        ):
            payload = header + "\n" + seed.split("\n", 1)[1]
            probe_path = temp_root / f"{probe_id}.cdbc"
            probe_path.write_text(payload, encoding="utf-8")
            rejected = run_process(
                [
                    "cargo",
                    "run",
                    "--quiet",
                    "--manifest-path",
                    str(vm_manifest),
                    "--",
                    "dump",
                    str(probe_path),
                ],
                repo_root,
            )
            passed = rejected.returncode != 0 and not rejected.stdout and bool(rejected.stderr)
            invalid_probes.append(
                {
                    "probe_id": probe_id,
                    "header": header,
                    "mode": "dump-only",
                    "passed": passed,
                    "returncode": rejected.returncode,
                    "stdout_empty": not rejected.stdout,
                    "stderr_nonempty": bool(rejected.stderr),
                }
            )
            if not passed:
                errors.append(f"invalid {probe_id} was not rejected by Rust dump")

        artifact_probe_specs = (
            ("invalid-register", "tests/bytecode_artifacts/arithmetic/expected.cdbc", "invalid_register"),
            ("invalid-constant", "tests/bytecode_artifacts/arithmetic/expected.cdbc", "invalid_constant"),
            ("invalid-jump", "tests/bytecode_artifacts/arithmetic/expected.cdbc", "invalid_jump"),
            ("invalid-name", "tests/bytecode_artifacts/arithmetic/expected.cdbc", "invalid_name"),
            ("invalid-number", "tests/bytecode_artifacts/arithmetic/expected.cdbc", "invalid_number"),
            ("invalid-function", "tests/bytecode_artifacts/functions_arrays/expected.cdbc", "invalid_function"),
            ("invalid-native", "tests/bytecode_artifacts/maps/expected.cdbc", "invalid_native"),
        )
        for probe_id, seed_relative, mutation in artifact_probe_specs:
            probe_seed = (repo_root / seed_relative).read_text(encoding="utf-8")
            probe_path = temp_root / f"{probe_id}.cdbc"
            probe_path.write_text(malformed_corpus.mutate_cdbc(probe_seed, mutation), encoding="utf-8")
            rejected = run_process(
                [
                    "cargo",
                    "run",
                    "--quiet",
                    "--manifest-path",
                    str(vm_manifest),
                    "--",
                    "dump",
                    str(probe_path),
                ],
                repo_root,
            )
            passed = rejected.returncode != 0 and not rejected.stdout and bool(rejected.stderr)
            invalid_artifact_probes.append(
                {
                    "probe_id": probe_id,
                    "seed": seed_relative,
                    "mutation": mutation,
                    "mode": "dump-only",
                    "passed": passed,
                    "returncode": rejected.returncode,
                    "stdout_empty": not rejected.stdout,
                    "stderr_nonempty": bool(rejected.stderr),
                }
            )
            if not passed:
                errors.append(f"invalid artifact probe {probe_id} was not rejected by Rust dump")

    report["dynamic_verification"] = {
        "artifact_assertions": assertion_report,
        "reference_dumps": {
            "total": len(reference_dumps),
            "passed": sum(item["passed"] for item in reference_dumps),
            "failed": sum(not item["passed"] for item in reference_dumps),
            "byte_for_byte": all(item["passed"] for item in reference_dumps),
        },
        "invalid_header_probes": {
            "total": len(invalid_probes),
            "passed": sum(item["passed"] for item in invalid_probes),
            "failed": sum(not item["passed"] for item in invalid_probes),
            "probes": invalid_probes,
            "dump_invocations": len(invalid_probes),
            "run_invocations": 0,
        },
        "invalid_artifact_probes": {
            "total": len(invalid_artifact_probes),
            "passed": sum(item["passed"] for item in invalid_artifact_probes),
            "failed": sum(not item["passed"] for item in invalid_artifact_probes),
            "probes": invalid_artifact_probes,
            "dump_invocations": len(invalid_artifact_probes),
            "run_invocations": 0,
        },
    }
    report["errors"] = errors
    report["summary"] = {
        "passed": not errors,
        "artifact_case_count": report["corpus"]["artifact_case_count"],
        "artifact_fixture_count": report["corpus"]["artifact_fixture_count"],
        "artifact_assertions": assertion_report["passed"],
        "reference_dumps": report["dynamic_verification"]["reference_dumps"]["passed"],
        "invalid_header_probes": report["dynamic_verification"]["invalid_header_probes"]["passed"],
        "invalid_artifact_probes": report["dynamic_verification"]["invalid_artifact_probes"]["passed"],
        "errors": len(errors),
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit the cdbc 0.1 artifact contract.")
    parser.add_argument("compiler", type=Path, help="Path to compiler_design executable")
    parser.add_argument("vm", type=Path, help="Path to vm-rs directory or Cargo.toml")
    parser.add_argument("--decision-log", type=Path, default=DEFAULT_DECISION_LOG)
    parser.add_argument("--inventory", type=Path, default=TESTS_DIR / "verification_inventory.json")
    parser.add_argument("--report", type=Path, required=True, help="Write the machine-readable audit report")
    args = parser.parse_args()
    repo_root = REPO_ROOT.resolve()
    compiler = args.compiler.resolve()
    vm_path = args.vm.resolve()
    vm_manifest = vm_path / "Cargo.toml" if vm_path.is_dir() else vm_path
    if not compiler.is_file():
        print(f"compiler not found: {compiler}", file=sys.stderr)
        return 64
    if not vm_manifest.is_file():
        print(f"Rust VM manifest not found: {vm_manifest}", file=sys.stderr)
        return 64
    try:
        decisions = load_decisions(args.decision_log.resolve())
        inventory = load_inventory(args.inventory.resolve())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"cdbc contract audit: {error}", file=sys.stderr)
        return 64
    report = build_static_report(decisions, inventory, repo_root)
    report = run_dynamic_audit(report, compiler, vm_manifest, repo_root)
    report_path = args.report.resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    summary = report["summary"]
    print(
        "cdbc contract audit: "
        f"{summary['artifact_assertions']} artifact assertions, "
        f"{summary['reference_dumps']} reference dumps, "
        f"{summary['invalid_header_probes']} invalid-header probes, "
        f"{summary['invalid_artifact_probes']} invalid-artifact probes passed"
    )
    for error in report["errors"]:
        print(f"FAIL {error}", file=sys.stderr)
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
