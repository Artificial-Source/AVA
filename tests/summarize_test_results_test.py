#!/usr/bin/env python3
"""Focused tests for scripts/summarize-test-results.py."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SECRET = "canary-secret-body-content"


def junit_document(*cases: str) -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuite name="(empty)" tests="{n}" time="3.5">\n'
        "{cases}"
        "</testsuite>\n"
    ).format(n=len(cases), cases="".join(cases))


PASS_CASE = '<testcase name="suite.pass" time="0.25" status="run"/>\n'
SLOW_CASE = '<testcase name="suite.slow" time="9.5" status="run"/>\n'
FAIL_CASE = (
    '<testcase name="suite.fail" time="1.0" status="fail">'
    f'<failure message="{SECRET}"/>'
    f'<system-out>{SECRET} stdout</system-out>'
    "</testcase>\n"
)
SKIP_CASE = (
    '<testcase name="suite.skip" time="0.01" status="notrun">'
    '<skipped message="SKIP_RETURN_CODE=77"/>'
    "</testcase>\n"
)
CONTROL_CASE = '<testcase name="suite.ctl\x7fend" time="0.1" status="run"/>\n'


class SummarizeTestResultsTests(unittest.TestCase):
    script: Path

    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write(self, name: str, content: str) -> Path:
        path = self.root / name
        path.write_text(content, encoding="utf-8")
        return path

    def run_summary(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.script), *arguments],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def test_passed_summary_counts_and_slowest(self) -> None:
        path = self.write("pass.xml", junit_document(PASS_CASE, SLOW_CASE))

        result = self.run_summary(str(path))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("2 total, 2 passed, 0 failed, 0 skipped", result.stdout)
        self.assertIn("elapsed suite time: 3.50s", result.stdout)
        self.assertIn("9.50s suite.slow", result.stdout)
        self.assertIn("0.25s suite.pass", result.stdout)

    def test_failed_and_skipped_counts_without_failing_normal_summary(self) -> None:
        path = self.write("mixed.xml", junit_document(PASS_CASE, FAIL_CASE, SKIP_CASE))

        result = self.run_summary(str(path))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3 total, 1 passed, 1 failed, 1 skipped", result.stdout)

    def test_summary_never_prints_output_or_failure_bodies(self) -> None:
        path = self.write("private.xml", junit_document(FAIL_CASE))

        result = self.run_summary(str(path), "--require", "suite.fail")

        self.assertEqual(result.returncode, 1)
        self.assertNotIn(SECRET, result.stdout)
        self.assertNotIn(SECRET, result.stderr)
        self.assertIn("required test failed: suite.fail", result.stderr)

    def test_not_run_is_not_an_intentional_skip(self) -> None:
        for message in ("Not Run", "Unable to find executable", "", "Disabled"):
            with self.subTest(message=message):
                case = f'<testcase name="suite.missing" status="notrun"><skipped message="{message}"/></testcase>'
                path = self.write("not-run.xml", junit_document(case))
                result = self.run_summary(str(path))
                self.assertEqual(result.returncode, 1)
                self.assertIn("1 failed, 0 skipped", result.stdout)
                self.assertIn("test could not run: suite.missing", result.stderr)

    def test_names_are_escaped(self) -> None:
        path = self.write("control.xml", junit_document(CONTROL_CASE))

        result = self.run_summary(str(path))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("\x7f", result.stdout)
        self.assertIn("suite.ctl\\x7fend", result.stdout)

    def test_empty_file_fails(self) -> None:
        path = self.write("empty.xml", junit_document())

        result = self.run_summary(str(path))

        self.assertEqual(result.returncode, 1)
        self.assertIn("empty JUnit XML", result.stderr)

    def test_malformed_file_fails(self) -> None:
        path = self.write("malformed.xml", "<testsuite><testcase")

        result = self.run_summary(str(path))

        self.assertEqual(result.returncode, 1)
        self.assertIn("malformed JUnit XML", result.stderr)

    def test_missing_file_fails(self) -> None:
        result = self.run_summary(str(self.root / "absent.xml"))

        self.assertEqual(result.returncode, 1)
        self.assertIn("missing JUnit XML file", result.stderr)

    def test_strict_gate_accepts_required_passes(self) -> None:
        path = self.write("gate.xml", junit_document(PASS_CASE, SLOW_CASE))

        result = self.run_summary(
            str(path),
            "--require",
            "suite.pass",
            "--require",
            "suite.slow",
            "--require-count",
            "2",
            "--require-no-skips",
        )

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_strict_gate_rejects_missing_required(self) -> None:
        path = self.write("gate.xml", junit_document(PASS_CASE))

        result = self.run_summary(str(path), "--require", "suite.absent")

        self.assertEqual(result.returncode, 1)
        self.assertIn("required test did not execute: suite.absent", result.stderr)

    def test_strict_gate_rejects_skipped_required(self) -> None:
        path = self.write("gate.xml", junit_document(SKIP_CASE))

        result = self.run_summary(str(path), "--require", "suite.skip")

        self.assertEqual(result.returncode, 1)
        self.assertIn("required test was skipped: suite.skip", result.stderr)

    def test_strict_gate_rejects_count_mismatch(self) -> None:
        path = self.write("gate.xml", junit_document(PASS_CASE))

        result = self.run_summary(str(path), "--require-count", "2")

        self.assertEqual(result.returncode, 1)
        self.assertIn("expected exactly 2 test case(s), found 1", result.stderr)

    def test_strict_gate_rejects_unexpected_skip(self) -> None:
        path = self.write("gate.xml", junit_document(PASS_CASE, SKIP_CASE))

        result = self.run_summary(str(path), "--require-no-skips")

        self.assertEqual(result.returncode, 1)
        self.assertIn("skipped but skips are forbidden", result.stderr)

    def test_strict_gate_rejects_failed_tests(self) -> None:
        path = self.write("gate.xml", junit_document(PASS_CASE, FAIL_CASE))

        result = self.run_summary(str(path), "--require", "suite.pass")

        self.assertEqual(result.returncode, 1)
        self.assertIn("test failed under a strict gate: suite.fail", result.stderr)

    def test_multiple_files_aggregate(self) -> None:
        first = self.write("first.xml", junit_document(PASS_CASE))
        second = self.write("second.xml", junit_document(SLOW_CASE))

        result = self.run_summary(str(first), str(second), "--require-count", "2")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("elapsed suite time: 7.00s", result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script", required=True, type=Path)
    args = parser.parse_args()
    SummarizeTestResultsTests.script = args.script.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SummarizeTestResultsTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
