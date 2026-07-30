#!/usr/bin/env python3

import copy
import json
import stat
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import run_benchmarks


class BenchmarkRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parent.parent
        self.manifest_path = self.repo_root / "tests" / "benchmark_manifest.json"
        self.manifest = run_benchmarks.load_manifest(self.manifest_path)

    def test_checked_in_manifest_is_valid(self) -> None:
        self.assertEqual(
            run_benchmarks.validate_manifest(self.manifest, self.repo_root), []
        )

    def test_validation_rejects_duplicate_and_unsorted_workloads(self) -> None:
        changed = copy.deepcopy(self.manifest)
        changed["workloads"] = [
            copy.deepcopy(changed["workloads"][1]),
            copy.deepcopy(changed["workloads"][0]),
            copy.deepcopy(changed["workloads"][0]),
        ]
        errors = run_benchmarks.validate_manifest(changed, self.repo_root)
        self.assertIn("workload IDs must be unique", errors)
        self.assertIn("workloads must be sorted by workload_id", errors)

    def test_timing_record_reports_samples_and_summary(self) -> None:
        record = run_benchmarks.timing_record([3.0, 1.0, 2.0])
        self.assertEqual(record["samples_seconds"], [3.0, 1.0, 2.0])
        self.assertEqual(record["min"], 1.0)
        self.assertEqual(record["median"], 2.0)
        self.assertEqual(record["max"], 3.0)

    def test_run_command_times_out_with_a_controlled_result(self) -> None:
        duration, returncode, stdout, stderr = run_benchmarks.run_command(
            [sys.executable, "-c", "import time; time.sleep(1)"],
            self.repo_root,
            timeout_seconds=0.01,
        )
        self.assertGreaterEqual(duration, 0.0)
        self.assertEqual(returncode, 124)
        self.assertEqual(stdout, "")
        self.assertIn("timed out", stderr)

    def test_vm_manifest_path_resolves_to_built_binary(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            manifest = root / "Cargo.toml"
            binary = root / "target" / "debug" / "compiler-design-vm"
            manifest.write_text("[package]\n", encoding="utf-8")
            binary.parent.mkdir(parents=True)
            binary.write_text("binary\n", encoding="utf-8")
            self.assertEqual(run_benchmarks.resolve_vm_binary(manifest), binary)

    def test_runner_validates_output_and_uses_direct_vm_binary(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "input.cd"
            expected = root / "run.out"
            source.write_text("print 1;\n", encoding="utf-8")
            expected.write_text("ok\n", encoding="utf-8")

            compiler = root / "fake-compiler.py"
            compiler.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import pathlib
                    import sys

                    if "--emit-module-bytecode" in sys.argv:
                        output = pathlib.Path(
                            sys.argv[sys.argv.index("--emit-module-bytecode") + 1]
                        )
                        output.mkdir(parents=True)
                        (output / "module-0.cdbc").write_text(
                            "module\\n", encoding="utf-8"
                        )
                    else:
                        artifact = pathlib.Path(
                            sys.argv[sys.argv.index("--emit-bytecode") + 1]
                        )
                        source = pathlib.Path(sys.argv[-1])
                        artifact.write_text(source.read_text(), encoding="utf-8")
                    """
                ),
                encoding="utf-8",
            )
            vm = root / "fake-vm.py"
            vm.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import pathlib
                    import sys

                    command = sys.argv[1]
                    if command == "dump":
                        print("artifact")
                    elif command == "link":
                        pathlib.Path(sys.argv[3]).write_text("linked\\n", encoding="utf-8")
                    elif command == "run":
                        artifact = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
                        if artifact.startswith("error"):
                            sys.stderr.write("expected runtime\\n")
                            raise SystemExit(7)
                        print("ok")
                    else:
                        raise SystemExit(64)
                    """
                ),
                encoding="utf-8",
            )
            for executable in (compiler, vm):
                executable.chmod(executable.stat().st_mode | stat.S_IXUSR)

            manifest = {
                "schema_version": 2,
                "benchmark_revision": "test",
                "default_repeat": 2,
                "workloads": [
                    {
                        "workload_id": "fake",
                        "artifact_mode": "linked",
                        "execution": "success",
                        "sources": ["input.cd"],
                        "expected_output": "run.out",
                    }
                ],
            }
            self.assertEqual(run_benchmarks.validate_manifest(manifest, root), [])
            report = run_benchmarks.run_benchmarks(
                manifest,
                root,
                compiler,
                vm,
                repetitions=2,
            )

        result = report["workloads"][0]
        self.assertTrue(result["passed"])
        self.assertEqual(len(result["compile_seconds"]["samples_seconds"]), 2)
        self.assertEqual(len(result["load_seconds"]["samples_seconds"]), 2)
        self.assertEqual(result["link_seconds"]["samples_seconds"], [])
        self.assertEqual(len(result["runtime_seconds"]["samples_seconds"]), 2)
        self.assertTrue(result["stdout_validated"])
        self.assertTrue(result["load_stdout_validated"])
        self.assertEqual(report["commands"]["runtime"], [str(vm), "run", "<artifact>"])
        self.assertEqual(report["commands"]["load"], [str(vm), "dump", "<artifact>"])
        self.assertNotIn("cargo", json.dumps(report["commands"]))

    def test_runner_measures_module_link_and_accepts_expected_runtime_error(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "input.cd"
            source.write_text("print 1;\n", encoding="utf-8")
            expected = root / "run.out"
            expected.write_text("ok\n", encoding="utf-8")
            expected_stderr = root / "runtime.err"
            expected_stderr.write_text("expected runtime\n", encoding="utf-8")

            compiler = root / "fake-compiler.py"
            compiler.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib\n"
                "import sys\n"
                "if '--emit-module-bytecode' in sys.argv:\n"
                "    output = pathlib.Path(sys.argv[sys.argv.index('--emit-module-bytecode') + 1])\n"
                "    output.mkdir(parents=True)\n"
                "    (output / 'module-0.cdbc').write_text('module\\n', encoding='utf-8')\n"
                "else:\n"
                "    artifact = pathlib.Path(sys.argv[sys.argv.index('--emit-bytecode') + 1])\n"
                "    artifact.write_text(pathlib.Path(sys.argv[-1]).read_text(), encoding='utf-8')\n",
                encoding="utf-8",
            )
            vm = root / "fake-vm.py"
            vm.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib\n"
                "import sys\n"
                "command = sys.argv[1]\n"
                "if command == 'dump':\n"
                "    print('artifact')\n"
                "elif command == 'link':\n"
                "    pathlib.Path(sys.argv[3]).write_text('linked\\n', encoding='utf-8')\n"
                "elif command == 'run':\n"
                "    if pathlib.Path(sys.argv[2]).read_text(encoding='utf-8').startswith('error'):\n"
                "        sys.stderr.write('expected runtime\\n')\n"
                "        raise SystemExit(7)\n"
                "    print('ok')\n"
                "else:\n"
                "    raise SystemExit(64)\n",
                encoding="utf-8",
            )
            for executable in (compiler, vm):
                executable.chmod(executable.stat().st_mode | stat.S_IXUSR)

            module_manifest = {
                "schema_version": 2,
                "benchmark_revision": "module-test",
                "default_repeat": 2,
                "workloads": [
                    {
                        "workload_id": "module",
                        "artifact_mode": "module",
                        "execution": "success",
                        "sources": ["input.cd"],
                        "expected_output": "run.out",
                    }
                ],
            }
            self.assertEqual(
                run_benchmarks.validate_manifest(module_manifest, root), []
            )
            module_report = run_benchmarks.run_benchmarks(
                module_manifest, root, compiler, vm, repetitions=2
            )
            module_result = module_report["workloads"][0]
            self.assertTrue(module_result["passed"])
            self.assertTrue(module_result["link_required"])
            self.assertEqual(len(module_result["link_seconds"]["samples_seconds"]), 2)
            self.assertEqual(len(module_result["load_seconds"]["samples_seconds"]), 2)
            self.assertTrue(module_result["link_artifact_validated"])

            error_source = root / "error.cd"
            error_source.write_text("error\n", encoding="utf-8")
            error_manifest = {
                "schema_version": 2,
                "benchmark_revision": "error-test",
                "default_repeat": 2,
                "workloads": [
                    {
                        "workload_id": "runtime_error",
                        "artifact_mode": "linked",
                        "execution": "runtime_error",
                        "sources": ["error.cd"],
                        "expected_stderr": "runtime.err",
                        "expected_exit_code": 7,
                    }
                ],
            }
            self.assertEqual(
                run_benchmarks.validate_manifest(error_manifest, root), []
            )
            error_report = run_benchmarks.run_benchmarks(
                error_manifest, root, compiler, vm, repetitions=2
            )
            error_result = error_report["workloads"][0]
            self.assertTrue(error_result["passed"])
            self.assertTrue(error_result["exit_code_validated"])
            self.assertTrue(error_result["stderr_validated"])
            self.assertTrue(error_result["stdout_validated"])


if __name__ == "__main__":
    unittest.main()
