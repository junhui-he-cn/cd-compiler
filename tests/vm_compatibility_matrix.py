#!/usr/bin/env python3

"""Validate the compiler/Rust VM compatibility matrix against source facts."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any


TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
MATRIX_PATH = REPO_ROOT / "docs" / "decisions" / "x1-compiler-vm-compatibility-001.json"
INVENTORY_PATH = TESTS_DIR / "verification_inventory.json"
SCHEMA_VERSION = 1
REQUIRED_CELL_IDS = {
    "artifact.debug_metadata.cdbc_0_1",
    "artifact.linked.cdbc_0_1",
    "artifact.metadata_free.cdbc_0_1",
    "artifact.module.cdbc_0_1",
    "library.cli.version_0_1",
    "module_cache.cdbc_cache_0_2_schema4",
    "native.fixed_registry",
}
REQUIRED_COMMAND_MARKERS = {
    "tests/verification_inventory.py",
    "tests/cdbc_contract_audit.py",
    "tests/bytecode_artifact_tests.py",
    "tests/bytecode_module_artifact_tests.py",
    "tests/bytecode_module_cache_tests.py",
    "vm-rs/Cargo.toml",
}
NATIVE_NAME_PATTERN = re.compile(r'^\s*name: "([^"]+)",$', re.MULTILINE)


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def load_matrix(path: Path = MATRIX_PATH) -> dict[str, Any]:
    return read_json(path)


def _read_version(path: Path) -> str:
    return path.read_text(encoding="utf-8").strip()


def _cargo_version(path: Path) -> str | None:
    match = re.search(r'^version = "([^"]+)"$', path.read_text(encoding="utf-8"), re.MULTILINE)
    return match.group(1) if match else None


def _constant(path: Path, name: str) -> str | None:
    match = re.search(
        rf'pub const {re.escape(name)}: &str = "([^"]+)";',
        path.read_text(encoding="utf-8"),
    )
    return match.group(1) if match else None


def _native_names(path: Path) -> list[str]:
    source = path.read_text(encoding="utf-8")
    start = source.index("const NATIVE_SPECS")
    end = source.index("fn native_spec", start)
    return NATIVE_NAME_PATTERN.findall(source[start:end])


def _field(mapping: Any, name: str) -> Any:
    return mapping.get(name) if isinstance(mapping, dict) else None


def _check_source_path(repo_root: Path, value: Any, label: str, errors: list[str]) -> Path | None:
    if not isinstance(value, str) or not value:
        errors.append(f"{label} source is required")
        return None
    path = repo_root / value
    if not path.is_file():
        errors.append(f"{label} source is missing: {value}")
        return None
    return path


def validate_matrix(matrix: dict[str, Any], repo_root: Path = REPO_ROOT) -> list[str]:
    errors: list[str] = []
    if matrix.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}")
    if matrix.get("decision_id") != "X1-COMPILER-VM-COMPAT-001":
        errors.append("decision_id must be X1-COMPILER-VM-COMPAT-001")
    if matrix.get("status") != "resolved":
        errors.append("X1 matrix must be resolved")
    for field in ("matrix_revision", "inventory_revision", "reference_commit"):
        if not isinstance(matrix.get(field), str) or not matrix[field]:
            errors.append(f"{field} is required")

    try:
        inventory = read_json(repo_root / "tests" / "verification_inventory.json")
        if matrix.get("inventory_revision") != inventory.get("inventory_revision"):
            errors.append("matrix inventory revision does not match verification inventory")
    except (OSError, ValueError, json.JSONDecodeError) as error:
        errors.append(f"unable to read verification inventory: {error}")

    contract = matrix.get("version_contract")
    if not isinstance(contract, dict):
        errors.append("version_contract must be an object")
        contract = {}

    compiler = _field(contract, "compiler")
    compiler_source = _check_source_path(repo_root, _field(compiler, "source"), "compiler", errors)
    if compiler_source is not None and _field(compiler, "version") != _read_version(repo_root / "VERSION"):
        errors.append("compiler version does not match VERSION")

    artifact = _field(contract, "artifact")
    artifact_source = _check_source_path(repo_root, _field(artifact, "source"), "artifact", errors)
    if _field(artifact, "family") != "cdbc":
        errors.append("artifact family must remain cdbc")
    if _field(artifact, "version") != "0.1":
        errors.append("artifact version must remain 0.1")
    if _field(artifact, "header") != "cdbc 0.1":
        errors.append("artifact header must remain cdbc 0.1")
    if artifact_source is not None:
        source = artifact_source.read_text(encoding="utf-8")
        if 'ARTIFACT_FORMAT_FAMILY: &str = "cdbc"' not in source:
            errors.append("artifact source does not declare cdbc family")
        if 'ARTIFACT_FORMAT_VERSION: &str = "0.1"' not in source:
            errors.append("artifact source does not declare version 0.1")

    module_cache = _field(contract, "module_cache")
    module_cache_source = _check_source_path(
        repo_root,
        _field(module_cache, "source"),
        "module cache",
        errors,
    )
    if _field(module_cache, "family") != "cdbc-cache":
        errors.append("module cache family must remain cdbc-cache")
    if _field(module_cache, "version") != "0.2":
        errors.append("module cache version must remain 0.2")
    if _field(module_cache, "schema") != 4:
        errors.append("module cache schema must remain 4")
    if module_cache_source is not None:
        source = module_cache_source.read_text(encoding="utf-8")
        if '"cdbc-cache 0.2"' not in source or '"expected schema = 4"' not in source:
            errors.append("module cache source does not enforce cdbc-cache 0.2 schema 4")

    vm_library = _field(contract, "vm_library")
    vm_cargo = _check_source_path(repo_root, _field(vm_library, "cargo"), "VM library", errors)
    vm_source = _check_source_path(repo_root, _field(vm_library, "source"), "VM library", errors)
    if vm_cargo is not None and _field(vm_library, "crate_version") != _cargo_version(vm_cargo):
        errors.append("VM crate version does not match Cargo.toml")
    if vm_source is not None and _field(vm_library, "api_version") != _constant(vm_source, "LIBRARY_API_VERSION"):
        errors.append("VM library API version does not match lib.rs")
    if _field(vm_library, "api_version") != "0.1":
        errors.append("VM library API version must remain 0.1")

    native_abi = _field(contract, "native_abi")
    native_source = _check_source_path(repo_root, _field(native_abi, "source"), "native ABI", errors)
    if _field(native_abi, "mode") != "fixed-registered-names":
        errors.append("native ABI mode must remain fixed-registered-names")
    if _field(native_abi, "serialized") is not False:
        errors.append("native ABI names must remain non-serialized")
    if native_source is not None and _field(native_abi, "names") != _native_names(native_source):
        errors.append("native ABI names do not match vm.rs registry")

    debug_metadata = _field(contract, "debug_metadata")
    debug_source = _check_source_path(
        repo_root,
        _field(debug_metadata, "source"),
        "debug metadata",
        errors,
    )
    if _field(debug_metadata, "optional") is not True:
        errors.append("debug metadata must remain optional")
    if _field(debug_metadata, "sections") != ["debug_sources", "debug_locations", "debug_ranges"]:
        errors.append("debug metadata sections do not match cdbc 0.1")
    if _field(debug_metadata, "range_unit") != "source-local half-open byte interval":
        errors.append("debug range unit must remain source-local half-open byte interval")
    if debug_source is not None:
        source = debug_source.read_text(encoding="utf-8")
        for section in ("debug_sources", "debug_locations", "debug_ranges"):
            if f'"{section}"' not in source and f'line == "{section}:"' not in source:
                errors.append(f"debug metadata source does not recognize {section}")

    cells = matrix.get("compatibility_cells")
    if not isinstance(cells, list) or not cells:
        errors.append("compatibility_cells must be a non-empty list")
        cells = []
    cell_ids = [cell.get("cell_id") for cell in cells if isinstance(cell, dict)]
    if set(cell_ids) != REQUIRED_CELL_IDS:
        errors.append("compatibility cell IDs do not match the required set")
    if cell_ids != sorted(cell_ids):
        errors.append("compatibility cells must be sorted by cell_id")
    for cell in cells:
        if not isinstance(cell, dict):
            errors.append("every compatibility cell must be an object")
            continue
        cell_id = str(cell.get("cell_id", "<unknown>"))
        for field in ("domain", "state", "producer", "consumer", "evidence"):
            if field not in cell:
                errors.append(f"{cell_id} missing field: {field}")
        evidence = cell.get("evidence")
        if not isinstance(evidence, list) or not evidence:
            errors.append(f"{cell_id} evidence must be a non-empty list")
            continue
        for path_value in evidence:
            if not isinstance(path_value, str) or not (repo_root / path_value).exists():
                errors.append(f"{cell_id} missing evidence: {path_value}")

    rules = matrix.get("rules")
    if not isinstance(rules, list) or not rules:
        errors.append("rules must be a non-empty list")

    verification = matrix.get("verification")
    if not isinstance(verification, dict):
        errors.append("verification must be an object")
    else:
        commands = verification.get("commands")
        if not isinstance(commands, list) or not commands:
            errors.append("verification commands must be a non-empty list")
        else:
            for command in commands:
                if not isinstance(command, list) or not command or not all(
                    isinstance(part, str) and part for part in command
                ):
                    errors.append("verification commands must be non-empty string lists")
            command_text = {part for command in commands for part in command}
            for marker in sorted(REQUIRED_COMMAND_MARKERS - command_text):
                errors.append(f"verification commands missing {marker}")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=MATRIX_PATH)
    args = parser.parse_args(argv)
    try:
        matrix = load_matrix(args.matrix)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"VM compatibility matrix: {error}", file=sys.stderr)
        return 1
    errors = validate_matrix(matrix, REPO_ROOT)
    if errors:
        for error in errors:
            print(f"FAIL {error}", file=sys.stderr)
        return 1
    print(f"VM compatibility matrix: {len(matrix['compatibility_cells'])} cells validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
