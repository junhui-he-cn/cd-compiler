#!/usr/bin/env python3
import json
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class CheckResult:
    name: str
    passed: bool
    message: str = ""


IMPORT_GRAPH_DIRECTIVE = re.compile(
    r"(?m)^\s*(?:import\b|export\b[^\n;]*\bfrom\b)"
)
EXPECTED_IMPORT_GRAPH_ENTRIES = 44
EXPECTED_IMPORT_DIAGNOSTIC_ENTRIES = 31


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


def emit_without_cache(compiler: Path, entry: Path, output: Path) -> None:
    result = run(
        [
            str(compiler),
            "--emit-module-bytecode",
            str(output),
            str(entry),
        ]
    )
    if result.returncode != 0 or result.stdout or result.stderr:
        raise AssertionError(
            "module artifact baseline emission failed\n"
            f"exit={result.returncode}\nstdout={result.stdout}\nstderr={result.stderr}"
        )


def module_products(output: Path) -> dict[str, bytes]:
    return {
        path.name: path.read_bytes()
        for path in sorted(output.glob("module-*.cdbc"))
    }


def has_import_graph_directive(source: Path) -> bool:
    return bool(IMPORT_GRAPH_DIRECTIVE.search(source.read_text(encoding="utf-8")))


def discover_import_graph_entries() -> tuple[list[Path], list[Path]]:
    golden = Path(__file__).resolve().parent / "golden"
    successful = [
        path
        for path in sorted(golden.glob("*/input.cd"))
        if has_import_graph_directive(path)
    ]
    successful.extend(
        path
        for path in sorted((golden / "runtime_errors").glob("*.cd"))
        if has_import_graph_directive(path)
    )

    diagnostic = []
    for category in ("parse_errors", "type_errors", "import_errors"):
        diagnostic.extend(
            path
            for path in sorted((golden / category).glob("*.cd"))
            if has_import_graph_directive(path)
        )
    return successful, sorted(diagnostic)


def process_signature(result: subprocess.CompletedProcess[str]) -> tuple[int, str, str]:
    return result.returncode, result.stdout, result.stderr


def assert_process_parity(
    baseline: subprocess.CompletedProcess[str],
    candidate: subprocess.CompletedProcess[str],
    label: str,
) -> None:
    if process_signature(baseline) != process_signature(candidate):
        raise AssertionError(
            f"{label} changed compiler output\n"
            f"baseline exit={baseline.returncode}\nstdout={baseline.stdout}\nstderr={baseline.stderr}\n"
            f"candidate exit={candidate.returncode}\nstdout={candidate.stdout}\nstderr={candidate.stderr}"
        )


def run_successful_import_inventory_case(compiler: Path, entry: Path, root: Path) -> None:
    baseline = frontend_output(compiler, entry)
    if baseline.returncode != 0 or baseline.stderr:
        raise AssertionError(
            f"successful import fixture did not type-check: {entry}\n"
            f"exit={baseline.returncode}\nstdout={baseline.stdout}\nstderr={baseline.stderr}"
        )

    fixture_root = root / entry.parent.name
    baseline_output = fixture_root / "baseline-products"
    cached_output = fixture_root / "cached-products"
    cache = fixture_root / "cache"
    first_report_path = fixture_root / "first-report.json"
    second_report_path = fixture_root / "second-report.json"
    emit_without_cache(compiler, entry, baseline_output)
    emit(compiler, entry, cached_output, cache, first_report_path)

    baseline_products = module_products(baseline_output)
    first_products = module_products(cached_output)
    if not baseline_products or baseline_products != first_products:
        raise AssertionError(
            f"cold cache changed module products for {entry}\n"
            f"baseline={sorted(baseline_products)}\nfirst={sorted(first_products)}"
        )

    emit(compiler, entry, cached_output, cache, second_report_path)
    second = report(second_report_path)
    module_count = len(first_products)
    if second.get("cache_status") != "loaded" or second.get("summary") != {
        "module_count": module_count,
        "reused": module_count,
        "rebuilt": 0,
    }:
        raise AssertionError(f"valid sidecar reuse was incomplete for {entry}: {second}")
    if module_products(cached_output) != first_products:
        raise AssertionError(f"valid sidecar reuse changed products for {entry}")

    sidecars = sorted((cache / "interfaces").glob("*.cdi"))
    if not sidecars:
        raise AssertionError(f"cache produced no interfaces for imported fixture {entry}")
    for sidecar in sidecars:
        sidecar.write_text("cdi 9.9\n", encoding="utf-8")

    missing_cache = fixture_root / "missing-cache"
    missing_cache.mkdir(parents=True)
    missing = frontend_output(compiler, entry, cache=missing_cache, fallback=True)
    assert_process_parity(baseline, missing, f"missing-sidecar fallback for {entry}")

    malformed = frontend_output(compiler, entry, cache=cache, fallback=True)
    assert_process_parity(baseline, malformed, f"malformed-sidecar fallback for {entry}")
    repeated = frontend_output(compiler, entry, cache=cache, fallback=True)
    assert_process_parity(malformed, repeated, f"repeated malformed-sidecar fallback for {entry}")

    strict = frontend_output(compiler, entry, cache=cache, strict=True)
    if (
        strict.returncode != 1
        or strict.stdout
        or "Import error:" not in strict.stderr
        or "malformed sidecar" not in strict.stderr
    ):
        raise AssertionError(
            f"strict malformed-sidecar rejection was unstable for {entry}\n"
            f"exit={strict.returncode}\nstdout={strict.stdout}\nstderr={strict.stderr}"
        )


def run_diagnostic_import_inventory_case(compiler: Path, entry: Path, root: Path) -> None:
    baseline = frontend_output(compiler, entry)
    if baseline.returncode == 0 or not baseline.stderr:
        raise AssertionError(
            f"diagnostic import fixture unexpectedly succeeded: {entry}\n"
            f"exit={baseline.returncode}\nstdout={baseline.stdout}\nstderr={baseline.stderr}"
        )
    missing_cache = root / entry.parent.name / entry.stem
    missing_cache.mkdir(parents=True)
    fallback = frontend_output(compiler, entry, cache=missing_cache, fallback=True)
    assert_process_parity(baseline, fallback, f"diagnostic fallback for {entry}")


def run_complete_import_inventory(compiler: Path) -> None:
    successful, diagnostic = discover_import_graph_entries()
    if len(successful) != EXPECTED_IMPORT_GRAPH_ENTRIES:
        raise AssertionError(
            "import graph inventory count changed: "
            f"expected {EXPECTED_IMPORT_GRAPH_ENTRIES}, found {len(successful)}"
        )
    if len(diagnostic) != EXPECTED_IMPORT_DIAGNOSTIC_ENTRIES:
        raise AssertionError(
            "import diagnostic inventory count changed: "
            f"expected {EXPECTED_IMPORT_DIAGNOSTIC_ENTRIES}, found {len(diagnostic)}"
        )

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        for entry in successful:
            run_successful_import_inventory_case(compiler, entry, root / "successful")
        for entry in diagnostic:
            run_diagnostic_import_inventory_case(compiler, entry, root / "diagnostic")


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


def frontend_output(
    compiler: Path,
    entry: Path,
    cache: Path | None = None,
    import_paths: list[Path] | None = None,
    strict: bool = False,
    fallback: bool = False,
) -> subprocess.CompletedProcess[str]:
    command = [str(compiler)]
    if cache is not None:
        command.extend(["--module-interface-cache", str(cache)])
    if strict:
        command.append("--module-cache-strict")
    if fallback:
        command.append("--module-cache-fallback")
    for import_path in import_paths or []:
        command.extend(["--import-path", str(import_path)])
    command.append(str(entry))
    return run(command)


def assert_frontend_parity(
    compiler: Path,
    entry: Path,
    cache: Path,
    import_paths: list[Path] | None = None,
    fallback: bool = False,
) -> subprocess.CompletedProcess[str]:
    baseline = frontend_output(compiler, entry, import_paths=import_paths)
    cached = frontend_output(
        compiler,
        entry,
        cache=cache,
        import_paths=import_paths,
        fallback=fallback,
    )
    if (baseline.returncode, baseline.stdout, baseline.stderr) != (
        cached.returncode,
        cached.stdout,
        cached.stderr,
    ):
        raise AssertionError(
            "cache fallback changed frontend output\n"
            f"baseline exit={baseline.returncode}\nstdout={baseline.stdout}\nstderr={baseline.stderr}\n"
            f"cached exit={cached.returncode}\nstdout={cached.stdout}\nstderr={cached.stderr}"
        )
    return baseline


def assert_interface_parity(
    compiler: Path,
    entry: Path,
    cache: Path,
    import_paths: list[Path] | None = None,
) -> None:
    baseline = assert_frontend_parity(compiler, entry, cache, import_paths, fallback=True)
    if baseline.returncode != 0:
        raise AssertionError(
            "fallback parity fixture did not type-check\n"
            f"exit={baseline.returncode}\nstdout={baseline.stdout}\nstderr={baseline.stderr}"
        )


def populate_interface_cache(
    compiler: Path,
    entry: Path,
    output: Path,
    cache: Path,
    import_paths: list[Path] | None = None,
) -> None:
    command = [
        str(compiler),
        "--emit-module-bytecode",
        str(output),
        "--module-cache",
        str(cache),
    ]
    for import_path in import_paths or []:
        command.extend(["--import-path", str(import_path)])
    command.append(str(entry))
    emitted = run(command)
    if emitted.returncode != 0 or emitted.stdout or emitted.stderr:
        raise AssertionError(
            "fallback parity cache population failed\n"
            f"exit={emitted.returncode}\nstdout={emitted.stdout}\nstderr={emitted.stderr}"
        )


def assert_invalid_sidecar_parity(
    compiler: Path,
    root: Path,
    entry: Path,
    import_paths: list[Path] | None = None,
) -> None:
    missing_cache = root / "missing-cache"
    missing_cache.mkdir()
    assert_interface_parity(compiler, entry, missing_cache, import_paths)

    populated_cache = root / "invalid-cache"
    populate_interface_cache(
        compiler,
        entry,
        root / "module-products",
        populated_cache,
        import_paths,
    )
    sidecars = sorted((populated_cache / "interfaces").glob("*.cdi"))
    if not sidecars:
        raise AssertionError("fallback parity cache population produced no sidecars")
    for sidecar in sidecars:
        sidecar.write_text("cdi 9.9\n", encoding="utf-8")

    assert_interface_parity(compiler, entry, populated_cache, import_paths)
    first = frontend_output(
        compiler,
        entry,
        cache=populated_cache,
        import_paths=import_paths,
        fallback=True,
    )
    second = frontend_output(
        compiler,
        entry,
        cache=populated_cache,
        import_paths=import_paths,
        fallback=True,
    )
    if (first.returncode, first.stdout, first.stderr) != (
        second.returncode,
        second.stdout,
        second.stderr,
    ):
        raise AssertionError("repeated malformed-sidecar fallback was not deterministic")


def assert_strict_rejection(
    compiler: Path,
    entry: Path,
    cache: Path,
    reason: str,
    strict: bool = True,
) -> None:
    result = frontend_output(compiler, entry, cache=cache, strict=strict)
    if result.returncode != 1 or result.stdout or "Import error:" not in result.stderr:
        raise AssertionError(
            "strict cache rejection did not produce an Import diagnostic\n"
            f"expected_reason={reason}\nexit={result.returncode}\n"
            f"stdout={result.stdout}\nstderr={result.stderr}"
        )
    if reason not in result.stderr:
        raise AssertionError(
            "strict cache rejection omitted its stable reason\n"
            f"expected_reason={reason}\nstderr={result.stderr}"
        )


def sidecar_for_identity(cache: Path, identity: Path) -> Path:
    expected = str(identity.resolve())
    for sidecar in sorted((cache / "interfaces").glob("*.cdi")):
        if f'identity = "{expected}"' in sidecar.read_text(encoding="utf-8").splitlines():
            return sidecar
    raise AssertionError(f"sidecar not found for {identity}")


def module_cache_hash(value: str) -> str:
    digest = 14695981039346656037
    for byte in value.encode("utf-8"):
        digest ^= byte
        digest = (digest * 1099511628211) & ((1 << 64) - 1)
    return f"fnv1a64-{digest:016x}"


def populate_strict_case(
    compiler: Path,
    root: Path,
    library_source: str = "let value = 7;\nexport value;\n",
) -> tuple[Path, Path, Path]:
    root.mkdir(parents=True, exist_ok=True)
    library = root / "lib.cd"
    entry = root / "entry.cd"
    output = root / "module-products"
    cache = root / "cache"
    library.write_text(library_source, encoding="utf-8")
    entry.write_text('import "./lib.cd";\nprint value;\n', encoding="utf-8")
    populate_interface_cache(compiler, entry, output, cache)
    return library, entry, cache


def run_strict_cache_matrix(compiler: Path) -> None:
    usage = run([str(compiler), "--module-cache-strict"])
    if usage.returncode != 64 or usage.stdout or "requires --module-cache" not in usage.stderr:
        raise AssertionError(
            "strict cache option without a cache path was accepted\n"
            f"exit={usage.returncode}\nstdout={usage.stdout}\nstderr={usage.stderr}"
        )

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)

        valid = root / "valid"
        library, entry, cache = populate_strict_case(compiler, valid)
        strict_hit = frontend_output(compiler, entry, cache=cache, strict=True)
        if strict_hit.returncode != 0 or strict_hit.stderr:
            raise AssertionError(
                "valid strict sidecar was rejected\n"
                f"exit={strict_hit.returncode}\nstdout={strict_hit.stdout}\nstderr={strict_hit.stderr}"
            )

        missing = root / "missing"
        missing_library = missing / "lib.cd"
        missing_entry = missing / "entry.cd"
        missing_library.parent.mkdir(parents=True)
        missing_library.write_text("let value = 7;\nexport value;\n", encoding="utf-8")
        missing_entry.write_text('import "./lib.cd";\nprint value;\n', encoding="utf-8")
        (missing / "cache").mkdir()
        assert_strict_rejection(compiler, missing_entry, missing / "cache", "missing sidecar")

        malformed = root / "malformed"
        malformed_library, malformed_entry, malformed_cache = populate_strict_case(compiler, malformed)
        sidecar_for_identity(malformed_cache, malformed_library).write_text(
            "cdi 9.9\n", encoding="utf-8"
        )
        assert_strict_rejection(compiler, malformed_entry, malformed_cache, "malformed sidecar")

        changed = root / "changed"
        changed_library, changed_entry, changed_cache = populate_strict_case(compiler, changed)
        changed_library.write_text("let value = 8;\nexport value;\n", encoding="utf-8")
        assert_strict_rejection(compiler, changed_entry, changed_cache, "source hash mismatch")

        missing_product = root / "missing-product"
        product_library, product_entry, product_cache = populate_strict_case(compiler, missing_product)
        products = sorted((product_cache / "products").glob("*.cdbc"))
        if not products:
            raise AssertionError("strict missing-product case has no cached products")
        for product in products:
            product.unlink()
        assert_strict_rejection(
            compiler,
            product_entry,
            product_cache,
            "missing paired product",
        )

        identity_mismatch = root / "identity-mismatch"
        identity_library, identity_entry, identity_cache = populate_strict_case(compiler, identity_mismatch)
        identity_sidecar = sidecar_for_identity(identity_cache, identity_library)
        identity = str(identity_library.resolve())
        wrong_identity = str((identity_mismatch / "other.cd").resolve())
        identity_text = identity_sidecar.read_text(encoding="utf-8")
        identity_text = identity_text.replace(
            f'identity = "{identity}"', f'identity = "{wrong_identity}"', 1
        )
        identity_text = identity_text.replace(
            f'canonical_path = "{identity}"',
            f'canonical_path = "{wrong_identity}"',
            1,
        )
        identity_sidecar.write_text(identity_text, encoding="utf-8")
        assert_strict_rejection(
            compiler,
            identity_entry,
            identity_cache,
            "identity/canonical path mismatch",
        )

        dependency = root / "dependency"
        dependency.mkdir(parents=True)
        base = dependency / "base.cd"
        middle = dependency / "mid.cd"
        dependency_entry = dependency / "entry.cd"
        base.write_text("let baseValue = 3;\nexport baseValue;\n", encoding="utf-8")
        middle.write_text(
            'import "./base.cd";\nlet value = 7;\nexport value;\n',
            encoding="utf-8",
        )
        dependency_entry.write_text('import "./mid.cd";\nprint value;\n', encoding="utf-8")
        dependency_cache = dependency / "cache"
        populate_interface_cache(compiler, dependency_entry, dependency / "module-products", dependency_cache)
        middle_sidecar = sidecar_for_identity(dependency_cache, middle)
        original_middle_lines = middle_sidecar.read_text(encoding="utf-8").splitlines()
        middle_lines = list(original_middle_lines)

        def quoted_field(prefix: str) -> str:
            line = next(line for line in original_middle_lines if line.startswith(prefix))
            return json.loads(line.split(" = ", 1)[1])

        middle_identity = quoted_field("identity = ")
        middle_source_hash = quoted_field("source = ")
        middle_interface_hash = quoted_field("interface = ")
        base_identity = str(base.resolve())
        dependency_hash = ""
        for index, line in enumerate(middle_lines):
            if line.startswith("  interface = "):
                dependency_hash = json.loads(line.split(" = ", 1)[1])
                middle_lines[index] = line[:-1] + "-stale\""
                break
        else:
            raise AssertionError("strict dependency-hash case has no dependency interface field")
        key_input = (
            "module-cache-key-v1"
            + f"artifact:{len('cdbc 0.1')}:cdbc 0.1"
            + f"identity:{len(middle_identity)}:{middle_identity}"
            + f"source:{len(middle_source_hash)}:{middle_source_hash}"
            + f"interface:{len(middle_interface_hash)}:{middle_interface_hash}"
            + "entry:5:false"
            + "entry_order:4:none"
            + "dependencies:1"
            + f"identity:{len(base_identity)}:{base_identity}"
            + "kind:6:import"
            + "requested:9:./base.cd"
            + f"interface:{len(dependency_hash + '-stale')}:{dependency_hash}-stale"
        )
        replacement_product = dependency_cache / "products" / (
            "product-" + module_cache_hash(key_input) + ".cdbc"
        )
        existing_product = next((dependency_cache / "products").glob("*.cdbc"))
        replacement_product.write_bytes(existing_product.read_bytes())
        middle_sidecar.write_text("\n".join(middle_lines) + "\n", encoding="utf-8")
        assert_strict_rejection(
            compiler,
            dependency_entry,
            dependency_cache,
            "dependency interface hash mismatch",
        )


def run_cache_policy_matrix(compiler: Path) -> None:
    fallback_usage = run([str(compiler), "--module-cache-fallback"])
    if (
        fallback_usage.returncode != 64
        or fallback_usage.stdout
        or "requires --module-interface-cache" not in fallback_usage.stderr
    ):
        raise AssertionError(
            "fallback cache option without an interface-cache path was accepted\n"
            f"exit={fallback_usage.returncode}\nstdout={fallback_usage.stdout}\n"
            f"stderr={fallback_usage.stderr}"
        )

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        valid = root / "valid"
        library, entry, cache = populate_strict_case(compiler, valid)

        default_hit = frontend_output(compiler, entry, cache=cache)
        if default_hit.returncode != 0 or default_hit.stderr:
            raise AssertionError(
                "default interface-only cache policy rejected a valid sidecar\n"
                f"exit={default_hit.returncode}\nstdout={default_hit.stdout}\n"
                f"stderr={default_hit.stderr}"
            )

        linked_with_interface_cache = run(
            [
                str(compiler),
                "--emit-bytecode",
                str(root / "linked.cdbc"),
                "--module-interface-cache",
                str(cache),
                str(entry),
            ]
        )
        if (
            linked_with_interface_cache.returncode != 64
            or linked_with_interface_cache.stdout
            or "cannot provide bytecode bodies" not in linked_with_interface_cache.stderr
        ):
            raise AssertionError(
                "linked bytecode accepted an interface-only cache\n"
                f"exit={linked_with_interface_cache.returncode}\n"
                f"stdout={linked_with_interface_cache.stdout}\n"
                f"stderr={linked_with_interface_cache.stderr}"
            )

        module_without_product_cache = run(
            [
                str(compiler),
                "--emit-module-bytecode",
                str(root / "module-products-without-cache"),
                "--module-interface-cache",
                str(cache),
                str(entry),
            ]
        )
        if (
            module_without_product_cache.returncode != 64
            or module_without_product_cache.stdout
            or "cannot provide bytecode bodies" not in module_without_product_cache.stderr
        ):
            raise AssertionError(
                "module emission accepted an interface-only cache without a product cache\n"
                f"exit={module_without_product_cache.returncode}\n"
                f"stdout={module_without_product_cache.stdout}\n"
                f"stderr={module_without_product_cache.stderr}"
            )

        baseline = frontend_output(compiler, entry)
        missing_cache = root / "missing-cache"
        missing_cache.mkdir()
        assert_strict_rejection(
            compiler,
            entry,
            missing_cache,
            "missing sidecar",
            strict=False,
        )
        fallback_hit = frontend_output(compiler, entry, cache=missing_cache, fallback=True)
        if (baseline.returncode, baseline.stdout, baseline.stderr) != (
            fallback_hit.returncode,
            fallback_hit.stdout,
            fallback_hit.stderr,
        ):
            raise AssertionError(
                "explicit interface-cache fallback changed source-backed output\n"
                f"baseline exit={baseline.returncode}\nstdout={baseline.stdout}\nstderr={baseline.stderr}\n"
                f"fallback exit={fallback_hit.returncode}\nstdout={fallback_hit.stdout}\n"
                f"stderr={fallback_hit.stderr}"
            )

        sidecar_for_identity(cache, library).write_text("cdi 9.9\n", encoding="utf-8")
        assert_strict_rejection(
            compiler,
            entry,
            cache,
            "malformed sidecar",
            strict=False,
        )
        malformed_fallback = frontend_output(compiler, entry, cache=cache, fallback=True)
        if (baseline.returncode, baseline.stdout, baseline.stderr) != (
            malformed_fallback.returncode,
            malformed_fallback.stdout,
            malformed_fallback.stderr,
        ):
            raise AssertionError("explicit malformed-sidecar fallback changed source output")

        conflict = run(
            [
                str(compiler),
                "--module-interface-cache",
                str(cache),
                "--module-cache-strict",
                "--module-cache-fallback",
                str(entry),
            ]
        )
        if (
            conflict.returncode != 64
            or conflict.stdout
            or "mutually exclusive" not in conflict.stderr
        ):
            raise AssertionError(
                "strict and fallback cache options were accepted together\n"
                f"exit={conflict.returncode}\nstdout={conflict.stdout}\nstderr={conflict.stderr}"
            )

        scope = run(
            [
                str(compiler),
                "--emit-module-bytecode",
                str(root / "scope-products"),
                "--module-interface-cache",
                str(cache),
                "--module-cache-fallback",
                str(entry),
            ]
        )
        if (
            scope.returncode != 64
            or scope.stdout
            or "only valid for interface-only cache consumers" not in scope.stderr
        ):
            raise AssertionError(
                "fallback cache option escaped the interface-only scope\n"
                f"exit={scope.returncode}\nstdout={scope.stdout}\nstderr={scope.stderr}"
            )

        repair_root = root / "module-product-repair"
        repair_library, repair_entry, repair_cache = populate_strict_case(compiler, repair_root)
        repair_library.write_text("let value = 8;\nexport value;\n", encoding="utf-8")
        repair_report = repair_root / "repair-report.json"
        emit(
            compiler,
            repair_entry,
            repair_root / "repaired-products",
            repair_cache,
            repair_report,
        )
        repair_statuses = module_statuses(report(repair_report))
        if repair_statuses["lib.cd"][:2] != ("rebuilt", "source_changed"):
            raise AssertionError(
                "module-product repair did not retain source fallback by default: "
                f"{repair_statuses}"
            )


def run_rebuild_matrix(compiler: Path, vm: Path) -> None:
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


def run_product_source_fallback_boundary(compiler: Path, vm: Path) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        lib = root / "lib.cd"
        mid = root / "mid.cd"
        entry = root / "entry.cd"
        lib.write_text('print("lib");\n', encoding="utf-8")
        mid.write_text('import "./lib.cd";\nprint("mid");\n', encoding="utf-8")
        entry.write_text('import "./mid.cd";\nprint("entry");\n', encoding="utf-8")

        cache = root / "cache"
        first_output = root / "first-products"
        first_report = root / "first-report.json"
        emit(compiler, entry, first_output, cache, first_report)

        manifest = cache / "module-cache.cdbc"
        if not manifest.is_file():
            raise AssertionError("cold module-product build did not write its cache manifest")
        manifest.unlink()

        missing_manifest_output = root / "missing-manifest-products"
        missing_manifest_report = root / "missing-manifest-report.json"
        emit(compiler, entry, missing_manifest_output, cache, missing_manifest_report)
        missing_manifest = report(missing_manifest_report)
        if missing_manifest["cache_status"] != "missing" or missing_manifest["summary"] != {
            "module_count": 3,
            "reused": 0,
            "rebuilt": 3,
        }:
            raise AssertionError(f"missing-manifest repair did not rebuild from source: {missing_manifest}")
        if not any('string "lib"' in text.decode("utf-8") for text in module_products(missing_manifest_output).values()):
            raise AssertionError("missing-manifest repair lowered an empty preloaded dependency")
        link_and_run(
            vm,
            missing_manifest_output,
            root / "missing-manifest-linked.cdbc",
            "lib\nmid\nentry\n",
        )

        manifest.write_text("cdbc-cache 9.9\n", encoding="utf-8")
        invalid_manifest_output = root / "invalid-manifest-products"
        invalid_manifest_report = root / "invalid-manifest-report.json"
        emit(compiler, entry, invalid_manifest_output, cache, invalid_manifest_report)
        invalid_manifest = report(invalid_manifest_report)
        if invalid_manifest["cache_status"] != "invalid" or invalid_manifest["summary"] != {
            "module_count": 3,
            "reused": 0,
            "rebuilt": 3,
        }:
            raise AssertionError(f"invalid-manifest repair did not rebuild from source: {invalid_manifest}")
        if not any('string "lib"' in text.decode("utf-8") for text in module_products(invalid_manifest_output).values()):
            raise AssertionError("invalid-manifest repair lowered an empty preloaded dependency")
        link_and_run(
            vm,
            invalid_manifest_output,
            root / "invalid-manifest-linked.cdbc",
            "lib\nmid\nentry\n",
        )


def run_fallback_case(compiler: Path, case: str) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)

        if case == "direct":
            directory = root / "direct"
            directory.mkdir()
            (directory / "lib.cd").write_text(
                "let value = 7;\nexport value;\n",
                encoding="utf-8",
            )
            entry = directory / "entry.cd"
            entry.write_text(
                'import "./lib.cd";\nprint value;\n',
                encoding="utf-8",
            )
            assert_invalid_sidecar_parity(compiler, directory, entry)
            return

        if case == "namespace":
            directory = root / "namespace"
            directory.mkdir()
            (directory / "lib.cd").write_text(
                "let value = 7;\nexport value;\n",
                encoding="utf-8",
            )
            entry = directory / "entry.cd"
            entry.write_text(
                'import "./lib.cd" as ns;\nprint ns.value;\n',
                encoding="utf-8",
            )
            assert_invalid_sidecar_parity(compiler, directory, entry)
            return

        if case == "re-export":
            directory = root / "re-export"
            directory.mkdir()
            (directory / "lib.cd").write_text(
                "let value = 7;\nexport value;\n",
                encoding="utf-8",
            )
            (directory / "api.cd").write_text(
                'export value from "./lib.cd";\n',
                encoding="utf-8",
            )
            entry = directory / "entry.cd"
            entry.write_text(
                'import "./api.cd";\nprint value;\n',
                encoding="utf-8",
            )
            assert_invalid_sidecar_parity(compiler, directory, entry)
            return

        if case == "search-path":
            directory = root / "search-path"
            modules = directory / "modules"
            app = directory / "app"
            modules.mkdir(parents=True)
            app.mkdir(parents=True)
            (modules / "lib.cd").write_text(
                "let value = 7;\nexport value;\n",
                encoding="utf-8",
            )
            entry = app / "entry.cd"
            entry.write_text(
                'import "lib";\nprint value;\n',
                encoding="utf-8",
            )
            assert_invalid_sidecar_parity(compiler, directory, entry, [modules])
            return

        raise AssertionError(f"unknown module-cache fallback case: {case}")


def run_fallback_diagnostic_case(compiler: Path) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        library = root / "lib.cd"
        entry = root / "entry.cd"
        valid_source = "let value = 7;\nexport value;\n"
        library.write_text(valid_source, encoding="utf-8")
        entry.write_text('import "./lib.cd";\nprint value;\n', encoding="utf-8")
        cache = root / "cache"
        populate_interface_cache(compiler, entry, root / "module-products", cache)

        for invalid_source, diagnostic_kind in (
            ("let value = ;\nexport value;\n", "Parse error"),
            ("let value: number = \"bad\";\nexport value;\n", "Type error"),
        ):
            library.write_text(invalid_source, encoding="utf-8")
            baseline = assert_frontend_parity(compiler, entry, cache, fallback=True)
            if baseline.returncode != 1 or baseline.stdout:
                raise AssertionError(
                    "invalid-sidecar diagnostic fallback had unexpected process result\n"
                    f"kind={diagnostic_kind}\nexit={baseline.returncode}\n"
                    f"stdout={baseline.stdout}\nstderr={baseline.stderr}"
                )
            if diagnostic_kind not in baseline.stderr or str(library.resolve()) not in baseline.stderr:
                raise AssertionError(
                    "invalid-sidecar diagnostic fallback lost file-aware source diagnostics\n"
                    f"kind={diagnostic_kind}\nstderr={baseline.stderr}"
                )


def run_partial_dependency_fallback_case(compiler: Path) -> None:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        base = root / "base.cd"
        middle = root / "middle.cd"
        entry = root / "entry.cd"
        base.write_text("let baseValue = 3;\nexport baseValue;\n", encoding="utf-8")
        middle.write_text(
            'import "./base.cd";\nlet value = 7;\nexport value;\n',
            encoding="utf-8",
        )
        entry.write_text('import "./middle.cd";\nprint value;\n', encoding="utf-8")
        cache = root / "cache"
        populate_interface_cache(compiler, entry, root / "module-products", cache)
        sidecar_for_identity(cache, middle).write_text("cdi 9.9\n", encoding="utf-8")
        cached = frontend_output(compiler, entry, cache=cache, fallback=True)
        if cached.returncode != 0 or cached.stderr:
            raise AssertionError(
                "partial dependency fallback failed to load the importer\n"
                f"exit={cached.returncode}\nstdout={cached.stdout}\nstderr={cached.stderr}"
            )
        if "Let baseValue = 3" in cached.stdout:
            raise AssertionError("valid lower dependency sidecar was not reused")
        if "Import \"./base.cd\"" not in cached.stdout or "Let value = 7" not in cached.stdout:
            raise AssertionError("invalid middle sidecar did not fall back to its source body")
        repeated = frontend_output(compiler, entry, cache=cache, fallback=True)
        if (cached.returncode, cached.stdout, cached.stderr) != (
            repeated.returncode,
            repeated.stdout,
            repeated.stderr,
        ):
            raise AssertionError("partial dependency fallback was not deterministic")


def run_case(name: str, callback) -> CheckResult:
    try:
        callback()
    except (AssertionError, OSError, json.JSONDecodeError) as error:
        return CheckResult(name, False, f"FAIL {name}: {error}")
    return CheckResult(name, True)


def run_all(compiler: Path, vm: Path) -> list[CheckResult]:
    return [
        run_case(
            "module cache rebuild/reuse matrix",
            lambda: run_rebuild_matrix(compiler, vm),
        ),
        run_case(
            "module product source fallback boundary",
            lambda: run_product_source_fallback_boundary(compiler, vm),
        ),
        run_case(
            "module cache fallback/direct import",
            lambda: run_fallback_case(compiler, "direct"),
        ),
        run_case(
            "module cache fallback/namespace import",
            lambda: run_fallback_case(compiler, "namespace"),
        ),
        run_case(
            "module cache fallback/re-export chain",
            lambda: run_fallback_case(compiler, "re-export"),
        ),
        run_case(
            "module cache fallback/search path",
            lambda: run_fallback_case(compiler, "search-path"),
        ),
        run_case(
            "module cache fallback/diagnostics",
            lambda: run_fallback_diagnostic_case(compiler),
        ),
        run_case(
            "module cache fallback/partial dependency reuse",
            lambda: run_partial_dependency_fallback_case(compiler),
        ),
        run_case(
            "module cache strict rejection matrix",
            lambda: run_strict_cache_matrix(compiler),
        ),
        run_case(
            "module cache default strict/fallback policy",
            lambda: run_cache_policy_matrix(compiler),
        ),
        run_case(
            "module cache complete import inventory",
            lambda: run_complete_import_inventory(compiler),
        ),
    ]


def main() -> int:
    if len(sys.argv) != 3:
        return fail("usage: bytecode_module_cache_tests.py <compiler> <vm>")
    compiler = Path(sys.argv[1]).resolve()
    vm = Path(sys.argv[2]).resolve()
    if not compiler.is_file() or not (vm / "Cargo.toml").is_file():
        return fail("compiler or Rust VM manifest not found")

    results = run_all(compiler, vm)
    failed = [result for result in results if not result.passed]
    for failure in failed:
        print(failure.message, file=sys.stderr)
    passed_count = len(results) - len(failed)
    print(f"module bytecode cache tests: {passed_count} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
