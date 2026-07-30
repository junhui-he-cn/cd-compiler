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


def debug_command(
    manifest: Path, artifact: Path, commands: list[str]
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "debug",
            str(artifact),
        ],
        input="\n".join(commands) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )


def link_command(
    manifest: Path, module_directory: Path, artifact: Path
) -> subprocess.CompletedProcess[str]:
    return run([
        "cargo",
        "run",
        "--quiet",
        "--manifest-path",
        str(manifest),
        "--",
        "link",
        str(module_directory),
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

        debugged = debug_command(
            manifest,
            artifact,
            [
                f"break {source.resolve()}:2",
                "continue",
                "step",
                "next",
                "continue",
            ],
        )
        if debugged.returncode != 0 or debugged.stderr:
            return fail(
                "interactive debugger failed\n"
                f"exit={debugged.returncode}\nstdout={debugged.stdout}\nstderr={debugged.stderr}"
            )
        required_debug = (
            "pause reason=entry function=main",
            "pause reason=breakpoint function=add",
            "pause reason=step function=add",
            "pause reason=next function=add",
            "module=none location=",
            'locals={value="4"',
            "5\n5\n",
        )
        missing_debug = [fragment for fragment in required_debug if fragment not in debugged.stdout]
        if missing_debug:
            return fail(
                f"interactive debugger omitted required fields: {missing_debug}\n"
                f"{debugged.stdout}"
            )

        range_start = source.read_text(encoding="utf-8").index("print result")
        range_end = range_start + len("print result")
        ranged = debug_command(
            manifest,
            artifact,
            [
                f"break-range {source.resolve()}:{range_start}-{range_end}",
                "continue",
                "continue",
            ],
        )
        if ranged.returncode != 0 or ranged.stderr:
            return fail(
                "range breakpoint session failed\n"
                f"exit={ranged.returncode}\nstdout={ranged.stdout}\nstderr={ranged.stderr}"
            )
        if "pause reason=breakpoint function=add" not in ranged.stdout or "range=s" not in ranged.stdout:
            return fail(f"range breakpoint did not hit an artifact range:\n{ranged.stdout}")

        module_root = root / "module-debug"
        module_root.mkdir()
        module_source = module_root / "entry.cd"
        module_library = module_root / "lib.cd"
        module_source.write_text('import "./lib.cd";\nprint "entry";\n', encoding="utf-8")
        module_library.write_text('print "lib";\n', encoding="utf-8")
        module_products = root / "module-products"
        emitted_modules = run([
            str(compiler),
            "--emit-module-bytecode",
            str(module_products),
            str(module_source),
        ])
        if emitted_modules.returncode != 0 or emitted_modules.stdout or emitted_modules.stderr:
            return fail(
                "debugger module fixture emission failed\n"
                f"exit={emitted_modules.returncode}\nstdout={emitted_modules.stdout}\nstderr={emitted_modules.stderr}"
            )
        linked_module_artifact = root / "module-linked.cdbc"
        linked_modules = link_command(manifest, module_products, linked_module_artifact)
        if linked_modules.returncode != 0 or linked_modules.stdout or linked_modules.stderr:
            return fail(
                "debugger module fixture link failed\n"
                f"exit={linked_modules.returncode}\nstdout={linked_modules.stdout}\nstderr={linked_modules.stderr}"
            )
        imported = debug_command(
            manifest,
            linked_module_artifact,
            [f"break {module_library.resolve()}:1", "continue", "continue"],
        )
        if imported.returncode != 0 or imported.stderr:
            return fail(
                "interactive imported-module debugger failed\n"
                f"exit={imported.returncode}\nstdout={imported.stdout}\nstderr={imported.stderr}"
            )
        if "pause reason=breakpoint function=main" not in imported.stdout or "lib.cd:1:" not in imported.stdout:
            return fail(f"interactive debugger did not stop in imported source:\n{imported.stdout}")
        if "lib\nentry\n" not in imported.stdout:
            return fail(f"interactive imported-module run lost program output:\n{imported.stdout}")

        closure_source = root / "closure.cd"
        closure_source.write_text(
            "fun makeAdder(base: number) {\n"
            "  return fun (value: number) { return base + value; };\n"
            "}\n"
            "let add = makeAdder(4);\n"
            "print add(3);\n",
            encoding="utf-8",
        )
        closure_artifact = root / "closure.cdbc"
        emitted = compile_artifact(compiler, closure_source, closure_artifact)
        if emitted is not None:
            return emitted
        closure_debug = debug_command(
            manifest,
            closure_artifact,
            [f"break {closure_source.resolve()}:2", "continue", "continue"],
        )
        if closure_debug.returncode != 0 or closure_debug.stderr:
            return fail(
                "interactive closure debugger failed\n"
                f"exit={closure_debug.returncode}\nstdout={closure_debug.stdout}\nstderr={closure_debug.stderr}"
            )
        if 'base="4"' not in closure_debug.stdout or 'value="3"' not in closure_debug.stdout:
            return fail(f"interactive debugger lost closure locals:\n{closure_debug.stdout}")

        failed_debug = debug_command(
            manifest, failing_artifact, ["continue", "continue", "continue"]
        )
        if (
            failed_debug.returncode == 0
            or "pause reason=entry" not in failed_debug.stdout
            or "pause reason=error function=fail" not in failed_debug.stdout
            or "pause reason=error function=main" not in failed_debug.stdout
        ):
            return fail(
                "interactive debugger did not preserve runtime failure control\n"
                f"exit={failed_debug.returncode}\nstdout={failed_debug.stdout}\nstderr={failed_debug.stderr}"
            )
        if "Runtime error" not in failed_debug.stderr or "Call stack:" not in failed_debug.stderr:
            return fail(f"interactive debugger lost runtime failure diagnostics:\n{failed_debug.stderr}")

        metadata_free = root / "metadata-free.cdbc"
        metadata_free.write_text(
            "cdbc 0.1\n\n"
            "constants:\n"
            "  c0 = number 1\n\n"
            "names:\n\n"
            "main registers=1:\n"
            "  r0 = constant c0\n"
            "  print r0\n",
            encoding="utf-8",
        )
        metadata_free_debug = debug_command(manifest, metadata_free, ["continue"])
        if metadata_free_debug.returncode != 0 or metadata_free_debug.stderr:
            return fail(
                "metadata-free interactive debugger failed\n"
                f"exit={metadata_free_debug.returncode}\nstdout={metadata_free_debug.stdout}\nstderr={metadata_free_debug.stderr}"
            )
        if "location=<unknown>" not in metadata_free_debug.stdout or "1\n" not in metadata_free_debug.stdout:
            return fail(f"metadata-free interactive debugger lost explicit unknown location:\n{metadata_free_debug.stdout}")

        first_quit = debug_command(manifest, artifact, ["quit"])
        second_quit = debug_command(manifest, artifact, ["quit"])
        if (first_quit.returncode, first_quit.stdout, first_quit.stderr) != (
            second_quit.returncode,
            second_quit.stdout,
            second_quit.stderr,
        ):
            return fail("interactive debugger quit session was not deterministic")
        if first_quit.returncode != 0 or "pause reason=entry" not in first_quit.stdout:
            return fail(
                "interactive debugger quit did not expose the entry pause\n"
                f"exit={first_quit.returncode}\nstdout={first_quit.stdout}\nstderr={first_quit.stderr}"
            )

    print("debugger tests: deterministic trace and interactive source debugging validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
