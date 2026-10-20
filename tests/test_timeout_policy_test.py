#!/usr/bin/env python3
"""Focused tests for tests/test_timeout_policy.cmake.

Each case configures a minimal temporary CMake/CTest project that includes
the real helper and applies it to three tests (no explicit timeout, explicit
narrower timeout, explicit longer timeout), then reads the effective
properties back through `ctest --show-only=json-v1`.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

PROJECT = """\
cmake_minimum_required(VERSION 3.27)
project(timeout_policy_fixture NONE)
enable_testing()
include({helper})
add_test(NAME fixture.default COMMAND ${{CMAKE_COMMAND}} -E true)
add_test(NAME fixture.explicit_narrow COMMAND ${{CMAKE_COMMAND}} -E true)
add_test(NAME fixture.explicit_long COMMAND ${{CMAKE_COMMAND}} -E true)
set_tests_properties(fixture.explicit_narrow PROPERTIES TIMEOUT 30)
set_tests_properties(fixture.explicit_long PROPERTIES TIMEOUT 240)
ava_apply_test_timeout_policy(fixture.default fixture.explicit_narrow fixture.explicit_long)
"""

TEST_NAMES = ("fixture.default", "fixture.explicit_narrow", "fixture.explicit_long")


class TimeoutPolicyTests(unittest.TestCase):
    helper: Path
    cmake: Path
    ctest: Path

    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def configure_timeouts(self, env_override: dict[str, str] | None = None) -> dict[str, int]:
        source = self.root / "src"
        build = self.root / "build"
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            PROJECT.format(helper=self.helper.as_posix()), encoding="utf-8"
        )
        env = os.environ.copy()
        env.pop("AVA_DEBUG_NO_TIMEOUT", None)
        env.pop("AVA_DEBUG_NO_TIMEOUT_SECONDS", None)
        if env_override:
            env.update(env_override)
        configure = subprocess.run(
            [str(self.cmake), "-S", str(source), "-B", str(build), "-G", "Ninja"],
            check=False,
            capture_output=True,
            text=True,
            timeout=60,
            env=env,
        )
        self.assertEqual(configure.returncode, 0, configure.stderr)
        show = subprocess.run(
            [str(self.ctest), "--test-dir", str(build), "--show-only=json-v1"],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
            env=env,
        )
        self.assertEqual(show.returncode, 0, show.stderr)
        document = json.loads(show.stdout)
        timeouts: dict[str, int] = {}
        for test in document["tests"]:
            for prop in test["properties"]:
                if prop["name"] == "TIMEOUT":
                    timeouts[test["name"]] = int(prop["value"])
        self.assertEqual(sorted(timeouts), sorted(TEST_NAMES))
        return timeouts

    def test_default_fills_only_unset_timeouts(self) -> None:
        timeouts = self.configure_timeouts()
        self.assertEqual(timeouts["fixture.default"], 60)
        self.assertEqual(timeouts["fixture.explicit_narrow"], 30)
        self.assertEqual(timeouts["fixture.explicit_long"], 240)

    def test_debug_override_presence_raises_all_to_one_hour(self) -> None:
        timeouts = self.configure_timeouts({"AVA_DEBUG_NO_TIMEOUT": "1"})
        self.assertEqual([timeouts[name] for name in TEST_NAMES], [3600, 3600, 3600])

    def test_debug_override_positive_seconds_win(self) -> None:
        timeouts = self.configure_timeouts(
            {"AVA_DEBUG_NO_TIMEOUT": "1", "AVA_DEBUG_NO_TIMEOUT_SECONDS": "90"}
        )
        self.assertEqual([timeouts[name] for name in TEST_NAMES], [90, 90, 90])

    def test_debug_override_rejects_non_positive_integer(self) -> None:
        for invalid in ("0", "abc", "-5", "12.5"):
            with self.subTest(invalid=invalid):
                timeouts = self.configure_timeouts(
                    {"AVA_DEBUG_NO_TIMEOUT": "1", "AVA_DEBUG_NO_TIMEOUT_SECONDS": invalid}
                )
                self.assertEqual([timeouts[name] for name in TEST_NAMES], [3600, 3600, 3600])
                self.tearDown()
                self.setUp()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", required=True, type=Path)
    parser.add_argument("--cmake", required=True, type=Path)
    args = parser.parse_args()

    ctest = args.cmake.with_name("ctest")
    if not ctest.exists():
        discovered = shutil.which("ctest")
        if discovered is None:
            print("error: ctest not found next to --cmake or on PATH", file=sys.stderr)
            return 2
        ctest = Path(discovered)

    TimeoutPolicyTests.helper = args.helper.resolve()
    TimeoutPolicyTests.cmake = args.cmake.resolve()
    TimeoutPolicyTests.ctest = ctest.resolve()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(TimeoutPolicyTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
