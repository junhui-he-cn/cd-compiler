#!/usr/bin/env python3

import tempfile
import textwrap
import unittest
import unittest.mock
import subprocess
from pathlib import Path

import run_golden_tests


class GoldenRunnerQualityTests(unittest.TestCase):
    def make_fake_compiler(
        self,
        directory: Path,
        stdout: str = "",
        stderr: str = "",
        returncode: int = 0,
    ) -> Path:
        compiler = directory / "fake_compiler.py"
        compiler.write_text(
            textwrap.dedent(
                f"""\
                #!/usr/bin/env python3
                import sys

                sys.stdout.write({stdout!r})
                sys.stderr.write({stderr!r})
                raise SystemExit({returncode})
                """
            ),
            encoding="utf-8",
        )
        compiler.chmod(compiler.stat().st_mode | 0o111)
        return compiler

    def test_non_update_success_case_without_expected_files_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "empty_success_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("1;\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root)

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("has no expected files", results[0].message)

    def test_non_update_empty_suite_fails_instead_of_passing_zero_checks(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            golden_dir.mkdir()
            compiler = self.make_fake_compiler(root)

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("no golden test checks", results[0].message)

    def test_update_rewrites_only_existing_success_outputs_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "safe_update_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("print(1);\n", encoding="utf-8")
            (case_dir / "ast.out").write_text("old ast\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root, stdout="new ast\n")

            results = run_golden_tests.run_all(
                compiler,
                golden_dir,
                update=True,
                update_missing=False,
            )

            self.assertEqual(len(results), 1)
            self.assertTrue(results[0].passed)
            self.assertEqual((case_dir / "ast.out").read_text(encoding="utf-8"), "new ast\n")
            self.assertFalse((case_dir / "ir.out").exists())
            self.assertFalse((case_dir / "bytecode.out").exists())
            self.assertFalse((case_dir / "module-interface.out").exists())

    def test_update_missing_creates_missing_success_outputs_explicitly(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "update_missing_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("print(1);\n", encoding="utf-8")
            (case_dir / "ast.out").write_text("old ast\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root, stdout="new output\n")

            results = run_golden_tests.run_all(
                compiler,
                golden_dir,
                update=True,
                update_missing=True,
            )

            self.assertEqual(len(results), 4)
            self.assertTrue(all(result.passed for result in results), results)
            for golden_name in ("ast.out", "ir.out", "bytecode.out", "module-interface.out"):
                self.assertEqual(
                    (case_dir / golden_name).read_text(encoding="utf-8"),
                    "new output\n",
                )
            self.assertFalse((case_dir / "run.out").exists())

    def test_case_filter_limits_success_cases_by_substring(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            wanted_dir = golden_dir / "wanted_case"
            skipped_dir = golden_dir / "skipped_case"
            wanted_dir.mkdir(parents=True)
            skipped_dir.mkdir(parents=True)
            (wanted_dir / "input.cd").write_text("print(1);\n", encoding="utf-8")
            (wanted_dir / "ast.out").write_text("ok\n", encoding="utf-8")
            (skipped_dir / "input.cd").write_text("print(2);\n", encoding="utf-8")
            (skipped_dir / "ast.out").write_text("ok\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root, stdout="ok\n")

            results = run_golden_tests.run_all(
                compiler,
                golden_dir,
                update=False,
                case_filters=("wanted",),
            )

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed)
        self.assertEqual(results[0].name, "wanted_case default(ast)")

    def test_success_case_with_unexpected_stderr_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "stderr_success_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("1;\n", encoding="utf-8")
            (case_dir / "ast.out").write_text("ok\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root, stdout="ok\n", stderr="warning\n")

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("unexpected stderr", results[0].message)
        self.assertIn("warning", results[0].message)

    def test_run_out_only_success_case_is_owned_by_rust_vm_runner(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "run_only_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("print(1);\n", encoding="utf-8")
            (case_dir / "run.out").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root, stdout="unexpected compiler output\n")

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("no golden test checks", results[0].message)

    def test_default_ast_check_name_does_not_look_like_missing_cli_flag(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "default_ast_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("1;\n", encoding="utf-8")
            (case_dir / "ast.out").write_text("expected\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root, stdout="actual\n")

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("default_ast_case default(ast)", results[0].message)
        self.assertNotIn("--ast", results[0].message)

    def test_runtime_error_cases_are_not_checked_by_compiler_golden_runner(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            runtime_dir = golden_dir / "runtime_errors"
            runtime_dir.mkdir(parents=True)
            (runtime_dir / "division_by_zero.cd").write_text("1 / 0;\n", encoding="utf-8")
            (runtime_dir / "division_by_zero.run.err").write_text("Runtime error: division by zero\n", encoding="utf-8")
            (runtime_dir / "division_by_zero.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(root)

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("no golden test checks", results[0].message)

    def test_parse_error_case_checks_default_mode_stderr_exit_and_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            parse_dir = golden_dir / "parse_errors"
            parse_dir.mkdir(parents=True)
            (parse_dir / "bad_assignment.cd").write_text("(x + 1) = 2;\n", encoding="utf-8")
            (parse_dir / "bad_assignment.err").write_text("parse error\n", encoding="utf-8")
            (parse_dir / "bad_assignment.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stdout="unexpected output\n",
                stderr="parse error\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("parse_errors/bad_assignment default(ast)", results[0].message)
        self.assertIn("unexpected stdout", results[0].message)
        self.assertIn("unexpected output", results[0].message)

    def test_parse_error_input_cd_is_not_discovered_as_success_case(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            parse_dir = golden_dir / "parse_errors"
            parse_dir.mkdir(parents=True)
            (parse_dir / "input.cd").write_text("(x + 1) = 2;\n", encoding="utf-8")
            (parse_dir / "input.err").write_text("parse error\n", encoding="utf-8")
            (parse_dir / "input.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr="parse error\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed)
        self.assertEqual(results[0].name, "parse_errors/input default(ast)")


    def test_type_error_case_checks_default_mode_stderr_exit_and_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            type_dir = golden_dir / "type_errors"
            type_dir.mkdir(parents=True)
            (type_dir / "bad_type.cd").write_text('let x: number = "bad";\n', encoding="utf-8")
            (type_dir / "bad_type.err").write_text("type error\n", encoding="utf-8")
            (type_dir / "bad_type.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stdout="unexpected output\n",
                stderr="type error\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("type_errors/bad_type default(ast)", results[0].message)
        self.assertIn("unexpected stdout", results[0].message)
        self.assertIn("unexpected output", results[0].message)

    def test_type_error_input_cd_is_not_discovered_as_success_case(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            type_dir = golden_dir / "type_errors"
            type_dir.mkdir(parents=True)
            (type_dir / "input.cd").write_text('let x: number = "bad";\n', encoding="utf-8")
            (type_dir / "input.err").write_text("type error\n", encoding="utf-8")
            (type_dir / "input.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr="type error\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed)
        self.assertEqual(results[0].name, "type_errors/input default(ast)")


    def test_success_case_with_bytecode_expected_file_runs_bytecode_flag(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "bytecode_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("print(1);\n", encoding="utf-8")
            (case_dir / "bytecode.out").write_text("bytecode output\n", encoding="utf-8")
            compiler = root / "fake_compiler.py"
            compiler.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import sys

                    if "--bytecode" not in sys.argv:
                        sys.stderr.write("missing --bytecode\\n")
                        raise SystemExit(1)
                    sys.stdout.write("bytecode output\\n")
                    """
                ),
                encoding="utf-8",
            )
            compiler.chmod(compiler.stat().st_mode | 0o111)

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed)
        self.assertEqual(results[0].name, "bytecode_case --bytecode")

    def test_success_case_with_module_interface_expected_file_runs_module_interface_flag(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "module_interface_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("print(1);\n", encoding="utf-8")
            (case_dir / "module-interface.out").write_text("module interface output\n", encoding="utf-8")
            compiler = root / "fake_compiler.py"
            compiler.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import sys

                    if "--module-interface" not in sys.argv:
                        sys.stderr.write("missing --module-interface\\n")
                        raise SystemExit(1)
                    sys.stdout.write("module interface output\\n")
                    """
                ),
                encoding="utf-8",
            )
            compiler.chmod(compiler.stat().st_mode | 0o111)

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed)
        self.assertEqual(results[0].name, "module_interface_case --module-interface")

    def test_module_interface_output_absolute_repo_paths_are_checkout_independent(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            case_dir = golden_dir / "module_interface_path_case"
            case_dir.mkdir(parents=True)
            (case_dir / "input.cd").write_text("print(1);\n", encoding="utf-8")
            (case_dir / "module-interface.out").write_text(
                'module 0 entry "/old checkout/tests/golden/module_interface_path_case/input.cd"\n',
                encoding="utf-8",
            )
            compiler = self.make_fake_compiler(
                root,
                stdout='module 0 entry "/new checkout/tests/golden/module_interface_path_case/input.cd"\n',
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed, results[0].message)


    def test_success_case_args_txt_replaces_default_input_path_for_ir(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            case_dir = root / "multi_file"
            case_dir.mkdir()
            (case_dir / "lib.cd").write_text("print(1);\n", encoding="utf-8")
            (case_dir / "main.cd").write_text("print(2);\n", encoding="utf-8")
            (case_dir / "args.txt").write_text("lib.cd main.cd\n", encoding="utf-8")
            (case_dir / "ir.out").write_text("ir\n", encoding="utf-8")

            commands: list[list[str]] = []

            def fake_run(command, text, capture_output, check):
                commands.append([str(part) for part in command])
                return subprocess.CompletedProcess(command, 0, "ir\n", "")

            with unittest.mock.patch.object(run_golden_tests.subprocess, "run", side_effect=fake_run):
                results = run_golden_tests.check_success_case(Path("compiler"), case_dir, False)

            self.assertTrue(all(result.passed for result in results), results)
            self.assertIn(["compiler", "--ir", str(case_dir / "lib.cd"), str(case_dir / "main.cd")], commands)


    def test_import_error_case_checks_default_mode_stderr_exit_and_stdout(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            import_dir = golden_dir / "import_errors"
            import_dir.mkdir(parents=True)
            (import_dir / "missing.cd").write_text('import "./missing_dep.cd";\n', encoding="utf-8")
            (import_dir / "missing.err").write_text("import error\n", encoding="utf-8")
            (import_dir / "missing.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stdout="unexpected output\n",
                stderr="import error\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("import_errors/missing default(ast)", results[0].message)
        self.assertIn("unexpected stdout", results[0].message)
        self.assertIn("unexpected output", results[0].message)

    def test_import_error_input_cd_is_not_discovered_as_success_case(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            import_dir = golden_dir / "import_errors"
            import_dir.mkdir(parents=True)
            (import_dir / "input.cd").write_text('import "./missing_dep.cd";\n', encoding="utf-8")
            (import_dir / "input.err").write_text("import error\n", encoding="utf-8")
            (import_dir / "input.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr="import error\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed)
        self.assertEqual(results[0].name, "import_errors/input default(ast)")


    def test_parse_error_one_line_expected_accepts_actual_source_snippet(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            parse_dir = golden_dir / "parse_errors"
            parse_dir.mkdir(parents=True)
            (parse_dir / "snippet.cd").write_text("let ;\n", encoding="utf-8")
            (parse_dir / "snippet.err").write_text("Parse error at 1:5: expected variable name after `let`, found Semicolon `;`\n", encoding="utf-8")
            (parse_dir / "snippet.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr="Parse error at 1:5: expected variable name after `let`, found Semicolon `;`\n  let ;\n      ^\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed)

    def test_parse_error_multiline_expected_requires_exact_source_snippet(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            parse_dir = golden_dir / "parse_errors"
            parse_dir.mkdir(parents=True)
            (parse_dir / "snippet.cd").write_text("let ;\n", encoding="utf-8")
            (parse_dir / "snippet.err").write_text(
                "Parse error at 1:5: expected variable name after `let`, found Semicolon `;`\n"
                "  let ;\n"
                "      ^\n",
                encoding="utf-8",
            )
            (parse_dir / "snippet.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr="Parse error at 1:5: expected variable name after `let`, found Semicolon `;`\n  let ;\n     ^\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("stderr mismatch", results[0].message)

    def test_parse_error_absolute_repo_paths_are_checkout_independent(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            parse_dir = golden_dir / "parse_errors"
            parse_dir.mkdir(parents=True)
            (parse_dir / "bad_import.cd").write_text("import path;\n", encoding="utf-8")
            (parse_dir / "bad_import.err").write_text(
                "Parse error at /old/checkout/tests/golden/parse_errors/bad_import.cd:1:8: expected import path string\n"
                "  import path;\n"
                "         ^\n",
                encoding="utf-8",
            )
            (parse_dir / "bad_import.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr=(
                    "Parse error at /new/checkout/tests/golden/parse_errors/bad_import.cd:1:8: expected import path string\n"
                    "  import path;\n"
                    "         ^\n"
                ),
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed, results[0].message)

    def test_import_error_absolute_repo_paths_are_checkout_independent(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            import_dir = golden_dir / "import_errors"
            import_dir.mkdir(parents=True)
            (import_dir / "missing.cd").write_text('import "./missing.cd";\n', encoding="utf-8")
            (import_dir / "missing.err").write_text(
                "Import error: failed to open import: /old/checkout/tests/golden/import_errors/missing.cd\n",
                encoding="utf-8",
            )
            (import_dir / "missing.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr="Import error: failed to open import: /new/checkout/tests/golden/import_errors/missing.cd\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertTrue(results[0].passed, results[0].message)

    def test_import_error_one_line_expected_does_not_accept_extra_snippet(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            golden_dir = root / "golden"
            import_dir = golden_dir / "import_errors"
            import_dir.mkdir(parents=True)
            (import_dir / "missing.cd").write_text('import "./missing.cd";\n', encoding="utf-8")
            (import_dir / "missing.err").write_text("Import error: failed to open import: missing.cd\n", encoding="utf-8")
            (import_dir / "missing.exit").write_text("1\n", encoding="utf-8")
            compiler = self.make_fake_compiler(
                root,
                stderr="Import error: failed to open import: missing.cd\n  import \"./missing.cd\";\n  ^\n",
                returncode=1,
            )

            results = run_golden_tests.run_all(compiler, golden_dir, update=False)

        self.assertEqual(len(results), 1)
        self.assertFalse(results[0].passed)
        self.assertIn("stderr mismatch", results[0].message)


if __name__ == "__main__":
    raise SystemExit(unittest.main())
