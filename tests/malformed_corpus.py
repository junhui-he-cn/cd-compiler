#!/usr/bin/env python3

"""Deterministic malformed-input corpus expansion and mutation helpers."""

from __future__ import annotations

import json
import math
import re
from pathlib import Path
from typing import Callable, Dict, Optional, Union


TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
MANIFEST_PATH = TESTS_DIR / "malformed_cases.json"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def load_manifest(path: Path = MANIFEST_PATH) -> dict[str, object]:
    value = json.loads(read_text(path))
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise ValueError(f"invalid malformed corpus manifest: {path}")
    return value


def relative_path(path: Path, repo_root: Path = REPO_ROOT) -> str:
    return path.resolve().relative_to(repo_root.resolve()).as_posix()


def _source_case(manifest_case: dict[str, object]) -> dict[str, object]:
    stage = str(manifest_case["stage"])
    terminal = "tokens" if stage == "lexical" else "parse_diagnostic"
    if stage == "diagnostic-recovery":
        terminal = "parse_diagnostic"
    return {
        "case_id": str(manifest_case["case_id"]),
        "result_name": str(manifest_case["case_id"]),
        "input_kind": "source_text",
        "input_text": str(manifest_case["source"]),
        "fixture": relative_path(MANIFEST_PATH),
        "sources": [relative_path(MANIFEST_PATH)],
        "expected_files": [],
        "stage": stage,
        "capability_tags": [str(tag) for tag in manifest_case["capability_tags"]],
        "backend": "cpp",
        "expected_result_kind": "reject",
        "expected": "reject",
        "boundary_sequence": ["tokens", "ast", terminal] if terminal != "tokens" else ["tokens"],
        "terminal_boundary": terminal,
    }


def _parse_error_cases(manifest: dict[str, object], repo_root: Path) -> list[dict[str, object]]:
    family = manifest["parse_error_family"]
    root = repo_root / str(family["root"])
    pattern = str(family["pattern"])
    prefix = str(family["case_prefix"])
    stage = str(family["stage"])
    tags = [str(tag) for tag in family["capability_tags"]]
    cases = []
    for source in sorted(root.glob(pattern)):
        stem = source.with_suffix("")
        cases.append(
            {
                "case_id": f"{prefix}.{source.stem}",
                "result_name": f"{prefix}.{source.stem}",
                "input_kind": "source_path",
                "source_path": relative_path(source, repo_root),
                "fixture": relative_path(source.parent, repo_root),
                "sources": [relative_path(source, repo_root)],
                "expected_files": [
                    relative_path(source, repo_root),
                    relative_path(stem.with_suffix(".err"), repo_root),
                    relative_path(stem.with_suffix(".exit"), repo_root),
                ],
                "stage": stage,
                "capability_tags": tags,
                "backend": "cpp",
                "expected_result_kind": "reject",
                "expected": "reject",
                "boundary_sequence": ["tokens", "ast", "parse_diagnostic"],
                "terminal_boundary": "parse_diagnostic",
            }
        )
    return cases


def mutate_cdbc(source: str, mutation: str) -> str:
    if mutation == "bad_header":
        lines = source.splitlines(keepends=True)
        return "cdbc 9.9\n" + "".join(lines[1:])
    if mutation == "truncate":
        main_header = re.search(r"(?m)^main registers=", source)
        if main_header is not None:
            return source[: main_header.end()]
        return source[: max(1, len(source) // 4)]
    if mutation == "unknown_opcode":
        match = re.search(r"(?m)^(\s*(?:r\d+ = )?)print\b", source)
        if match is None:
            match = re.search(r"(?m)^(\s*r\d+ = )([a-z_]+)\b", source)
            if match is None:
                raise ValueError("cdbc seed has no instruction to mutate")
            start = match.start(2)
            end = match.end(2)
        else:
            start = match.start() + len(match.group(1))
            end = start + len("print")
        return source[:start] + "unknown_opcode" + source[end:]
    if mutation == "trailing_garbage":
        return source.rstrip("\n") + "\nnot_a_cdbc_section\n"
    if mutation == "duplicate_section":
        marker = "\nnames:\n"
        if marker not in source:
            raise ValueError("cdbc seed has no names section to duplicate around")
        return source.replace(marker, "\nconstants:\n\n\nnames:\n", 1)
    if mutation == "invalid_escape":
        match = re.search(r"(?m)^  c\d+ = string \"", source)
        if match is None:
            raise ValueError("cdbc seed has no string constant to mutate")
        return source[: match.end()] + r"\q" + source[match.end() :]
    if mutation == "reverse_range":
        marker = "\ndebug_ranges:\n"
        if marker not in source:
            raise ValueError("cdbc seed has no debug range section to reverse")
        prefix, ranges = source.split(marker, 1)
        match = re.search(r"(?m)^(  (?:main|function f\d+) \d+ = s\d+:)(\d+):(\d+)$", ranges)
        if match is None:
            raise ValueError("cdbc seed has no debug range to reverse")
        ranges = ranges[: match.start()] + match.group(1) + match.group(3) + ":" + match.group(2) + ranges[match.end() :]
        return prefix + marker + ranges
    if mutation == "invalid_register":
        match = re.search(r"(?m)^  r0 = ", source)
        if match is None:
            raise ValueError("cdbc seed has no main destination register to mutate")
        start = match.start() + 2
        return source[:start] + "r999" + source[start + 2 :]
    if mutation == "invalid_constant":
        match = re.search(r"constant c\d+", source)
        if match is None:
            raise ValueError("cdbc seed has no constant reference to mutate")
        return source[: match.start()] + "constant c999" + source[match.end() :]
    if mutation == "invalid_function":
        match = re.search(r"make_function f\d+", source)
        if match is None:
            raise ValueError("cdbc seed has no function reference to mutate")
        return source[: match.start()] + "make_function f999" + source[match.end() :]
    if mutation == "invalid_name":
        insertion = "  store_var n999, r0\n"
        for marker in ("\nfunction ", "\ndebug_sources:", "\ndebug_locations:", "\ndebug_ranges:"):
            position = source.find(marker)
            if position != -1:
                return source[:position] + "\n" + insertion + source[position:]
        return source.rstrip("\n") + "\n" + insertion
    if mutation == "invalid_jump":
        insertion = "  jump 999\n"
        for marker in ("\nfunction ", "\ndebug_sources:", "\ndebug_locations:", "\ndebug_ranges:"):
            position = source.find(marker)
            if position != -1:
                return source[:position] + "\n" + insertion + source[position:]
        return source.rstrip("\n") + "\n" + insertion
    if mutation == "invalid_number":
        match = re.search(r"(?m)^  c\d+ = number [^\n]+", source)
        if match is None:
            raise ValueError("cdbc seed has no number constant to mutate")
        prefix = source[match.start() :].split(" = number ", 1)[0]
        return source[: match.start()] + prefix + " = number not-a-number" + source[match.end() :]
    if mutation == "invalid_native":
        match = re.search(r"call_native i\d+", source)
        if match is None:
            raise ValueError("cdbc seed has no native call to mutate")
        return source[: match.start()] + "call_native i999" + source[match.end() :]
    raise ValueError(f"unknown cdbc mutation: {mutation}")


def _cdbc_cases(manifest: dict[str, object], repo_root: Path) -> list[dict[str, object]]:
    cases = []
    for family in manifest["cdbc_cases"]:
        fixture = repo_root / str(family["fixture"])
        for mutation in family["mutations"]:
            mutation_name = str(mutation)
            cases.append(
                {
                    "case_id": f"{family['case_prefix']}.{mutation_name}",
                    "result_name": f"{family['case_prefix']}.{mutation_name}",
                    "input_kind": "cdbc_mutation",
                    "artifact_seed": relative_path(fixture, repo_root),
                    "mutation": mutation_name,
                    "fixture": relative_path(fixture.parent, repo_root),
                    "sources": [relative_path(fixture, repo_root)],
                    "expected_files": [relative_path(fixture, repo_root)],
                    "stage": "cdbc-parser",
                    "capability_tags": ["malformed", "cdbc", "rust-parser", mutation_name],
                    "backend": "rust-vm",
                    "expected_result_kind": "reject",
                    "expected": "reject",
                    "boundary_sequence": ["cdbc", "rust_decode"],
                    "terminal_boundary": "rust_decode",
                }
            )
    return cases


def expand_cases(manifest: Optional[dict[str, object]] = None, repo_root: Path = REPO_ROOT) -> list[dict[str, object]]:
    selected = manifest if manifest is not None else load_manifest()
    cases = [_source_case(case) for case in selected["source_cases"]]
    cases.extend(_parse_error_cases(selected, repo_root))
    cases.extend(_cdbc_cases(selected, repo_root))
    cases.sort(key=lambda case: str(case["case_id"]))
    case_ids = [str(case["case_id"]) for case in cases]
    if len(case_ids) != len(set(case_ids)):
        raise ValueError("malformed corpus has duplicate case IDs")
    return cases


def limits(manifest: Optional[dict[str, object]] = None) -> Dict[str, Union[float, int]]:
    selected = manifest if manifest is not None else load_manifest()
    raw = selected["limits"]
    return {
        "max_input_bytes": int(raw["max_input_bytes"]),
        "per_case_timeout_seconds": float(raw["per_case_timeout_seconds"]),
        "max_total_seconds": float(raw["max_total_seconds"]),
    }


def case_payload(case: dict[str, object], repo_root: Path = REPO_ROOT) -> str:
    input_kind = case["input_kind"]
    if input_kind == "source_text":
        return str(case["input_text"])
    if input_kind == "source_path":
        return read_text(repo_root / str(case["source_path"]))
    if input_kind == "cdbc_mutation":
        seed = read_text(repo_root / str(case["artifact_seed"]))
        return mutate_cdbc(seed, str(case["mutation"]))
    raise ValueError(f"unknown malformed case input kind: {input_kind}")


def minimize_text(text: str, predicate: Callable[[str], bool], max_rounds: int = 1000) -> str:
    """Deterministically shrink text while ``predicate`` remains true."""

    current = text
    if not predicate(current):
        return current
    granularity = 2
    rounds = 0
    while len(current) >= 2 and rounds < max_rounds:
        rounds += 1
        chunk = max(1, math.ceil(len(current) / granularity))
        reduced = False
        for start in range(0, len(current), chunk):
            candidate = current[:start] + current[start + chunk :]
            if candidate and predicate(candidate):
                current = candidate
                granularity = max(2, granularity - 1)
                reduced = True
                break
        if not reduced:
            if granularity >= len(current):
                break
            granularity = min(len(current), granularity * 2)
    return current
