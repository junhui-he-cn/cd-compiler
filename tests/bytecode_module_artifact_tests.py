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
            if not text.startswith("cdbc 0.2\n\nartifact: module\n"):
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
        if 'kind=import requested="./lib.cd"' not in entry_text:
            return fail("entry artifact did not preserve the import marker")
        if "init = f0\n" not in entry_text or "init = f0\n" not in dependency_text:
            return fail("module artifact did not record its init function")
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

        malformed_init = entry_text.replace("  init = f0\n", "  init = f999\n", 1)
        rejected = expect_dump_rejection("invalid-module-init", malformed_init)
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

        method_fixture = Path(__file__).resolve().parent / "golden" / "import_struct_method_direct"
        method_source = method_fixture / "input.cd"
        method_output_dir = Path(temporary) / "method-modules"
        emitted_method = run([
            str(compiler),
            "--emit-module-bytecode",
            str(method_output_dir),
            str(method_source),
        ])
        if emitted_method.returncode != 0 or emitted_method.stdout or emitted_method.stderr:
            return fail(
                "method module emission failed\n"
                f"exit={emitted_method.returncode}\nstdout={emitted_method.stdout}\nstderr={emitted_method.stderr}"
            )

        method_artifacts = sorted(method_output_dir.glob("module-*.cdbc"))
        if len(method_artifacts) != 2:
            return fail(
                "expected two method module artifacts, found "
                f"{[path.name for path in method_artifacts]}"
            )

        method_entry_text = None
        method_owner_text = None
        for artifact in method_artifacts:
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
                    f"{artifact.name} failed Rust method artifact dump\n"
                    f"exit={dumped.returncode}\nstdout={dumped.stdout}\nstderr={dumped.stderr}"
                )
            if "  entry = true\n" in text:
                method_entry_text = text
            elif "  entry = false\n" in text:
                method_owner_text = text
            else:
                return fail(f"{artifact.name} has no method entry declaration")

        if method_entry_text is None or method_owner_text is None:
            return fail("did not identify method entry and owner artifacts")

        if 'kind=import requested="./lib.cd"' not in method_entry_text:
            return fail("method entry artifact did not preserve its dependency marker")
        linkage_names = ("__method_Point_sum#0",)
        for linkage_name in linkage_names:
            if f'"{linkage_name}"' not in method_owner_text:
                return fail(f"method owner artifact lost linkage name {linkage_name}")
            if f'"{linkage_name}"' not in method_entry_text:
                return fail(f"method entry artifact lost linkage name {linkage_name}")
        if 'name="sum"' not in method_owner_text:
            return fail("method owner artifact lost function sum")
        if 'name="sum"' in method_entry_text:
            return fail("method entry artifact unexpectedly contained owner functions")

        method_linked_path = Path(temporary) / "method-linked.cdbc"
        linked_method = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "link",
            str(method_output_dir),
            str(method_linked_path),
        ])
        if linked_method.returncode != 0 or linked_method.stdout or linked_method.stderr:
            return fail(
                "method module link failed\n"
                f"exit={linked_method.returncode}\nstdout={linked_method.stdout}\nstderr={linked_method.stderr}"
            )
        method_run = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "run",
            str(method_linked_path),
        ])
        if method_run.returncode != 0 or method_run.stdout != "7\n" or method_run.stderr:
            return fail(
                "linked method module execution mismatch\n"
                f"exit={method_run.returncode}\nstdout={method_run.stdout}\nstderr={method_run.stderr}"
            )

        recursive_fixture = Path(__file__).resolve().parent / "golden" / "recursive_node_import"
        recursive_source = recursive_fixture / "input.cd"
        recursive_output_dir = Path(temporary) / "recursive-modules"
        emitted_recursive = run([
            str(compiler),
            "--emit-module-bytecode",
            str(recursive_output_dir),
            str(recursive_source),
        ])
        if emitted_recursive.returncode != 0 or emitted_recursive.stdout or emitted_recursive.stderr:
            return fail(
                "recursive module emission failed\n"
                f"exit={emitted_recursive.returncode}\nstdout={emitted_recursive.stdout}"
                f"\nstderr={emitted_recursive.stderr}"
            )

        recursive_artifacts = sorted(recursive_output_dir.glob("module-*.cdbc"))
        if len(recursive_artifacts) != 2:
            return fail(
                "expected two recursive module artifacts, found "
                f"{[path.name for path in recursive_artifacts]}"
            )

        recursive_entry_text = None
        recursive_dependency_text = None
        for artifact in recursive_artifacts:
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
                    f"{artifact.name} failed Rust recursive artifact dump\n"
                    f"exit={dumped.returncode}\nstdout={dumped.stdout}\nstderr={dumped.stderr}"
                )
            if "  entry = true\n" in text:
                recursive_entry_text = text
            elif "  entry = false\n" in text:
                recursive_dependency_text = text
            else:
                return fail(f"{artifact.name} has no recursive entry declaration")

        if recursive_entry_text is None or recursive_dependency_text is None:
            return fail("did not identify recursive entry and dependency artifacts")
        if 'kind=import requested="./lib.cd"' not in recursive_entry_text:
            return fail("recursive entry artifact did not preserve its dependency marker")
        if "make_struct " not in recursive_entry_text or "struct_set " not in recursive_entry_text:
            return fail("recursive entry artifact did not use existing struct field operations")
        if "new_ref" in recursive_entry_text or "new_ref" in recursive_dependency_text:
            return fail("recursive artifact introduced an unsupported reference opcode")

        recursive_linked_path = Path(temporary) / "recursive-linked.cdbc"
        linked_recursive = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "link",
            str(recursive_output_dir),
            str(recursive_linked_path),
        ])
        if linked_recursive.returncode != 0 or linked_recursive.stdout or linked_recursive.stderr:
            return fail(
                "recursive module link failed\n"
                f"exit={linked_recursive.returncode}\nstdout={linked_recursive.stdout}"
                f"\nstderr={linked_recursive.stderr}"
            )
        recursive_run = run([
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "run",
            str(recursive_linked_path),
        ])
        if recursive_run.returncode != 0 or recursive_run.stdout != "nil\n20\n" or recursive_run.stderr:
            return fail(
                "linked recursive module execution mismatch\n"
                f"exit={recursive_run.returncode}\nstdout={recursive_run.stdout}"
                f"\nstderr={recursive_run.stderr}"
            )

    print("module bytecode artifact tests: import-order, function, method, and recursive module sets validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
