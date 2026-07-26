#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def report(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def module_statuses(value: dict) -> dict[str, tuple[str, str, bool]]:
    return {
        Path(item["identity"]).name: (
            item["status"],
            item["reason"],
            item["public_impact"],
        )
        for item in value["modules"]
    }


def emit(compiler: Path, entry: Path, output: Path, cache: Path, report_path: Path) -> None:
    result = run(
        [
            str(compiler),
            "--emit-module-bytecode",
            str(output),
            "--module-cache",
            str(cache),
            "--module-rebuild-report",
            str(report_path),
            str(entry),
        ]
    )
    if result.returncode != 0 or result.stdout or result.stderr:
        raise AssertionError(
            "module cache emission failed\n"
            f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
        )


def link_and_run(vm: Path, modules: Path, output: Path, expected: str) -> None:
    manifest = vm / "Cargo.toml"
    linked = run(
        [
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "link",
            str(modules),
            str(output),
        ]
    )
    if linked.returncode != 0 or linked.stdout or linked.stderr:
        raise AssertionError(
            "cached module link failed\n"
            f"exit={linked.returncode}\nstdout={linked.stdout}\nstderr={linked.stderr}"
        )
    executed = run(
        [
            "cargo",
            "run",
            "--quiet",
            "--manifest-path",
            str(manifest),
            "--",
            "run",
            str(output),
        ]
    )
    if executed.returncode != 0 or executed.stdout != expected or executed.stderr:
        raise AssertionError(
            "cached module execution mismatch\n"
            f"exit={executed.returncode}\nstdout={executed.stdout}\nstderr={executed.stderr}"
        )


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: bytecode_module_cache_tests.py <compiler> <vm>")
    compiler = Path(sys.argv[1]).resolve()
    vm = Path(sys.argv[2]).resolve()
    if not compiler.is_file() or not (vm / "Cargo.toml").is_file():
        return fail("compiler or Rust VM manifest not found")

    try:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lib = root / "lib.cd"
            mid = root / "mid.cd"
            entry = root / "entry.cd"
            lib.write_text('print("lib-v1");\n', encoding="utf-8")
            mid.write_text('import "./lib.cd";\nprint("mid");\n', encoding="utf-8")
            entry.write_text('import "./mid.cd";\nprint("entry");\n', encoding="utf-8")

            output = root / "modules"
            cache = root / "cache"
            first_report_path = root / "first.json"
            emit(compiler, entry, output, cache, first_report_path)
            first = report(first_report_path)
            if first["cache_status"] != "missing" or first["summary"] != {
                "module_count": 3,
                "reused": 0,
                "rebuilt": 3,
            }:
                raise AssertionError(f"unexpected first cache report: {first}")
            if any(item[1] != "cache_miss" for item in module_statuses(first).values()):
                raise AssertionError(f"first build did not report cache misses: {first}")
            link_and_run(vm, output, root / "first-linked.cdbc", "lib-v1\nmid\nentry\n")

            baseline_products = {
                path.name: path.read_bytes() for path in output.glob("module-*.cdbc")
            }
            second_report_path = root / "second.json"
            emit(compiler, entry, output, cache, second_report_path)
            second = report(second_report_path)
            if second["cache_status"] != "loaded" or second["summary"] != {
                "module_count": 3,
                "reused": 3,
                "rebuilt": 0,
            }:
                raise AssertionError(f"unexpected no-change cache report: {second}")
            if any(item[0] != "reused" for item in module_statuses(second).values()):
                raise AssertionError(f"no-change build did not reuse all modules: {second}")
            if baseline_products != {
                path.name: path.read_bytes() for path in output.glob("module-*.cdbc")
            }:
                raise AssertionError("no-change cache hit changed a module product")
            link_and_run(vm, output, root / "second-linked.cdbc", "lib-v1\nmid\nentry\n")

            lib.write_text('print("lib-v2");\n', encoding="utf-8")
            private_report_path = root / "private.json"
            emit(compiler, entry, output, cache, private_report_path)
            private = module_statuses(report(private_report_path))
            if private["lib.cd"][:2] != ("rebuilt", "source_changed") or private["lib.cd"][2]:
                raise AssertionError(f"private leaf change was classified incorrectly: {private}")
            if private["mid.cd"][0] != "reused" or private["entry.cd"][0] != "reused":
                raise AssertionError(f"private leaf change rebuilt dependents: {private}")
            link_and_run(vm, output, root / "private-linked.cdbc", "lib-v2\nmid\nentry\n")

            lib.write_text(
                'let exported: number = 1;\nexport exported;\nprint("lib-v3");\n',
                encoding="utf-8",
            )
            public_report_path = root / "public.json"
            emit(compiler, entry, output, cache, public_report_path)
            public = module_statuses(report(public_report_path))
            if public["lib.cd"][:2] != ("rebuilt", "source_and_public_interface_changed"):
                raise AssertionError(f"public leaf change was classified incorrectly: {public}")
            if public["mid.cd"][:2] != ("rebuilt", "dependency_interface_changed"):
                raise AssertionError(f"direct dependency invalidation was incorrect: {public}")
            if public["entry.cd"][:2] != (
                "rebuilt",
                "transitive_dependency_interface_changed",
            ):
                raise AssertionError(f"transitive dependency invalidation was incorrect: {public}")
            link_and_run(vm, output, root / "public-linked.cdbc", "lib-v3\nmid\nentry\n")
    except (AssertionError, OSError, json.JSONDecodeError) as error:
        return fail(str(error))

    print("module bytecode cache tests: reuse, private-change, and public-change propagation validated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
