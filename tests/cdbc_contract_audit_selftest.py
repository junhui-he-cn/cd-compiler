#!/usr/bin/env python3

import copy
import hashlib
import json
import tempfile
import sys
import unittest
from pathlib import Path


TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
sys.path.insert(0, str(TESTS_DIR))

import cdbc_contract_audit  # noqa: E402


class CdbcContractAuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self.decisions = cdbc_contract_audit.load_decisions()
        self.inventory = cdbc_contract_audit.load_inventory()

    def test_checked_in_evolution_decisions_are_valid(self) -> None:
        self.assertEqual(
            cdbc_contract_audit.validate_evolution_decisions(self.decisions),
            [],
        )

    def test_m4a_compatibility_matrix_is_valid(self) -> None:
        decision = cdbc_contract_audit.load_m4a_decision()
        self.assertEqual(cdbc_contract_audit.validate_m4a_decision(decision), [])
        self.assertEqual(decision["decision_id"], "M4A-VALIDATION-001")
        self.assertFalse(decision["successor_decision"]["selected"])

    def test_static_audit_covers_every_artifact_case(self) -> None:
        report = cdbc_contract_audit.build_static_report(
            self.decisions,
            self.inventory,
            REPO_ROOT,
        )
        self.assertEqual(report["errors"], [])
        self.assertEqual(report["corpus"]["artifact_case_count"], 126)
        self.assertEqual(report["corpus"]["artifact_fixture_count"], 63)
        self.assertEqual(len(report["envelope"]["case_capabilities"]), 126)
        self.assertEqual(report["envelope"]["observed_header_versions"], ["cdbc 0.1"])

    def test_static_audit_is_deterministic(self) -> None:
        first = cdbc_contract_audit.build_static_report(
            self.decisions,
            self.inventory,
            REPO_ROOT,
        )
        second = cdbc_contract_audit.build_static_report(
            self.decisions,
            self.inventory,
            REPO_ROOT,
        )
        self.assertEqual(first, second)
        json.dumps(first)

    def test_checked_in_baseline_matches_static_audit(self) -> None:
        baseline = json.loads(
            (REPO_ROOT / "docs" / "verification" / "m05b-baseline.json").read_text(
                encoding="utf-8"
            )
        )
        report = cdbc_contract_audit.build_static_report(
            self.decisions,
            self.inventory,
            REPO_ROOT,
        )
        digest = hashlib.sha256()
        for artifact in report["envelope"]["artifacts"]:
            digest.update(artifact["path"].encode("utf-8"))
            digest.update(b"\0")
            digest.update(artifact["sha256"].encode("utf-8"))
            digest.update(b"\0")
        self.assertEqual(
            baseline["corpus"]["reference_corpus_sha256"],
            digest.hexdigest(),
        )
        self.assertEqual(
            baseline["corpus"]["artifact_case_ids"],
            report["corpus"]["artifact_case_count"],
        )
        self.assertEqual(
            baseline["envelope"]["observed_capability_counts"],
            report["envelope"]["observed_capability_counts"],
        )

    def test_checked_in_capability_report_covers_dynamic_audit(self) -> None:
        checked = json.loads(
            (
                REPO_ROOT
                / "docs"
                / "verification"
                / "m05b-artifact-audit-report.json"
            ).read_text(encoding="utf-8")
        )
        report = cdbc_contract_audit.build_static_report(
            self.decisions,
            self.inventory,
            REPO_ROOT,
        )
        self.assertEqual(checked["errors"], [])
        self.assertEqual(checked["envelope"]["case_capabilities"], report["envelope"]["case_capabilities"])
        self.assertEqual(checked["dynamic_verification"]["artifact_assertions"]["passed"], 126)
        self.assertEqual(checked["dynamic_verification"]["reference_dumps"]["passed"], 63)
        self.assertEqual(checked["dynamic_verification"]["invalid_header_probes"]["passed"], 2)
        self.assertEqual(checked["dynamic_verification"]["invalid_artifact_probes"]["passed"], 7)

    def test_inspector_rejects_bad_header_and_section_order(self) -> None:
        with tempfile.TemporaryDirectory(prefix="compiler-design-cdbc-selftest-") as temp_dir:
            path = Path(temp_dir) / "bad.cdbc"
            path.write_text(
                "cdbc 9.9\n\nnames:\n\nconstants:\n\nmain registers=0:\n",
                encoding="utf-8",
            )
            _, errors = cdbc_contract_audit.inspect_artifact(path, Path(temp_dir))
        self.assertTrue(any("expected cdbc 0.1 header" in error for error in errors))
        self.assertTrue(any("core sections must start" in error for error in errors))

    def test_inspector_records_optional_debug_source_module_identity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="compiler-design-cdbc-selftest-") as temp_dir:
            path = Path(temp_dir) / "module-debug.cdbc"
            path.write_text(
                "cdbc 0.1\n\n"
                "constants:\n\n"
                "names:\n\n"
                "main registers=0:\n\n"
                "debug_sources:\n"
                "  s0 module=\"/workspace/lib.cd\" path=\"lib.cd\" text=\"\"\n",
                encoding="utf-8",
            )
            info, errors = cdbc_contract_audit.inspect_artifact(path, Path(temp_dir))
        self.assertEqual(errors, [])
        self.assertTrue(info["has_debug_source_modules"])
        self.assertIn("debug_source_module", info["envelope_capabilities"])

    def test_validation_requires_a_rationale_for_deferred_fields(self) -> None:
        changed = copy.deepcopy(self.decisions)
        changed["evolution_decisions"][3]["rationale"] = ""
        errors = cdbc_contract_audit.validate_evolution_decisions(changed)
        self.assertTrue(any("requires a rationale" in error for error in errors))

    def test_successor_candidate_has_consumer_and_fixture(self) -> None:
        candidates = [
            item
            for item in self.decisions["evolution_decisions"]
            if item["classification"] == "successor-version candidate"
        ]
        self.assertEqual(len(candidates), 1)
        self.assertTrue(candidates[0]["named_consumer"])
        self.assertTrue(candidates[0]["planned_fixtures"])


if __name__ == "__main__":
    unittest.main()
