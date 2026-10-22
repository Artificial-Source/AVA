#!/usr/bin/env python3
"""Summarize CTest JUnit XML results without leaking test output.

Reads one or more `--output-junit` files written by CTest and prints only
counts (passed/failed/skipped), the elapsed suite time when the XML supplies
it, and a bounded list of the slowest test names and times. Test stdout,
stderr, and failure body content are never printed; test names are escaped
and length-limited.

Exit status is 0 when every file exists, parses, and contains at least one
test case, no test was unable to run, and all strict gate options below are
satisfied; 1 otherwise (argparse usage errors exit 2). Strict options turn the summary into a gate:
`--require` names a test that must be present exactly once, executed (not
skipped), and passed; `--require-count` pins the exact total number of test
cases; `--require-no-skips` rejects any skipped test. When any strict option
is given, failed tests also fail the gate, so a harness skip (CTest skip
return code 77) or a failure cannot masquerade as a green required run.
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET

MAX_NAME_LENGTH = 120
DEFAULT_SLOWEST = 5


class SummaryError(Exception):
    """A missing, malformed, or empty JUnit input, or a failed strict gate."""


def sanitize_name(name: str) -> str:
    escaped = "".join(
        char if char.isprintable() and char not in "\\" else f"\\x{ord(char):02x}"
        for char in name
    )
    if len(escaped) > MAX_NAME_LENGTH:
        escaped = escaped[: MAX_NAME_LENGTH - 3] + "..."
    return escaped


class TestCase:
    def __init__(self, name: str, time: float | None, failed: bool, skipped: bool, not_run: bool = False) -> None:
        self.name = name
        self.time = time
        self.failed = failed
        self.skipped = skipped
        self.not_run = not_run


def parse_time(raw: str | None) -> float | None:
    if raw is None:
        return None
    try:
        value = float(raw)
    except ValueError:
        return None
    return value if value >= 0 else None


def load_cases(path: str) -> tuple[list[TestCase], float | None]:
    try:
        tree = ET.parse(path)
    except FileNotFoundError:
        raise SummaryError(f"missing JUnit XML file: {path}") from None
    except ET.ParseError as error:
        raise SummaryError(f"malformed JUnit XML in {path}: {error}") from None
    root = tree.getroot()
    suites = [root] if root.tag == "testsuite" else list(root.iter("testsuite"))
    if not suites:
        raise SummaryError(f"malformed JUnit XML in {path}: no testsuite element")
    cases: list[TestCase] = []
    suite_time = 0.0
    have_suite_time = False
    for suite in suites:
        time = parse_time(suite.get("time"))
        if time is not None:
            suite_time += time
            have_suite_time = True
        for case in suite.iter("testcase"):
            name = case.get("name")
            if not name:
                raise SummaryError(f"malformed JUnit XML in {path}: testcase without a name")
            skip = case.find("skipped")
            intentional_skip = skip is not None and skip.get("message", "") == "SKIP_RETURN_CODE=77"
            not_run = (skip is not None and not intentional_skip) or (
                case.get("status", "").lower() in ("notrun", "not run", "disabled") and not intentional_skip
            )
            cases.append(
                TestCase(
                    name=name,
                    time=parse_time(case.get("time")),
                    failed=not_run or case.find("failure") is not None or case.find("error") is not None,
                    skipped=intentional_skip,
                    not_run=not_run,
                )
            )
    return cases, suite_time if have_suite_time else None


def summarize(paths: list[str], slowest: int) -> tuple[list[TestCase], list[str]]:
    all_cases: list[TestCase] = []
    lines: list[str] = []
    total_time = 0.0
    have_time = False
    for path in paths:
        cases, suite_time = load_cases(path)
        if not cases:
            raise SummaryError(f"empty JUnit XML (no test cases): {path}")
        all_cases.extend(cases)
        if suite_time is not None:
            total_time += suite_time
            have_time = True
    passed = sum(1 for case in all_cases if not case.failed and not case.skipped)
    failed = sum(1 for case in all_cases if case.failed)
    skipped = sum(1 for case in all_cases if case.skipped)
    lines.append(
        f"tests: {len(all_cases)} total, {passed} passed, {failed} failed, {skipped} skipped"
    )
    if have_time:
        lines.append(f"elapsed suite time: {total_time:.2f}s")
    timed = sorted(
        (case for case in all_cases if case.time is not None),
        key=lambda case: case.time,
        reverse=True,
    )[: max(slowest, 0)]
    if timed:
        lines.append(f"slowest {len(timed)} test(s):")
        for case in timed:
            lines.append(f"  {case.time:.2f}s {sanitize_name(case.name)}")
    return all_cases, lines


def enforce_strict(
    cases: list[TestCase],
    required: list[str],
    required_count: int | None,
    no_skips: bool,
) -> list[str]:
    problems: list[str] = [
        f"test could not run: {sanitize_name(case.name)}" for case in cases if case.not_run
    ]
    by_name: dict[str, int] = {}
    for case in cases:
        by_name[case.name] = by_name.get(case.name, 0) + 1
    for name in required:
        matching = [case for case in cases if case.name == name]
        safe = sanitize_name(name)
        if not matching:
            problems.append(f"required test did not execute: {safe}")
        elif len(matching) > 1:
            problems.append(f"required test appears {len(matching)} times: {safe}")
        else:
            case = matching[0]
            if case.skipped:
                problems.append(f"required test was skipped: {safe}")
            elif case.failed:
                problems.append(f"required test failed: {safe}")
    if required_count is not None and len(cases) != required_count:
        problems.append(f"expected exactly {required_count} test case(s), found {len(cases)}")
    if no_skips:
        skipped = [case for case in cases if case.skipped]
        if skipped:
            names = ", ".join(sanitize_name(case.name) for case in skipped[:5])
            problems.append(f"{len(skipped)} test(s) skipped but skips are forbidden: {names}")
    if required or required_count is not None or no_skips:
        failed = [case for case in cases if case.failed]
        for case in failed[:5]:
            if case.name not in required:
                problems.append(f"test failed under a strict gate: {sanitize_name(case.name)}")
        if len(failed) > 5:
            problems.append(f"... and {len(failed) - 5} more failed test(s)")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("junit", nargs="+", help="CTest --output-junit XML file(s)")
    parser.add_argument(
        "--slowest",
        type=int,
        default=DEFAULT_SLOWEST,
        help=f"number of slowest tests to list (default {DEFAULT_SLOWEST})",
    )
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        metavar="NAME",
        help="required test: must execute exactly once and pass (repeatable)",
    )
    parser.add_argument(
        "--require-count",
        type=int,
        default=None,
        metavar="N",
        help="require exactly N test cases across all files",
    )
    parser.add_argument(
        "--require-no-skips",
        action="store_true",
        help="fail if any test was skipped",
    )
    args = parser.parse_args(argv)

    try:
        cases, lines = summarize(args.junit, args.slowest)
        problems = enforce_strict(cases, args.require, args.require_count, args.require_no_skips)
    except SummaryError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for line in lines:
        print(line)
    if problems:
        sys.stdout.flush()
        for problem in problems:
            print(f"error: {problem}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
