#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def require_clean(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0 or result.stderr:
        raise AssertionError(
            f"{label} failed\n"
            f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
        )


def emit(compiler: Path, source: Path, artifact: Path, level: Optional[str] = None) -> None:
    command = [str(compiler), "--emit-bytecode", str(artifact)]
    if level is not None:
        command.extend(["--opt-level", level])
    command.append(str(source))
    require_clean(run(command), f"emit O{level or '0'}")


def rust_run(vm: Path, artifact: Path) -> str:
    result = run(
        [
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(vm / "Cargo.toml"),
            "--",
            "run",
            str(artifact),
        ]
    )
    require_clean(result, f"Rust VM run {artifact}")
    return result.stdout


def emit_modules(
    compiler: Path,
    source: Path,
    products: Path,
    cache: Path,
    report: Path,
    level: str,
) -> dict:
    result = run(
        [
            str(compiler),
            "--emit-module-bytecode",
            str(products),
            "--module-cache",
            str(cache),
            "--module-rebuild-report",
            str(report),
            "--opt-level",
            level,
            str(source),
        ]
    )
    require_clean(result, f"module emission O{level}")
    return json.loads(report.read_text(encoding="utf-8"))


def link_and_run(vm: Path, products: Path, output: Path) -> str:
    linked = run(
        [
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(vm / "Cargo.toml"),
            "--",
            "link",
            str(products),
            str(output),
        ]
    )
    require_clean(linked, f"link {products}")
    return rust_run(vm, output)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: optimizer_cli_tests.py compiler vm-rs", file=sys.stderr)
        return 2

    compiler = Path(sys.argv[1]).resolve()
    vm = Path(sys.argv[2]).resolve()
    root = Path(__file__).resolve().parent.parent
    logical = root / "tests" / "golden" / "logical_ir" / "input.cd"
    match = root / "tests" / "golden" / "match_expression" / "input.cd"
    imported = root / "tests" / "golden" / "import_struct_operator_direct" / "input.cd"

    baseline = run([str(compiler), "--ir", str(logical)])
    require_clean(baseline, "baseline IR")
    o0 = run([str(compiler), "--ir", "--opt-level", "0", str(logical)])
    require_clean(o0, "explicit O0 IR")
    if o0.stdout != baseline.stdout:
        raise AssertionError("explicit O0 changed the established IR output")

    optimized = run([str(compiler), "--ir", "--opt-level", "1", str(logical)])
    require_clean(optimized, "O1 IR")
    if optimized.stdout == baseline.stdout:
        raise AssertionError("O1 did not change the copy-heavy logical IR case")

    invalid_level = run([str(compiler), "--ir", "--opt-level", "2", str(logical)])
    if (
        invalid_level.returncode != 64
        or invalid_level.stdout
        or "requires 0 or 1" not in invalid_level.stderr
    ):
        raise AssertionError(
            "invalid optimization level was accepted\n"
            f"exit={invalid_level.returncode}\nstdout={invalid_level.stdout}\n"
            f"stderr={invalid_level.stderr}"
        )

    missing_mode = run([str(compiler), "--opt-level", "1", str(logical)])
    if (
        missing_mode.returncode != 64
        or missing_mode.stdout
        or "requires --ir, --bytecode, or bytecode emission" not in missing_mode.stderr
    ):
        raise AssertionError("optimization level was accepted without an IR/bytecode mode")

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        baseline_artifact = root / "baseline.cdbc"
        optimized_artifact = root / "optimized.cdbc"
        emit(compiler, logical, baseline_artifact)
        emit(compiler, logical, optimized_artifact, "1")
        if baseline_artifact.read_bytes() == optimized_artifact.read_bytes():
            raise AssertionError("O1 did not change the logical bytecode artifact")
        if rust_run(vm, baseline_artifact) != "true\nfalse\n":
            raise AssertionError("baseline logical artifact changed runtime output")
        if rust_run(vm, optimized_artifact) != "true\nfalse\n":
            raise AssertionError("O1 logical artifact changed runtime output")

        constant_source = root / "constant-folding.cd"
        constant_source.write_text(
            'print 2 + 3;\nprint "a" + "b";\n'
            'if (true) { print 1; } else { print 2; }\n',
            encoding="utf-8",
        )
        constant_baseline = run([str(compiler), "--ir", str(constant_source)])
        require_clean(constant_baseline, "constant baseline IR")
        constant_optimized = run(
            [str(compiler), "--ir", "--opt-level", "1", str(constant_source)]
        )
        require_clean(constant_optimized, "constant O1 IR")
        if "add" in constant_optimized.stdout:
            raise AssertionError("O1 retained a proven-safe constant add")
        if "jump_if_false" in constant_optimized.stdout:
            raise AssertionError("O1 retained a known-condition conditional jump")
        if constant_optimized.stdout == constant_baseline.stdout:
            raise AssertionError("O1 did not fold the constant expression case")

        constant_baseline_artifact = root / "constant-baseline.cdbc"
        constant_optimized_artifact = root / "constant-optimized.cdbc"
        emit(compiler, constant_source, constant_baseline_artifact)
        emit(compiler, constant_source, constant_optimized_artifact, "1")
        if rust_run(vm, constant_baseline_artifact) != "5\nab\n1\n":
            raise AssertionError("baseline constant artifact changed runtime output")
        if rust_run(vm, constant_optimized_artifact) != "5\nab\n1\n":
            raise AssertionError("O1 constant artifact changed runtime output")

        merge_source = root / "linear-block-merge.cd"
        merge_source.write_text(
            "fun choose(): bool { return true; }\n"
            "fun flow(): number {\n"
            "  let condition = choose();\n"
            "  if (condition) {\n"
            "    print 1;\n"
            "  } else {\n"
            "    return 2;\n"
            "  }\n"
            "  print 3;\n"
            "  return 4;\n"
            "}\n"
            "print flow();\n",
            encoding="utf-8",
        )
        merge_baseline = run([str(compiler), "--ir", str(merge_source)])
        require_clean(merge_baseline, "linear block merge baseline IR")
        merge_optimized = run(
            [str(compiler), "--ir", "--opt-level", "1", str(merge_source)]
        )
        require_clean(merge_optimized, "linear block merge O1 IR")
        if "jump 0010" not in merge_baseline.stdout:
            raise AssertionError("baseline did not retain the merge candidate jump")
        if "jump 0010" in merge_optimized.stdout:
            raise AssertionError("O1 did not merge the non-empty linear block")

        merge_baseline_artifact = root / "linear-block-merge-baseline.cdbc"
        merge_optimized_artifact = root / "linear-block-merge-optimized.cdbc"
        emit(compiler, merge_source, merge_baseline_artifact)
        emit(compiler, merge_source, merge_optimized_artifact, "1")
        if merge_baseline_artifact.read_bytes() == merge_optimized_artifact.read_bytes():
            raise AssertionError("O1 did not change the linear block merge artifact")
        if rust_run(vm, merge_baseline_artifact) != "1\n3\n4\n":
            raise AssertionError("baseline linear block merge artifact changed output")
        if rust_run(vm, merge_optimized_artifact) != "1\n3\n4\n":
            raise AssertionError("O1 linear block merge artifact changed output")

        match_artifact = root / "match.cdbc"
        emit(compiler, match, match_artifact, "1")
        if rust_run(vm, match_artifact) != "ok:7\n3\nok\n":
            raise AssertionError("O1 exhaustive-match artifact changed runtime output")

        products = root / "products"
        cache = root / "cache"
        report = root / "report.json"
        first = emit_modules(compiler, imported, products, cache, report, "1")
        if first["summary"]["rebuilt"] != first["summary"]["module_count"]:
            raise AssertionError(f"O1 cold module build was incomplete: {first}")
        if any(
            module["optimization_level"] != "O1"
            or module["optimizer_pipeline"] != "m7-ssa-o1-copy-phi-const-branch-dce-reach-thread-merge-v7"
            for module in first["modules"]
        ):
            raise AssertionError(f"O1 cache identity was not recorded: {first}")

        second = emit_modules(compiler, imported, products, cache, report, "1")
        if second["summary"] != {
            "module_count": first["summary"]["module_count"],
            "reused": first["summary"]["module_count"],
            "rebuilt": 0,
        }:
            raise AssertionError(f"O1 cache hit was incomplete: {second}")
        if link_and_run(vm, products, root / "linked-o1.cdbc") != "true\ntrue\ntrue\ntrue\n":
            raise AssertionError("O1 linked module output changed")

        third = emit_modules(compiler, imported, products, cache, report, "0")
        if not any(
            module["reason"] == "optimization_configuration_changed"
            for module in third["modules"]
        ):
            raise AssertionError(f"optimization identity did not invalidate the cache: {third}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
