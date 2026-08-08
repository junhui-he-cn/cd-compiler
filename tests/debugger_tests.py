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


def compile_artifact(
    compiler: Path,
    source: Path,
    artifact: Path,
    optimization_level: str | None = None,
) -> int | None:
    command = [str(compiler), "--emit-bytecode", str(artifact)]
    if optimization_level is not None:
        command.extend(["--opt-level", optimization_level])
    command.append(str(source))
    completed = run(command)
    if completed.returncode != 0 or completed.stdout or completed.stderr:
        return fail(
            "debugger fixture emission failed\n"
            f"exit={completed.returncode}\n"
            f"stdout={completed.stdout}\n"
            f"stderr={completed.stderr}"
        )
    return None


def trace_semantic_signature(output: str) -> list[tuple[str, ...]]:
    signature = []
    for line in output.splitlines():
        fields = line.split()
        kind = next((field for field in fields if field.startswith("kind=")), None)
        if kind is None or kind.split("=", 1)[1] == "line":
            continue
        signature.append(
            tuple(
                field
                for field in fields
                if field.startswith("kind=")
                or field.startswith("function=")
                or field.startswith("value=")
            )
        )
    return signature


def compare_trace_pair(
    baseline, optimized, label: str
) -> str | None:
    if baseline.returncode != optimized.returncode:
        return f"{label} O0/O1 trace exit codes differ"
    if "location=<unknown>" in optimized.stdout:
        return f"{label} O1 trace lost source-backed debug locations\n{optimized.stdout}"
    if any(
        "location=" in line and "range=" not in line
        for line in optimized.stdout.splitlines()
    ):
        return f"{label} O1 trace lost source-backed debug ranges\n{optimized.stdout}"
    if trace_semantic_signature(baseline.stdout) != trace_semantic_signature(optimized.stdout):
        return (
            f"{label} O0/O1 trace semantic events differ\n"
            f"O0={trace_semantic_signature(baseline.stdout)}\n"
            f"O1={trace_semantic_signature(optimized.stdout)}"
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

        optimized_artifact = root / "trace-o1.cdbc"
        emitted = compile_artifact(compiler, source, optimized_artifact, "1")
        if emitted is not None:
            return emitted
        optimized_first = trace_command(manifest, optimized_artifact)
        optimized_second = trace_command(manifest, optimized_artifact)
        if (optimized_first.returncode, optimized_first.stdout, optimized_first.stderr) != (
            optimized_second.returncode,
            optimized_second.stdout,
            optimized_second.stderr,
        ):
            return fail("optimized source trace output was not deterministic")
        if optimized_first.returncode != 0 or optimized_first.stderr:
            return fail(
                "optimized source trace failed\n"
                f"exit={optimized_first.returncode}\n"
                f"stdout={optimized_first.stdout}\n"
                f"stderr={optimized_first.stderr}"
            )
        trace_difference = compare_trace_pair(first, optimized_first, "function trace")
        if trace_difference is not None:
            return fail(trace_difference)
        if 'locals={value="4"' not in optimized_first.stdout or 'value="5"' not in optimized_first.stdout:
            return fail(f"optimized source trace lost locals or output:\n{optimized_first.stdout}")

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

        optimized_failing_artifact = root / "failure-o1.cdbc"
        emitted = compile_artifact(compiler, failing_source, optimized_failing_artifact, "1")
        if emitted is not None:
            return emitted
        optimized_failed = trace_command(manifest, optimized_failing_artifact)
        if (
            optimized_failed.returncode == 0
            or not optimized_failed.stdout
            or "kind=error function=main" not in optimized_failed.stdout
            or "location=<unknown>" in optimized_failed.stdout
        ):
            return fail(
                "optimized runtime trace did not preserve the failing source frame\n"
                f"exit={optimized_failed.returncode}\n"
                f"stdout={optimized_failed.stdout}\n"
                f"stderr={optimized_failed.stderr}"
            )
        trace_difference = compare_trace_pair(failed, optimized_failed, "runtime failure trace")
        if trace_difference is not None:
            return fail(trace_difference)
        if "Runtime error" not in optimized_failed.stderr or "Call stack:" not in optimized_failed.stderr:
            return fail(
                "optimized runtime trace lost the existing runtime diagnostic:\n"
                f"{optimized_failed.stderr}"
            )

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

        optimized_debugged = debug_command(
            manifest,
            optimized_artifact,
            [
                f"break {source.resolve()}:2",
                "continue",
                "step",
                "next",
                "continue",
            ],
        )
        if optimized_debugged.returncode != 0 or optimized_debugged.stderr:
            return fail(
                "optimized interactive debugger failed\n"
                f"exit={optimized_debugged.returncode}\n"
                f"stdout={optimized_debugged.stdout}\n"
                f"stderr={optimized_debugged.stderr}"
            )
        if "location=<unknown>" in optimized_debugged.stdout:
            return fail(
                "optimized interactive debugger lost source-backed locations\n"
                f"{optimized_debugged.stdout}"
            )
        missing_optimized_debug = [
            fragment
            for fragment in required_debug
            if fragment not in optimized_debugged.stdout
        ]
        if missing_optimized_debug:
            return fail(
                "optimized interactive debugger omitted required fields: "
                f"{missing_optimized_debug}\n{optimized_debugged.stdout}"
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

        optimized_ranged = debug_command(
            manifest,
            optimized_artifact,
            [
                f"break-range {source.resolve()}:{range_start}-{range_end}",
                "continue",
                "continue",
            ],
        )
        if optimized_ranged.returncode != 0 or optimized_ranged.stderr:
            return fail(
                "optimized range breakpoint session failed\n"
                f"exit={optimized_ranged.returncode}\n"
                f"stdout={optimized_ranged.stdout}\n"
                f"stderr={optimized_ranged.stderr}"
            )
        if (
            "pause reason=breakpoint function=add" not in optimized_ranged.stdout
            or "range=s" not in optimized_ranged.stdout
            or "location=<unknown>" in optimized_ranged.stdout
        ):
            return fail(
                "optimized range breakpoint did not retain source mapping:\n"
                f"{optimized_ranged.stdout}"
            )

        module_root = root / "module-debug"
        module_root.mkdir()
        module_source = module_root / "entry.cd"
        module_library = module_root / "lib.cd"
        module_source.write_text('import "./lib.cd";\nprint add(1, 2);\n', encoding="utf-8")
        module_library.write_text(
            "fun add(left, right) { return left + right; }\n"
            "export add;\n",
            encoding="utf-8",
        )
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
        module_products_o1 = root / "module-products-o1"
        emitted_modules_o1 = run([
            str(compiler),
            "--emit-module-bytecode",
            str(module_products_o1),
            "--opt-level",
            "1",
            str(module_source),
        ])
        if emitted_modules_o1.returncode != 0 or emitted_modules_o1.stdout or emitted_modules_o1.stderr:
            return fail(
                "optimized debugger module fixture emission failed\n"
                f"exit={emitted_modules_o1.returncode}\n"
                f"stdout={emitted_modules_o1.stdout}\n"
                f"stderr={emitted_modules_o1.stderr}"
            )
        linked_module_artifact = root / "module-linked.cdbc"
        linked_modules = link_command(manifest, module_products, linked_module_artifact)
        if linked_modules.returncode != 0 or linked_modules.stdout or linked_modules.stderr:
            return fail(
                "debugger module fixture link failed\n"
                f"exit={linked_modules.returncode}\nstdout={linked_modules.stdout}\nstderr={linked_modules.stderr}"
            )
        linked_module_artifact_o1 = root / "module-linked-o1.cdbc"
        linked_modules_o1 = link_command(
            manifest,
            module_products_o1,
            linked_module_artifact_o1,
        )
        if linked_modules_o1.returncode != 0 or linked_modules_o1.stdout or linked_modules_o1.stderr:
            return fail(
                "optimized debugger module fixture link failed\n"
                f"exit={linked_modules_o1.returncode}\n"
                f"stdout={linked_modules_o1.stdout}\n"
                f"stderr={linked_modules_o1.stderr}"
            )
        imported = debug_command(
            manifest,
            linked_module_artifact,
            [f"break {module_library.resolve()}:1", "continue", "continue", "continue"],
        )
        if imported.returncode != 0 or imported.stderr:
            return fail(
                "interactive imported-module debugger failed\n"
                f"exit={imported.returncode}\nstdout={imported.stdout}\nstderr={imported.stderr}"
            )
        if "pause reason=breakpoint function=" not in imported.stdout or "lib.cd:1:" not in imported.stdout:
            return fail(f"interactive debugger did not stop in imported source:\n{imported.stdout}")
        if "3\n" not in imported.stdout:
            return fail(f"interactive imported-module run lost program output:\n{imported.stdout}")

        imported_o1 = debug_command(
            manifest,
            linked_module_artifact_o1,
            [f"break {module_library.resolve()}:1", "continue", "continue", "continue"],
        )
        if imported_o1.returncode != 0 or imported_o1.stderr:
            return fail(
                "optimized interactive imported-module debugger failed\n"
                f"exit={imported_o1.returncode}\nstdout={imported_o1.stdout}\n"
                f"stderr={imported_o1.stderr}"
            )
        if (
            "pause reason=breakpoint function=" not in imported_o1.stdout
            or "lib.cd:1:" not in imported_o1.stdout
            or "location=<unknown>" in imported_o1.stdout
            or "3\n" not in imported_o1.stdout
        ):
            return fail(
                "optimized interactive debugger did not preserve imported source mapping:\n"
                f"{imported_o1.stdout}"
            )
        module_trace = trace_command(manifest, linked_module_artifact)
        module_o1_trace = trace_command(manifest, linked_module_artifact_o1)
        if module_trace.returncode != 0 or module_trace.stderr:
            return fail(
                "imported-module trace failed\n"
                f"exit={module_trace.returncode}\nstdout={module_trace.stdout}\n"
                f"stderr={module_trace.stderr}"
            )
        if module_o1_trace.returncode != 0 or module_o1_trace.stderr:
            return fail(
                "optimized imported-module trace failed\n"
                f"exit={module_o1_trace.returncode}\nstdout={module_o1_trace.stdout}\n"
                f"stderr={module_o1_trace.stderr}"
            )
        trace_difference = compare_trace_pair(
            module_trace,
            module_o1_trace,
            "imported-module trace",
        )
        if trace_difference is not None:
            return fail(trace_difference)

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

        closure_o1_artifact = root / "closure-o1.cdbc"
        emitted = compile_artifact(compiler, closure_source, closure_o1_artifact, "1")
        if emitted is not None:
            return emitted
        closure_trace = trace_command(manifest, closure_artifact)
        closure_o1_trace = trace_command(manifest, closure_o1_artifact)
        if closure_trace.returncode != 0 or closure_trace.stderr:
            return fail(
                "closure trace failed\n"
                f"exit={closure_trace.returncode}\nstdout={closure_trace.stdout}\n"
                f"stderr={closure_trace.stderr}"
            )
        if closure_o1_trace.returncode != 0 or closure_o1_trace.stderr:
            return fail(
                "optimized closure trace failed\n"
                f"exit={closure_o1_trace.returncode}\nstdout={closure_o1_trace.stdout}\n"
                f"stderr={closure_o1_trace.stderr}"
            )
        trace_difference = compare_trace_pair(
            closure_trace,
            closure_o1_trace,
            "closure trace",
        )
        if trace_difference is not None:
            return fail(trace_difference)
        closure_o1_debug = debug_command(
            manifest,
            closure_o1_artifact,
            [f"break {closure_source.resolve()}:2", "continue", "continue"],
        )
        if closure_o1_debug.returncode != 0 or closure_o1_debug.stderr:
            return fail(
                "optimized closure debugger failed\n"
                f"exit={closure_o1_debug.returncode}\nstdout={closure_o1_debug.stdout}\n"
                f"stderr={closure_o1_debug.stderr}"
            )
        if (
            'base="4"' not in closure_o1_debug.stdout
            or 'value="3"' not in closure_o1_debug.stdout
            or "location=<unknown>" in closure_o1_debug.stdout
        ):
            return fail(
                "optimized debugger lost closure locals or source mapping:\n"
                f"{closure_o1_debug.stdout}"
            )

        golden_root = Path(__file__).resolve().parent / "golden"
        for label, relative_source in (
            ("branch/loop trace", "loop_control_nested/input.cd"),
            ("eliminated-value trace", "logical_ir/input.cd"),
        ):
            control_source = golden_root / relative_source
            control_o0_artifact = root / (label.replace("/", "-") + "-o0.cdbc")
            control_o1_artifact = root / (label.replace("/", "-") + "-o1.cdbc")
            emitted = compile_artifact(compiler, control_source, control_o0_artifact)
            if emitted is not None:
                return emitted
            emitted = compile_artifact(compiler, control_source, control_o1_artifact, "1")
            if emitted is not None:
                return emitted
            control_o0_trace = trace_command(manifest, control_o0_artifact)
            control_o1_trace = trace_command(manifest, control_o1_artifact)
            if control_o0_trace.returncode != 0 or control_o0_trace.stderr:
                return fail(
                    f"{label} O0 trace failed\n"
                    f"exit={control_o0_trace.returncode}\n"
                    f"stdout={control_o0_trace.stdout}\n"
                    f"stderr={control_o0_trace.stderr}"
                )
            if control_o1_trace.returncode != 0 or control_o1_trace.stderr:
                return fail(
                    f"{label} O1 trace failed\n"
                    f"exit={control_o1_trace.returncode}\n"
                    f"stdout={control_o1_trace.stdout}\n"
                    f"stderr={control_o1_trace.stderr}"
                )
            trace_difference = compare_trace_pair(
                control_o0_trace,
                control_o1_trace,
                label,
            )
            if trace_difference is not None:
                return fail(trace_difference)

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

        optimized_failed_debug = debug_command(
            manifest,
            optimized_failing_artifact,
            ["continue", "continue", "continue"],
        )
        if (
            optimized_failed_debug.returncode == 0
            or "pause reason=entry" not in optimized_failed_debug.stdout
            or "pause reason=error function=main" not in optimized_failed_debug.stdout
            or "location=<unknown>" in optimized_failed_debug.stdout
        ):
            return fail(
                "optimized interactive debugger did not preserve runtime failure control\n"
                f"exit={optimized_failed_debug.returncode}\n"
                f"stdout={optimized_failed_debug.stdout}\n"
                f"stderr={optimized_failed_debug.stderr}"
            )
        if "Runtime error" not in optimized_failed_debug.stderr or "Call stack:" not in optimized_failed_debug.stderr:
            return fail(
                "optimized interactive debugger lost runtime failure diagnostics:\n"
                f"{optimized_failed_debug.stderr}"
            )

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
