#!/usr/bin/env python3

import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def compile_artifact(compiler: Path, source: Path, artifact: Path) -> int | None:
    completed = run([str(compiler), "--emit-bytecode", str(artifact), str(source)])
    if completed.returncode != 0 or completed.stdout or completed.stderr:
        return fail(
            "debugger fixture emission failed\n"
            f"exit={completed.returncode}\n"
            f"stdout={completed.stdout}\n"
            f"stderr={completed.stderr}"
        )
    return None


def trace_command(manifest: Path, artifact: Path) -> subprocess.CompletedProcess[str]:
    return run([
        "cargo",
        "run",
        "--quiet",
        "--manifest-path",
        str(manifest),
        "--",
        "trace",
        str(artifact),
    ])


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: debugger_tests.py <compiler> <vm-rs>")

    compiler = Path(sys.argv[1]).resolve()
    vm_path = Path(sys.argv[2]).resolve()
    manifest = vm_path / "Cargo.toml"
    if not compiler.is_file() or not manifest.is_file():
        return fail("compiler or Rust VM manifest not found")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "trace.cd"
        source.write_text(
            "fun add(value: number): number {\n"
            "  let result: number = value + 1;\n"
            "  print result;\n"
            "  return result;\n"
            "}\n"
            "let answer: number = add(4);\n"
            "print answer;\n",
            encoding="utf-8",
        )
        artifact = root / "trace.cdbc"
        emitted = compile_artifact(compiler, source, artifact)
        if emitted is not None:
            return emitted

        first = trace_command(manifest, artifact)
        second = trace_command(manifest, artifact)
        if (first.returncode, first.stdout, first.stderr) != (
            second.returncode,
            second.stdout,
            second.stderr,
        ):
            return fail("source trace output was not deterministic")
        if first.returncode != 0 or first.stderr:
            return fail(
                "source trace failed\n"
                f"exit={first.returncode}\nstdout={first.stdout}\nstderr={first.stderr}"
            )

        lines = first.stdout.splitlines()
        if not lines or any(not line.startswith("trace ") for line in lines):
            return fail(f"trace output contained a non-trace line: {first.stdout}")
        required = (
            "kind=enter function=main",
            "kind=enter function=add",
            "kind=output function=add",
            'value="5"',
            "kind=return function=add",
            'locals={value="4"',
            'answer="5"',
            "stack=main@",
            "range=s",
        )
        missing = [fragment for fragment in required if fragment not in first.stdout]
        if missing:
            return fail(f"source trace omitted required fields: {missing}\n{first.stdout}")

        failing_source = root / "failure.cd"
        failing_source.write_text(
            "fun fail() { return 1 / 0; }\n"
            "fail();\n",
            encoding="utf-8",
        )
        failing_artifact = root / "failure.cdbc"
        emitted = compile_artifact(compiler, failing_source, failing_artifact)
        if emitted is not None:
            return emitted
        failed = trace_command(manifest, failing_artifact)
        if failed.returncode == 0 or not failed.stdout or "kind=error function=fail" not in failed.stdout:
            return fail(
                "runtime trace did not expose the failing source frame\n"
                f"exit={failed.returncode}\nstdout={failed.stdout}\nstderr={failed.stderr}"
            )
        if "Runtime error" not in failed.stderr or "Call stack:" not in failed.stderr:
            return fail(f"runtime trace lost the existing runtime diagnostic:\n{failed.stderr}")

    print("debugger tests: deterministic source tracing, stack, locals, values, and failure frames validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
