"""Fail-closed coverage, cleanup evidence and failure artifact regression."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from run_backend_regression import REQUIRED, Regression, validate_cleanup, validate_matrix


class ReportingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.path = self.root / "summary.json"

    def tearDown(self):
        self.temp.cleanup()

    def write(self, report):
        self.path.write_text(json.dumps(report))
        return self.path

    def matrix(self):
        return dict(status="pass", backend="imagefile",
                    stages=[dict(name=name, returncode=code) for name, code in REQUIRED.items()])

    def test_complete_matrix_and_expected_negative_case(self):
        report = self.matrix()
        self.assertEqual(validate_matrix(self.write(report), "imagefile"), report)
        next(s for s in report["stages"] if s["name"] == "exclusive-open")["returncode"] = 0
        with self.assertRaises(ValueError):
            validate_matrix(self.write(report), "imagefile")

    def test_pass_label_cannot_hide_missing_or_duplicate_coverage(self):
        for stages in (self.matrix()["stages"][:-1], self.matrix()["stages"] + [self.matrix()["stages"][0]]):
            report = self.matrix()
            report["stages"] = stages
            with self.assertRaises(ValueError):
                validate_matrix(self.write(report), "imagefile")

    def test_wrong_backend_or_failed_status_is_rejected(self):
        report = self.matrix()
        with self.assertRaises(ValueError):
            validate_matrix(self.write(report), "kernel")
        report["status"] = "fail"
        with self.assertRaises(ValueError):
            validate_matrix(self.write(report), "imagefile")

    def test_cleanup_must_confirm_all_restored_state_and_handoff(self):
        report = dict(status="pass", driver="nvme", hugepages=0, expected_hugepages=0,
                      interrupts="N", expected_interrupts="N", spdk_to_kernel="pass")
        validate_cleanup(self.write(report))
        for key, value in (("driver", "vfio-pci"), ("hugepages", 256),
                           ("interrupts", "Y"), ("spdk_to_kernel", "not_run")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                validate_cleanup(self.write(dict(report, **{key: value})))
        del report["hugepages"]
        with self.assertRaises(ValueError):
            validate_cleanup(self.write(report))

    def test_failure_preserves_log_and_independent_stage_can_run(self):
        runner = Regression(self.root)
        self.assertFalse(runner.run("failed", [sys.executable, "-c", "print('failure evidence'); raise SystemExit(7)"]))
        self.assertIn("failure evidence", (self.root / "failed.log").read_text())
        self.assertTrue(runner.run("next", [sys.executable, "-c", "print('next independent backend')"]))
        self.assertEqual([r["status"] for r in runner.stages], ["fail", "pass"])

    def test_timeout_is_failure_and_keeps_partial_output(self):
        runner = Regression(self.root)
        self.assertFalse(runner.run("timeout", [sys.executable, "-c", "import time; print('started', flush=True); time.sleep(60)"], timeout=.2))
        self.assertIn("started", (self.root / "timeout.log").read_text())
        self.assertEqual(runner.stages[0]["status"], "fail")

    def test_zero_exit_without_valid_evidence_fails(self):
        runner = Regression(self.root)
        self.assertFalse(runner.run("missing", [sys.executable, "-c", "pass"],
                                    evidence=lambda: validate_matrix(self.path, "imagefile")))


if __name__ == "__main__":
    unittest.main()
