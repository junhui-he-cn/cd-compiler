#!/usr/bin/env python3

import sys
import tempfile
import unittest
from pathlib import Path


TESTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR))

import malformed_corpus  # noqa: E402
import run_malformed_tests  # noqa: E402


class MalformedHarnessTests(unittest.TestCase):
    def test_corpus_expansion_is_stable_and_bounded(self) -> None:
        manifest = malformed_corpus.load_manifest()
        cases = malformed_corpus.expand_cases(manifest)
        case_ids = [case["case_id"] for case in cases]
        self.assertEqual(len(cases), 102)
        self.assertEqual(case_ids, sorted(case_ids))
        self.assertEqual(len(case_ids), len(set(case_ids)))
        self.assertEqual(manifest["seed"], 20260722)
        self.assertEqual(malformed_corpus.limits(manifest)["max_input_bytes"], 8192)

    def test_cdbc_mutations_are_deterministic(self) -> None:
        source = malformed_corpus.read_text(TESTS_DIR.parent / "tests/bytecode_artifacts/arithmetic/expected.cdbc")
        for mutation in (
            "bad_header",
            "truncate",
            "unknown_opcode",
            "trailing_garbage",
            "invalid_register",
            "invalid_constant",
            "invalid_jump",
            "invalid_name",
            "invalid_number",
        ):
            first = malformed_corpus.mutate_cdbc(source, mutation)
            second = malformed_corpus.mutate_cdbc(source, mutation)
            self.assertEqual(first, second)
            self.assertNotEqual(first, source)

    def test_injected_failure_is_minimized_and_saved(self) -> None:
        original = "prefix\nneedle\nsuffix\n"
        minimized = malformed_corpus.minimize_text(original, lambda text: "needle" in text)
        self.assertEqual(minimized, "needle")
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "minimized.input"
            output.write_text(minimized, encoding="utf-8")
            self.assertEqual(output.read_text(encoding="utf-8"), "needle")

    def test_failure_classes_are_distinct(self) -> None:
        case = {"case_id": "test", "expected": "reject"}
        completed = {"status": "completed", "returncode": 1, "stdout": "", "stderr": "error"}
        self.assertEqual(run_malformed_tests.classify_observations(case, completed, completed)[0], True)
        timeout = {"status": "timeout", "returncode": None, "stdout": "", "stderr": ""}
        self.assertEqual(run_malformed_tests.classify_observations(case, timeout, timeout)[1], "timeout")
        crash = {"status": "crash", "returncode": 101, "stdout": "", "stderr": "panicked at"}
        self.assertEqual(run_malformed_tests.classify_observations(case, crash, crash)[1], "crash")
        nondeterministic = {"status": "completed", "returncode": 1, "stdout": "", "stderr": "other"}
        self.assertEqual(run_malformed_tests.classify_observations(case, completed, nondeterministic)[1], "non_deterministic")


if __name__ == "__main__":
    unittest.main()
