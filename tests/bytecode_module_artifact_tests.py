#!/usr/bin/env python3
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Check independently emitted module cdbc artifacts.")
    parser.add_argument("compiler", type=Path)
    parser.add_argument("vm", type=Path)
    args = parser.parse_args()

    compiler = args.compiler.resolve()
    manifest = args.vm.resolve() / "Cargo.toml"
    fixture = Path(__file__).resolve().parent / "golden" / "module_import_order"
    source = fixture / "input.cd"
    if not compiler.is_file() or not manifest.is_file():
        return fail("compiler or Rust VM manifest not found")

    with tempfile.TemporaryDirectory() as temporary:
        output_dir = Path(temporary) / "modules"
        emitted = run([str(compiler), "--emit-module-bytecode", str(output_dir), str(source)])
        if emitted.returncode != 0 or emitted.stdout or emitted.stderr:
            return fail(
                "module emission failed\n"
                f"exit={emitted.returncode}\nstdout={emitted.stdout}\nstderr={emitted.stderr}"
            )

        artifacts = sorted(output_dir.glob("module-*.cdbc"))
        if len(artifacts) != 2:
            return fail(f"expected two module artifacts, found {[path.name for path in artifacts]}")

        entry_artifact = None
        dependency_artifact = None
        for artifact in artifacts:
            text = artifact.read_text(encoding="utf-8")
            if not text.startswith("cdbc 0.1\n\nartifact: module\n"):
                return fail(f"{artifact.name} is missing the module envelope")
            if "  entry = true\n" in text:
                entry_artifact = (artifact, text)
            elif "  entry = false\n" in text:
                dependency_artifact = (artifact, text)
            else:
                return fail(f"{artifact.name} has no entry declaration")

            dumped = run([
                "cargo",
                "run",
                "--quiet",
                "--manifest-path",
                str(manifest),
                "--",
                "dump",
                str(artifact),
            ])
            if dumped.returncode != 0 or dumped.stdout != text or dumped.stderr:
                return fail(
                    f"{artifact.name} failed Rust canonical dump\n"
                    f"exit={dumped.returncode}\nstdout={dumped.stdout}\nstderr={dumped.stderr}"
                )

            ran = run([
                "cargo",
                "run",
                "--quiet",
                "--manifest-path",
                str(manifest),
                "--",
                "run",
                str(artifact),
            ])
            if ran.returncode == 0 or ran.stdout or "error: cannot run an unlinked module artifact" not in ran.stderr:
                return fail(
                    f"{artifact.name} was unexpectedly executable\n"
                    f"exit={ran.returncode}\nstdout={ran.stdout}\nstderr={ran.stderr}"
                )

        if entry_artifact is None or dependency_artifact is None:
            return fail("did not identify entry and dependency artifacts")

        _, entry_text = entry_artifact
        _, dependency_text = dependency_artifact
        if 'kind=import at=2 requested="./lib.cd"' not in entry_text:
            return fail("entry artifact did not preserve the import insertion marker")
        if 'string "before"' not in entry_text or 'string "after"' not in entry_text:
            return fail("entry artifact lost local statements")
        if 'string "lib"' in entry_text:
            return fail("entry artifact lowered dependency statements into its body")
        if 'string "lib"' not in dependency_text:
            return fail("dependency artifact did not contain its own body")

        linked_path = Path(temporary) / "linked.cdbc"
        linked = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "link",
            str(output_dir),
            str(linked_path),
        ])
        if linked.returncode != 0 or linked.stdout or linked.stderr or not linked_path.is_file():
            return fail(
                "module link failed\n"
                f"exit={linked.returncode}\nstdout={linked.stdout}\nstderr={linked.stderr}"
            )

        linked_text = linked_path.read_text(encoding="utf-8")
        dumped = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "dump",
            str(linked_path),
        ])
        if dumped.returncode != 0 or dumped.stdout != linked_text or dumped.stderr:
            return fail(
                "linked artifact failed Rust canonical dump\n"
                f"exit={dumped.returncode}\nstdout={dumped.stdout}\nstderr={dumped.stderr}"
            )

        ran = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "run",
            str(linked_path),
        ])
        if ran.returncode != 0 or ran.stdout != "before\nlib\nafter\n" or ran.stderr:
            return fail(
                "linked artifact execution mismatch\n"
                f"exit={ran.returncode}\nstdout={ran.stdout}\nstderr={ran.stderr}"
            )

        function_output_dir = Path(temporary) / "function-modules"
        function_source = Path(__file__).resolve().parent / "golden" / "import_basic" / "input.cd"
        emitted_function = run([
            str(compiler),
            "--emit-module-bytecode",
            str(function_output_dir),
            str(function_source),
        ])
        if emitted_function.returncode != 0 or emitted_function.stdout or emitted_function.stderr:
            return fail(
                "function module emission failed\n"
                f"exit={emitted_function.returncode}\nstdout={emitted_function.stdout}\nstderr={emitted_function.stderr}"
            )
        function_linked_path = Path(temporary) / "function-linked.cdbc"
        linked_function = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "link",
            str(function_output_dir),
            str(function_linked_path),
        ])
        if linked_function.returncode != 0 or linked_function.stdout or linked_function.stderr:
            return fail(
                "function module link failed\n"
                f"exit={linked_function.returncode}\nstdout={linked_function.stdout}\nstderr={linked_function.stderr}"
            )
        function_run = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "run",
            str(function_linked_path),
        ])
        if function_run.returncode != 0 or function_run.stdout != "3\n" or function_run.stderr:
            return fail(
                "linked function module execution mismatch\n"
                f"exit={function_run.returncode}\nstdout={function_run.stdout}\nstderr={function_run.stderr}"
            )

    print("module bytecode artifact tests: import-order and function module sets validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
