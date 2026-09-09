#!/usr/bin/env python3
"""Fast regressions for benchmark self-test grouping and fixture Git isolation."""

from __future__ import annotations

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

TESTS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
HARNESS_PATH = TESTS_DIR / "backend_benchmark_test.py"
SCRIPT_PATH = REPO_ROOT / "scripts" / "benchmark-backend.py"

if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

import backend_benchmark_test as harness  # noqa: E402


def discovered_method_names() -> list[str]:
    return [
        name
        for name, value in harness.BenchmarkHarnessTests.__dict__.items()
        if name.startswith("test_") and callable(value)
    ]


def iter_suite_tests(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from iter_suite_tests(item)
        else:
            yield item


def suite_case_names(suite) -> set[str]:
    return {test.id().rsplit(".", 1)[-1] for test in iter_suite_tests(suite)}


def expected_general_method_names() -> frozenset[str]:
    names = discovered_method_names()
    return frozenset([*names[:13], "test_historical_v2_artifact_still_validates"])


def ran_count(completed: subprocess.CompletedProcess[str]) -> int | None:
    match = re.search(r"Ran (\d+) tests?", f"{completed.stderr}\n{completed.stdout}")
    return int(match.group(1)) if match else None


def write_executable(path: pathlib.Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    path.chmod(0o755)


def write_canary(path: pathlib.Path, marker: pathlib.Path, code: int = 42) -> None:
    write_executable(
        path,
        "#!/bin/sh\n"
        f"printf 'executed\\n' > '{marker}'\n"
        f"exit {code}\n",
    )


class BenchmarkDriverTests(unittest.TestCase):
    def load_group(
        self,
        process_tests: bool,
        patterns: list[str] | None = None,
        selector: str | None = None,
    ):
        loader = harness.BenchmarkTestLoader(process_tests)
        if patterns is not None:
            loader.testNamePatterns = patterns
        if selector is not None:
            return loader.loadTestsFromName(selector, harness)
        return loader.loadTestsFromModule(harness)

    def run_harness(self, *args: str, script: pathlib.Path | str | None = None, timeout: int = 10):
        command = [
            sys.executable,
            str(HARNESS_PATH),
            "--script",
            str(script if script is not None else "/nonexistent-ava-benchmark-script"),
            *args,
        ]
        environment = os.environ.copy()
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        return subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            env=environment,
            check=False,
        )

    def test_loader_classification_matches_first_thirteen_plus_historical_v2(self) -> None:
        names = discovered_method_names()
        expected_general = expected_general_method_names()
        classified_general = frozenset(
            name for name in names if not harness.BenchmarkTestLoader.is_process_test(name)
        )
        process_names = frozenset(names) - classified_general
        self.assertEqual(len(names), 70)
        self.assertEqual(classified_general, expected_general)
        self.assertEqual(len(classified_general), 14)
        self.assertEqual(len(process_names), 56)
        self.assertTrue(classified_general.isdisjoint(process_names))
        self.assertEqual(classified_general | process_names, frozenset(names))

    def test_ctest_entries_preserve_both_names_and_timeouts(self) -> None:
        cmake = (TESTS_DIR / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("NAME ava_benchmark.harness_self_test", cmake)
        self.assertIn("NAME ava_benchmark.process_harness_self_test", cmake)
        self.assertIn("--process-tests", cmake)
        self.assertRegex(
            cmake,
            r"set_tests_properties\(ava_benchmark\.harness_self_test PROPERTIES\s+TIMEOUT 30",
        )
        self.assertRegex(
            cmake,
            r"set_tests_properties\(ava_benchmark\.process_harness_self_test PROPERTIES\s+TIMEOUT 30",
        )
        self.assertRegex(
            cmake,
            r"set_tests_properties\(ava_tests\.model_selector_wait PROPERTIES TIMEOUT 15",
        )
        self.assertRegex(
            cmake,
            r"set_tests_properties\(ava_tests\.benchmark_driver PROPERTIES TIMEOUT 15",
        )

    def test_loader_groups_are_disjoint_and_complete(self) -> None:
        names = frozenset(discovered_method_names())
        expected_general = expected_general_method_names()
        general = suite_case_names(self.load_group(False))
        process = suite_case_names(self.load_group(True))
        self.assertEqual(general, expected_general)
        self.assertEqual(process, names - expected_general)
        self.assertEqual(len(general), 14)
        self.assertEqual(len(process), 56)
        self.assertFalse(general & process)
        self.assertEqual(general | process, names)

    def test_k_filter_stays_inside_selected_group(self) -> None:
        general_k = suite_case_names(self.load_group(False, ["*statistics_use_nearest*"]))
        process_k = suite_case_names(self.load_group(True, ["*statistics_use_nearest*"]))
        self.assertEqual(general_k, {"test_statistics_use_nearest_rank_p95"})
        self.assertEqual(process_k, set())

        process_schema = suite_case_names(
            self.load_group(True, ["*process_schema_preserves_order*"])
        )
        general_schema = suite_case_names(
            self.load_group(False, ["*process_schema_preserves_order*"])
        )
        self.assertEqual(
            process_schema,
            {"test_process_schema_preserves_order_raw_correlation_and_metric_statistics"},
        )
        self.assertEqual(general_schema, set())

    def test_cli_discovery_and_k_stay_in_selected_group(self) -> None:
        excluded_process = self.run_harness(
            "--process-tests",
            "-k",
            "test_statistics_use_nearest_rank_p95",
        )
        self.assertEqual(excluded_process.returncode, 0, excluded_process.stderr)
        self.assertEqual(ran_count(excluded_process), 0)

        excluded_general = self.run_harness(
            "-k",
            "test_process_schema_preserves_order_raw_correlation_and_metric_statistics",
        )
        self.assertEqual(excluded_general.returncode, 0, excluded_general.stderr)
        self.assertEqual(ran_count(excluded_general), 0)

        selected = self.run_harness(
            "-v",
            "-k",
            "test_statistics_use_nearest_rank_p95",
            script=SCRIPT_PATH,
        )
        self.assertEqual(selected.returncode, 0, selected.stderr)
        self.assertEqual(ran_count(selected), 1)
        self.assertIn("test_statistics_use_nearest_rank_p95", f"{selected.stderr}\n{selected.stdout}")

    def test_explicit_selectors_override_automatic_grouping(self) -> None:
        general_class = suite_case_names(
            self.load_group(False, selector="BenchmarkHarnessTests")
        )
        process_class = suite_case_names(
            self.load_group(True, selector="BenchmarkHarnessTests")
        )
        self.assertEqual(len(general_class), 14)
        self.assertEqual(len(process_class), 56)

        process_without_flag = self.run_harness(
            "-v",
            "BenchmarkHarnessTests.test_process_schema_preserves_order_raw_correlation_and_metric_statistics",
            script=SCRIPT_PATH,
        )
        self.assertEqual(process_without_flag.returncode, 0, process_without_flag.stderr)
        self.assertEqual(ran_count(process_without_flag), 1)
        self.assertIn(
            "test_process_schema_preserves_order_raw_correlation_and_metric_statistics",
            f"{process_without_flag.stderr}\n{process_without_flag.stdout}",
        )

        general_with_flag = self.run_harness(
            "--process-tests",
            "-v",
            "BenchmarkHarnessTests.test_statistics_use_nearest_rank_p95",
            script=SCRIPT_PATH,
        )
        self.assertEqual(general_with_flag.returncode, 0, general_with_flag.stderr)
        self.assertEqual(ran_count(general_with_flag), 1)
        self.assertIn(
            "test_statistics_use_nearest_rank_p95",
            f"{general_with_flag.stderr}\n{general_with_flag.stdout}",
        )

    def test_invalid_selectors_fail_nonzero(self) -> None:
        for selector in (
            "BenchmarkHarnessTests.test_does_not_exist",
            "NoSuchBenchmarkModule",
            "BenchmarkHarnessTests.script",
        ):
            with self.subTest(selector=selector):
                completed = self.run_harness(selector)
                output = f"{completed.stderr}\n{completed.stdout}"
                self.assertNotEqual(completed.returncode, 0, output)
                self.assertIsNone(re.search(r"Ran 0 tests?\b", output))

    def test_sanitize_strips_terminal_controls_and_bounds_output(self) -> None:
        payload = "\x1b[31msecret=value\x1b[0m\x07" + ("x" * 5000)
        cleaned = harness.sanitize_fixture_git_output(payload)
        self.assertNotIn("\x1b", cleaned)
        self.assertNotIn("\x07", cleaned)
        self.assertLessEqual(len(cleaned), harness._FIXTURE_GIT_OUTPUT_LIMIT)
        self.assertIn("secret=value", cleaned)

    def test_checked_git_failure_is_actionable_and_cleanup_stays_quiet(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ava-bench-driver-fail-") as temporary:
            missing = pathlib.Path(temporary) / "not-a-repo"
            missing.mkdir()
            with self.assertRaises(harness.FixtureGitError) as context:
                harness.run_fixture_git(missing, "status")
            error = context.exception
            rendered = str(error)
            self.assertEqual(error.operation, "status")
            self.assertEqual(error.path, str(missing))
            self.assertNotEqual(error.exit_code, 0)
            self.assertIn("status", rendered)
            self.assertIn(str(missing), rendered)
            self.assertIn(str(error.exit_code), rendered)
            self.assertTrue(error.stderr)
            self.assertNotIn("\x1b", rendered)
            self.assertNotIn("GIT_CONFIG_GLOBAL", rendered)
            self.assertNotIn("GIT_ASKPASS", rendered)
            self.assertNotIn("credential", rendered.lower())
            self.assertLessEqual(len(rendered), 4096)
            self.assertEqual(harness.run_fixture_git(missing, "status", check=False), "")

        with self.assertRaises(ValueError):
            harness.run_fixture_git(pathlib.Path("/tmp"), "status", allow_file_protocol=True)

    def test_private_global_and_local_hooks_and_signing_are_not_executed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ava-bench-driver-hooks-") as temporary:
            root = pathlib.Path(temporary)
            markers = {
                "global_pre_commit": root / "global-pre-commit.marker",
                "global_post_checkout": root / "global-post-checkout.marker",
                "local_pre_commit": root / "local-pre-commit.marker",
                "local_post_checkout": root / "local-post-checkout.marker",
                "repo_hook": root / "repo-hook.marker",
                "gpg": root / "gpg.marker",
            }
            global_hooks = root / "hostile-global-hooks"
            local_hooks = root / "hostile-local-hooks"
            write_canary(global_hooks / "pre-commit", markers["global_pre_commit"])
            write_canary(global_hooks / "post-checkout", markers["global_post_checkout"])
            write_canary(local_hooks / "pre-commit", markers["local_pre_commit"])
            write_canary(local_hooks / "post-checkout", markers["local_post_checkout"])
            gpg_program = root / "hostile-gpg"
            write_canary(gpg_program, markers["gpg"], code=1)
            hostile_global = root / "hostile-global.gitconfig"
            hostile_global.write_text(
                "\n".join(
                    (
                        "[core]",
                        f"\thooksPath = {global_hooks}",
                        "[commit]",
                        "\tgpgSign = true",
                        "[gpg]",
                        f"\tprogram = {gpg_program}",
                        "",
                    )
                ),
                encoding="utf-8",
            )
            repository = root / "repository"
            linked = root / "linked"
            repository.mkdir()
            previous = {
                "GIT_CONFIG_GLOBAL": os.environ.get("GIT_CONFIG_GLOBAL"),
                "GIT_CONFIG_SYSTEM": os.environ.get("GIT_CONFIG_SYSTEM"),
                "GIT_CONFIG_COUNT": os.environ.get("GIT_CONFIG_COUNT"),
                "GIT_CONFIG_KEY_0": os.environ.get("GIT_CONFIG_KEY_0"),
                "GIT_CONFIG_VALUE_0": os.environ.get("GIT_CONFIG_VALUE_0"),
            }
            os.environ["GIT_CONFIG_GLOBAL"] = str(hostile_global)
            os.environ["GIT_CONFIG_SYSTEM"] = str(hostile_global)
            os.environ["GIT_CONFIG_COUNT"] = "1"
            os.environ["GIT_CONFIG_KEY_0"] = "core.hooksPath"
            os.environ["GIT_CONFIG_VALUE_0"] = str(global_hooks)
            try:
                harness.run_fixture_git(repository, "init")
                write_canary(repository / ".git" / "hooks" / "pre-commit", markers["repo_hook"])
                write_canary(repository / ".git" / "hooks" / "post-checkout", markers["local_post_checkout"])
                harness.run_fixture_git(repository, "config", "core.hooksPath", str(local_hooks))
                harness.run_fixture_git(repository, "config", "commit.gpgSign", "true")
                harness.run_fixture_git(repository, "config", "gpg.program", str(gpg_program))
                (repository / "README").write_text("fixture\n", encoding="utf-8")
                harness.run_fixture_git(repository, "add", "README")
                harness.run_fixture_git(repository, "commit", "-m", "isolated")
                harness.run_fixture_git(repository, "worktree", "add", "--detach", str(linked), "HEAD")
            finally:
                if linked.exists():
                    harness.run_fixture_git(
                        repository,
                        "worktree",
                        "remove",
                        "--force",
                        str(linked),
                        check=False,
                    )
                for name, value in previous.items():
                    if value is None:
                        os.environ.pop(name, None)
                    else:
                        os.environ[name] = value

            self.assertTrue((repository / "README").is_file())
            self.assertFalse(linked.exists())
            for marker in markers.values():
                self.assertFalse(marker.exists(), marker)

    def test_hostile_git_redirects_use_owned_decoys_only(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ava-bench-driver-decoy-") as temporary:
            root = pathlib.Path(temporary)
            decoy = root / "decoy"
            decoy_work = root / "decoy-work"
            fixture = root / "fixture"
            decoy.mkdir()
            decoy_work.mkdir()
            fixture.mkdir()
            decoy_marker = decoy_work / "DECOY_MARKER"
            decoy_marker.write_text("owned-decoy\n", encoding="utf-8")
            decoy_hook_marker = root / "decoy-hook.marker"
            write_canary(root / "decoy-hooks" / "pre-commit", decoy_hook_marker)
            hostile_config = root / "hostile.gitconfig"
            hostile_config.write_text(
                f"[core]\n\thooksPath = {root / 'decoy-hooks'}\n",
                encoding="utf-8",
            )
            harness.run_fixture_git(decoy, "init")
            (decoy / "canary.txt").write_text("decoy\n", encoding="utf-8")
            harness.run_fixture_git(decoy, "add", "canary.txt")
            harness.run_fixture_git(decoy, "commit", "-m", "decoy")
            decoy_head = harness.run_fixture_git(decoy, "rev-parse", "HEAD")
            previous = {
                "GIT_DIR": os.environ.get("GIT_DIR"),
                "GIT_WORK_TREE": os.environ.get("GIT_WORK_TREE"),
                "GIT_CONFIG": os.environ.get("GIT_CONFIG"),
                "GIT_INDEX_FILE": os.environ.get("GIT_INDEX_FILE"),
                "GIT_OBJECT_DIRECTORY": os.environ.get("GIT_OBJECT_DIRECTORY"),
            }
            os.environ["GIT_DIR"] = str(decoy / ".git")
            os.environ["GIT_WORK_TREE"] = str(decoy_work)
            os.environ["GIT_CONFIG"] = str(hostile_config)
            os.environ["GIT_INDEX_FILE"] = str(root / "hostile.index")
            os.environ["GIT_OBJECT_DIRECTORY"] = str(decoy / ".git" / "objects")
            try:
                harness.run_fixture_git(fixture, "init")
                (fixture / "README").write_text("fixture\n", encoding="utf-8")
                harness.run_fixture_git(fixture, "add", "README")
                harness.run_fixture_git(fixture, "commit", "-m", "fixture")
            finally:
                for name, value in previous.items():
                    if value is None:
                        os.environ.pop(name, None)
                    else:
                        os.environ[name] = value

            self.assertEqual(harness.run_fixture_git(decoy, "rev-parse", "HEAD"), decoy_head)
            self.assertEqual(decoy_marker.read_text(encoding="utf-8"), "owned-decoy\n")
            self.assertFalse(decoy_hook_marker.exists())
            self.assertTrue((fixture / "README").is_file())
            self.assertNotEqual(harness.run_fixture_git(fixture, "rev-parse", "HEAD"), decoy_head)
            self.assertNotIn(str(REPO_ROOT), os.environ.get("GIT_DIR", ""))
            self.assertFalse((decoy_work / "README").exists())


if __name__ == "__main__":
    unittest.main()
