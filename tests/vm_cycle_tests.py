#!/usr/bin/env python3

"""Exercise source-backed VM cycle, replacement, and observation behavior."""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str], input_text: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        input=input_text,
        text=True,
        capture_output=True,
        check=False,
    )


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def compile_source(compiler: Path, source: Path, artifact: Path, label: str) -> str | None:
    result = run([str(compiler), "--emit-bytecode", str(artifact), str(source)])
    if result.returncode != 0 or result.stdout or result.stderr:
        return (
            f"{label} emission failed\n"
            f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
        )
    return None


def profile_fields(output: str, label: str) -> tuple[dict[str, int], str | None]:
    line = next(
        (line for line in output.splitlines() if line.startswith("profile heap ")),
        None,
    )
    if line is None:
        return {}, f"{label} profile omitted the heap line:\n{output}"
    fields: dict[str, int] = {}
    for token in line.removeprefix("profile heap ").split():
        name, separator, value = token.partition("=")
        if not separator:
            return {}, f"{label} profile has malformed heap token: {token!r}"
        try:
            fields[name] = int(value)
        except ValueError:
            return {}, f"{label} profile has non-numeric heap token: {token!r}"
    required = {
        "tracked_heap_allocations",
        "tracked_heap_peak_live",
        "tracked_heap_estimated_live_bytes",
        "tracked_heap_estimated_peak_live_bytes",
    }
    missing = sorted(required - fields.keys())
    if missing:
        return {}, f"{label} profile omitted heap fields: {missing}"
    if fields["tracked_heap_allocations"] < fields["tracked_heap_peak_live"]:
        return {}, f"{label} profile allocation peak exceeds total allocations: {fields}"
    if fields["tracked_heap_estimated_live_bytes"] > fields["tracked_heap_estimated_peak_live_bytes"]:
        return {}, f"{label} profile live bytes exceed peak bytes: {fields}"
    return fields, None


def check_profile(
    vm_binary: Path,
    artifact: Path,
    label: str,
    expected_status: str,
    expected_error: str | None = None,
) -> tuple[dict[str, int], str | None]:
    command = [str(vm_binary), "profile", str(artifact)]
    first = run(command)
    second = run(command)
    if (first.returncode, first.stdout, first.stderr) != (
        second.returncode,
        second.stdout,
        second.stderr,
    ):
        return {}, f"{label} profile output was not deterministic"
    status = f"profile status={expected_status}"
    if status not in first.stdout:
        return {}, f"{label} profile omitted {status!r}:\n{first.stdout}"
    if expected_status == "ok":
        if first.returncode != 0 or first.stderr:
            return {}, f"{label} profile unexpectedly failed:\n{first.stderr}"
    else:
        if first.returncode == 0:
            return {}, f"{label} profile unexpectedly succeeded"
        if not expected_error or expected_error not in first.stderr:
            return {}, f"{label} profile lost runtime diagnostic:\n{first.stderr}"
    return profile_fields(first.stdout, label)


def check_run(vm_binary: Path, artifact: Path, label: str, expected: str) -> str | None:
    result = run([str(vm_binary), "run", str(artifact)])
    if result.returncode != 0 or result.stdout != expected or result.stderr:
        return (
            f"{label} run mismatch\n"
            f"exit={result.returncode}\nstdout={result.stdout!r}\nstderr={result.stderr!r}\n"
            f"expected={expected!r}"
        )
    return None


def check_trace(vm_binary: Path, artifact: Path, label: str) -> str | None:
    command = [str(vm_binary), "trace", str(artifact)]
    first = run(command)
    second = run(command)
    if (first.returncode, first.stdout, first.stderr) != (
        second.returncode,
        second.stdout,
        second.stderr,
    ):
        return f"{label} trace output was not deterministic"
    if first.returncode != 0 or first.stderr or 'value="[<cycle>]"' not in first.stdout:
        return f"{label} trace did not preserve the cycle marker:\n{first.stdout}\n{first.stderr}"
    return None


def check_debug(vm_binary: Path, source: Path, artifact: Path, label: str) -> str | None:
    command = [str(vm_binary), "debug", str(artifact)]
    input_text = f"break {source.resolve()}:3\ncontinue\ncontinue\n"
    first = run(command, input_text)
    second = run(command, input_text)
    if (first.returncode, first.stdout, first.stderr) != (
        second.returncode,
        second.stdout,
        second.stderr,
    ):
        return f"{label} debugger output was not deterministic"
    required = ("pause reason=breakpoint", 'locals={xs="[<cycle>]"}', "[<cycle>]\n")
    missing = [fragment for fragment in required if fragment not in first.stdout]
    if first.returncode != 0 or first.stderr or missing:
        return f"{label} debugger cycle observation failed: {missing}\n{first.stdout}\n{first.stderr}"
    return None


CASES: tuple[tuple[str, str, str, str | None], ...] = (
    (
        "self_array",
        "let xs = [];\npush(xs, xs);\nprint xs;\n",
        "[<cycle>]\n",
        None,
    ),
    (
        "self_map",
        'let values = {};\nvalues["self"] = values;\nprint values;\n',
        "map{self: <cycle>}\n",
        None,
    ),
    (
        "self_struct",
        "struct Node { next: optional<Node> }\n"
        "let node = Node { next: nil };\n"
        "node.next = node;\n"
        "print node;\n",
        "{next: <cycle>}\n",
        None,
    ),
    (
        "mutual_struct",
        "struct Node { value: number, next: optional<Node> }\n"
        "let first = Node { value: 1, next: nil };\n"
        "let second = Node { value: 2, next: first };\n"
        "first.next = second;\n"
        "print first;\n",
        "{value: 1, next: {value: 2, next: <cycle>}}\n",
        None,
    ),
    (
        "closure_environment",
        "fun makeCountdown() {\n"
        "  fun count(n) {\n"
        "    if n <= 0 { return 0; }\n"
        "    return count(n - 1) + 1;\n"
        "  }\n"
        "  return count;\n"
        "}\n"
        "let c = makeCountdown();\n"
        "print c(4);\n",
        "4\n",
        None,
    ),
    (
        "local_cycle",
        "fun make() {\n"
        "  let xs = [];\n"
        "  push(xs, xs);\n"
        "  print xs;\n"
        "}\n"
        "make();\n",
        "[<cycle>]\n",
        None,
    ),
    (
        "callback_cycle",
        "fun make(value) {\n"
        "  let xs = [];\n"
        "  push(xs, xs);\n"
        "  return xs;\n"
        "}\n"
        "let values = [1];\n"
        "let mapped = map(values, make);\n"
        "print mapped;\n",
        "[[<cycle>]]\n",
        None,
    ),
    (
        "replace_array",
        "let xs = [];\npush(xs, xs);\nxs[0] = nil;\nprint xs;\n",
        "[nil]\n",
        None,
    ),
    (
        "replace_map",
        'let values = {};\nvalues["self"] = values;\nvalues["self"] = nil;\nprint values;\n',
        "map{self: nil}\n",
        None,
    ),
    (
        "replace_struct",
        "struct Node { next: optional<Node> }\n"
        "let node = Node { next: nil };\n"
        "node.next = node;\n"
        "node.next = nil;\n"
        "print node;\n",
        "{next: nil}\n",
        None,
    ),
    (
        "runtime_error",
        "let xs = [];\n"
        "push(xs, xs);\n"
        "print xs;\n"
        "let zero: number = 0;\n"
        "print 1 / zero;\n",
        "",
        "division by zero",
    ),
)


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: vm_cycle_tests.py <compiler> <vm-rs>")

    compiler = Path(sys.argv[1]).resolve()
    vm_path = Path(sys.argv[2]).resolve()
    manifest = vm_path / "Cargo.toml"
    if not compiler.is_file() or not manifest.is_file():
        return fail("compiler or Rust VM manifest not found")

    build = run(["cargo", "build", "--quiet", "--manifest-path", str(manifest)])
    if build.returncode != 0 or build.stdout or build.stderr:
        return fail(f"Rust VM build failed:\n{build.stdout}\n{build.stderr}")
    vm_binary = vm_path / "target" / "debug" / "compiler-design-vm"
    if not vm_binary.is_file():
        return fail(f"Rust VM binary was not built: {vm_binary}")

    with tempfile.TemporaryDirectory(prefix="compiler-design-vm-cycles-") as temporary:
        root = Path(temporary)
        for name, source_text, expected_output, expected_error in CASES:
            source = root / f"{name}.cd"
            artifact = root / f"{name}.cdbc"
            source.write_text(source_text, encoding="utf-8")
            error = compile_source(compiler, source, artifact, name)
            if error is not None:
                return fail(error)

            if expected_error is None:
                error = check_run(vm_binary, artifact, name, expected_output)
                if error is not None:
                    return fail(error)
                fields, error = check_profile(vm_binary, artifact, name, "ok")
                if error is not None:
                    return fail(error)
                if fields["tracked_heap_peak_live"] == 0:
                    return fail(f"{name} profile did not observe tracked storage")
                if name in {"local_cycle", "callback_cycle"} and fields[
                    "tracked_heap_estimated_live_bytes"
                ] >= fields["tracked_heap_estimated_peak_live_bytes"]:
                    return fail(f"{name} profile did not reclaim the cycle: {fields}")
            else:
                result = run([str(vm_binary), "run", str(artifact)])
                if result.returncode == 0 or result.stdout or expected_error not in result.stderr:
                    return fail(
                        f"{name} run did not preserve the runtime failure:\n"
                        f"exit={result.returncode}\nstdout={result.stdout!r}\nstderr={result.stderr!r}"
                    )
                _, error = check_profile(
                    vm_binary,
                    artifact,
                    name,
                    "error kind=runtime",
                    expected_error,
                )
                if error is not None:
                    return fail(error)

        self_array_source = root / "self_array.cd"
        self_array_artifact = root / "self_array.cdbc"
        error = check_trace(vm_binary, self_array_artifact, "self_array")
        if error is not None:
            return fail(error)
        error = check_debug(vm_binary, self_array_source, self_array_artifact, "self_array")
        if error is not None:
            return fail(error)

    print(
        "VM cycle tests: source-backed self and mutual cycles, array/map/struct "
        "replacement, closure environments, callback cycles, runtime errors, and "
        "trace/debug/profile "
        "determinism validated"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
