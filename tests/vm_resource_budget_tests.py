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
            "resource budget fixture emission failed\n"
            f"exit={completed.returncode}\n"
            f"stdout={completed.stdout}\n"
            f"stderr={completed.stderr}"
        )
    return None


def vm_command(manifest: Path, *arguments: str) -> list[str]:
    return [
        "cargo",
        "run",
        "--quiet",
        "--manifest-path",
        str(manifest),
        "--",
        *arguments,
    ]


def assert_resource_error(
    completed: subprocess.CompletedProcess[str],
    fragment: str,
) -> str | None:
    if completed.returncode == 0:
        return f"expected resource failure for {fragment}, got success"
    if completed.stdout:
        return f"resource failure for {fragment} produced stdout: {completed.stdout!r}"
    if fragment not in completed.stderr:
        return (
            f"resource failure for {fragment} omitted diagnostic {fragment!r}\n"
            f"stderr={completed.stderr}"
        )
    return None


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: vm_resource_budget_tests.py <compiler> <vm-rs>")

    compiler = Path(sys.argv[1]).resolve()
    vm_path = Path(sys.argv[2]).resolve()
    manifest = vm_path / "Cargo.toml"
    if not compiler.is_file() or not manifest.is_file():
        return fail("compiler or Rust VM manifest not found")

    with tempfile.TemporaryDirectory(prefix="compiler-design-vm-budget-") as temporary:
        root = Path(temporary)
        source = root / "budget.cd"
        source.write_text('print "é";\n', encoding="utf-8")
        artifact = root / "budget.cdbc"
        emitted = compile_source(compiler, source, artifact)
        if emitted is not None:
            return emitted

        elements_source = root / "elements.cd"
        elements_source.write_text("print [1];\n", encoding="utf-8")
        elements_artifact = root / "elements.cdbc"
        emitted = compile_source(compiler, elements_source, elements_artifact)
        if emitted is not None:
            return emitted

        normal = run(vm_command(manifest, "run", str(artifact)))
        if normal.returncode != 0 or normal.stdout != "é\n" or normal.stderr:
            return fail(
                "default resource configuration changed normal output\n"
                f"exit={normal.returncode}\nstdout={normal.stdout}\nstderr={normal.stderr}"
            )

        for arguments, fragment in (
            (("run", str(artifact), "--max-output-bytes", "2"), "output bytes (limit 2)"),
            (("run", str(elements_artifact), "--max-elements", "1"), "runtime elements (limit 1)"),
            (("dump", str(artifact), "--max-artifact-bytes", "1"), "artifact bytes (limit 1)"),
        ):
            error = assert_resource_error(run(vm_command(manifest, *arguments)), fragment)
            if error is not None:
                return fail(error)

        invalid_utf8 = root / "invalid-utf8.cdbc"
        invalid_utf8.write_bytes(b"cdbc 0.1\n\xff\n")
        invalid = run(vm_command(manifest, "dump", str(invalid_utf8)))
        if invalid.returncode == 0 or invalid.stdout or "failed to read" not in invalid.stderr:
            return fail(
                "invalid UTF-8 artifact was not rejected safely\n"
                f"exit={invalid.returncode}\nstdout={invalid.stdout}\nstderr={invalid.stderr}"
            )

        unlimited = run(
            vm_command(
                manifest,
                "run",
                "--max-steps",
                "1",
                "--unlimited",
                str(artifact),
            )
        )
        if unlimited.returncode != 0 or unlimited.stdout != "é\n" or unlimited.stderr:
            return fail(
                "--unlimited did not disable explicit budgets\n"
                f"exit={unlimited.returncode}\nstdout={unlimited.stdout}\nstderr={unlimited.stderr}"
            )

        module_source = root / "main.cd"
        library_source = root / "lib.cd"
        library_source.write_text(
            "fun helper(): number { return 1; }\nexport helper;\n",
            encoding="utf-8",
        )
        module_source.write_text(
            'import "./lib.cd";\nprint 1;\n',
            encoding="utf-8",
        )
        module_directory = root / "modules"
        emitted = run(
            [
                str(compiler),
                "--emit-module-bytecode",
                str(module_directory),
                str(module_source),
            ]
        )
        if emitted.returncode != 0 or emitted.stdout or emitted.stderr:
            return fail(
                "module budget fixture emission failed\n"
                f"exit={emitted.returncode}\nstdout={emitted.stdout}\nstderr={emitted.stderr}"
            )
        linked = run(
            vm_command(
                manifest,
                "link",
                "--max-modules",
                "1",
                str(module_directory),
                str(root / "linked.cdbc"),
            )
        )
        error = assert_resource_error(linked, "module count (limit 1)")
        if error is not None:
            return fail(error)

    print("VM resource budget tests: defaults, overrides, cancellation boundary, artifact, and module limits validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
