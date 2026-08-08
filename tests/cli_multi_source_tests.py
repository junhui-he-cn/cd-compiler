#!/usr/bin/env python3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class CliMultiSourceTests(unittest.TestCase):
    def setUp(self) -> None:
        if len(sys.argv) != 3:
            self.fail("usage: cli_multi_source_tests.py <compiler> <vm-rs>")
        self.compiler = Path(sys.argv[1]).resolve()
        if not self.compiler.is_file():
            self.fail(f"compiler not found: {self.compiler}")
        self.vm = Path(sys.argv[2]).resolve()
        self.vm_manifest = self.vm / "Cargo.toml" if self.vm.is_dir() else self.vm
        if not self.vm_manifest.is_file():
            self.fail(f"vm manifest not found: {self.vm_manifest}")

    def run_compiler(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.compiler), *args],
            text=True,
            capture_output=True,
            check=False,
        )

    def emit_and_run_vm(self, root: Path, *sources: Path) -> subprocess.CompletedProcess[str]:
        artifact = root / "program.cdbc"
        emitted = self.run_compiler("--emit-bytecode", str(artifact), *(str(source) for source in sources))
        self.assertEqual(emitted.returncode, 0, emitted.stderr)
        self.assertEqual(emitted.stdout, "")
        self.assertEqual(emitted.stderr, "")
        self.assertTrue(artifact.is_file())
        return subprocess.run(
            ["cargo", "run", "--quiet", "--manifest-path", str(self.vm_manifest), "--", "run", str(artifact)],
            text=True,
            capture_output=True,
            check=False,
        )

    def emit_and_run_vm_with_compiler_args(
        self,
        root: Path,
        compiler_args: tuple[str, ...],
        *sources: Path,
    ) -> subprocess.CompletedProcess[str]:
        artifact = root / "program.cdbc"
        emitted = self.run_compiler(
            *compiler_args,
            "--emit-bytecode",
            str(artifact),
            *(str(source) for source in sources),
        )
        self.assertEqual(emitted.returncode, 0, emitted.stderr)
        self.assertEqual(emitted.stdout, "")
        self.assertEqual(emitted.stderr, "")
        self.assertTrue(artifact.is_file())
        return subprocess.run(
            ["cargo", "run", "--quiet", "--manifest-path", str(self.vm_manifest), "--", "run", str(artifact)],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_vm_execution_accepts_multiple_input_files_in_order(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            lib = root / "lib.cd"
            main = root / "main.cd"
            lib.write_text("fun add(a, b) { return a + b; }\n", encoding="utf-8")
            main.write_text("print add(1, 2);\n", encoding="utf-8")

            completed = self.emit_and_run_vm(root, lib, main)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stderr, "")
            self.assertEqual(completed.stdout, "3\n")

    def test_direct_input_files_parse_as_one_combined_source(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.cd"
            second = root / "second.cd"
            first.write_text("let value =\n", encoding="utf-8")
            second.write_text("41;\nprint value;\n", encoding="utf-8")

            completed = self.emit_and_run_vm(root, first, second)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "41\n")
            self.assertEqual(completed.stderr, "")

    def test_direct_input_files_lex_as_one_combined_source(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.cd"
            second = root / "second.cd"
            first.write_text('print "first\n', encoding="utf-8")
            second.write_text('second";\n', encoding="utf-8")

            completed = self.emit_and_run_vm(root, first, second)

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "first\nsecond\n")
            self.assertEqual(completed.stderr, "")

    def test_emit_bytecode_accepts_multiple_input_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            lib = root / "lib.cd"
            main = root / "main.cd"
            artifact = root / "program.cdbc"
            lib.write_text("fun add(a, b) { return a + b; }\n", encoding="utf-8")
            main.write_text("print add(2, 3);\n", encoding="utf-8")

            completed = self.run_compiler("--emit-bytecode", str(artifact), str(lib), str(main))

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(completed.stderr, "")
            self.assertTrue(artifact.is_file())
            self.assertIn("add", artifact.read_text(encoding="utf-8"))

    def test_missing_second_input_file_reports_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            existing = root / "existing.cd"
            missing = root / "missing.cd"
            existing.write_text("print 1;\n", encoding="utf-8")

            artifact = root / "program.cdbc"
            completed = self.run_compiler("--emit-bytecode", str(artifact), str(existing), str(missing))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(completed.stderr, f"failed to open input file: {missing}\n")
            self.assertFalse(artifact.exists())

    def test_import_loading_precedes_later_entry_lex_errors(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.cd"
            second = root / "second.cd"
            missing = root / "missing.cd"
            first.write_text('import "./missing.cd";\n', encoding="utf-8")
            second.write_text("print @;\n", encoding="utf-8")

            completed = self.run_compiler(str(first), str(second))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(completed.stderr, f"Import error: failed to open import: {missing}\n")

    def test_importer_frontend_errors_precede_dependency_loading(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "input.cd"
            source.write_text('import "./missing.cd";\nprint @;\n', encoding="utf-8")

            completed = self.run_compiler(str(source))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Lex error at {source}:2:7: unexpected character `@`\n"
                "  print @;\n"
                "        ^\n",
            )

    def test_no_input_files_are_rejected(self) -> None:
        completed = subprocess.run(
            [str(self.compiler)],
            input="print 7;\n",
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(completed.returncode, 64)
        self.assertEqual(completed.stdout, "")
        self.assertIn("All non-LSP modes require at least one source file.", completed.stderr)

    def test_emit_bytecode_requires_at_least_one_input_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            artifact = Path(temp_dir) / "program.cdbc"

            completed = self.run_compiler("--emit-bytecode", str(artifact))

            self.assertEqual(completed.returncode, 64)
            self.assertEqual(completed.stdout, "")
            self.assertFalse(artifact.exists())


    def test_run_mode_is_removed(self) -> None:
        completed = self.run_compiler("--run")

        self.assertEqual(completed.returncode, 64)
        self.assertEqual(completed.stdout, "")
        self.assertIn("Usage:", completed.stderr)
        self.assertNotIn("[--run]", completed.stderr)

    def test_direct_cli_files_with_export_still_share_entry_scope(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "lib.cd").write_text('let value = "direct";\nexport value;\n', encoding="utf-8")
            (root / "main.cd").write_text('print value;\n', encoding="utf-8")

            completed = self.emit_and_run_vm(root, root / "lib.cd", root / "main.cd")

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "direct\n")
            self.assertEqual(completed.stderr, "")

    def test_import_path_resolves_relative_to_importing_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            nested = root / "nested"
            nested.mkdir()
            (root / "input.cd").write_text('import "./nested/lib.cd";\nprint getValue();\n', encoding="utf-8")
            (nested / "lib.cd").write_text(
                'import "./inner.cd";\n'
                'fun getValue() { return value; }\n'
                'export getValue;\n',
                encoding="utf-8",
            )
            (nested / "inner.cd").write_text('let value = "relative";\nexport value;\n', encoding="utf-8")

            completed = self.emit_and_run_vm(root, root / "input.cd")

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "relative\n")
            self.assertEqual(completed.stderr, "")

    def test_canonical_duplicate_import_spellings_are_deduplicated(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "nested").mkdir()
            (root / "shared.cd").write_text("let value = 9;\nexport value;\n", encoding="utf-8")
            (root / "input.cd").write_text(
                'import "./shared.cd";\n'
                'import "./nested/../shared.cd";\n'
                'print value;\n',
                encoding="utf-8",
            )

            completed = self.emit_and_run_vm(root, root / "input.cd")

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "9\n")
            self.assertEqual(completed.stderr, "")

    def test_imported_file_parse_error_reports_imported_file_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            lib = root / "lib.cd"
            (root / "input.cd").write_text('import "./lib.cd";\nprint 1;\n', encoding="utf-8")
            lib.write_text('let value = ;\nexport value;\n', encoding="utf-8")

            completed = self.run_compiler(str(root / "input.cd"))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Parse error at {lib}:1:13: expected expression\n"
                "  let value = ;\n"
                "              ^\n",
            )

    def test_imported_file_lex_error_reports_imported_file_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            lib = root / "lib.cd"
            (root / "input.cd").write_text('import "./lib.cd";\nprint 1;\n', encoding="utf-8")
            lib.write_text("print @;\n", encoding="utf-8")

            completed = self.run_compiler(str(root / "input.cd"))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Lex error at {lib}:1:7: unexpected character `@`\n"
                "  print @;\n"
                "        ^\n",
            )

    def test_imported_file_type_error_reports_imported_file_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            lib = root / "lib.cd"
            (root / "input.cd").write_text('import "./lib.cd";\nprint value;\n', encoding="utf-8")
            lib.write_text('let value = missing;\nexport value;\n', encoding="utf-8")

            completed = self.run_compiler(str(root / "input.cd"))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Type error at {lib}:1:13: undefined variable `missing`\n"
                "  let value = missing;\n"
                "              ^\n",
            )

    def test_direct_multi_file_parse_error_reports_own_file_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.cd"
            second = root / "second.cd"
            first.write_text('let ok = 1;\n', encoding="utf-8")
            second.write_text('print ;\n', encoding="utf-8")

            completed = self.run_compiler(str(first), str(second))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Parse error at {second}:1:7: expected expression\n"
                "  print ;\n"
                "        ^\n",
            )

    def test_direct_multi_file_type_error_reports_own_file_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.cd"
            second = root / "second.cd"
            first.write_text('let ok = 1;\n', encoding="utf-8")
            second.write_text('print missing;\n', encoding="utf-8")

            completed = self.run_compiler(str(first), str(second))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Type error at {second}:1:7: undefined variable `missing`\n"
                "  print missing;\n"
                "        ^\n",
            )

    def test_import_text_inside_string_and_comment_is_ignored_by_loader(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "input.cd").write_text(
                '// import "./missing_from_comment.cd";\n'
                'print "import ./missing_from_string.cd;";\n',
                encoding="utf-8",
            )

            completed = self.emit_and_run_vm(root, root / "input.cd")

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, 'import ./missing_from_string.cd;\n')
            self.assertEqual(completed.stderr, "")

    def test_short_import_path_option_resolves_extensionless_module(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            app = root / "app"
            stdlib = root / "stdlib"
            app.mkdir()
            stdlib.mkdir()
            (app / "input.cd").write_text('import "math";\nprint value;\n', encoding="utf-8")
            (stdlib / "math.cd").write_text('let value = "short";\nexport value;\n', encoding="utf-8")

            completed = self.emit_and_run_vm_with_compiler_args(root, ("-I", str(stdlib)), app / "input.cd")

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "short\n")
            self.assertEqual(completed.stderr, "")

    def test_long_import_path_option_resolves_subdirectory_module(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            app = root / "app"
            stdlib = root / "stdlib"
            (stdlib / "pkg").mkdir(parents=True)
            app.mkdir()
            (app / "input.cd").write_text('import "pkg/math";\nprint value;\n', encoding="utf-8")
            (stdlib / "pkg" / "math.cd").write_text('let value = "subdir";\nexport value;\n', encoding="utf-8")

            completed = self.emit_and_run_vm_with_compiler_args(
                root,
                ("--import-path", str(stdlib)),
                app / "input.cd",
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "subdir\n")
            self.assertEqual(completed.stderr, "")

    def test_import_search_paths_are_tried_in_cli_order(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            app = root / "app"
            first = root / "first"
            second = root / "second"
            app.mkdir()
            first.mkdir()
            second.mkdir()
            (app / "input.cd").write_text('import "lib";\nprint value;\n', encoding="utf-8")
            (first / "lib.cd").write_text('let value = "first";\nexport value;\n', encoding="utf-8")
            (second / "lib.cd").write_text('let value = "second";\nexport value;\n', encoding="utf-8")

            completed = self.emit_and_run_vm_with_compiler_args(
                root,
                ("-I", str(first), "--import-path", str(second)),
                app / "input.cd",
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "first\n")
            self.assertEqual(completed.stderr, "")

    def test_importing_file_directory_wins_over_search_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            app = root / "app"
            stdlib = root / "stdlib"
            app.mkdir()
            stdlib.mkdir()
            (app / "input.cd").write_text('import "lib";\nprint value;\n', encoding="utf-8")
            (app / "lib.cd").write_text('let value = "local";\nexport value;\n', encoding="utf-8")
            (stdlib / "lib.cd").write_text('let value = "search";\nexport value;\n', encoding="utf-8")

            completed = self.emit_and_run_vm_with_compiler_args(root, ("-I", str(stdlib)), app / "input.cd")

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "local\n")
            self.assertEqual(completed.stderr, "")

    def test_explicit_relative_import_does_not_fall_back_to_search_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            app = root / "app"
            stdlib = root / "stdlib"
            app.mkdir()
            stdlib.mkdir()
            (app / "input.cd").write_text('import "./missing";\nprint value;\n', encoding="utf-8")
            (stdlib / "missing.cd").write_text('let value = "search";\nexport value;\n', encoding="utf-8")

            completed = self.run_compiler("-I", str(stdlib), str(app / "input.cd"))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(completed.stderr, f"Import error: failed to open import: {app / 'missing'}\n")
            self.assertNotIn(str(stdlib), completed.stderr)

    def test_missing_search_path_import_reports_tried_candidates(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            app = root / "app"
            stdlib = root / "stdlib"
            app.mkdir()
            stdlib.mkdir()
            (app / "input.cd").write_text('import "missing";\n', encoding="utf-8")

            completed = self.run_compiler("-I", str(stdlib), str(app / "input.cd"))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                "Import error: failed to resolve import `missing`; tried: "
                f"{app / 'missing'}, {app / 'missing.cd'}, "
                f"{stdlib / 'missing'}, {stdlib / 'missing.cd'}\n",
            )

    def test_import_path_options_require_arguments(self) -> None:
        for option in ("-I", "--import-path"):
            with self.subTest(option=option):
                completed = self.run_compiler(option)

                self.assertEqual(completed.returncode, 64)
                self.assertEqual(completed.stdout, "")
                self.assertIn("Usage:", completed.stderr)

    def test_module_interface_uses_import_search_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            app = root / "app"
            stdlib = root / "stdlib"
            app.mkdir()
            stdlib.mkdir()
            (app / "input.cd").write_text('export value from "math";\n', encoding="utf-8")
            (stdlib / "math.cd").write_text('let value: number = 7;\nexport value;\n', encoding="utf-8")

            completed = self.run_compiler("-I", str(stdlib), "--module-interface", str(app / "input.cd"))

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stderr, "")
            self.assertIn(f'module 0 "{stdlib / "math.cd"}"\n', completed.stdout)
            self.assertIn(f'module 1 entry "{app / "input.cd"}"\n', completed.stdout)
            self.assertEqual(completed.stdout.count('  export value value: number\n'), 2)

    def test_direct_multi_file_parse_errors_reports_all_file_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "first.cd"
            second = root / "second.cd"
            first.write_text("print ;\n", encoding="utf-8")
            second.write_text("let x = ;\n", encoding="utf-8")

            completed = self.run_compiler(str(first), str(second))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Parse error at {first}:1:7: expected expression\n"
                "  print ;\n"
                "        ^\n"
                f"Parse error at {second}:1:9: expected expression\n"
                "  let x = ;\n"
                "          ^\n",
            )

    def test_imported_file_parse_errors_reports_all_diagnostics_with_path(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            input_file = root / "input.cd"
            lib = root / "lib.cd"
            input_file.write_text('import "./lib.cd";\nprint 1;\n', encoding="utf-8")
            lib.write_text("print ;\nlet x = ;\n", encoding="utf-8")

            completed = self.run_compiler(str(input_file))

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertEqual(
                completed.stderr,
                f"Parse error at {lib}:1:7: expected expression\n"
                "  print ;\n"
                "        ^\n"
                f"Parse error at {lib}:2:9: expected expression\n"
                "  let x = ;\n"
                "          ^\n",
            )


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
