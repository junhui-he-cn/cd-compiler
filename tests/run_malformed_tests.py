#!/usr/bin/env python3

"""Run the bounded deterministic malformed-input corpus."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

from boundary_comparison import canonicalize, load_allowlist
from malformed_corpus import MANIFEST_PATH, case_payload, expand_cases, limits, load_manifest, minimize_text
from run_golden_tests import CheckResult


TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
ALLOWLIST_PATH = TESTS_DIR / "boundary_allowlist.json"


def command_observation(
    command: List[str],
    *,
    timeout_seconds: float,
    allowlist: Dict[str, object],
) -> dict[str, object]:
    try:
        completed = subprocess.run(
            command,
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            check=False,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        return {
            "status": "timeout",
            "returncode": None,
            "stdout": canonicalize(error.stdout or "", allowlist),
            "stderr": canonicalize(error.stderr or "", allowlist),
        }
    except OSError as error:
        return {"status": "crash", "returncode": None, "stdout": "", "stderr": str(error)}

    stdout = canonicalize(completed.stdout, allowlist)
    stderr = canonicalize(completed.stderr, allowlist)
    crash_markers = (
        "AddressSanitizer",
        "UndefinedBehaviorSanitizer",
        "Segmentation fault",
        "panicked at",
    )
    status = "crash" if completed.returncode < 0 or any(marker in stderr for marker in crash_markers) else "completed"
    return {
        "status": status,
        "returncode": completed.returncode,
        "stdout": stdout,
        "stderr": stderr,
    }


def case_command(
    compiler: Path,
    vm_manifest: Path,
    case: dict[str, object],
    payload_path: Optional[Path] = None,
) -> List[str]:
    input_kind = str(case["input_kind"])
    if input_kind == "source_text":
        if payload_path is None:
            raise ValueError("source text requires a temporary source path")
        return [str(compiler), str(payload_path)]
    if input_kind == "source_path":
        return [str(compiler), str(REPO_ROOT / str(case["source_path"]))]
    if input_kind == "cdbc_mutation":
        if payload_path is None:
            raise ValueError("cdbc mutation requires a temporary artifact path")
        return [
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(vm_manifest),
            "--",
            "dump",
            str(payload_path),
        ]
    raise ValueError(f"unknown malformed case input kind: {input_kind}")


def observation_signature(observation: dict[str, object]) -> tuple[object, ...]:
    return (
        observation.get("status"),
        observation.get("returncode"),
        observation.get("stdout"),
        observation.get("stderr"),
    )


def classify_observations(
    case: dict[str, object],
    first: dict[str, object],
    second: dict[str, object],
) -> tuple[bool, str, str]:
    case_id = str(case["case_id"])
    if observation_signature(first) != observation_signature(second):
        return False, "non_deterministic", f"FAIL {case_id} produced different observations for the same seed"
    if first["status"] == "timeout":
        return False, "timeout", f"FAIL {case_id} exceeded the per-case timeout"
    if first["status"] == "crash":
        return False, "crash", f"FAIL {case_id} crashed or triggered a sanitizer/panic marker\n{first['stderr']}"
    if first.get("returncode") == 0:
        return False, "unexpected_accept", f"FAIL {case_id} accepted malformed input"
    if first.get("stdout"):
        return False, "unexpected_stdout", f"FAIL {case_id} produced unexpected stdout\n{first['stdout']}"
    return True, "pass", ""


def run_case_once(
    compiler: Path,
    vm_manifest: Path,
    case: dict[str, object],
    payload: str,
    timeout_seconds: float,
    allowlist: Dict[str, object],
    workspace: Optional[Path] = None,
) -> dict[str, object]:
    input_kind = str(case["input_kind"])
    if input_kind in ("source_text", "cdbc_mutation"):
        if workspace is None:
            prefix = "compiler-design-malformed-source-" if input_kind == "source_text" else "compiler-design-malformed-"
            with tempfile.TemporaryDirectory(prefix=prefix) as temp_dir:
                return run_case_once(
                    compiler,
                    vm_manifest,
                    case,
                    payload,
                    timeout_seconds,
                    allowlist,
                    Path(temp_dir),
                )
        payload_name = "input.cd" if input_kind == "source_text" else "mutated.cdbc"
        payload_path = workspace / payload_name
        payload_path.write_text(payload, encoding="utf-8")
        command = case_command(compiler, vm_manifest, case, payload_path)
        return command_observation(
            command,
            timeout_seconds=timeout_seconds,
            allowlist=allowlist,
        )
    command = case_command(compiler, vm_manifest, case)
    return command_observation(
        command,
        timeout_seconds=timeout_seconds,
        allowlist=allowlist,
    )


def run_case_pair(
    compiler: Path,
    vm_manifest: Path,
    case: dict[str, object],
    payload: str,
    timeout_seconds: float,
    allowlist: Dict[str, object],
) -> tuple[dict[str, object], dict[str, object]]:
    input_kind = str(case["input_kind"])
    if input_kind not in ("source_text", "cdbc_mutation"):
        return (
            run_case_once(compiler, vm_manifest, case, payload, timeout_seconds, allowlist),
            run_case_once(compiler, vm_manifest, case, payload, timeout_seconds, allowlist),
        )
    prefix = "compiler-design-malformed-source-" if input_kind == "source_text" else "compiler-design-malformed-"
    with tempfile.TemporaryDirectory(prefix=prefix) as temp_dir:
        workspace = Path(temp_dir)
        first = run_case_once(
            compiler,
            vm_manifest,
            case,
            payload,
            timeout_seconds,
            allowlist,
            workspace,
        )
        second = run_case_once(
            compiler,
            vm_manifest,
            case,
            payload,
            timeout_seconds,
            allowlist,
            workspace,
        )
        return first, second


def save_failure_input(
    failure_dir: Path,
    case: dict[str, object],
    payload: str,
    compiler: Path,
    vm_manifest: Path,
    timeout_seconds: float,
    allowlist: Dict[str, object],
    first_observation: dict[str, object],
) -> str:
    signature = observation_signature(first_observation)

    def minimize_with_workspace(workspace: Optional[Path]) -> str:
        def still_fails(candidate: str) -> bool:
            observation = run_case_once(
                compiler,
                vm_manifest,
                case,
                candidate,
                timeout_seconds,
                allowlist,
                workspace,
            )
            return observation_signature(observation) == signature

        return minimize_text(payload, still_fails, max_rounds=200)

    if str(case["input_kind"]) == "source_text":
        with tempfile.TemporaryDirectory(prefix="compiler-design-malformed-source-") as temp_dir:
            minimized = minimize_with_workspace(Path(temp_dir))
    elif str(case["input_kind"]) == "cdbc_mutation":
        with tempfile.TemporaryDirectory(prefix="compiler-design-malformed-") as temp_dir:
            minimized = minimize_with_workspace(Path(temp_dir))
    else:
        minimized = minimize_with_workspace(None)
    safe_name = str(case["case_id"]).replace("/", "_")
    output_path = failure_dir / f"{safe_name}.input"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(minimized, encoding="utf-8")
    return str(output_path)


def run_all(
    compiler: Path,
    vm_manifest: Path,
    *,
    manifest_path: Path = MANIFEST_PATH,
    report_path: Optional[Path] = None,
    failure_dir: Optional[Path] = None,
    case_filters: tuple[str, ...] = (),
) -> List[CheckResult]:
    manifest = load_manifest(manifest_path)
    corpus_cases = expand_cases(manifest, REPO_ROOT)
    allowlist = load_allowlist(ALLOWLIST_PATH)
    corpus_limits = limits(manifest)
    started = time.monotonic()
    detailed_cases: List[dict[str, object]] = []
    results: List[CheckResult] = []

    selected_cases = [
        case
        for case in corpus_cases
        if not case_filters or any(case_filter in str(case["case_id"]) for case_filter in case_filters)
    ]
    for index, case in enumerate(selected_cases):
        case_id = str(case["case_id"])
        if time.monotonic() - started > float(corpus_limits["max_total_seconds"]):
            message = f"FAIL {case_id} corpus total budget exhausted"
            results.append(CheckResult(case_id, False, message))
            detailed_cases.append({"case_id": case_id, "passed": False, "classification": "budget_exhausted"})
            continue
        payload = case_payload(case, REPO_ROOT)
        input_bytes = len(payload.encode("utf-8"))
        if input_bytes > int(corpus_limits["max_input_bytes"]):
            message = f"FAIL {case_id} input is {input_bytes} bytes, over corpus limit"
            results.append(CheckResult(case_id, False, message))
            detailed_cases.append({"case_id": case_id, "passed": False, "classification": "input_too_large", "input_bytes": input_bytes})
            continue

        first, second = run_case_pair(
            compiler,
            vm_manifest,
            case,
            payload,
            float(corpus_limits["per_case_timeout_seconds"]),
            allowlist,
        )
        passed, classification, message = classify_observations(case, first, second)
        detailed: dict[str, object] = {
            "case_id": case_id,
            "kind": case["input_kind"],
            "stage": case["stage"],
            "input_bytes": input_bytes,
            "passed": passed,
            "classification": classification,
            "first": first,
            "second": second,
        }
        if not passed and failure_dir is not None:
            detailed["minimized_input"] = save_failure_input(
                failure_dir,
                case,
                payload,
                compiler,
                vm_manifest,
                float(corpus_limits["per_case_timeout_seconds"]),
                allowlist,
                first,
            )
        detailed_cases.append(detailed)
        results.append(CheckResult(case_id, passed, message))

    failed = [case for case in detailed_cases if not case["passed"]]
    report = {
        "schema_version": 1,
        "corpus_revision": manifest["corpus_revision"],
        "seed": manifest["seed"],
        "limits": corpus_limits,
        "summary": {
            "total": len(detailed_cases),
            "passed": len(detailed_cases) - len(failed),
            "failed": len(failed),
            "timeouts": sum(case["classification"] == "timeout" for case in detailed_cases),
            "crashes": sum(case["classification"] == "crash" for case in detailed_cases),
            "non_deterministic": sum(case["classification"] == "non_deterministic" for case in detailed_cases),
        },
        "cases": detailed_cases,
    }
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return results


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the bounded malformed-input corpus.")
    parser.add_argument("compiler", type=Path, help="Path to compiler_design executable")
    parser.add_argument("vm", type=Path, help="Path to vm-rs directory or Cargo.toml")
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    parser.add_argument("--report", type=Path, help="Write a machine-readable corpus report")
    parser.add_argument("--failure-dir", type=Path, help="Save minimized failing inputs under this directory")
    parser.add_argument("--case", action="append", default=[], dest="case_filters")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
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
        results = run_all(
            compiler,
            vm_manifest,
            manifest_path=args.manifest.resolve(),
            report_path=args.report.resolve() if args.report else None,
            failure_dir=args.failure_dir.resolve() if args.failure_dir else None,
            case_filters=tuple(args.case_filters),
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"malformed tests: {error}", file=sys.stderr)
        return 64
    failed = [result for result in results if not result.passed]
    for failure in failed:
        print(failure.message, file=sys.stderr)
    passed_count = len(results) - len(failed)
    print(f"malformed tests: {passed_count} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
