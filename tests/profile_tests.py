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


def compile_source(compiler: Path, source: Path, artifact: Path) -> int | None:
    completed = run([str(compiler), "--emit-bytecode", str(artifact), str(source)])
    if completed.returncode != 0 or completed.stdout or completed.stderr:
        return fail(
            "profile fixture emission failed\n"
            f"exit={completed.returncode}\n"
            f"stdout={completed.stdout}\n"
            f"stderr={completed.stderr}"
        )
    return None


def profile_command(manifest: Path, artifact: Path, *options: str) -> subprocess.CompletedProcess[str]:
    return run(
        [
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "profile",
            str(artifact),
            *options,
        ]
    )


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: profile_tests.py <compiler> <vm-rs>")

    compiler = Path(sys.argv[1]).resolve()
    vm_path = Path(sys.argv[2]).resolve()
    manifest = vm_path / "Cargo.toml"
    if not compiler.is_file() or not manifest.is_file():
        return fail("compiler or Rust VM manifest not found")

    with tempfile.TemporaryDirectory(prefix="compiler-design-vm-profile-") as temporary:
        root = Path(temporary)
        source = root / "profile.cd"
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
        artifact = root / "profile.cdbc"
        emitted = compile_source(compiler, source, artifact)
        if emitted is not None:
            return emitted

        first = profile_command(manifest, artifact)
        second = profile_command(manifest, artifact)
        if (first.returncode, first.stdout, first.stderr) != (
            second.returncode,
            second.stdout,
            second.stderr,
        ):
            return fail("profile output was not deterministic")
        if first.returncode != 0 or first.stderr:
            return fail(
                "profile execution failed\n"
                f"exit={first.returncode}\nstdout={first.stdout}\nstderr={first.stderr}"
            )
        required = (
            "profile status=ok",
            "profile instruction_count=",
            "output_bytes=4",
            'profile function index=main name="main" calls=1 instructions=',
            'profile function index=f0 name="add" calls=1 instructions=',
            'profile source_range source=s0 path=',
            " hits=",
        )
        missing = [fragment for fragment in required if fragment not in first.stdout]
        if missing:
            return fail(f"profile report omitted required fields: {missing}\n{first.stdout}")
        if any(line == "4" for line in first.stdout.splitlines()):
            return fail(f"profile mixed program stdout into the report:\n{first.stdout}")

        native_source = root / "native.cd"
        native_source.write_text("print str(7);\n", encoding="utf-8")
        native_artifact = root / "native.cdbc"
        emitted = compile_source(compiler, native_source, native_artifact)
        if emitted is not None:
            return emitted
        native = profile_command(manifest, native_artifact)
        if native.returncode != 0 or native.stderr:
            return fail(
                "native profile execution failed\n"
                f"exit={native.returncode}\nstdout={native.stdout}\nstderr={native.stderr}"
            )
        if 'profile native name="str" calls=1' not in native.stdout:
            return fail(f"native profile counter missing:\n{native.stdout}")
        if "output_bytes=2" not in native.stdout or any(
            line == "7" for line in native.stdout.splitlines()
        ):
            return fail(f"native profile output accounting is incorrect:\n{native.stdout}")

        failing_source = root / "failure.cd"
        failing_source.write_text(
            'fun fail() { print "ok"; return 1 / 0; }\nfail();\n',
            encoding="utf-8",
        )
        failing_artifact = root / "failure.cdbc"
        emitted = compile_source(compiler, failing_source, failing_artifact)
        if emitted is not None:
            return emitted
        failed = profile_command(manifest, failing_artifact)
        if failed.returncode == 0 or "profile status=error kind=runtime" not in failed.stdout:
            return fail(
                "profile did not retain a failure status\n"
                f"exit={failed.returncode}\nstdout={failed.stdout}\nstderr={failed.stderr}"
            )
        if "output_bytes=3" not in failed.stdout or any(
            line == "ok" for line in failed.stdout.splitlines()
        ):
            return fail(f"profile did not retain partial output bytes:\n{failed.stdout}")
        if "Runtime error" not in failed.stderr or "division by zero" not in failed.stderr:
            return fail(f"profile failure lost the runtime diagnostic:\n{failed.stderr}")

        limited = profile_command(manifest, artifact, "--max-steps", "1")
        if limited.returncode == 0 or "profile status=error kind=resource" not in limited.stdout:
            return fail(
                "profile did not expose the instruction budget failure\n"
                f"exit={limited.returncode}\nstdout={limited.stdout}\nstderr={limited.stderr}"
            )
        if "instruction steps (limit 1)" not in limited.stderr:
            return fail(f"profile budget failure lost its diagnostic:\n{limited.stderr}")

    print("VM profile tests: deterministic counters, native calls, source ranges, output bytes, and failure reports validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
