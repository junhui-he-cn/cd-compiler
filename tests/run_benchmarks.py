#!/usr/bin/env python3

"""Measure compiler and Rust VM wall-clock time for fixed workloads.

Compilation, artifact loading, module linking, and execution are intentionally
separate measurements.  Each compile sample writes fresh temporary products.
Load samples invoke the already-built VM's canonical ``dump`` path, link
samples invoke the already-built VM's ``link`` path for module workloads, and
runtime samples invoke the resulting artifact; Cargo is never part of the
runtime command.

This runner is informational.  It validates every workload's output and
returns a failure when a benchmark program is not correct, but it does not
compare timings with a baseline or enforce a performance threshold.
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import math
import os
import platform
import shlex
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Iterable


TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
DEFAULT_MANIFEST = TESTS_DIR / "benchmark_manifest.json"
DEFAULT_REPORT = REPO_ROOT / "build" / "benchmark-report.json"
DEFAULT_COMPILER = REPO_ROOT / "build" / "compiler_design"
DEFAULT_VM = REPO_ROOT / "vm-rs"
SCHEMA_VERSION = 2
DEFAULT_TIMEOUT_SECONDS = 60.0
ARTIFACT_MODES = {"linked", "module"}
EXECUTION_MODES = {"success", "runtime_error"}


def read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON root must be an object: {path}")
    return value


def load_manifest(path: Path = DEFAULT_MANIFEST) -> dict[str, Any]:
    return read_json(path)


def _repo_path(repo_root: Path, value: Any, label: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{label} must be a non-empty repository-relative path")
    path = (repo_root / value).resolve()
    try:
        path.relative_to(repo_root.resolve())
    except ValueError as error:
        raise ValueError(f"{label} escapes the repository: {value}") from error
    return path


def validate_manifest(manifest: dict[str, Any], repo_root: Path) -> list[str]:
    """Return stable validation errors for a benchmark manifest."""

    errors: list[str] = []
    if manifest.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}")
    if not isinstance(manifest.get("benchmark_revision"), str) or not manifest.get(
        "benchmark_revision"
    ):
        errors.append("benchmark_revision is required")

    default_repeat = manifest.get("default_repeat")
    if isinstance(default_repeat, bool) or not isinstance(default_repeat, int) or default_repeat <= 0:
        errors.append("default_repeat must be a positive integer")

    workloads = manifest.get("workloads")
    if not isinstance(workloads, list) or not workloads:
        errors.append("workloads must be a non-empty list")
        return errors

    workload_ids: list[str] = []
    for workload in workloads:
        if not isinstance(workload, dict):
            errors.append("every workload must be an object")
            continue

        workload_id = str(workload.get("workload_id", "<unknown>"))
        workload_ids.append(workload_id)
        for field in ("workload_id", "sources", "artifact_mode", "execution"):
            if field not in workload:
                errors.append(f"{workload_id} missing field: {field}")

        artifact_mode = workload.get("artifact_mode")
        if artifact_mode not in ARTIFACT_MODES:
            errors.append(
                f"{workload_id}.artifact_mode must be one of: linked, module"
            )

        execution = workload.get("execution")
        if execution not in EXECUTION_MODES:
            errors.append(
                f"{workload_id}.execution must be one of: success, runtime_error"
            )

        sources = workload.get("sources")
        if not isinstance(sources, list) or not sources:
            errors.append(f"{workload_id}.sources must be a non-empty list")
        else:
            for index, source in enumerate(sources):
                try:
                    source_path = _repo_path(
                        repo_root, source, f"{workload_id}.sources[{index}]"
                    )
                except ValueError as error:
                    errors.append(str(error))
                    continue
                if not source_path.is_file():
                    errors.append(f"{workload_id} missing source: {source}")

        if execution == "success":
            if "expected_output" not in workload:
                errors.append(f"{workload_id} missing field: expected_output")
            else:
                try:
                    expected_path = _repo_path(
                        repo_root,
                        workload.get("expected_output"),
                        f"{workload_id}.expected_output",
                    )
                except ValueError as error:
                    errors.append(str(error))
                else:
                    if not expected_path.is_file():
                        errors.append(
                            f"{workload_id} missing expected output: "
                            f"{workload.get('expected_output')}"
                        )
        elif execution == "runtime_error":
            if "expected_stderr" not in workload:
                errors.append(f"{workload_id} missing field: expected_stderr")
            else:
                try:
                    expected_path = _repo_path(
                        repo_root,
                        workload.get("expected_stderr"),
                        f"{workload_id}.expected_stderr",
                    )
                except ValueError as error:
                    errors.append(str(error))
                else:
                    if not expected_path.is_file():
                        errors.append(
                            f"{workload_id} missing expected stderr: "
                            f"{workload.get('expected_stderr')}"
                        )

            expected_exit_code = workload.get("expected_exit_code")
            if isinstance(expected_exit_code, bool) or not isinstance(
                expected_exit_code, int
            ):
                errors.append(f"{workload_id}.expected_exit_code must be an integer")

    if len(workload_ids) != len(set(workload_ids)):
        errors.append("workload IDs must be unique")
    if workload_ids != sorted(workload_ids):
        errors.append("workloads must be sorted by workload_id")
    return errors


def command_display(command: Iterable[str]) -> str:
    return shlex.join(str(part) for part in command)


def rounded(value: float) -> float:
    return round(value, 6)


def _captured_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def run_command(
    command: list[str],
    cwd: Path,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
) -> tuple[float, int, str, str]:
    """Run a command and measure its wall-clock duration."""

    environment = os.environ.copy()
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            text=True,
            capture_output=True,
            check=False,
            env=environment,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        stdout = _captured_text(error.stdout)
        stderr = _captured_text(error.stderr)
        timeout_message = f"command timed out after {timeout_seconds:g}s"
        if stderr:
            stderr = f"{timeout_message}\n{stderr}"
        else:
            stderr = timeout_message
        return time.perf_counter() - started, 124, stdout, stderr
    except OSError as error:
        return time.perf_counter() - started, 127, "", f"{type(error).__name__}: {error}"
    return (
        time.perf_counter() - started,
        completed.returncode,
        completed.stdout,
        completed.stderr,
    )


def timing_record(samples: list[float]) -> dict[str, Any]:
    if not samples:
        return {"samples_seconds": [], "min": None, "median": None, "max": None}
    return {
        "samples_seconds": [rounded(sample) for sample in samples],
        "min": rounded(min(samples)),
        "median": rounded(statistics.median(samples)),
        "max": rounded(max(samples)),
    }


def _failure(command: list[str], returncode: int, stdout: str, stderr: str) -> str:
    parts = [f"command exited with {returncode}: {command_display(command)}"]
    if stdout:
        parts.extend(["STDOUT:", stdout.rstrip()])
    if stderr:
        parts.extend(["STDERR:", stderr.rstrip()])
    return "\n".join(parts)


def _output_mismatch(expected: str, actual: str) -> str:
    diff = "".join(
        difflib.unified_diff(
            expected.splitlines(keepends=True),
            actual.splitlines(keepends=True),
            fromfile="expected",
            tofile="actual",
        )
    )
    return diff.rstrip() or "output differs"


def _workload_sources(repo_root: Path, workload: dict[str, Any]) -> list[Path]:
    return [
        _repo_path(repo_root, source, f"{workload['workload_id']}.source")
        for source in workload["sources"]
    ]


def _all_validated(values: list[bool]) -> bool:
    return bool(values) and all(values)


def run_workload(
    repo_root: Path,
    compiler: Path,
    vm_binary: Path,
    workload: dict[str, Any],
    repetitions: int,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    workload_id = str(workload["workload_id"])
    artifact_mode = str(workload["artifact_mode"])
    execution = str(workload["execution"])
    expected_output_path = workload.get("expected_output")
    if execution == "success":
        expected_output = _repo_path(
            repo_root, expected_output_path, f"{workload_id}.expected_output"
        ).read_text(encoding="utf-8")
        expected_output_digest: str | None = hashlib.sha256(
            expected_output.encode("utf-8")
        ).hexdigest()
        expected_stderr = ""
        expected_stderr_path = None
        expected_exit_code = 0
        expected_stderr_digest = None
    else:
        expected_output = ""
        expected_output_digest = None
        expected_stderr_path = workload.get("expected_stderr")
        expected_stderr = _repo_path(
            repo_root, expected_stderr_path, f"{workload_id}.expected_stderr"
        ).read_text(encoding="utf-8")
        expected_stderr_digest = hashlib.sha256(
            expected_stderr.encode("utf-8")
        ).hexdigest()
        expected_exit_code = int(workload["expected_exit_code"])
    sources = _workload_sources(repo_root, workload)
    compile_samples: list[float] = []
    link_samples: list[float] = []
    load_samples: list[float] = []
    runtime_samples: list[float] = []
    errors: list[str] = []
    compile_exit_ok: list[bool] = []
    compile_stdout_ok: list[bool] = []
    compile_stderr_ok: list[bool] = []
    compile_artifact_ok: list[bool] = []
    link_exit_ok: list[bool] = []
    link_stdout_ok: list[bool] = []
    link_stderr_ok: list[bool] = []
    link_artifact_ok: list[bool] = []
    load_exit_ok: list[bool] = []
    load_stdout_ok: list[bool] = []
    load_stderr_ok: list[bool] = []
    runtime_exit_ok: list[bool] = []
    runtime_stdout_ok: list[bool] = []
    runtime_stderr_ok: list[bool] = []
    link_required = artifact_mode == "module"

    with tempfile.TemporaryDirectory(prefix=f"compiler-design-benchmark-{workload_id}-") as temp_dir:
        temp_root = Path(temp_dir)
        last_artifact: Path | None = None
        for repetition in range(repetitions):
            artifact = temp_root / f"compile-{repetition + 1}.cdbc"
            module_directory = temp_root / f"modules-{repetition + 1}"
            if artifact_mode == "linked":
                compile_command = [
                    str(compiler),
                    "--emit-bytecode",
                    str(artifact),
                    *(str(source) for source in sources),
                ]
            else:
                compile_command = [
                    str(compiler),
                    "--emit-module-bytecode",
                    str(module_directory),
                    *(str(source) for source in sources),
                ]
            duration, returncode, stdout, stderr = run_command(
                compile_command, repo_root, timeout_seconds
            )
            compile_samples.append(duration)
            exit_ok = returncode == 0
            stdout_ok = not stdout
            stderr_ok = not stderr
            compile_exit_ok.append(exit_ok)
            compile_stdout_ok.append(stdout_ok)
            compile_stderr_ok.append(stderr_ok)
            artifact_ok = (
                artifact.is_file()
                if artifact_mode == "linked"
                else module_directory.is_dir()
                and any(module_directory.glob("module-*.cdbc"))
            )
            compile_artifact_ok.append(artifact_ok)
            if not exit_ok:
                errors.append(
                    f"{workload_id} compile {repetition + 1}: "
                    + _failure(compile_command, returncode, stdout, stderr)
                )
            elif not stdout_ok:
                errors.append(
                    f"{workload_id} compile {repetition + 1} produced stdout: {stdout.rstrip()}"
                )
            elif not stderr_ok:
                errors.append(
                    f"{workload_id} compile {repetition + 1} produced stderr: {stderr.rstrip()}"
                )
            elif not artifact_ok:
                target = artifact if artifact_mode == "linked" else module_directory
                errors.append(f"{workload_id} compile {repetition + 1} did not emit {target}")
            else:
                linked_artifact = artifact
                if link_required:
                    linked_artifact = temp_root / f"linked-{repetition + 1}.cdbc"
                    link_command = [
                        str(vm_binary),
                        "link",
                        str(module_directory),
                        str(linked_artifact),
                    ]
                    (
                        link_duration,
                        link_returncode,
                        link_stdout,
                        link_stderr,
                    ) = run_command(link_command, repo_root, timeout_seconds)
                    link_samples.append(link_duration)
                    link_exit_is_ok = link_returncode == 0
                    link_stdout_is_ok = not link_stdout
                    link_stderr_is_ok = not link_stderr
                    link_artifact_is_ok = linked_artifact.is_file()
                    link_exit_ok.append(link_exit_is_ok)
                    link_stdout_ok.append(link_stdout_is_ok)
                    link_stderr_ok.append(link_stderr_is_ok)
                    link_artifact_ok.append(link_artifact_is_ok)
                    if not link_exit_is_ok:
                        errors.append(
                            f"{workload_id} link {repetition + 1}: "
                            + _failure(
                                link_command,
                                link_returncode,
                                link_stdout,
                                link_stderr,
                            )
                        )
                    elif not link_stdout_is_ok:
                        errors.append(
                            f"{workload_id} link {repetition + 1} produced stdout: "
                            f"{link_stdout.rstrip()}"
                        )
                    elif not link_stderr_is_ok:
                        errors.append(
                            f"{workload_id} link {repetition + 1} produced stderr: "
                            f"{link_stderr.rstrip()}"
                        )
                    elif not link_artifact_is_ok:
                        errors.append(
                            f"{workload_id} link {repetition + 1} did not emit "
                            f"{linked_artifact}"
                        )

                link_sample_ok = (
                    not link_required
                    or (
                        link_exit_ok[-1]
                        and link_stdout_ok[-1]
                        and link_stderr_ok[-1]
                        and link_artifact_ok[-1]
                    )
                )
                if link_sample_ok:
                    load_command = [str(vm_binary), "dump", str(linked_artifact)]
                    (
                        load_duration,
                        load_returncode,
                        load_stdout,
                        load_stderr,
                    ) = run_command(load_command, repo_root, timeout_seconds)
                    load_samples.append(load_duration)
                    load_exit_is_ok = load_returncode == 0
                    load_stdout_is_ok = bool(load_stdout)
                    load_stderr_is_ok = not load_stderr
                    load_exit_ok.append(load_exit_is_ok)
                    load_stdout_ok.append(load_stdout_is_ok)
                    load_stderr_ok.append(load_stderr_is_ok)
                    if not load_exit_is_ok:
                        errors.append(
                            f"{workload_id} load {repetition + 1}: "
                            + _failure(
                                load_command,
                                load_returncode,
                                load_stdout,
                                load_stderr,
                            )
                        )
                    elif not load_stdout_is_ok:
                        errors.append(
                            f"{workload_id} load {repetition + 1} produced no "
                            "canonical artifact output"
                        )
                    elif not load_stderr_is_ok:
                        errors.append(
                            f"{workload_id} load {repetition + 1} produced stderr: "
                            f"{load_stderr.rstrip()}"
                        )
                    else:
                        last_artifact = linked_artifact

        compile_passed = (
            len(compile_samples) == repetitions
            and _all_validated(compile_exit_ok)
            and _all_validated(compile_stdout_ok)
            and _all_validated(compile_stderr_ok)
            and _all_validated(compile_artifact_ok)
        )
        link_passed = not link_required or (
            len(link_samples) == repetitions
            and _all_validated(link_exit_ok)
            and _all_validated(link_stdout_ok)
            and _all_validated(link_stderr_ok)
            and _all_validated(link_artifact_ok)
        )
        load_passed = (
            len(load_samples) == repetitions
            and _all_validated(load_exit_ok)
            and _all_validated(load_stdout_ok)
            and _all_validated(load_stderr_ok)
        )

        if compile_passed and link_passed and load_passed and last_artifact is not None:
            runtime_command = [str(vm_binary), "run", str(last_artifact)]
            for repetition in range(repetitions):
                duration, returncode, stdout, stderr = run_command(
                    runtime_command, repo_root, timeout_seconds
                )
                runtime_samples.append(duration)
                exit_ok = returncode == expected_exit_code
                stdout_ok = stdout == expected_output
                stderr_ok = stderr == expected_stderr
                runtime_exit_ok.append(exit_ok)
                runtime_stdout_ok.append(stdout_ok)
                runtime_stderr_ok.append(stderr_ok)
                if not exit_ok:
                    errors.append(
                        f"{workload_id} runtime {repetition + 1} expected exit "
                        f"{expected_exit_code}, got {returncode}: "
                        + _failure(runtime_command, returncode, stdout, stderr)
                    )
                elif not stderr_ok:
                    errors.append(
                        f"{workload_id} runtime {repetition + 1} stderr mismatch:\n"
                        + _output_mismatch(expected_stderr, stderr)
                    )
                elif not stdout_ok:
                    errors.append(
                        f"{workload_id} runtime {repetition + 1} stdout mismatch:\n"
                        + _output_mismatch(expected, stdout)
                    )

    result: dict[str, Any] = {
        "workload_id": workload_id,
        "description": workload.get("description", ""),
        "sources": workload["sources"],
        "artifact_mode": artifact_mode,
        "execution": execution,
        "expected_output": expected_output_path,
        "expected_output_sha256": expected_output_digest,
        "expected_stderr": expected_stderr_path,
        "expected_stderr_sha256": expected_stderr_digest,
        "expected_exit_code": expected_exit_code,
        "repetitions": repetitions,
        "compile_seconds": timing_record(compile_samples),
        "link_seconds": timing_record(link_samples),
        "load_seconds": timing_record(load_samples),
        "runtime_seconds": timing_record(runtime_samples),
        "link_required": link_required,
        "compile_exit_code_validated": _all_validated(compile_exit_ok),
        "compile_stdout_validated": _all_validated(compile_stdout_ok),
        "compile_stderr_validated": _all_validated(compile_stderr_ok),
        "compile_artifact_validated": _all_validated(compile_artifact_ok),
        "link_exit_code_validated": (
            _all_validated(link_exit_ok) if link_required else None
        ),
        "link_stdout_validated": (
            _all_validated(link_stdout_ok) if link_required else None
        ),
        "link_stderr_validated": (
            _all_validated(link_stderr_ok) if link_required else None
        ),
        "link_artifact_validated": (
            _all_validated(link_artifact_ok) if link_required else None
        ),
        "load_exit_code_validated": _all_validated(load_exit_ok),
        "load_stdout_validated": _all_validated(load_stdout_ok),
        "load_stderr_validated": _all_validated(load_stderr_ok),
        "exit_code_validated": _all_validated(runtime_exit_ok),
        "stdout_validated": _all_validated(runtime_stdout_ok),
        "stderr_validated": _all_validated(runtime_stderr_ok),
        "passed": not errors
        and len(compile_samples) == repetitions
        and (not link_required or len(link_samples) == repetitions)
        and len(load_samples) == repetitions
        and len(runtime_samples) == repetitions,
    }
    if errors:
        result["errors"] = errors
    return result


def resolve_vm_binary(vm_path: Path) -> Path:
    path = vm_path.resolve()
    if path.name == "Cargo.toml":
        if not path.is_file():
            raise ValueError(f"Rust VM manifest not found: {path}")
        path = path.parent
    if path.is_file():
        return path
    if path.is_dir():
        candidates = (
            path / "target" / "debug" / "compiler-design-vm",
            path / "target" / "release" / "compiler-design-vm",
        )
        for candidate in candidates:
            if candidate.is_file():
                return candidate
    raise ValueError(
        "Rust VM executable not found; build it with "
        "cargo build --manifest-path vm-rs/Cargo.toml, or pass its path explicitly"
    )


def git_commit(repo_root: Path) -> str:
    _, returncode, stdout, _ = run_command(["git", "rev-parse", "HEAD"], repo_root)
    return stdout.strip() if returncode == 0 and stdout.strip() else "unknown"


def tool_version(repo_root: Path, command: list[str]) -> str:
    _, returncode, stdout, stderr = run_command(command, repo_root)
    if returncode != 0:
        return "unavailable"
    text = (stdout or stderr).strip()
    return text.splitlines()[0] if text else "unknown"


def environment_record(repo_root: Path, compiler: Path, vm_binary: Path) -> dict[str, Any]:
    host = platform.uname()
    return {
        "compiler": str(compiler),
        "vm_binary": str(vm_binary),
        "python": sys.version.split()[0],
        "toolchain": {
            "cmake": tool_version(repo_root, ["cmake", "--version"]),
            "rustc": tool_version(repo_root, ["rustc", "--version"]),
            "cargo": tool_version(repo_root, ["cargo", "--version"]),
        },
        "host": {
            "system": host.system,
            "release": host.release,
            "machine": host.machine,
            "processor": host.processor,
            "python_implementation": platform.python_implementation(),
            "cpu_count": os.cpu_count(),
        },
        "cwd": str(repo_root),
    }


def run_benchmarks(
    manifest: dict[str, Any],
    repo_root: Path,
    compiler: Path,
    vm_binary: Path,
    repetitions: int,
    selected_workloads: list[str] | None = None,
    manifest_path: Path | None = None,
    timeout_seconds: float = DEFAULT_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    workload_map = {str(item["workload_id"]): item for item in manifest["workloads"]}
    selected = [str(item["workload_id"]) for item in manifest["workloads"]]
    if selected_workloads:
        unknown = sorted(set(selected_workloads) - set(workload_map))
        if unknown:
            raise ValueError("unknown workload(s): " + ", ".join(unknown))
        selected = [workload_id for workload_id in selected if workload_id in selected_workloads]

    workload_results = [
        run_workload(
            repo_root,
            compiler,
            vm_binary,
            workload_map[workload_id],
            repetitions,
            timeout_seconds,
        )
        for workload_id in selected
    ]
    errors = [
        error
        for result in workload_results
        for error in result.get("errors", [])
    ]
    manifest_display = (
        manifest_path.resolve().relative_to(repo_root.resolve()).as_posix()
        if manifest_path is not None and manifest_path.resolve().is_relative_to(repo_root.resolve())
        else str(manifest_path) if manifest_path is not None else "<in-memory>"
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "benchmark_revision": manifest["benchmark_revision"],
        "commit": git_commit(repo_root),
        "manifest": manifest_display,
        "repetitions": repetitions,
        "timeout_seconds": rounded(timeout_seconds),
        "measurement": {
            "compile": "compiler_design bytecode emission wall-clock time, including process startup and artifact write",
            "link": "Rust VM link wall-clock time for module workloads, including module product load and linked artifact write",
            "load": "Rust VM dump wall-clock time, including artifact read, parse, verification, and canonical formatting",
            "runtime": "already-built Rust VM run wall-clock time, including process startup and artifact load during run",
            "statistic": "min, median, and max over the requested repetitions",
            "timeout_seconds": rounded(timeout_seconds),
            "enforcement": "informational; correctness failures return non-zero",
        },
        "commands": {
            "compile": [str(compiler), "--emit-bytecode", "<artifact>", "<sources...>"],
            "compile_linked": [
                str(compiler),
                "--emit-bytecode",
                "<artifact>",
                "<sources...>",
            ],
            "compile_module": [
                str(compiler),
                "--emit-module-bytecode",
                "<module-directory>",
                "<sources...>",
            ],
            "link": [str(vm_binary), "link", "<module-directory>", "<artifact>"],
            "load": [str(vm_binary), "dump", "<artifact>"],
            "runtime": [str(vm_binary), "run", "<artifact>"],
        },
        "environment": environment_record(repo_root, compiler, vm_binary),
        "workloads": workload_results,
        "summary": {
            "total_workloads": len(workload_results),
            "passed_workloads": sum(result["passed"] for result in workload_results),
            "failed_workloads": sum(not result["passed"] for result in workload_results),
        },
        "errors": errors,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure Compiler Design compile and Rust VM runtime wall-clock time."
    )
    parser.add_argument(
        "compiler",
        nargs="?",
        type=Path,
        default=DEFAULT_COMPILER,
        help="compiler_design executable (default: build/compiler_design)",
    )
    parser.add_argument(
        "vm",
        nargs="?",
        type=Path,
        default=DEFAULT_VM,
        help="Rust VM executable or vm-rs directory (default: vm-rs)",
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument(
        "--repeat",
        type=int,
        help="number of compile and runtime samples per workload (default: manifest value)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="per-process timeout in seconds (default: 60)",
    )
    parser.add_argument(
        "--workload",
        "--case",
        action="append",
        dest="workloads",
        help="run only this workload; repeat the option to select multiple workloads",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = REPO_ROOT.resolve()
    manifest_path = args.manifest.resolve()
    try:
        manifest = load_manifest(manifest_path)
        errors = validate_manifest(manifest, repo_root)
        if errors:
            for error in errors:
                print(f"FAIL {error}", file=sys.stderr)
            return 64
        repetitions = args.repeat if args.repeat is not None else int(manifest["default_repeat"])
        if repetitions <= 0:
            print("FAIL --repeat must be a positive integer", file=sys.stderr)
            return 64
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            print("FAIL --timeout must be a positive finite number", file=sys.stderr)
            return 64
        compiler = args.compiler.resolve()
        if not compiler.is_file():
            print(f"compiler not found: {compiler}", file=sys.stderr)
            return 64
        vm_binary = resolve_vm_binary(args.vm)
        report = run_benchmarks(
            manifest,
            repo_root,
            compiler,
            vm_binary,
            repetitions,
            selected_workloads=args.workloads,
            manifest_path=manifest_path,
            timeout_seconds=args.timeout,
        )
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"benchmark runner: {error}", file=sys.stderr)
        return 64

    try:
        report_path = args.report.resolve()
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except OSError as error:
        print(f"benchmark runner: unable to write report {args.report}: {error}", file=sys.stderr)
        return 1

    summary = report["summary"]
    print(
        "benchmarks: "
        f"{summary['passed_workloads']} passed, "
        f"{summary['failed_workloads']} failed, "
        f"repeat={report['repetitions']}"
    )
    for result in report["workloads"]:
        compile_median = result["compile_seconds"]["median"]
        link_median = result["link_seconds"]["median"]
        load_median = result["load_seconds"]["median"]
        runtime_median = result["runtime_seconds"]["median"]
        print(
            f"  {result['workload_id']}: "
            f"compile median={compile_median if compile_median is not None else 'n/a'}s, "
            f"link median={link_median if link_median is not None else 'n/a'}s, "
            f"load median={load_median if load_median is not None else 'n/a'}s, "
            f"runtime median={runtime_median if runtime_median is not None else 'n/a'}s, "
            f"{'PASS' if result['passed'] else 'FAIL'}"
        )
    for error in report["errors"]:
        print(f"FAIL {error}", file=sys.stderr)
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
