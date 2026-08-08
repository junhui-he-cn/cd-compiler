#!/usr/bin/env python3

import json
import sys
import unittest
from pathlib import Path


TESTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR))

import boundary_comparison  # noqa: E402


class BoundaryComparisonTests(unittest.TestCase):
    def test_allowlist_normalizes_known_environment_paths(self) -> None:
        allowlist = boundary_comparison.load_allowlist()
        actual = "/old/checkout/tests/golden/case/input.cd:1:1\n"
        expected = "<repo>/tests/golden/case/input.cd:1:1\n"
        self.assertEqual(boundary_comparison.canonicalize(actual, allowlist), expected)
        self.assertEqual(boundary_comparison.canonicalize("tests/golden/case/input.cd\n", allowlist), "tests/golden/case/input.cd\n")
        malformed_path = "/tmp/compiler-design-malformed-source-ab12/input.cd:1:1\n"
        self.assertEqual(
            boundary_comparison.canonicalize(malformed_path, allowlist),
            "<malformed-source>/input.cd:1:1\n",
        )

    def test_injected_mismatch_is_attributed_to_boundary(self) -> None:
        comparison = boundary_comparison.compare_text("ir", "IR\n", "different IR\n")
        self.assertFalse(comparison.matches)
        self.assertIn("-IR", comparison.diff)
        self.assertIn("+different IR", comparison.diff)

    def test_first_failure_uses_pipeline_order(self) -> None:
        cases = [
            {
                "case_id": "later",
                "passed": False,
                "failure_boundary": "bytecode",
                "boundary_sequence": ["tokens", "ast", "ir", "bytecode"],
            },
            {
                "case_id": "earlier",
                "passed": False,
                "failure_boundary": "ir",
                "boundary_sequence": ["tokens", "ast", "ir"],
            },
        ]
        self.assertEqual(
            boundary_comparison.first_failure(cases),
            {"case_id": "earlier", "boundary": "ir"},
        )
        self.assertEqual(boundary_comparison.failure_counts(cases), {"ir": 1, "bytecode": 1})

    def test_allowlist_is_machine_readable(self) -> None:
        value = json.loads((TESTS_DIR / "boundary_allowlist.json").read_text(encoding="utf-8"))
        self.assertEqual(value["schema_version"], 1)
        self.assertTrue(value["rules"])


if __name__ == "__main__":
    unittest.main()
