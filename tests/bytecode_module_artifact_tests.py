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
        for module_path in (source.resolve(), (source.parent / "lib.cd").resolve()):
            identity = f'module="{module_path}" path="'
            if identity not in entry_text or identity not in dependency_text:
                return fail(
                    "module products did not retain canonical source-to-module identity\n"
                    f"missing={identity}"
                )
        if 'kind=import at=2 requested="./lib.cd"' not in entry_text:
            return fail("entry artifact did not preserve the import insertion marker")
        if 'string "before"' not in entry_text or 'string "after"' not in entry_text:
            return fail("entry artifact lost local statements")
        if 'string "lib"' in entry_text:
            return fail("entry artifact lowered dependency statements into its body")
        if 'string "lib"' not in dependency_text:
            return fail("dependency artifact did not contain its own body")

        def expect_dump_rejection(label: str, text: str) -> int | None:
            malformed_path = Path(temporary) / f"{label}.cdbc"
            malformed_path.write_text(text, encoding="utf-8")
            rejected = run([
                "cargo",
                "run",
                "--quiet",
                "--manifest-path",
                str(manifest),
                "--",
                "dump",
                str(malformed_path),
            ])
            if rejected.returncode == 0 or rejected.stdout or not rejected.stderr:
                return fail(
                    f"{label} module artifact was accepted or produced stdout\n"
                    f"exit={rejected.returncode}\nstdout={rejected.stdout}\nstderr={rejected.stderr}"
                )
            return None

        malformed_offset = entry_text.replace(
            ' at=2 requested="./lib.cd"',
            ' at=999 requested="./lib.cd"',
            1,
        )
        rejected = expect_dump_rejection("invalid-module-offset", malformed_offset)
        if rejected is not None:
            return rejected
        malformed_entry_order = entry_text.replace("  entry_order = 0\n", "", 1)
        rejected = expect_dump_rejection("missing-entry-order", malformed_entry_order)
        if rejected is not None:
            return rejected

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

        operator_fixture = Path(__file__).resolve().parent / "golden" / "import_struct_operator_direct"
        operator_source = operator_fixture / "input.cd"
        operator_output_dir = Path(temporary) / "operator-modules"
        emitted_operator = run([
            str(compiler),
            "--emit-module-bytecode",
            str(operator_output_dir),
            str(operator_source),
        ])
        if emitted_operator.returncode != 0 or emitted_operator.stdout or emitted_operator.stderr:
            return fail(
                "operator module emission failed\n"
                f"exit={emitted_operator.returncode}\nstdout={emitted_operator.stdout}\nstderr={emitted_operator.stderr}"
            )

        operator_artifacts = sorted(operator_output_dir.glob("module-*.cdbc"))
        if len(operator_artifacts) != 2:
            return fail(
                "expected two operator module artifacts, found "
                f"{[path.name for path in operator_artifacts]}"
            )

        operator_entry_text = None
        operator_owner_text = None
        for artifact in operator_artifacts:
            text = artifact.read_text(encoding="utf-8")
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
                    f"{artifact.name} failed Rust operator artifact dump\n"
                    f"exit={dumped.returncode}\nstdout={dumped.stdout}\nstderr={dumped.stderr}"
                )
            if "  entry = true\n" in text:
                operator_entry_text = text
            elif "  entry = false\n" in text:
                operator_owner_text = text
            else:
                return fail(f"{artifact.name} has no operator entry declaration")

        if operator_entry_text is None or operator_owner_text is None:
            return fail("did not identify operator entry and owner artifacts")

        if 'kind=import at=0 requested="./lib.cd"' not in operator_entry_text:
            return fail("operator entry artifact did not preserve its dependency marker")
        linkage_names = (
            "__method_Point_operator_Less#0",
            "__method_Point_operator_LessEqual#1",
            "__method_Point_operator_Greater#2",
            "__method_Point_operator_GreaterEqual#3",
        )
        for linkage_name in linkage_names:
            if f'"{linkage_name}"' not in operator_owner_text:
                return fail(f"operator owner artifact lost linkage name {linkage_name}")
            if f'"{linkage_name}"' not in operator_entry_text:
                return fail(f"operator entry artifact lost linkage name {linkage_name}")
        for function_name in ("<", "<=", ">", ">="):
            if f'name="{function_name}"' not in operator_owner_text:
                return fail(f"operator owner artifact lost function {function_name}")
        if "function f0 name=" in operator_entry_text:
            return fail("operator entry artifact unexpectedly contained owner functions")

        operator_linked_path = Path(temporary) / "operator-linked.cdbc"
        linked_operator = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "link",
            str(operator_output_dir),
            str(operator_linked_path),
        ])
        if linked_operator.returncode != 0 or linked_operator.stdout or linked_operator.stderr:
            return fail(
                "operator module link failed\n"
                f"exit={linked_operator.returncode}\nstdout={linked_operator.stdout}\nstderr={linked_operator.stderr}"
            )
        operator_run = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "run",
            str(operator_linked_path),
        ])
        if operator_run.returncode != 0 or operator_run.stdout != "true\ntrue\ntrue\ntrue\n" or operator_run.stderr:
            return fail(
                "linked operator module execution mismatch\n"
                f"exit={operator_run.returncode}\nstdout={operator_run.stdout}\nstderr={operator_run.stderr}"
            )

    print("module bytecode artifact tests: import-order, function, and operator module sets validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
