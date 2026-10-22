#!/usr/bin/env python3
"""Isolated wrapper contracts; each case has a private workspace and child owner.

The outer process keeps the workspace alive until benchmark-backend's Linux
subreaper has killed and reaped all descendants, including detached workers and
failures before a PID marker is consumed. No cleanup trusts PID-file values.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest

from timeout_support import test_timeout


FAKE_WORKER = r'''#!/usr/bin/env python3
import json, os, pathlib, signal, subprocess, sys, time
ready = os.environ.get("FAKE_CTEST_READY")
release = os.environ.get("FAKE_CTEST_RELEASE")
descendant = os.environ.get("FAKE_CTEST_DESCENDANT_READY")
if os.environ.get("FAKE_CTEST_IGNORE_TERM") == "1":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
if descendant:
    subprocess.Popen([sys.executable, "-c",
        "import os,pathlib,signal,sys,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); "
        "p=pathlib.Path(sys.argv[1]); t=p.with_name('.'+p.name+'.tmp'); "
        "t.write_text(str(os.getpid())); os.replace(t,p); time.sleep(60)", descendant],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)
if ready:
    p = pathlib.Path(ready)
    t = p.with_name('.'+p.name+'.tmp')
    t.write_text(str(os.getpid()))
    os.replace(t,p)
if release:
    deadline = time.monotonic() + float(os.environ["FAKE_CTEST_DEADLINE"])
    while not pathlib.Path(release).exists():
        if time.monotonic() >= deadline:
            raise SystemExit(9)
        time.sleep(0.01)
pathlib.Path(os.environ["FAKE_CTEST_ARGS"]).write_text(json.dumps(sys.argv[1:]))
'''


def wait_for_path(path: Path, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + test_timeout(5)
    while not path.exists():
        if process.poll() is not None or time.monotonic() >= deadline:
            raise AssertionError(f"worker did not publish {path.name}; wrapper status={process.returncode}")
        time.sleep(0.01)


def wait_for_process_exit(pid: int) -> None:
    deadline = time.monotonic() + test_timeout(5)
    while time.monotonic() < deadline:
        try:
            state = Path(f"/proc/{pid}/stat").read_text().split()[2]
        except FileNotFoundError:
            return
        if state == "Z":
            return
        time.sleep(0.01)
    raise AssertionError(f"worker {pid} did not exit")


class RunnerTests(unittest.TestCase):
    root: Path
    script: Path
    build_script: Path
    injection: str = ""

    def setUp(self) -> None:
        self.build = self.root / "build"
        self.build.mkdir()
        (self.build / "CTestTestfile.cmake").write_text("# fake configured tree\n")
        (self.build / "CMakeCache.txt").write_text("# fake configured tree\n")
        self.invocation = self.root / "args.json"
        self.worker = self.root / "worker.py"
        self.worker.write_text(FAKE_WORKER)
        self.worker.chmod(0o755)
        self.env = os.environ.copy()
        self.env.pop("CTEST_PARALLEL_LEVEL", None)
        self.env.pop("CMAKE_BUILD_PARALLEL_LEVEL", None)
        self.env.update(AVA_CTEST_COMMAND=str(self.worker), AVA_CMAKE_COMMAND=str(self.worker),
                        FAKE_CTEST_ARGS=str(self.invocation), FAKE_CTEST_DEADLINE=str(test_timeout(8)))

    def run_wrapper(self, *arguments: str, build: bool = False, environment=None):
        return subprocess.run(
            [str(self.build_script if build else self.script), "--build-dir", str(self.build), *arguments],
            capture_output=True, text=True, check=False, timeout=test_timeout(10),
            env=self.env if environment is None else environment,
        )

    def invocation_args(self):
        return json.loads(self.invocation.read_text())

    def test_explicit_test_jobs_and_filter(self):
        result = self.run_wrapper("--jobs", "7", "-R", "^ava_tests\\.lsp$")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("7 parallel jobs", result.stdout)
        self.assertEqual(self.invocation_args(), ["--test-dir", str(self.build), "--output-on-failure",
                         "--parallel", "7", "-R", "^ava_tests\\.lsp$"])

    def test_inherited_test_jobs(self):
        result = self.run_wrapper("--output-junit", str(self.root / "results.xml"),
                                  environment={**self.env, "CTEST_PARALLEL_LEVEL": "3"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.invocation_args()[3:5], ["--parallel", "3"])

    def test_invalid_parallelism(self):
        for build in (False, True):
            with self.subTest(build=build):
                result = self.run_wrapper("--jobs", "0", build=build)
                self.assertEqual(result.returncode, 2)
                self.assertIn("positive integer", result.stderr)
                self.assertFalse(self.invocation.exists())

    def test_rejected_ctest_modes(self):
        other = self.root / "other"
        other.mkdir()
        (other / "CTestTestfile.cmake").write_text("# other configured tree\n")
        for arguments, diagnostic in (
            (("--", "--test-dir", str(other)), "post-boundary"),
            (("--build-and-test", str(self.root / "source"), str(other)), "unsupported"),
            (("--collect-instrumentation", str(other)), "instrumentation modes"),
        ):
            with self.subTest(arguments=arguments):
                result = self.run_wrapper(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertIn(diagnostic, result.stderr)
                self.assertFalse(self.invocation.exists())

    def test_explicit_build_jobs_and_target(self):
        result = self.run_wrapper("--jobs", "6", "--target", "ava_tests", build=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("6 parallel jobs", result.stdout)
        self.assertEqual(self.invocation_args(), ["--build", str(self.build), "--parallel", "6", "--target", "ava_tests"])

    def test_inherited_build_jobs(self):
        result = self.run_wrapper("--verbose", build=True, environment={**self.env, "CMAKE_BUILD_PARALLEL_LEVEL": "4"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.invocation_args()[2:4], ["--parallel", "4"])

    def test_native_build_delimiter(self):
        result = self.run_wrapper("--jobs", "5", "--", "-n", build=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.invocation_args(), ["--build", str(self.build), "--parallel", "5", "--", "-n"])

    def start_blocked(self, detached=False):
        env = {**self.env, "FAKE_CTEST_READY": str(self.root / "ready"), "FAKE_CTEST_RELEASE": str(self.root / "release")}
        if detached:
            env.update(FAKE_CTEST_DESCENDANT_READY=str(self.root / "descendant"), FAKE_CTEST_IGNORE_TERM="1")
        # The enclosing child owner, not teardown PID files, owns every worker.
        process = subprocess.Popen([str(self.script), "--build-dir", str(self.build), "--jobs", "2"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, env=env, start_new_session=True)
        if self.injection == "spawn":
            raise AssertionError("injected failure: spawn")
        wait_for_path(self.root / "ready", process)
        if self.injection == "ready":
            raise AssertionError("injected failure: ready")
        return process

    def test_shared_build_tree_lock(self):
        first = self.start_blocked()
        result = self.run_wrapper("--jobs", "2")
        self.assertEqual(result.returncode, 2)
        self.assertIn("already owns build tree", result.stderr)
        result = self.run_wrapper("--jobs", "2", build=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("already owns build tree", result.stderr)
        (self.root / "release").write_text("release\n")
        stdout, stderr = first.communicate(timeout=test_timeout(10))
        self.assertEqual(first.returncode, 0, stdout + stderr)

    def test_signal_retains_fail_closed_lock(self):
        process = self.start_blocked(detached=True)
        wait_for_path(self.root / "descendant", process)
        if self.injection == "descendant":
            raise AssertionError("injected failure: descendant")
        os.killpg(process.pid, signal.SIGTERM)
        result = self.run_wrapper("--jobs", "2", build=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("another AVA build or test run", result.stderr)
        os.killpg(process.pid, signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=test_timeout(10))
        self.assertIn(process.returncode, (-15, 143), stdout + stderr)
        result = self.run_wrapper("--jobs", "2")
        self.assertEqual(result.returncode, 2)
        self.assertIn("stale AVA build-tree lock", result.stderr)
        # Recovery is tested after the enclosing owner has reaped the detached
        # fixture, in its own subsequent invocation below.

    def test_sigkill_preserves_lock_until_manual_recovery(self):
        process = self.start_blocked()
        worker_pid = int((self.root / "ready").read_text())
        process.kill()
        process.wait(timeout=test_timeout(5))
        result = self.run_wrapper("--jobs", "2", build=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("stale AVA build-tree lock", result.stderr)
        (self.root / "release").write_text("release\n")
        wait_for_process_exit(worker_pid)
        shutil.rmtree(self.build / ".ava-build-tree.lock.d")
        result = self.run_wrapper("--jobs", "2")
        self.assertEqual(result.returncode, 0, result.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path, required=True)
    parser.add_argument("--build-script", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--case")
    parser.add_argument("--inject", default="")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        print("skipping runner ownership tests: Linux procfs is required")
        return 77
    RunnerTests.script = args.script.resolve(strict=True)
    RunnerTests.build_script = args.build_script.resolve(strict=True)
    RunnerTests.root = args.root.absolute()
    RunnerTests.injection = args.inject
    if args.case:
        result = unittest.TextTestRunner(verbosity=2).run(unittest.TestSuite([RunnerTests(args.case)]))
        return 0 if result.wasSuccessful() else 1

    parent = args.root.absolute()
    parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    status = parent.lstat()
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise RuntimeError(f"test root must be a real directory: {parent}")
    spec = importlib.util.spec_from_file_location("ava_benchmark_owner", args.script.parent / "benchmark-backend.py")
    owner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(owner)

    def run_case(name, injection=""):
        with tempfile.TemporaryDirectory(prefix="ava-parallel-runner-test.", dir=parent) as temporary:
            root = Path(temporary)
            command = [sys.executable, str(Path(__file__).resolve()), "--script", str(RunnerTests.script),
                       "--build-script", str(RunnerTests.build_script), "--root", str(root), "--case", name]
            if injection:
                command.extend(["--inject", injection])
            _, result = owner.run_process(command, root, os.environ.copy(), test_timeout(20))
            if injection:
                if result.returncode == 0 or f"injected failure: {injection}" not in result.stderr:
                    raise AssertionError(result.stdout + result.stderr)
            elif result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            for marker in (root / "ready", root / "descendant"):
                if marker.exists():
                    pid = int(marker.read_text())
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        pass
                    else:
                        raise AssertionError(f"owned worker {pid} survived case {name}/{injection}")
            if name == "test_signal_retains_fail_closed_lock" and not injection:
                shutil.rmtree(root / "build" / ".ava-build-tree.lock.d")
                environment = {**os.environ, "AVA_CTEST_COMMAND": str(root / "worker.py"),
                               "FAKE_CTEST_ARGS": str(root / "recovered.json")}
                _, recovered = owner.run_process(
                    [str(RunnerTests.script), "--build-dir", str(root / "build"), "--jobs", "2"],
                    root, environment, test_timeout(10),
                )
                if recovered.returncode != 0:
                    raise AssertionError(f"manual lock recovery failed: {recovered.stderr}")
        if root.exists():
            raise AssertionError(f"case workspace survived cleanup: {root}")

    cases = [(name, "") for name in unittest.defaultTestLoader.getTestCaseNames(RunnerTests)]
    cases.extend(("test_signal_retains_fail_closed_lock", boundary) for boundary in ("spawn", "ready", "descendant"))
    suite = unittest.TestSuite(unittest.FunctionTestCase(lambda n=name, i=injection: run_case(n, i),
                              description=f"{name}/{injection or 'normal'}") for name, injection in cases)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
