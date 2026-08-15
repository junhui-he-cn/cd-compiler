#!/usr/bin/env python3

"""Exercise VM capacity boundaries with reproducible temporary artifacts.

The pass/fail assertions cover capacity behavior and stable diagnostics. Timing
and peak RSS are emitted as observations only; they are not portability or CI
performance thresholds.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Measurement:
    label: str
    returncode: int
    stdout: str
    stderr: str
    elapsed_seconds: float
    peak_rss_kib: int | None


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def peak_rss_kib(usage: object) -> int | None:
    value = getattr(usage, "ru_maxrss", None)
    if value is None:
        return None
    if platform.system() == "Darwin":
        return int(value) // 1024
    return int(value)


def run_measured(command: list[str], label: str) -> Measurement:
    started = time.perf_counter()
    with tempfile.TemporaryFile() as stdout_file, tempfile.TemporaryFile() as stderr_file:
        process = subprocess.Popen(command, stdout=stdout_file, stderr=stderr_file)
        if hasattr(os, "wait4"):
            _, status, usage = os.wait4(process.pid, 0)
            returncode = os.waitstatus_to_exitcode(status)
            process.returncode = returncode
            rss = peak_rss_kib(usage)
        else:
            returncode = process.wait()
            rss = None
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read().decode("utf-8", errors="replace")
        stderr = stderr_file.read().decode("utf-8", errors="replace")
    return Measurement(
        label=label,
        returncode=returncode,
        stdout=stdout,
        stderr=stderr,
        elapsed_seconds=time.perf_counter() - started,
        peak_rss_kib=rss,
    )


def describe_failure(result: Measurement) -> str:
    return (
        f"{result.label} exited with {result.returncode}\n"
        f"stdout={result.stdout!r}\n"
        f"stderr={result.stderr!r}"
    )


def expect_success(result: Measurement, stdout: str | None = "") -> str | None:
    if result.returncode != 0 or (stdout is not None and result.stdout != stdout) or result.stderr:
        return describe_failure(result) + f"\nexpected stdout={stdout!r}"
    return None


def expect_failure(result: Measurement, fragment: str) -> str | None:
    if result.returncode == 0:
        return f"{result.label} unexpectedly succeeded"
    if result.stdout:
        return f"{result.label} produced stdout on failure: {result.stdout!r}"
    if fragment not in result.stderr:
        return f"{describe_failure(result)}\nmissing diagnostic fragment={fragment!r}"
    return None


def emit_bytecode(
    compiler: Path,
    source: Path,
    artifact: Path,
    label: str,
    measurements: list[Measurement],
) -> str | None:
    result = run_measured(
        [str(compiler), "--emit-bytecode", str(artifact), str(source)],
        label,
    )
    measurements.append(result)
    error = expect_success(result)
    if error is not None:
        return error
    if not artifact.is_file():
        return f"{label} did not create {artifact}"
    return None


def emit_modules(
    compiler: Path,
    source: Path,
    output_directory: Path,
    label: str,
    measurements: list[Measurement],
) -> str | None:
    result = run_measured(
        [str(compiler), "--emit-module-bytecode", str(output_directory), str(source)],
        label,
    )
    measurements.append(result)
    error = expect_success(result)
    if error is not None:
        return error
    if not sorted(output_directory.glob("module-*.cdbc")):
        return f"{label} did not create module products"
    return None


def vm_command(binary: Path, *arguments: str) -> list[str]:
    return [str(binary), *arguments]


def profile_heap(
    vm_binary: Path,
    artifact: Path,
    label: str,
    measurements: list[Measurement],
    observations: list[tuple[str, int, int]],
) -> str | None:
    first = run_measured(
        vm_command(vm_binary, "profile", str(artifact)),
        f"{label} profile",
    )
    measurements.append(first)
    second = run_measured(
        vm_command(vm_binary, "profile", str(artifact)),
        f"{label} profile repeat",
    )
    measurements.append(second)
    if (first.returncode, first.stdout, first.stderr) != (
        second.returncode,
        second.stdout,
        second.stderr,
    ):
        return f"{label} profile output was not deterministic"
    if first.returncode != 0 or first.stderr:
        return describe_failure(first)
    if "profile status=ok" not in first.stdout:
        return f"{label} profile did not report success: {first.stdout!r}"

    heap_line = next(
        (line for line in first.stdout.splitlines() if line.startswith("profile heap ")),
        None,
    )
    if heap_line is None:
        return f"{label} profile omitted the heap report: {first.stdout!r}"
    fields: dict[str, int] = {}
    for token in heap_line.split()[2:]:
        name, separator, raw_value = token.partition("=")
        if not separator:
            return f"{label} profile has malformed heap field: {token!r}"
        try:
            fields[name] = int(raw_value)
        except ValueError:
            return f"{label} profile has non-numeric heap field: {token!r}"

    live_bytes = fields.get("tracked_heap_estimated_live_bytes")
    peak_bytes = fields.get("tracked_heap_estimated_peak_live_bytes")
    if live_bytes is None or peak_bytes is None:
        return f"{label} profile omitted estimated heap fields: {heap_line!r}"
    if live_bytes <= 0 or peak_bytes < live_bytes:
        return (
            f"{label} profile has invalid estimated heap values: "
            f"live={live_bytes} peak={peak_bytes}"
        )
    observations.append((label, live_bytes, peak_bytes))
    return None


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: vm_capacity_tests.py <compiler> <vm-rs>")

    compiler = Path(sys.argv[1]).resolve()
    vm_path = Path(sys.argv[2]).resolve()
    manifest = vm_path / "Cargo.toml"
    if not compiler.is_file() or not manifest.is_file():
        return fail("compiler or Rust VM manifest not found")

    build = run_measured(
        ["cargo", "build", "--quiet", "--manifest-path", str(manifest)],
        "rust-vm build",
    )
    if build.returncode != 0 or build.stdout or build.stderr:
        return fail(describe_failure(build))

    vm_binary = vm_path / "target" / "debug" / "compiler-design-vm"
    if not vm_binary.is_file():
        return fail(f"Rust VM binary was not built: {vm_binary}")

    measurements: list[Measurement] = []
    profile_observations: list[tuple[str, int, int]] = []
    with tempfile.TemporaryDirectory(prefix="compiler-design-vm-capacity-") as temporary:
        root = Path(temporary)

        large_array_source = root / "large-array.cd"
        large_array_artifact = root / "large-array.cdbc"
        array_length = 4096
        values = ", ".join(str(index % 97) for index in range(array_length))
        large_array_source.write_text(
            f"let values = [{values}];\nprint(len(values));\n",
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            large_array_source,
            large_array_artifact,
            "large-array emit",
            measurements,
        )
        if error is not None:
            return fail(error)
        if large_array_artifact.stat().st_size <= 32 * 1024:
            return fail("large-array artifact did not cross the intended size boundary")

        result = run_measured(
            vm_command(vm_binary, "run", str(large_array_artifact)),
            "large-array run",
        )
        measurements.append(result)
        error = expect_success(result, f"{array_length}\n")
        if error is not None:
            return fail(error)
        error = profile_heap(
            vm_binary,
            large_array_artifact,
            "large-array",
            measurements,
            profile_observations,
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(large_array_artifact),
                "--max-elements",
                "1024",
            ),
            "large-array element budget",
        )
        measurements.append(result)
        error = expect_failure(result, "runtime elements (limit 1024)")
        if error is not None:
            return fail(error)

        large_map_source = root / "large-map.cd"
        large_map_artifact = root / "large-map.cdbc"
        map_entry_count = 2048
        map_entries = ", ".join(
            f'"key{index}": {index}' for index in range(map_entry_count)
        )
        large_map_source.write_text(
            f'let values = {{{map_entries}}};\n'
            "print(len(values));\n"
            f'print(values["key{map_entry_count - 1}"]);\n',
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            large_map_source,
            large_map_artifact,
            "large-map emit",
            measurements,
        )
        if error is not None:
            return fail(error)
        if large_map_artifact.stat().st_size <= 64 * 1024:
            return fail("large-map artifact did not cross the intended size boundary")

        result = run_measured(
            vm_command(vm_binary, "run", str(large_map_artifact)),
            "large-map run",
        )
        measurements.append(result)
        error = expect_success(
            result,
            f"{map_entry_count}\n{map_entry_count - 1}\n",
        )
        if error is not None:
            return fail(error)
        error = profile_heap(
            vm_binary,
            large_map_artifact,
            "large-map",
            measurements,
            profile_observations,
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(large_map_artifact),
                "--max-elements",
                "1024",
            ),
            "large-map element budget",
        )
        measurements.append(result)
        error = expect_failure(result, "runtime elements (limit 1024)")
        if error is not None:
            return fail(error)

        churn_source = root / "aggregate-allocation-churn.cd"
        churn_artifact = root / "aggregate-allocation-churn.cdbc"
        churn_iterations = 512
        churn_struct_cost = 1 + 1
        churn_map_cost = 1 + 1
        churn_array_cost = 1 + 3
        churn_initial_elements = churn_struct_cost + churn_map_cost + churn_array_cost
        churn_iteration_elements = churn_initial_elements
        churn_total_elements = churn_initial_elements + (
            churn_iterations * churn_iteration_elements
        )
        churn_source.write_text(
            "struct Bucket { value: number }\n"
            "let mut bucket = Bucket { value: 0 };\n"
            'let mut latest = {"value": 0};\n'
            "let mut values = [0, 0, 0];\n"
            "let mut iteration = 0;\n"
            f"while (iteration < {churn_iterations}) {{\n"
            "  bucket = Bucket { value: iteration };\n"
            '  latest = {"value": iteration};\n'
            "  values = [iteration, iteration + 1, iteration + 2];\n"
            "  iteration += 1;\n"
            "}\n"
            "print(bucket.value);\n"
            'print(latest["value"]);\n'
            "print(len(values));\n",
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            churn_source,
            churn_artifact,
            "aggregate-allocation-churn emit",
            measurements,
        )
        if error is not None:
            return fail(error)

        churn_output = f"{churn_iterations - 1}\n{churn_iterations - 1}\n3\n"
        result = run_measured(
            vm_command(vm_binary, "run", str(churn_artifact)),
            "aggregate-allocation-churn run",
        )
        measurements.append(result)
        error = expect_success(result, churn_output)
        if error is not None:
            return fail(error)
        error = profile_heap(
            vm_binary,
            churn_artifact,
            "aggregate-allocation-churn",
            measurements,
            profile_observations,
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(churn_artifact),
                "--max-elements",
                str(churn_total_elements),
            ),
            "aggregate-allocation-churn exact element budget",
        )
        measurements.append(result)
        error = expect_success(result, churn_output)
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(churn_artifact),
                "--max-elements",
                str(churn_total_elements - 1),
            ),
            "aggregate-allocation-churn element budget",
        )
        measurements.append(result)
        error = expect_failure(
            result,
            f"runtime elements (limit {churn_total_elements - 1})",
        )
        if error is not None:
            return fail(error)

        large_struct_source = root / "large-struct-array.cd"
        large_struct_artifact = root / "large-struct-array.cdbc"
        struct_entry_count = 1024
        struct_entries = ", ".join(
            "Entry { "
            f"index: {index}, "
            "payload: Payload { "
            f'label: "row{index}", value: {index}'
            " } }"
            for index in range(struct_entry_count)
        )
        large_struct_source.write_text(
            "struct Payload { label: string, value: number }\n"
            "struct Entry { index: number, payload: Payload }\n"
            f"let values = [{struct_entries}];\n"
            "print(len(values));\n"
            f"print(values[{struct_entry_count - 1}].payload.value);\n",
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            large_struct_source,
            large_struct_artifact,
            "large-struct-array emit",
            measurements,
        )
        if error is not None:
            return fail(error)
        if large_struct_artifact.stat().st_size <= 96 * 1024:
            return fail(
                "large-struct-array artifact did not cross the intended size boundary"
            )

        result = run_measured(
            vm_command(vm_binary, "run", str(large_struct_artifact)),
            "large-struct-array run",
        )
        measurements.append(result)
        error = expect_success(
            result,
            f"{struct_entry_count}\n{struct_entry_count - 1}\n",
        )
        if error is not None:
            return fail(error)
        error = profile_heap(
            vm_binary,
            large_struct_artifact,
            "large-struct-array",
            measurements,
            profile_observations,
        )
        if error is not None:
            return fail(error)

        struct_payload_cost = 1 + 2
        struct_entry_cost = 1 + 2
        struct_array_cost = 1 + struct_entry_count
        struct_element_limit = (
            struct_entry_count * (struct_payload_cost + struct_entry_cost)
            + struct_array_cost
            - 1
        )
        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(large_struct_artifact),
                "--max-elements",
                str(struct_element_limit + 1),
            ),
            "large-struct-array exact element budget",
        )
        measurements.append(result)
        error = expect_success(
            result,
            f"{struct_entry_count}\n{struct_entry_count - 1}\n",
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(large_struct_artifact),
                "--max-elements",
                str(struct_element_limit),
            ),
            "large-struct-array element budget",
        )
        measurements.append(result)
        error = expect_failure(
            result,
            f"runtime elements (limit {struct_element_limit})",
        )
        if error is not None:
            return fail(error)

        long_string_source = root / "long-unicode-string.cd"
        long_string_artifact = root / "long-unicode-string.cdbc"
        scalar_count = 32768
        long_string = "é" * scalar_count
        long_string_source.write_text(
            f'let text = "{long_string}";\nprint(len(text));\n',
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            long_string_source,
            long_string_artifact,
            "long-unicode-string emit",
            measurements,
        )
        if error is not None:
            return fail(error)
        if long_string_artifact.stat().st_size <= scalar_count * 2:
            return fail(
                "long-unicode-string artifact did not retain the expected UTF-8 payload"
            )

        result = run_measured(
            vm_command(vm_binary, "run", str(long_string_artifact)),
            "long-unicode-string run",
        )
        measurements.append(result)
        error = expect_success(result, f"{scalar_count}\n")
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(vm_binary, "dump", str(long_string_artifact)),
            "long-unicode-string dump",
        )
        measurements.append(result)
        error = expect_success(result, None)
        if error is not None:
            return fail(error)
        if (
            "constants:\n" not in result.stdout
            or "debug_sources:\n" not in result.stdout
            or f'string "{long_string}"' not in result.stdout
        ):
            return fail(
                "long-unicode-string dump did not retain the Unicode constant and debug source"
            )

        long_string_output_source = root / "long-unicode-output.cd"
        long_string_output_artifact = root / "long-unicode-output.cdbc"
        expected_long_string_output = f"{long_string}\n"
        output_bytes = len(expected_long_string_output.encode("utf-8"))
        long_string_output_source.write_text(
            f'let text = "{long_string}";\nprint(text);\n',
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            long_string_output_source,
            long_string_output_artifact,
            "long-unicode-output emit",
            measurements,
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(vm_binary, "run", str(long_string_output_artifact)),
            "long-unicode-output run",
        )
        measurements.append(result)
        error = expect_success(result, expected_long_string_output)
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(long_string_output_artifact),
                "--max-output-bytes",
                str(output_bytes),
            ),
            "long-unicode-output exact budget",
        )
        measurements.append(result)
        error = expect_success(result, expected_long_string_output)
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(long_string_output_artifact),
                "--max-output-bytes",
                str(output_bytes - 1),
            ),
            "long-unicode-output budget rejection",
        )
        measurements.append(result)
        error = expect_failure(
            result,
            f"output bytes (limit {output_bytes - 1})",
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "dump",
                str(long_string_artifact),
                "--max-artifact-bytes",
                "1024",
            ),
            "long-unicode-string artifact budget",
        )
        measurements.append(result)
        error = expect_failure(result, "artifact bytes (limit 1024)")
        if error is not None:
            return fail(error)

        artifact_size = long_string_artifact.stat().st_size
        result = run_measured(
            vm_command(
                vm_binary,
                "verify",
                str(long_string_artifact),
                "--max-artifact-bytes",
                str(artifact_size),
            ),
            "long-unicode-string verify exact artifact budget",
        )
        measurements.append(result)
        error = expect_success(result)
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "verify",
                str(long_string_artifact),
                "--max-artifact-bytes",
                str(artifact_size - 1),
            ),
            "long-unicode-string verify artifact budget rejection",
        )
        measurements.append(result)
        error = expect_failure(
            result,
            f"artifact bytes (limit {artifact_size - 1})",
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "dump",
                str(long_string_artifact),
                "--max-artifact-bytes",
                str(artifact_size),
            ),
            "long-unicode-string dump exact artifact budget",
        )
        measurements.append(result)
        error = expect_success(result, None)
        if error is not None:
            return fail(error)

        recursive_source = root / "deep-recursion.cd"
        recursive_artifact = root / "deep-recursion.cdbc"
        recursive_source.write_text(
            "fun descend(value: number): number {\n"
            "  if (value <= 0) { return 0; }\n"
            "  return descend(value - 1);\n"
            "}\n"
            "print(descend(64));\n",
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            recursive_source,
            recursive_artifact,
            "deep-recursion emit",
            measurements,
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(recursive_artifact),
                "--max-call-depth",
                "128",
            ),
            "deep-recursion run",
        )
        measurements.append(result)
        error = expect_success(result, "0\n")
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "run",
                str(recursive_artifact),
                "--max-call-depth",
                "8",
            ),
            "deep-recursion call-depth budget",
        )
        measurements.append(result)
        error = expect_failure(result, "call depth (limit 8)")
        if error is not None:
            return fail(error)

        debug_source = root / "large-debug-table.cd"
        debug_artifact = root / "large-debug-table.cdbc"
        debug_statement_count = 1400
        debug_source.write_text(
            "".join(
                f"let value{index}: number = {index};\n"
                for index in range(debug_statement_count)
            )
            + f"print(value{debug_statement_count - 1});\n",
            encoding="utf-8",
        )
        error = emit_bytecode(
            compiler,
            debug_source,
            debug_artifact,
            "large-debug-table emit",
            measurements,
        )
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(vm_binary, "dump", str(debug_artifact)),
            "large-debug-table dump",
        )
        measurements.append(result)
        error = expect_success(result, None)
        if error is not None:
            return fail(error)
        location_lines = sum(
            line.startswith("  main ") for line in result.stdout.splitlines()
        )
        if "debug_locations:" not in result.stdout or location_lines < debug_statement_count:
            return fail(
                "large-debug-table dump did not retain the expected debug locations "
                f"(found {location_lines}, expected at least {debug_statement_count})"
            )

        result = run_measured(
            vm_command(
                vm_binary,
                "dump",
                str(debug_artifact),
                "--max-artifact-bytes",
                "512",
            ),
            "large-debug-table artifact budget",
        )
        measurements.append(result)
        error = expect_failure(result, "artifact bytes (limit 512)")
        if error is not None:
            return fail(error)

        chain_directory = root / "chain-modules"
        chain_output = root / "chain-products"
        chain_count = 12
        chain_directory.mkdir()
        for index in range(chain_count):
            dependency = (
                f'import "./chain{index - 1}.cd";\n' if index > 0 else ""
            )
            (chain_directory / f"chain{index}.cd").write_text(
                dependency + f'print("chain{index}");\n',
                encoding="utf-8",
            )
        chain_entry = chain_directory / f"chain{chain_count - 1}.cd"
        error = emit_modules(
            compiler,
            chain_entry,
            chain_output,
            "long-chain module emit",
            measurements,
        )
        if error is not None:
            return fail(error)
        chain_products = sorted(chain_output.glob("module-*.cdbc"))
        if len(chain_products) != chain_count:
            return fail(
                f"long-chain module graph emitted {len(chain_products)} products, "
                f"expected {chain_count}"
            )

        chain_linked = root / "chain-linked.cdbc"
        result = run_measured(
            vm_command(vm_binary, "link", str(chain_output), str(chain_linked)),
            "long-chain module link",
        )
        measurements.append(result)
        error = expect_success(result)
        if error is not None:
            return fail(error)

        chain_output_text = "".join(f"chain{index}\n" for index in range(chain_count))
        result = run_measured(
            vm_command(vm_binary, "run", str(chain_linked)),
            "long-chain linked run",
        )
        measurements.append(result)
        error = expect_success(result, chain_output_text)
        if error is not None:
            return fail(error)

        result = run_measured(
            vm_command(
                vm_binary,
                "link",
                str(chain_output),
                str(root / "chain-limited.cdbc"),
                "--max-modules",
                "8",
            ),
            "long-chain module-count budget",
        )
        measurements.append(result)
        error = expect_failure(result, "module count (limit 8)")
        if error is not None:
            return fail(error)

        diamond_directory = root / "diamond-modules"
        diamond_output = root / "diamond-products"
        diamond_directory.mkdir()
        (diamond_directory / "diamond-base.cd").write_text(
            'print("base");\n',
            encoding="utf-8",
        )
        (diamond_directory / "diamond-left.cd").write_text(
            'import "./diamond-base.cd";\nprint("left");\n',
            encoding="utf-8",
        )
        (diamond_directory / "diamond-right.cd").write_text(
            'import "./diamond-base.cd";\nprint("right");\n',
            encoding="utf-8",
        )
        diamond_entry = diamond_directory / "diamond-entry.cd"
        diamond_entry.write_text(
            'import "./diamond-left.cd";\n'
            'import "./diamond-right.cd";\n'
            'print("entry");\n',
            encoding="utf-8",
        )
        error = emit_modules(
            compiler,
            diamond_entry,
            diamond_output,
            "diamond module emit",
            measurements,
        )
        if error is not None:
            return fail(error)
        diamond_products = sorted(diamond_output.glob("module-*.cdbc"))
        if len(diamond_products) != 4:
            return fail(
                f"diamond module graph emitted {len(diamond_products)} products, expected 4"
            )
        diamond_linked = root / "diamond-linked.cdbc"
        result = run_measured(
            vm_command(vm_binary, "link", str(diamond_output), str(diamond_linked)),
            "diamond module link",
        )
        measurements.append(result)
        error = expect_success(result)
        if error is not None:
            return fail(error)
        result = run_measured(
            vm_command(vm_binary, "run", str(diamond_linked)),
            "diamond linked run",
        )
        measurements.append(result)
        error = expect_success(result, "base\nleft\nright\nentry\n")
        if error is not None:
            return fail(error)

    print("VM capacity observations:")
    for result in measurements:
        rss = "n/a" if result.peak_rss_kib is None else str(result.peak_rss_kib)
        print(
            f"  {result.label}: elapsed_ms={result.elapsed_seconds * 1000:.3f} "
            f"peak_rss_kib={rss} exit={result.returncode}"
        )
    print("VM profile retained-byte observations:")
    for label, live_bytes, peak_bytes in profile_observations:
        print(
            f"  {label}: estimated_live_bytes={live_bytes} "
            f"estimated_peak_live_bytes={peak_bytes}"
        )
    print(
        "VM capacity tests: aggregate churn, large arrays/maps/structs, long Unicode "
        "strings and output budgets, deep calls, debug tables, long-chain and diamond "
        "module graphs, budget rejection boundaries, and retained-byte profiles validated"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
