#!/usr/bin/env python3
"""Fast regressions for benchmark self-test grouping and fixture Git isolation."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

TESTS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
HARNESS_PATH = TESTS_DIR / "backend_benchmark_test.py"
SCRIPT_PATH = REPO_ROOT / "scripts" / "benchmark-backend.py"

if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

import backend_benchmark_test as harness  # noqa: E402
from timeout_support import test_timeout


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
    return frozenset(names[:13])


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
    def test_benchmark_temporary_directory_is_named_and_removed_on_failure(self) -> None:
        """An exceptional fixture exit removes only its owned /tmp/ava-* root."""
        with self.assertRaisesRegex(RuntimeError, "fixture failure"):
            with harness.benchmark_temporary_directory() as temporary:
                root = pathlib.Path(temporary)
                self.assertEqual(root.parent, pathlib.Path("/tmp"))
                self.assertTrue(root.name.startswith("ava-bench-test-"))
                (root / "fixture").write_text("test", encoding="utf-8")
                raise RuntimeError("fixture failure")
        self.assertFalse(root.exists())

    def test_live_comparison_reuses_private_seed_but_isolates_worktrees(self) -> None:
        """Two provenance fixtures share only private Git objects, not writable worktrees."""
        class FixtureCase(harness.BenchmarkHarnessTests):
            script = SCRIPT_PATH

        FixtureCase.setUpClass()
        try:
            case = FixtureCase("test_comparison_resolves_recorded_commits_and_trees")
            required_paths = (
                *case.module.PROCESS_PRODUCTION_SOURCE_PATHS,
                ".gitmodules",
                "scripts/benchmark-backend.py",
            )
            source_path = "src/ava/tools/bash_tool.cpp"
            source_original = (REPO_ROOT / source_path).read_bytes()
            with mock.patch.object(harness, "run_fixture_git", wraps=harness.run_fixture_git) as git:
                roots = []
                for _ in range(2):
                    with case.live_plugin_comparison_documents() as documents:
                        roots.append(documents[2])
                        self.assertEqual(roots[-1].parent.parent, pathlib.Path("/tmp"))
                        self.assertTrue(roots[-1].parent.name.startswith("ava-bench-test-"))
                        self.assertTrue(documents[2].exists())
                        self.assertTrue(documents[3].exists())
                        self.assertTrue(documents[4].exists())
                        for worktree in documents[2:]:
                            for path in required_paths:
                                self.assertTrue((worktree / path).exists(), path)
                            for omitted in ("apps", "docs"):
                                self.assertFalse((worktree / omitted).exists(), omitted)
                            entries = case.module._scope_entries_from_git(
                                worktree, "HEAD", required_paths, ()
                            )
                            for entry in entries:
                                path = worktree / entry["path"]
                                mode = path.lstat().st_mode
                                if entry["mode"] == "160000":
                                    self.assertTrue(stat.S_ISDIR(mode), path)
                                    continue
                                self.assertIn(entry["mode"], ("100644", "100755"), path)
                                self.assertTrue(stat.S_ISREG(mode), path)
                                self.assertEqual(
                                    bool(mode & stat.S_IXUSR), entry["mode"] == "100755", path
                                )
                                content = path.read_bytes()
                                blob = b"blob " + str(len(content)).encode("ascii") + b"\0" + content
                                self.assertEqual(hashlib.sha1(blob).hexdigest(), entry["object"], path)
                        comparison = case.module.compare_process_documents(*documents[:2])
                        self.assertEqual(comparison["status"], "measured")

                        # Leave this dirty until cleanup: the next fixture must still
                        # match committed bytes, and neither source nor seed may change.
                        changed = documents[2] / source_path
                        original = changed.read_bytes()
                        changed.write_bytes(original + b"\nprivate fixture mutation\n")
                        comparison = case.module.compare_process_documents(*documents[:2])
                        self.assertEqual(comparison["status"], "unsupported")
                        self.assertIn("before.measured_checkout.current_dirty", comparison["mismatches"])
                        self.assertIn(
                            "before.runtime_reference.current_production_path_equality",
                            comparison["mismatches"],
                        )
                        self.assertIn(
                            "before.runtime_reference.current_production_paths_dirty",
                            comparison["mismatches"],
                        )
                        self.assertEqual((REPO_ROOT / source_path).read_bytes(), source_original)
                        self.assertEqual(
                            case.module._git_blob_at_path(
                                FixtureCase._comparison_seed,
                                documents[0]["provenance"]["measured_checkout"]["commit"],
                                source_path,
                            )[1],
                            original,
                        )
                        alternates = roots[-1].parent / "repository/.git/objects/info/alternates"
                        self.assertEqual(
                            alternates.read_text(encoding="utf-8").splitlines(),
                            [str(FixtureCase._comparison_seed / ".git/objects")],
                        )
                    self.assertFalse(roots[-1].exists())
                    self.assertFalse(roots[-1].parent.exists())

                worktree_adds = [call.args for call in git.call_args_list if call.args[1:3] == ("worktree", "add")]
                self.assertEqual(len(worktree_adds), 6)
                self.assertTrue(all("--no-checkout" in args for args in worktree_adds))
                clones = [call.args for call in git.call_args_list if len(call.args) > 1 and call.args[1] == "clone"]
                self.assertEqual(len([args for args in clones if "--no-local" in args]), 1)
                self.assertEqual(len([args for args in clones if "--shared" in args]), 2)
                self.assertNotEqual(roots[0], roots[1])
                self.assertTrue(FixtureCase._comparison_seed.exists())
                seed = FixtureCase._comparison_seed
                self.assertEqual(seed.parent.parent, pathlib.Path("/tmp"))
                self.assertTrue(seed.parent.name.startswith("ava-bench-comparison-seed-"))
                self.assertFalse((seed / ".git/objects/info/alternates").exists())
        finally:
            FixtureCase.doClassCleanups()
        self.assertFalse(seed.exists())

    def load_group(
        self,
        process_tests: bool,
        patterns: list[str] | None = None,
        selector: str | None = None,
        live_git_tests: bool = False,
    ):
        loader = harness.BenchmarkTestLoader(process_tests)
        loader.live_git_tests = live_git_tests
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
            timeout=test_timeout(timeout),
            env=environment,
            check=False,
        )

    def test_loader_classification_matches_fixture_dependencies(self) -> None:
        names = frozenset(discovered_method_names())
        self.assertTrue(harness.LIVE_GIT_TESTS)
        self.assertLess(harness.LIVE_GIT_TESTS, names)
        self.assertTrue(all(harness.BenchmarkTestLoader.is_process_test(name) for name in harness.LIVE_GIT_TESTS))
        self.assertTrue(expected_general_method_names().isdisjoint(harness.LIVE_GIT_TESTS))

    def test_ctest_entries_preserve_scheduling_contract(self) -> None:
        build = os.environ.get("AVA_TEST_BUILD_DIR")
        if build is None:
            self.skipTest("effective registration checks require AVA_TEST_BUILD_DIR")
        completed = subprocess.run(
            ["ctest", "--test-dir", build, "--show-only=json-v1"],
            check=True, capture_output=True, text=True, timeout=test_timeout(15),
        )
        tests = {
            test["name"]: {prop["name"]: prop["value"] for prop in test["properties"]}
            for test in json.loads(completed.stdout)["tests"]
        }
        for name in ("ava_benchmark.harness_self_test", "ava_benchmark.process_harness_self_test", "ava_tests.benchmark_driver"):
            properties = tests[name]
            self.assertGreater(properties["TIMEOUT"], 0, name)
            self.assertTrue({"benchmark", "offline", "self-test"} <= set(properties["LABELS"]), name)
        live = tests["ava_benchmark.live_git_self_test"]
        self.assertGreaterEqual(live["TIMEOUT"], 120)
        self.assertTrue(live["RUN_SERIAL"])
        self.assertIn("ava_benchmark_git_fixture", live["RESOURCE_LOCK"])
        for name in ("ava_benchmark.smoke", "ava_benchmark.process_smoke"):
            self.assertTrue(tests[name]["RUN_SERIAL"], name)
        for name, properties in tests.items():
            if name.startswith("ava_tests."):
                self.assertEqual(properties["SKIP_RETURN_CODE"], 77, name)

    def test_loader_groups_are_disjoint_and_complete(self) -> None:
        names = frozenset(discovered_method_names())
        expected_general = expected_general_method_names()
        general = suite_case_names(self.load_group(False))
        process = suite_case_names(self.load_group(True))
        live = suite_case_names(self.load_group(True, live_git_tests=True))
        optional = {harness.OPTIONAL_ARTIFACT_TEST}
        self.assertEqual(general, expected_general)
        self.assertEqual(live, harness.LIVE_GIT_TESTS)
        self.assertEqual(process, names - general - live - optional)
        self.assertFalse(general & process or live & process or live & general)
        self.assertEqual(general | process | live | optional, names)

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
        self.assertEqual(general_class, expected_general_method_names())
        self.assertEqual(process_class, frozenset(discovered_method_names()) - general_class
                         - harness.LIVE_GIT_TESTS - {harness.OPTIONAL_ARTIFACT_TEST})

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


@unittest.skipUnless(sys.platform.startswith("linux"), "descendant custody requires Linux procfs")
class OwnedProcessTests(unittest.TestCase):
    def run_owned_fixture(self, boundary: str) -> None:
        module = harness.load_module(SCRIPT_PATH)
        with tempfile.TemporaryDirectory(prefix="ava-bench-owner-") as temporary:
            root = pathlib.Path(temporary)
            marker = root / "descendant"
            child = (
                "import os,pathlib,signal,sys,time; "
                "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                "pathlib.Path(sys.argv[1]).write_text(str(os.getpid())); time.sleep(60)"
            )
            leader = (
                "import pathlib,subprocess,sys,time; "
                "subprocess.Popen([sys.executable,'-c',sys.argv[1],sys.argv[2]],start_new_session=True); "
                "marker=pathlib.Path(sys.argv[2]); "
                "exec('while not marker.exists(): time.sleep(0.001)'); "
                "time.sleep(60 if sys.argv[3]!='normal' else 0)"
            )
            command = [sys.executable, "-c", leader, child, str(marker), boundary]
            original = subprocess.Popen.communicate
            first = True

            def communicate(process, *args, **kwargs):
                nonlocal first
                if first and boundary != "normal":
                    first = False
                    deadline = time.monotonic() + test_timeout(5)
                    while not marker.exists():
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise AssertionError("owned descendant did not publish readiness")
                        time.sleep(0.005)
                    if boundary == "cancel":
                        os.kill(os.getpid(), signal.SIGTERM)
                    if boundary == "exception":
                        raise RuntimeError("injected collection failure")
                    kwargs["timeout"] = 0.02
                return original(process, *args, **kwargs)

            with mock.patch.object(subprocess.Popen, "communicate", communicate):
                if boundary == "timeout":
                    with self.assertRaises(subprocess.TimeoutExpired):
                        module.run_process(command, root, os.environ.copy(), test_timeout(5))
                elif boundary == "cancel":
                    with self.assertRaises(SystemExit) as raised:
                        module.run_process(command, root, os.environ.copy(), test_timeout(5))
                    self.assertEqual(raised.exception.code, 143)
                elif boundary == "exception":
                    with self.assertRaisesRegex(RuntimeError, "injected collection failure"):
                        module.run_process(command, root, os.environ.copy(), test_timeout(5))
                else:
                    elapsed, completed = module.run_process(command, root, os.environ.copy(), test_timeout(5))
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    self.assertGreater(elapsed, 0)
            pid = int(marker.read_text())
            with self.assertRaises(ProcessLookupError, msg=f"owned descendant {pid} survived {boundary}"):
                os.kill(pid, 0)

    def test_timeout_reaps_detached_term_refusing_descendant(self) -> None:
        self.run_owned_fixture("timeout")

    def test_cancel_reaps_detached_term_refusing_descendant(self) -> None:
        self.run_owned_fixture("cancel")

    def test_collection_failure_reaps_detached_descendant(self) -> None:
        self.run_owned_fixture("exception")

    def test_success_reaps_unfinished_descendant(self) -> None:
        self.run_owned_fixture("normal")


def main() -> int:
    live = "--live-git-tests" in sys.argv
    if live:
        sys.argv.remove("--live-git-tests")
    loader = unittest.defaultTestLoader
    names = loader.getTestCaseNames(BenchmarkDriverTests)
    live_driver = "test_live_comparison_reuses_private_seed_but_isolates_worktrees"
    suite = unittest.TestSuite(
        BenchmarkDriverTests(name) for name in names if (name == live_driver) == live
    )
    if not live:
        suite.addTests(loader.loadTestsFromTestCase(OwnedProcessTests))
    if live:
        harness.BenchmarkHarnessTests.script = SCRIPT_PATH
        live_loader = harness.BenchmarkTestLoader(True)
        live_loader.live_git_tests = True
        suite.addTests(live_loader.loadTestsFromTestCase(harness.BenchmarkHarnessTests))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 1 if not result.wasSuccessful() else 77 if result.skipped else 0


if __name__ == "__main__":
    raise SystemExit(main())
