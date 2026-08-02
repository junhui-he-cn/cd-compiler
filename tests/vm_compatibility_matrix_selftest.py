#!/usr/bin/env python3

import copy
import sys
import unittest
from pathlib import Path


TESTS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TESTS_DIR))

import vm_compatibility_matrix  # noqa: E402


class VmCompatibilityMatrixTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = TESTS_DIR.parent
        self.matrix = vm_compatibility_matrix.load_matrix()

    def test_checked_in_matrix_is_valid(self) -> None:
        self.assertEqual(
            vm_compatibility_matrix.validate_matrix(self.matrix, self.repo_root),
            [],
        )

    def test_validation_detects_artifact_version_drift(self) -> None:
        changed = copy.deepcopy(self.matrix)
        changed["version_contract"]["artifact"]["version"] = "0.2"
        errors = vm_compatibility_matrix.validate_matrix(changed, self.repo_root)
        self.assertIn("artifact version must remain 0.1", errors)

    def test_validation_detects_missing_compatibility_cell(self) -> None:
        changed = copy.deepcopy(self.matrix)
        changed["compatibility_cells"] = changed["compatibility_cells"][1:]
        errors = vm_compatibility_matrix.validate_matrix(changed, self.repo_root)
        self.assertIn("compatibility cell IDs do not match the required set", errors)

    def test_validation_detects_native_registry_drift(self) -> None:
        changed = copy.deepcopy(self.matrix)
        changed["version_contract"]["native_abi"]["names"] = ["push"]
        errors = vm_compatibility_matrix.validate_matrix(changed, self.repo_root)
        self.assertIn("native ABI names do not match vm.rs registry", errors)

    def test_validation_detects_missing_evidence(self) -> None:
        changed = copy.deepcopy(self.matrix)
        changed["compatibility_cells"][0]["evidence"] = ["docs/missing-x1-evidence.md"]
        errors = vm_compatibility_matrix.validate_matrix(changed, self.repo_root)
        self.assertTrue(
            any(
                error.startswith("artifact.debug_metadata.cdbc_0_1 missing evidence")
                for error in errors
            )
        )


if __name__ == "__main__":
    unittest.main()
