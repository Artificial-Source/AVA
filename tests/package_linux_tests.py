#!/usr/bin/env python3
from __future__ import annotations

import argparse
import ctypes
import errno
import hashlib
import importlib.util
import json
import os
import pathlib
import platform
import re
import secrets
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from typing import Callable


PROCESS_POLL_INTERVAL = 0.01
PROCESS_TERM_GRACE = 0.25
PROCESS_KILL_GRACE = 1.0
PROCESS_REAP_DEADLINE = 1.0
PROCESS_DRAIN_DEADLINE = 1.0
TERMINATION_SIGNALS = (signal.SIGINT, signal.SIGTERM)

# A process is registered only after start_new_session=True created and we
# verified its private session/process group.  The group ID is then safe to
# retain after its leader exits: it cannot be reused while descendants remain.
VERIFIED_OWNED_GROUPS: dict[subprocess.Popen[str], int] = {}
ASYNC_PACKAGE_PROCESSES: set[subprocess.Popen[str]] = set()
_termination_deferral_depth = 0
_deferred_termination_signal: int | None = None


class PackageTestTermination(BaseException):
    def __init__(self, signal_number: int) -> None:
        super().__init__(f"package test interrupted by signal {signal_number}")
        self.signal_number = signal_number


class PackageTestTerminationHandlers:
    """Turn terminal signals into exceptions while the harness owns processes."""

    def __init__(self) -> None:
        self.previous: dict[int, object] = {}

    def __enter__(self) -> PackageTestTerminationHandlers:
        global _deferred_termination_signal
        _deferred_termination_signal = None
        for signal_number in TERMINATION_SIGNALS:
            self.previous[signal_number] = signal.getsignal(signal_number)
            signal.signal(signal_number, handle_package_test_termination)
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> bool:
        global _deferred_termination_signal
        for signal_number, previous in self.previous.items():
            signal.signal(signal_number, previous)
        _deferred_termination_signal = None
        return False


class PackageTestTerminationDeferral:
    """Defer cancellation until an owned process is registered or cleaned up."""

    def __enter__(self) -> PackageTestTerminationDeferral:
        global _termination_deferral_depth
        _termination_deferral_depth += 1
        return self

    def __exit__(self, _type: object, _value: object, _traceback: object) -> bool:
        global _termination_deferral_depth
        _termination_deferral_depth -= 1
        if _termination_deferral_depth < 0:
            raise RuntimeError("package-test termination deferral underflow")
        return False


def handle_package_test_termination(signal_number: int, _frame: object) -> None:
    global _deferred_termination_signal
    if _termination_deferral_depth:
        if _deferred_termination_signal is None:
            _deferred_termination_signal = signal_number
        return
    raise PackageTestTermination(signal_number)


def raise_deferred_package_test_termination() -> None:
    global _deferred_termination_signal
    if _termination_deferral_depth or _deferred_termination_signal is None:
        return
    signal_number = _deferred_termination_signal
    _deferred_termination_signal = None
    raise PackageTestTermination(signal_number)


def reraise_package_test_termination(cancellation: PackageTestTermination) -> None:
    # The handler scope has restored the caller's handlers.  Redeliver with the
    # default disposition so a supervising process observes the conventional
    # negative signal status instead of a Python traceback or a leaked group.
    signal.signal(cancellation.signal_number, signal.SIG_DFL)
    os.kill(os.getpid(), cancellation.signal_number)
    raise SystemExit(128 + cancellation.signal_number)


class ProcessCleanupError(RuntimeError):
    def __init__(self, message: str, stdout: str, stderr: str) -> None:
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr


def as_text(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode(errors="replace")
    return value


def combine_output(previous: str, current: str) -> str:
    if not current or current.startswith(previous):
        return current or previous
    if not previous:
        return current
    return previous + current


def close_process_pipes(process: subprocess.Popen[str]) -> None:
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream is not None and not stream.closed:
            try:
                stream.close()
            except OSError:
                pass


def start_owned_process(command: list[str], *, env: dict[str, str]) -> subprocess.Popen[str]:
    # Python dispatches signal handlers between bytecodes.  Do not let one
    # interrupt the launch-to-registration sequence: a deferred cancellation
    # is raised immediately after the verified group has been recorded.
    with PackageTestTerminationDeferral():
        process = subprocess.Popen(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            start_new_session=True,
        )
        pgid = process.pid
        try:
            if pgid <= 0 or pgid == os.getpgrp():
                raise RuntimeError(f"refusing to manage unsafe package process group {pgid}")
            if os.getpgid(process.pid) != pgid:
                raise RuntimeError(f"package process did not create its own process group: {pgid}")
        except BaseException:
            # Do not signal a group which was not verified as ours.  This branch is
            # unreachable for a successful POSIX start_new_session=True invocation,
            # but closing our pipe ends still keeps a failed launch finite.
            close_process_pipes(process)
            raise
        VERIFIED_OWNED_GROUPS[process] = pgid
    raise_deferred_package_test_termination()
    return process


def owned_group_id(process: subprocess.Popen[str]) -> int:
    try:
        pgid = VERIFIED_OWNED_GROUPS[process]
    except KeyError as exc:
        raise RuntimeError("package process is not a verified owned process") from exc
    if pgid <= 0 or pgid == os.getpgrp():
        raise RuntimeError(f"refusing to signal unsafe package process group {pgid}")
    return pgid


def owned_group_alive(pgid: int) -> bool:
    if pgid <= 0 or pgid == os.getpgrp():
        raise RuntimeError(f"refusing to inspect unsafe package process group {pgid}")
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError as exc:
        raise RuntimeError(f"unable to inspect owned package process group {pgid}: {exc}") from exc
    return True


def signal_owned_group(pgid: int, signal_number: int) -> bool:
    if pgid <= 0 or pgid == os.getpgrp():
        raise RuntimeError(f"refusing to signal unsafe package process group {pgid}")
    try:
        os.killpg(pgid, signal_number)
    except ProcessLookupError:
        return False
    except PermissionError as exc:
        raise RuntimeError(f"unable to signal owned package process group {pgid}: {exc}") from exc
    return True


def wait_for_owned_group_exit(process: subprocess.Popen[str], pgid: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while True:
        process.poll()  # Reap an exited leader while its descendants are checked.
        if not owned_group_alive(pgid):
            return True
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(PROCESS_POLL_INTERVAL, remaining))


def drain_process_output(
    process: subprocess.Popen[str],
    stdout: str,
    stderr: str,
    errors: list[str],
) -> tuple[str, str]:
    try:
        drained_stdout, drained_stderr = process.communicate(timeout=PROCESS_DRAIN_DEADLINE)
    except subprocess.TimeoutExpired as exc:
        stdout = combine_output(stdout, as_text(exc.stdout))
        stderr = combine_output(stderr, as_text(exc.stderr))
        errors.append(f"stdout/stderr did not drain within {PROCESS_DRAIN_DEADLINE:.2f}s")
        close_process_pipes(process)
    except (OSError, ValueError) as exc:
        errors.append(f"unable to drain stdout/stderr: {exc}")
        close_process_pipes(process)
    else:
        stdout = combine_output(stdout, as_text(drained_stdout))
        stderr = combine_output(stderr, as_text(drained_stderr))
    return stdout, stderr


def cleanup_owned_process(
    process: subprocess.Popen[str],
    *,
    stdout: str = "",
    stderr: str = "",
    output_already_drained: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Terminate one verified group and finish every local cleanup step finitely."""
    errors: list[str] = []
    try:
        # A second terminal signal must not interrupt TERM→KILL, reaping, or
        # pipe draining and strand the next verified group in the registry.
        with PackageTestTerminationDeferral():
            try:
                pgid = owned_group_id(process)
                if owned_group_alive(pgid):
                    signal_owned_group(pgid, signal.SIGTERM)
                    if not wait_for_owned_group_exit(process, pgid, PROCESS_TERM_GRACE):
                        signal_owned_group(pgid, signal.SIGKILL)
                        if not wait_for_owned_group_exit(process, pgid, PROCESS_KILL_GRACE):
                            errors.append(f"owned package process group {pgid} survived SIGKILL")
            except RuntimeError as exc:
                errors.append(str(exc))

            if process.poll() is None:
                try:
                    process.wait(timeout=PROCESS_REAP_DEADLINE)
                except subprocess.TimeoutExpired:
                    errors.append(f"package process leader did not reap within {PROCESS_REAP_DEADLINE:.2f}s")

            if not output_already_drained:
                stdout, stderr = drain_process_output(process, stdout, stderr, errors)
            close_process_pipes(process)
    finally:
        VERIFIED_OWNED_GROUPS.pop(process, None)
        ASYNC_PACKAGE_PROCESSES.discard(process)

    raise_deferred_package_test_termination()
    if errors:
        raise ProcessCleanupError("; ".join(errors), stdout, stderr)
    return subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)


def cleanup_after_communication_exception(process: subprocess.Popen[str], error: BaseException) -> None:
    try:
        result = cleanup_owned_process(
            process,
            stdout=as_text(getattr(error, "stdout", None)),
            stderr=as_text(getattr(error, "stderr", None)),
        )
    except ProcessCleanupError as cleanup_error:
        error.add_note(
            f"package process cleanup failed: {cleanup_error}\n"
            f"stdout:\n{cleanup_error.stdout}\nstderr:\n{cleanup_error.stderr}"
        )
    else:
        setattr(error, "package_cleanup_stdout", result.stdout)
        setattr(error, "package_cleanup_stderr", result.stderr)


def collect_process(process: subprocess.Popen[str], *, timeout: float) -> subprocess.CompletedProcess[str]:
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        cleanup_after_communication_exception(process, exc)
        raise
    except BaseException as exc:
        cleanup_after_communication_exception(process, exc)
        raise

    try:
        if owned_group_alive(owned_group_id(process)):
            return cleanup_owned_process(
                process,
                stdout=as_text(stdout),
                stderr=as_text(stderr),
                output_already_drained=True,
            )
    except BaseException as exc:
        cleanup_after_communication_exception(process, exc)
        raise

    close_process_pipes(process)
    VERIFIED_OWNED_GROUPS.pop(process, None)
    ASYNC_PACKAGE_PROCESSES.discard(process)
    return subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr)


def run(
    command: list[str],
    *,
    env: dict[str, str],
    check: bool = True,
    timeout: float = 120.0,
) -> subprocess.CompletedProcess[str]:
    result = collect_process(start_owned_process(command, env=env), timeout=timeout)
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def start(command: list[str], *, env: dict[str, str]) -> subprocess.Popen[str]:
    process = start_owned_process(command, env=env)
    ASYNC_PACKAGE_PROCESSES.add(process)
    return process


def finish(process: subprocess.Popen[str], *, timeout: float = 120.0) -> subprocess.CompletedProcess[str]:
    try:
        return collect_process(process, timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        stdout = getattr(exc, "package_cleanup_stdout", as_text(exc.stdout))
        stderr = getattr(exc, "package_cleanup_stderr", as_text(exc.stderr))
        raise RuntimeError(f"timed out waiting for package process\nstdout:\n{stdout}\nstderr:\n{stderr}") from exc


def result_after_cleanup(process: subprocess.Popen[str]) -> subprocess.CompletedProcess[str]:
    try:
        return cleanup_owned_process(process)
    except ProcessCleanupError as exc:
        raise RuntimeError(
            f"package process cleanup failed: {exc}\nstdout:\n{exc.stdout}\nstderr:\n{exc.stderr}"
        ) from exc


def wait_for_path(path: pathlib.Path, process: subprocess.Popen[str], *, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while True:
        if path.exists():
            return
        if process.poll() is not None:
            result = result_after_cleanup(process)
            raise RuntimeError(
                f"package process exited before creating synchronization marker {path}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(PROCESS_POLL_INTERVAL, remaining))
    result = result_after_cleanup(process)
    raise RuntimeError(
        f"package process did not create synchronization marker {path}\n"
        f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
    )


def cleanup_async_package_processes() -> None:
    errors: list[str] = []
    with PackageTestTerminationDeferral():
        for process in tuple(ASYNC_PACKAGE_PROCESSES):
            try:
                cleanup_owned_process(process)
            except ProcessCleanupError as exc:
                errors.append(f"{exc}\nstdout:\n{exc.stdout}\nstderr:\n{exc.stderr}")
    raise_deferred_package_test_termination()
    if errors:
        raise RuntimeError("abandoned package-process cleanup failed:\n" + "\n".join(errors))


def cleanup_verified_owned_processes() -> None:
    """Clean both synchronous and asynchronous groups left in the registry."""
    errors: list[str] = []
    with PackageTestTerminationDeferral():
        for process in tuple(VERIFIED_OWNED_GROUPS):
            try:
                cleanup_owned_process(process)
            except ProcessCleanupError as exc:
                errors.append(f"{exc}\nstdout:\n{exc.stdout}\nstderr:\n{exc.stderr}")
    raise_deferred_package_test_termination()
    if errors:
        raise RuntimeError("verified package-process cleanup failed:\n" + "\n".join(errors))


def parse_path(output: str, label: str) -> pathlib.Path:
    prefix = f"{label}: "
    for line in output.splitlines():
        if line.startswith(prefix):
            return pathlib.Path(line[len(prefix) :])
    raise RuntimeError(f"package output did not report {label}:\n{output}")


def project_version(repo: pathlib.Path) -> str:
    text = (repo / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"project\s*\(\s*ava\b.*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\b",
        text,
        re.DOTALL | re.IGNORECASE,
    )
    if not match:
        raise RuntimeError("unable to parse top-level project(ava VERSION X.Y.Z)")
    return match.group(1)


def package_architecture() -> str:
    machine = platform.machine()
    known = {
        "x86_64": "x64",
        "amd64": "x64",
        "aarch64": "arm64",
        "arm64": "arm64",
        "armv7l": "armv7",
        "armv7": "armv7",
        "ppc64le": "ppc64le",
        "riscv64": "riscv64",
    }
    if machine in known:
        return known[machine]
    normalized = re.sub(r"[^a-z0-9._-]", "-", machine.lower())
    if not normalized:
        raise RuntimeError("unable to normalize host architecture in package test")
    return normalized


def expected_files(package_name: str) -> set[str]:
    docs = {
        "README.md",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
        "PROVENANCE.json",
        "docs/core/usage.md",
        "docs/core/configuration.md",
        "docs/core/context-resources.md",
        "docs/core/custom-providers.md",
        "docs/core/environment-variables.md",
        "docs/core/providers.md",
        "docs/core/subagents.md",
        "docs/core/thinking-modes.md",
        "docs/core/tools.md",
        "docs/interfaces/themes-keybindings.md",
        "docs/operations/testing.md",
        "docs/operations/terminal-setup.md",
        "docs/operations/troubleshooting.md",
        "docs/operations/diagnostics.md",
        "docs/operations/release-checklist.md",
        "docs/extensions/lsp.md",
        "docs/extensions/mcp.md",
        "docs/extensions/plugin-system.md",
        "docs/security/sandboxing.md",
        "docs/security/containment.md",
        "docs/development/session-versioning.md",
        "docs/development/side-effect-safety-checklist.md",
        "docs/headless-protocol.md",
        "docs/rpc-protocol.md",
        "docs/acp.md",
        "docs/acp-support.json",
        "docs/session-format.md",
        "docs/plugin-compatibility-policy.md",
        "docs/interop/evidence/README.md",
        "docs/interop/evidence/zed-1.9.0-2026-07-14.md",
        "docs/product/mvp-coverage-ledger.md",
        "docs/schema/theme.schema.json",
    }
    return {f"{package_name}/bin/ava"} | {f"{package_name}/share/doc/ava/{doc}" for doc in docs}


def write_executable(path: pathlib.Path, contents: str) -> None:
    path.write_text(contents, encoding="utf-8")
    path.chmod(0o700)


def write_process_cleanup_fixture(path: pathlib.Path) -> None:
    write_executable(
        path,
        """#!/usr/bin/env python3
import os
import pathlib
import signal
import subprocess
import sys
import time

mode, ready_path, leader_path = sys.argv[1:]
ready = pathlib.Path(ready_path)
leader = pathlib.Path(leader_path)

if mode == "descendant":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    if os.environ["AVA_PACKAGE_CLEANUP_CLOSE_PIPES"] == "1":
        os.close(sys.stdout.fileno())
        os.close(sys.stderr.fileno())
    ready.write_text("ready\\n", encoding="utf-8")
    while True:
        time.sleep(1)

leader.write_text(f"{os.getpid()}\\n", encoding="utf-8")
child_env = os.environ.copy()
child_env["AVA_PACKAGE_CLEANUP_CLOSE_PIPES"] = "1" if mode == "exit-close-pipes" else "0"
subprocess.Popen(
    [sys.executable, __file__, "descendant", str(ready), str(leader)],
    env=child_env,
)
deadline = time.monotonic() + 1
while not ready.exists():
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise SystemExit("descendant did not publish its readiness marker")
    time.sleep(min(0.01, remaining))
if mode in ("exit", "exit-close-pipes"):
    raise SystemExit(0)
while True:
    time.sleep(1)
""",
    )


def write_signal_guard_fixture(path: pathlib.Path) -> None:
    write_executable(
        path,
        """#!/usr/bin/env python3
import pathlib
import signal
import sys
import time

ready, stop, signaled = (pathlib.Path(value) for value in sys.argv[1:])

def record_signal(_signal_number, _frame):
    signaled.write_text("unexpected signal\\n", encoding="utf-8")

signal.signal(signal.SIGTERM, record_signal)
ready.write_text("ready\\n", encoding="utf-8")
while not stop.exists():
    time.sleep(0.01)
""",
    )


def assert_owned_group_disappeared(pgid: int, context: str) -> None:
    deadline = time.monotonic() + PROCESS_KILL_GRACE
    while True:
        if not owned_group_alive(pgid):
            return
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError(f"{context}: owned process group {pgid} survived cleanup")
        time.sleep(min(PROCESS_POLL_INTERVAL, remaining))


def assert_cleanup_bounded(start_time: float, context: str) -> None:
    elapsed = time.monotonic() - start_time
    maximum = PROCESS_TERM_GRACE + PROCESS_KILL_GRACE + PROCESS_REAP_DEADLINE + PROCESS_DRAIN_DEADLINE + 0.5
    if elapsed > maximum:
        raise RuntimeError(f"{context}: cleanup exceeded its finite {maximum:.2f}s bound ({elapsed:.2f}s)")


def cleanup_signal_regression_backstop(
    harness: subprocess.Popen[str],
    published_group: int | None,
    *,
    disappearance_confirmed: bool,
) -> bool:
    """Delegate failed-regression cleanup to its still-owned harness.

    ``published_group`` is assertion-only data from a child.  In particular, it
    is never sent to killpg(): after an observed disappearance its numeric value
    may already identify an unrelated process group.
    """
    if disappearance_confirmed:
        return False
    if harness not in VERIFIED_OWNED_GROUPS:
        if published_group is None:
            return False
        raise RuntimeError(
            "signal-cleanup regression cannot safely clean an unconfirmed child group: "
            "its owning harness is no longer registered"
        )
    if harness.poll() is not None:
        # The leader's group number may now be reusable too.  Do not let later
        # broad cleanup turn this failed delegation into a numeric-PGID signal.
        VERIFIED_OWNED_GROUPS.pop(harness, None)
        ASYNC_PACKAGE_PROCESSES.discard(harness)
        close_process_pipes(harness)
        raise RuntimeError(
            "signal-cleanup regression cannot safely clean an unconfirmed child group: "
            "its owning harness already exited"
        )
    with PackageTestTerminationDeferral():
        cleanup_owned_process(harness)
    raise_deferred_package_test_termination()
    return True


def run_process_cleanup_regressions(root: pathlib.Path, env: dict[str, str]) -> None:
    """Prove all package-process paths kill pipe-holding descendants finitely."""
    regression_root = root / "process-cleanup-regressions"
    regression_root.mkdir(mode=0o700)
    fixture = regression_root / "pipe-holding-descendant.py"
    write_process_cleanup_fixture(fixture)

    def command(mode: str, name: str) -> tuple[list[str], pathlib.Path, pathlib.Path]:
        ready = regression_root / f"{name}.ready"
        leader = regression_root / f"{name}.leader"
        return [sys.executable, str(fixture), mode, str(ready), str(leader)], ready, leader

    sync_command, sync_ready, sync_leader = command("stall", "synchronous-timeout")
    started = time.monotonic()
    try:
        run(sync_command, env=env, timeout=1.0)
    except subprocess.TimeoutExpired:
        pass
    else:
        raise RuntimeError("synchronous package timeout fixture unexpectedly completed")
    assert_cleanup_bounded(started, "synchronous package timeout")
    if not sync_ready.is_file() or not sync_leader.is_file():
        raise RuntimeError("synchronous package timeout did not start its pipe-holding descendant")
    assert_owned_group_disappeared(int(sync_leader.read_text(encoding="utf-8")), "synchronous package timeout")

    async_command, async_ready, async_leader = command("stall", "async-finish-timeout")
    async_process = start(async_command, env=env)
    wait_for_path(async_ready, async_process, timeout=1.0)
    started = time.monotonic()
    try:
        finish(async_process, timeout=0.25)
    except RuntimeError as exc:
        if "timed out waiting for package process" not in str(exc):
            raise RuntimeError(f"async finish timeout failed for the wrong reason: {exc}") from exc
    else:
        raise RuntimeError("async finish timeout fixture unexpectedly completed")
    assert_cleanup_bounded(started, "async finish timeout")
    assert_owned_group_disappeared(
        int(async_leader.read_text(encoding="utf-8")), "async finish timeout"
    )

    marker_command, marker_ready, marker_leader = command("exit", "missing-marker")
    marker_process = start(marker_command, env=env)
    wait_for_path(marker_ready, marker_process, timeout=1.0)
    started = time.monotonic()
    try:
        wait_for_path(regression_root / "marker-that-is-never-created", marker_process, timeout=1.0)
    except RuntimeError as exc:
        if "exited before creating synchronization marker" not in str(exc):
            raise RuntimeError(f"missing-marker cleanup failed for the wrong reason: {exc}") from exc
    else:
        raise RuntimeError("missing-marker fixture unexpectedly created its synchronization marker")
    assert_cleanup_bounded(started, "missing synchronization marker")
    assert_owned_group_disappeared(int(marker_leader.read_text(encoding="utf-8")), "missing synchronization marker")

    normal_command, normal_ready, normal_leader = command("exit-close-pipes", "successful-leader")
    normal_process = start(normal_command, env=env)
    wait_for_path(normal_ready, normal_process, timeout=1.0)
    started = time.monotonic()
    normal_result = finish(normal_process, timeout=1.0)
    if normal_result.returncode != 0:
        raise RuntimeError(f"successful package fixture returned {normal_result.returncode}")
    assert_cleanup_bounded(started, "successful package process descendant cleanup")
    assert_owned_group_disappeared(
        int(normal_leader.read_text(encoding="utf-8")), "successful package process descendant cleanup"
    )

    abandoned_command, abandoned_ready, abandoned_leader = command("stall", "exceptional-abandonment")
    started = time.monotonic()
    try:
        abandoned_process = start(abandoned_command, env=env)
        wait_for_path(abandoned_ready, abandoned_process, timeout=1.0)
        raise RuntimeError("simulated assertion between package-process start and finish")
    except RuntimeError as exc:
        if str(exc) != "simulated assertion between package-process start and finish":
            raise
    finally:
        cleanup_async_package_processes()
    assert_cleanup_bounded(started, "exceptional package-process abandonment")
    assert_owned_group_disappeared(
        int(abandoned_leader.read_text(encoding="utf-8")), "exceptional package-process abandonment"
    )

    run_signal_regression_backstop_safety_regressions(regression_root, fixture, env)
    run_harness_signal_cleanup_regression(regression_root, fixture, env)


def run_signal_regression_backstop_safety_regressions(
    regression_root: pathlib.Path,
    fixture: pathlib.Path,
    env: dict[str, str],
) -> None:
    """A recycled published PGID must not receive a regression backstop signal."""
    guard_fixture = regression_root / "signal-regression-guard.py"
    write_signal_guard_fixture(guard_fixture)
    guard_ready = regression_root / "signal-regression-guard.ready"
    guard_stop = regression_root / "signal-regression-guard.stop"
    guard_signaled = regression_root / "signal-regression-guard.signaled"
    guard = start(
        [sys.executable, str(guard_fixture), str(guard_ready), str(guard_stop), str(guard_signaled)],
        env=env,
    )
    try:
        wait_for_path(guard_ready, guard, timeout=1.0)
        # Treat this live, unrelated group as the stale numeric value a child
        # could have published before its original group disappeared.
        simulated_reused_group = owned_group_id(guard)

        def start_backstop_harness(name: str) -> subprocess.Popen[str]:
            ready = regression_root / f"{name}.ready"
            leader = regression_root / f"{name}.leader"
            harness = start(
                [sys.executable, str(fixture), "stall", str(ready), str(leader)], env=env
            )
            wait_for_path(ready, harness, timeout=1.0)
            return harness

        confirmed_harness = start_backstop_harness("confirmed-disappearance-backstop")
        try:
            if cleanup_signal_regression_backstop(
                confirmed_harness,
                simulated_reused_group,
                disappearance_confirmed=True,
            ):
                raise RuntimeError("confirmed disappearance unexpectedly ran the signal-regression backstop")
            if confirmed_harness.poll() is not None:
                raise RuntimeError("confirmed disappearance unexpectedly terminated its harness")
        finally:
            if confirmed_harness in VERIFIED_OWNED_GROUPS:
                result_after_cleanup(confirmed_harness)

        delegated_harness = start_backstop_harness("reused-identity-backstop")
        try:
            if not cleanup_signal_regression_backstop(
                delegated_harness,
                simulated_reused_group,
                disappearance_confirmed=False,
            ):
                raise RuntimeError("unconfirmed signal-regression cleanup was not delegated to its harness")
        finally:
            if delegated_harness in VERIFIED_OWNED_GROUPS:
                result_after_cleanup(delegated_harness)

        if guard_signaled.exists() or guard.poll() is not None:
            raise RuntimeError("signal-regression backstop signaled an unrelated simulated-reused process group")
    finally:
        guard_stop.write_text("stop\n", encoding="utf-8")
        if guard in VERIFIED_OWNED_GROUPS:
            finish(guard, timeout=1.0)
    if guard_signaled.exists():
        raise RuntimeError("signal-regression backstop signaled the unrelated guard during cleanup")


def run_harness_signal_cleanup_regression(
    regression_root: pathlib.Path,
    fixture: pathlib.Path,
    env: dict[str, str],
) -> None:
    """SIGTERM a harness whose child keeps both of its inherited pipes open."""
    harness_ready = regression_root / "signal-harness.ready"
    descendant_ready = regression_root / "signal-descendant.ready"
    leader = regression_root / "signal-descendant.leader"
    harness = start(
        [
            sys.executable,
            str(pathlib.Path(__file__).absolute()),
            "--script",
            str(pathlib.Path(__file__).absolute()),
            "--ava",
            str(fixture),
            "--fake-provider",
            str(fixture),
            "--repo",
            str(regression_root),
            "--build-dir",
            str(regression_root),
            "--root",
            str(regression_root),
            "--signal-cleanup-regression-child",
            "--signal-cleanup-fixture",
            str(fixture),
            "--signal-cleanup-descendant-ready",
            str(descendant_ready),
            "--signal-cleanup-leader",
            str(leader),
            "--signal-cleanup-ready",
            str(harness_ready),
        ],
        env=env,
    )
    descendant_group: int | None = None
    descendant_group_disappeared = False
    try:
        wait_for_path(harness_ready, harness, timeout=5.0)
        if not descendant_ready.is_file() or not leader.is_file():
            raise RuntimeError("signal-cleanup harness did not publish descendant condition markers")
        descendant_group = int(leader.read_text(encoding="utf-8"))

        started = time.monotonic()
        os.kill(harness.pid, signal.SIGTERM)
        result = finish(
            harness,
            timeout=PROCESS_TERM_GRACE + PROCESS_KILL_GRACE + PROCESS_REAP_DEADLINE + PROCESS_DRAIN_DEADLINE,
        )
        if result.returncode != -signal.SIGTERM:
            raise RuntimeError(
                "SIGTERM-cleaned package harness did not preserve signal status "
                f"(return code {result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        assert_cleanup_bounded(started, "SIGTERM-cleaned package harness")
        assert_owned_group_disappeared(descendant_group, "SIGTERM-cleaned package harness")
        descendant_group_disappeared = True
    finally:
        cleanup_signal_regression_backstop(
            harness,
            descendant_group,
            disappearance_confirmed=descendant_group_disappeared,
        )


def run_signal_cleanup_regression_child(args: argparse.Namespace) -> int:
    fixture = args.signal_cleanup_fixture
    descendant_ready = args.signal_cleanup_descendant_ready
    leader = args.signal_cleanup_leader
    ready = args.signal_cleanup_ready
    if fixture is None or descendant_ready is None or leader is None or ready is None:
        raise RuntimeError("signal-cleanup regression child is missing its synchronization paths")

    with PackageTestTerminationHandlers():
        try:
            process = start(
                [sys.executable, str(fixture), "stall", str(descendant_ready), str(leader)],
                env=os.environ.copy(),
            )
            wait_for_path(descendant_ready, process, timeout=5.0)
            if owned_group_id(process) != int(leader.read_text(encoding="utf-8")):
                raise RuntimeError("signal-cleanup child did not verify the fixture process group")
            ready.write_text("ready\n", encoding="utf-8")
            signal.pause()
            raise RuntimeError("signal-cleanup child resumed without a terminal signal")
        finally:
            cleanup_verified_owned_processes()


def write_elf_shell_fixture(path: pathlib.Path, shell_script: str) -> None:
    compiler = shutil.which("cc") or shutil.which("c++")
    if compiler is None:
        raise RuntimeError("a C or C++ compiler is required for ELF package fixtures")
    source = f"""#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char fixture_script[] = {json.dumps(shell_script)};

int main(int argc, char **argv) {{
    char **shell_argv = (char **)calloc((size_t)argc + 4, sizeof(*shell_argv));
    if (shell_argv == NULL) return 127;
    shell_argv[0] = (char *)"sh";
    shell_argv[1] = (char *)"-c";
    shell_argv[2] = (char *)fixture_script;
    shell_argv[3] = (char *)"ava-fixture";
    for (int index = 1; index < argc; ++index) shell_argv[index + 3] = argv[index];
    shell_argv[argc + 3] = NULL;
    execv("/bin/sh", shell_argv);
    perror("execv /bin/sh");
    return 127;
}}
"""
    with tempfile.TemporaryDirectory(dir=path.parent) as directory:
        root = pathlib.Path(directory)
        source_path = root / "fixture.c"
        executable_path = root / "fixture"
        source_path.write_text(source, encoding="utf-8")
        result = subprocess.run(
            [compiler, "-O0", "-o", str(executable_path), str(source_path)],
            text=True,
            capture_output=True,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(f"unable to compile ELF package fixture:\n{result.stderr}")
        shutil.copy2(executable_path, path)
        path.chmod(0o700)


def write_ava_fixture(path: pathlib.Path, version: str) -> None:
    write_elf_shell_fixture(
        path,
        f"""set -eu
case "${{1-}}" in
  --version)
    if [ -n "${{AVA_PACKAGE_TEST_MARKER:-}}" ] && [ ! -e "$AVA_PACKAGE_TEST_MARKER" ]; then
      : > "$AVA_PACKAGE_TEST_MARKER"
      while [ ! -e "$AVA_PACKAGE_TEST_GATE" ]; do sleep 0.01; done
    fi
    printf 'ava {version}\\n'
    ;;
  --help)
    printf 'Usage: ava [options]\\n'
    ;;
  packages)
    if [ "${{2-}}" != list ]; then exit 2; fi
    printf 'package management deferred\\n'
    ;;
  *)
    printf 'unexpected fixture arguments\\n' >&2
    exit 2
    ;;
esac
""",
    )


def write_mutating_ava_fixture(path: pathlib.Path, version: str) -> None:
    write_elf_shell_fixture(
        path,
        f"""set -eu
case "${{1-}}" in
  --version)
    if [ ! -e "$AVA_PACKAGE_TEST_BUILD_MUTATION_MARKER" ]; then
      : > "$AVA_PACKAGE_TEST_BUILD_MUTATION_MARKER"
      build_dir=$(cat "$AVA_PACKAGE_TEST_BUILD_REPO/build-location")
      install -m 0700 "$AVA_PACKAGE_TEST_BUILD_AVA_REPLACEMENT" "$build_dir/ava.next"
      mv "$build_dir/ava.next" "$build_dir/ava"
      install -m 0700 "$AVA_PACKAGE_TEST_BUILD_FAKE_REPLACEMENT" "$build_dir/tests/ava_fake_provider_server.next"
      mv "$build_dir/tests/ava_fake_provider_server.next" "$build_dir/tests/ava_fake_provider_server"
    fi
    printf 'ava {version}\\n'
    ;;
  --help)
    printf 'Usage: ava [options]\\n'
    ;;
  packages)
    if [ "${{2-}}" != list ]; then exit 2; fi
    printf 'package management deferred\\n'
    ;;
  *)
    printf 'unexpected fixture arguments\\n' >&2
    exit 2
    ;;
esac
""",
    )


def package_command(
    script: pathlib.Path,
    binary: pathlib.Path,
    output: pathlib.Path | None = None,
    fake_provider: pathlib.Path | None = None,
) -> list[str]:
    command = [str(script), "--binary", str(binary)]
    if fake_provider is not None:
        command.extend(["--fake-provider", str(fake_provider)])
    if output is not None:
        command.extend(["--output-dir", str(output)])
    return command


def build_package_command(script: pathlib.Path, output: pathlib.Path) -> list[str]:
    return [str(script), "--output-dir", str(output)]


def load_publisher(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("ava_publish_linux_artifacts_test", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load publication helper from {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require_failure(result: subprocess.CompletedProcess[str], message: str, context: str) -> None:
    if result.returncode == 0 or message not in result.stderr:
        raise RuntimeError(
            f"{context}\nreturn code: {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def assert_output_empty(path: pathlib.Path, context: str) -> None:
    entries = list(path.iterdir())
    if entries:
        raise RuntimeError(f"{context}: {[entry.name for entry in entries]}")


def create_fake_build_repository(
    repo: pathlib.Path,
    script: pathlib.Path,
    publisher: pathlib.Path,
    root: pathlib.Path,
    package_name: str,
    version: str,
) -> tuple[pathlib.Path, pathlib.Path]:
    fake_repo = root / "build-mode-repository"
    (fake_repo / "scripts").mkdir(parents=True, mode=0o700)
    (fake_repo / "tests").mkdir(mode=0o700)
    shutil.copy2(script, fake_repo / "scripts" / "package-linux.sh")
    shutil.copy2(publisher, fake_repo / "scripts" / "publish-linux-artifacts.py")
    shutil.copy2(repo / "scripts" / "generate-release-provenance.py", fake_repo / "scripts" / "generate-release-provenance.py")
    shutil.copy2(repo / "scripts" / "verify-markdown-links.py", fake_repo / "scripts" / "verify-markdown-links.py")
    (fake_repo / "CMakeLists.txt").write_text(
        f"cmake_minimum_required(VERSION 3.27)\nproject(ava VERSION {version})\n",
        encoding="utf-8",
    )

    install_manifest: list[tuple[str, str]] = []
    package_prefix = f"{package_name}/"
    for member in sorted(expected_files(package_name)):
        relative = member.removeprefix(package_prefix)
        if relative in {"bin/ava", "share/doc/ava/PROVENANCE.json"}:
            continue
        if relative == "share/doc/ava/README.md":

            source_relative = "docs/operations/release-artifact-readme.md"
        elif relative == "share/doc/ava/LICENSE":
            source_relative = "LICENSE"
        else:
            doc_prefix = "share/doc/ava/"
            if not relative.startswith(doc_prefix):
                raise RuntimeError(f"unexpected package fixture member: {relative}")
            source_relative = relative.removeprefix(doc_prefix)
        source = repo / source_relative
        destination = fake_repo / source_relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        install_manifest.append((source_relative, relative))

    (fake_repo / "install-manifest.txt").write_text(
        "".join(f"{source}\t{destination}\n" for source, destination in install_manifest),
        encoding="utf-8",
    )
    (fake_repo / "tests" / "cli_headless_e2e_model_smoke.cmake").write_text(
        "# The fake cmake wrapper validates deterministic model-smoke inputs.\n",
        encoding="utf-8",
    )

    wrapper_dir = root / "build-mode-cmake-wrapper"
    wrapper_dir.mkdir(mode=0o700)
    fake_cmake = wrapper_dir / "cmake"
    write_executable(
        fake_cmake,
        """#!/usr/bin/env python3
import hashlib
import os
import pathlib
import shutil
import sys

args = sys.argv[1:]
repo = pathlib.Path(os.environ["AVA_PACKAGE_TEST_BUILD_REPO"])

if args and args[0] == "--preset":
    required_cache_arguments = {
        "-DAVA_ENABLE_GITACHE=OFF",
        "-DEnableLibcwd=OFF",
        "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
        "-DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON",
    }
    missing = sorted(argument for argument in required_cache_arguments if args.count(argument) != 1)
    if missing:
        raise RuntimeError(
            "source-build configure must be dependency-disconnected; "
            f"missing or duplicate arguments {missing}: {args}"
        )
    raise SystemExit(0)

if args and args[0] == "--build":
    build = pathlib.Path(args[1])
    (repo / "build-location").write_text(str(build), encoding="utf-8")
    ava = build / "ava"
    fake = build / "tests" / "ava_fake_provider_server"
    ava.parent.mkdir(parents=True, exist_ok=True)
    fake.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(os.environ["AVA_PACKAGE_TEST_BUILD_AVA_TEMPLATE"], ava)
    shutil.copy2(os.environ["AVA_PACKAGE_TEST_BUILD_FAKE_TEMPLATE"], fake)
    ava.chmod(0o700)
    fake.chmod(0o700)
    raise SystemExit(0)

if args and args[0] == "--install":
    try:
        prefix = pathlib.Path(args[args.index("--prefix") + 1])
    except (ValueError, IndexError) as exc:
        raise RuntimeError("fake cmake install did not receive --prefix") from exc
    build = pathlib.Path(args[1])
    installed_ava = prefix / "bin" / "ava"
    installed_ava.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / "ava", installed_ava)
    installed_ava.chmod(0o755)
    for line in (repo / "install-manifest.txt").read_text(encoding="utf-8").splitlines():
        source_name, destination_name = line.split("\\t", 1)
        destination = prefix / destination_name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / source_name, destination)
        destination.chmod(0o644)
    raise SystemExit(0)


def definition(prefix):
    values = [argument[len(prefix):] for argument in args if argument.startswith(prefix)]
    if len(values) != 1:
        raise RuntimeError(f"expected exactly one {prefix} definition")
    return pathlib.Path(values[0])


ava = definition("-DAVA_EXE=")
fake = definition("-DAVA_FAKE_PROVIDER_EXE=")
if fake.name != "fake-provider" or fake.parent.name != "inputs":
    raise RuntimeError(f"model smoke did not receive the private fake-provider snapshot: {fake}")
ava_digest = hashlib.sha256(ava.read_bytes()).hexdigest()
fake_digest = hashlib.sha256(fake.read_bytes()).hexdigest()
if ava_digest != os.environ["AVA_PACKAGE_TEST_BUILD_AVA_DIGEST"]:
    raise RuntimeError(f"model-smoke AVA digest changed: {ava_digest}")
if fake_digest != os.environ["AVA_PACKAGE_TEST_BUILD_FAKE_DIGEST"]:
    raise RuntimeError(f"model-smoke fake-provider digest changed: {fake_digest}")
build = pathlib.Path((repo / "build-location").read_text(encoding="utf-8"))
if hashlib.sha256((build / "ava").read_bytes()).hexdigest() == ava_digest:
    raise RuntimeError("private build-tree AVA was not replaced before install/model smoke")
if hashlib.sha256((build / "tests" / "ava_fake_provider_server").read_bytes()).hexdigest() == fake_digest:
    raise RuntimeError("private build-tree fake provider was not replaced before model smoke")
pathlib.Path(os.environ["AVA_PACKAGE_TEST_BUILD_MODEL_MARKER"]).write_text("smoked\\n", encoding="utf-8")
""",
    )
    return fake_repo, wrapper_dir


RENAME_NOREPLACE = 1
DirectoryIdentity = tuple[int, int]
OwnedDirectoryIdentity = tuple[int, int, int, int]


def directory_identity(status: os.stat_result) -> DirectoryIdentity:
    return status.st_dev, status.st_ino


def owned_directory_identity(status: os.stat_result) -> OwnedDirectoryIdentity:
    return status.st_dev, status.st_ino, status.st_uid, stat.S_IMODE(status.st_mode)


def rename_no_replace(directory_fd: int, source_name: str, destination_name: str) -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    if renameat2 is None:
        raise RuntimeError("Linux renameat2 is required for safe package-test cleanup")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(
        directory_fd,
        os.fsencode(source_name),
        directory_fd,
        os.fsencode(destination_name),
        RENAME_NOREPLACE,
    )
    if result != 0:
        error_number = ctypes.get_errno()
        raise OSError(error_number, os.strerror(error_number), source_name, destination_name)


def checked_final_directory(path: pathlib.Path, description: str) -> os.stat_result | None:
    try:
        status = os.lstat(path)
    except FileNotFoundError:
        return None
    if stat.S_ISLNK(status.st_mode):
        raise RuntimeError(f"{description} must not be a symlink: {path}")
    if not stat.S_ISDIR(status.st_mode):
        raise RuntimeError(f"{description} must be a directory: {path}")
    return status


def open_final_directory(path: pathlib.Path, description: str) -> tuple[int, os.stat_result]:
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
    try:
        directory_fd = os.open(path, flags)
    except OSError as exc:
        if exc.errno == errno.ELOOP:
            raise RuntimeError(f"{description} must not be a symlink: {path}") from exc
        raise RuntimeError(f"unable to open {description}: {path}: {exc}") from exc
    return directory_fd, os.fstat(directory_fd)


def open_owned_directory_entry(
    parent_fd: int,
    name: str,
    description: str,
    *,
    expected_identity: OwnedDirectoryIdentity | None = None,
) -> tuple[int, OwnedDirectoryIdentity]:
    """Open one private directory entry without trusting its pathname."""
    if not name or name in (".", "..") or "/" in name:
        raise RuntimeError(f"{description} must name one directory entry: {name!r}")
    try:
        listed = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
    except OSError as exc:
        raise RuntimeError(f"unable to inspect {description}: {name}: {exc}") from exc
    if stat.S_ISLNK(listed.st_mode) or not stat.S_ISDIR(listed.st_mode):
        raise RuntimeError(f"{description} must be a directory, not a link: {name}")
    try:
        directory_fd = os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_fd,
        )
    except OSError as exc:
        raise RuntimeError(f"unable to open {description}: {name}: {exc}") from exc
    try:
        opened = os.fstat(directory_fd)
        identity = owned_directory_identity(opened)
        if (
            directory_identity(opened) != directory_identity(listed)
            or not stat.S_ISDIR(opened.st_mode)
            or opened.st_uid != os.geteuid()
            or stat.S_IMODE(opened.st_mode) != 0o700
            or (expected_identity is not None and identity != expected_identity)
        ):
            raise RuntimeError(f"{description} changed identity, ownership, or mode while opening: {name}")
        return directory_fd, identity
    except BaseException:
        os.close(directory_fd)
        raise


def create_unique_owned_child(base_fd: int, base_path: pathlib.Path) -> tuple[str, OwnedDirectoryIdentity]:
    for _ in range(32):
        name = f".ava-package-linux-tests.{os.getpid()}.{secrets.token_hex(12)}"
        try:
            os.mkdir(name, 0o700, dir_fd=base_fd)
        except FileExistsError:
            continue
        try:
            child_fd = os.open(
                name,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=base_fd,
            )
        except OSError as exc:
            raise RuntimeError(f"unable to open private package-test child {base_path / name}: {exc}") from exc
        try:
            os.fchmod(child_fd, 0o700)
            opened = os.fstat(child_fd)
            listed = os.stat(name, dir_fd=base_fd, follow_symlinks=False)
        finally:
            os.close(child_fd)
        expected = owned_directory_identity(opened)
        if (
            expected != owned_directory_identity(listed)
            or opened.st_uid != os.geteuid()
            or stat.S_IMODE(opened.st_mode) != 0o700
            or not stat.S_ISDIR(opened.st_mode)
        ):
            raise RuntimeError(f"private package-test child changed while opening: {base_path / name}")
        return name, expected
    raise RuntimeError(f"could not allocate a unique private package-test child under {base_path}")


def make_quarantine_name(prefix: str) -> str:
    return f".{prefix}.quarantine.{os.getpid()}.{secrets.token_hex(12)}"


def detach_no_replace(base_fd: int, child_name: str, prefix: str) -> str:
    for _ in range(32):
        quarantine_name = make_quarantine_name(prefix)
        try:
            rename_no_replace(base_fd, child_name, quarantine_name)
        except FileExistsError:
            continue
        return quarantine_name
    raise RuntimeError(f"could not allocate a unique cleanup quarantine for {child_name}")


def restore_or_preserve(
    base_fd: int,
    quarantine_name: str,
    child_name: str,
    base_path: pathlib.Path,
    reason: str,
) -> str:
    try:
        rename_no_replace(base_fd, quarantine_name, child_name)
    except OSError as exc:
        return (
            f"{reason}; preserved untrusted cleanup quarantine at {base_path / quarantine_name} "
            f"because it could not be restored to {base_path / child_name}: {exc}"
        )
    return f"{reason}; restored it to {base_path / child_name} without deleting it"


def remove_verified_tree(
    parent_fd: int,
    name: str,
    expected: OwnedDirectoryIdentity,
    *,
    before_rmdir: Callable[[int, str, DirectoryIdentity], None] | None = None,
) -> None:
    """Remove a held, verified directory without following any child symlink."""
    try:
        directory_fd = os.open(
            name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_fd,
        )
    except OSError as exc:
        raise RuntimeError(f"cleanup refused to open quarantined directory {name}: {exc}") from exc
    try:
        opened = os.fstat(directory_fd)
        if owned_directory_identity(opened) != expected or not stat.S_ISDIR(opened.st_mode):
            raise RuntimeError(f"cleanup refused replaced quarantined directory {name}")
        remove_verified_tree_contents(directory_fd, before_rmdir=before_rmdir)
        if before_rmdir is not None:
            before_rmdir(parent_fd, name, directory_identity(opened))
        current = os.stat(name, dir_fd=parent_fd, follow_symlinks=False)
        if (
            directory_identity(current) != directory_identity(opened)
            or not stat.S_ISDIR(current.st_mode)
            or stat.S_ISLNK(current.st_mode)
        ):
            raise RuntimeError(f"cleanup refused changed directory entry before rmdir: {name}")
        os.rmdir(name, dir_fd=parent_fd)
    finally:
        os.close(directory_fd)


def remove_verified_tree_contents(
    directory_fd: int,
    *,
    before_rmdir: Callable[[int, str, DirectoryIdentity], None] | None = None,
) -> None:
    for name in os.listdir(directory_fd):
        listed = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if stat.S_ISDIR(listed.st_mode) and not stat.S_ISLNK(listed.st_mode):
            try:
                child_fd = os.open(
                    name,
                    os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=directory_fd,
                )
            except OSError as exc:
                raise RuntimeError(f"cleanup refused to open nested directory {name}: {exc}") from exc
            try:
                opened = os.fstat(child_fd)
                if (
                    directory_identity(opened) != directory_identity(listed)
                    or not stat.S_ISDIR(opened.st_mode)
                ):
                    raise RuntimeError(f"cleanup refused replaced nested directory {name}")
                remove_verified_tree_contents(child_fd, before_rmdir=before_rmdir)
                if before_rmdir is not None:
                    before_rmdir(directory_fd, name, directory_identity(opened))
                current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                if (
                    directory_identity(current) != directory_identity(opened)
                    or not stat.S_ISDIR(current.st_mode)
                    or stat.S_ISLNK(current.st_mode)
                ):
                    raise RuntimeError(f"cleanup refused changed directory entry before rmdir: {name}")
                os.rmdir(name, dir_fd=directory_fd)
            finally:
                os.close(child_fd)
            continue

        current = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
        if directory_identity(current) != directory_identity(listed):
            raise RuntimeError(f"cleanup refused changed non-directory entry: {name}")
        # unlinkat removes a symbolic link itself; it never follows its target.
        os.unlink(name, dir_fd=directory_fd)


class PackageTestWorkspace:
    def __init__(
        self,
        *,
        base_path: pathlib.Path,
        base_fd: int,
        base_identity: OwnedDirectoryIdentity,
        child_name: str,
        child_identity: OwnedDirectoryIdentity,
        owns_base: bool,
        base_parent_fd: int = -1,
    ) -> None:
        self.base_path = base_path
        self.base_fd = base_fd
        self.base_identity = base_identity
        self.child_name = child_name
        self.child_identity = child_identity
        self.owns_base = owns_base
        self.base_parent_fd = base_parent_fd
        self.quarantine_name: str | None = None

    @property
    def root(self) -> pathlib.Path:
        return self.base_path / self.child_name

    def cleanup(
        self,
        *,
        after_detach: Callable[[int, str], None] | None = None,
        before_rmdir: Callable[[int, str, DirectoryIdentity], None] | None = None,
    ) -> str | None:
        try:
            try:
                quarantine_name = detach_no_replace(self.base_fd, self.child_name, "ava-package-linux-tests")
            except OSError as exc:
                return f"cleanup refused to detach expected private child {self.root}: {exc}"
            self.quarantine_name = quarantine_name

            if after_detach is not None:
                after_detach(self.base_fd, quarantine_name)
            try:
                quarantined = os.stat(quarantine_name, dir_fd=self.base_fd, follow_symlinks=False)
            except OSError as exc:
                return restore_or_preserve(
                    self.base_fd,
                    quarantine_name,
                    self.child_name,
                    self.base_path,
                    f"cleanup could not inspect the detached child: {exc}",
                )
            if (
                owned_directory_identity(quarantined) != self.child_identity
                or not stat.S_ISDIR(quarantined.st_mode)
                or stat.S_ISLNK(quarantined.st_mode)
            ):
                return restore_or_preserve(
                    self.base_fd,
                    quarantine_name,
                    self.child_name,
                    self.base_path,
                    "cleanup refused detached child whose identity, owner, or mode changed",
                )

            try:
                remove_verified_tree(
                    self.base_fd,
                    quarantine_name,
                    self.child_identity,
                    before_rmdir=before_rmdir,
                )
                os.fsync(self.base_fd)
            except OSError as exc:
                return f"cleanup preserved verified quarantine {self.base_path / quarantine_name}: {exc}"
            except RuntimeError as exc:
                return f"cleanup preserved verified quarantine {self.base_path / quarantine_name}: {exc}"

            if self.owns_base:
                return self.remove_owned_empty_base()
            return None
        finally:
            os.close(self.base_fd)
            self.base_fd = -1
            if self.base_parent_fd >= 0:
                os.close(self.base_parent_fd)
                self.base_parent_fd = -1

    def remove_owned_empty_base(self) -> str | None:
        if self.base_parent_fd < 0:
            return f"cleanup preserved private temporary base {self.base_path}: its parent descriptor is unavailable"
        try:
            if os.listdir(self.base_fd):
                return f"cleanup preserved nonempty private temporary base {self.base_path}"
            quarantine_name = detach_no_replace(self.base_parent_fd, self.base_path.name, "ava-package-linux-tests-base")
            quarantined = os.stat(quarantine_name, dir_fd=self.base_parent_fd, follow_symlinks=False)
            if (
                owned_directory_identity(quarantined) != self.base_identity
                or not stat.S_ISDIR(quarantined.st_mode)
                or stat.S_ISLNK(quarantined.st_mode)
            ):
                return restore_or_preserve(
                    self.base_parent_fd,
                    quarantine_name,
                    self.base_path.name,
                    self.base_path.parent,
                    "cleanup refused private temporary base whose identity, owner, or mode changed",
                )
            if os.listdir(self.base_fd):
                return restore_or_preserve(
                    self.base_parent_fd,
                    quarantine_name,
                    self.base_path.name,
                    self.base_path.parent,
                    "cleanup found a nonempty private temporary base after detaching it",
                )
            current = os.stat(quarantine_name, dir_fd=self.base_parent_fd, follow_symlinks=False)
            if directory_identity(current) != directory_identity(quarantined):
                return restore_or_preserve(
                    self.base_parent_fd,
                    quarantine_name,
                    self.base_path.name,
                    self.base_path.parent,
                    "cleanup refused changed private temporary base before rmdir",
                )
            os.rmdir(quarantine_name, dir_fd=self.base_parent_fd)
            return None
        except OSError as exc:
            return f"cleanup preserved private temporary base {self.base_path}: {exc}"


def private_workspace_directory_identity(
    workspace: PackageTestWorkspace,
    path: pathlib.Path,
    description: str,
    *,
    expected_identity: OwnedDirectoryIdentity | None = None,
) -> OwnedDirectoryIdentity:
    """Verify a private direct child of the descriptor-owned test workspace."""
    if path.parent != workspace.root:
        raise RuntimeError(f"{description} is not a direct package-test workspace child: {path}")
    root_fd, _root_identity = open_owned_directory_entry(
        workspace.base_fd,
        workspace.child_name,
        "package-test workspace",
        expected_identity=workspace.child_identity,
    )
    try:
        child_fd, child_identity = open_owned_directory_entry(
            root_fd,
            path.name,
            description,
            expected_identity=expected_identity,
        )
    finally:
        os.close(root_fd)
    os.close(child_fd)
    return child_identity


def validate_default_package_output(
    workspace: PackageTestWorkspace,
    temp: pathlib.Path,
    temp_identity: OwnedDirectoryIdentity,
    artifact: pathlib.Path,
    checksum: pathlib.Path,
    archive_name: str,
    checksum_name: str,
) -> pathlib.Path:
    """Accept only the publisher's private direct child of our recorded TMPDIR."""
    output = artifact.parent
    if (
        output.parent != temp
        or not output.name.startswith("ava-release-output.")
        or len(output.name) == len("ava-release-output.")
        or artifact != output / archive_name
        or checksum != output / checksum_name
    ):
        raise RuntimeError(f"default output was not a private TMPDIR child: {output}")

    root_fd, _root_identity = open_owned_directory_entry(
        workspace.base_fd,
        workspace.child_name,
        "package-test workspace",
        expected_identity=workspace.child_identity,
    )
    try:
        temp_fd, _temp_identity = open_owned_directory_entry(
            root_fd,
            temp.name,
            "package-test TMPDIR",
            expected_identity=temp_identity,
        )
        try:
            output_fd, _output_identity = open_owned_directory_entry(
                temp_fd,
                output.name,
                "default package output",
            )
        finally:
            os.close(temp_fd)
    finally:
        os.close(root_fd)
    os.close(output_fd)
    return output


def create_private_base() -> tuple[pathlib.Path, int, OwnedDirectoryIdentity, int]:
    base_path = pathlib.Path(tempfile.mkdtemp(prefix="ava-package-linux-tests-"))
    parent_fd = -1
    base_fd = -1
    try:
        parent_fd, _parent_status = open_final_directory(base_path.parent, "private package-test base parent")
        base_fd = os.open(
            base_path.name,
            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            dir_fd=parent_fd,
        )
        # chmod the held inode, never the just-created pathname.
        os.fchmod(base_fd, 0o700)
        opened = os.fstat(base_fd)
        listed = os.stat(base_path.name, dir_fd=parent_fd, follow_symlinks=False)
        if (
            owned_directory_identity(opened) != owned_directory_identity(listed)
            or opened.st_uid != os.geteuid()
            or stat.S_IMODE(opened.st_mode) != 0o700
            or not stat.S_ISDIR(opened.st_mode)
        ):
            raise RuntimeError(f"private package-test base changed while opening: {base_path}")
        return base_path, base_fd, owned_directory_identity(opened), parent_fd
    except BaseException:
        if base_fd >= 0:
            os.close(base_fd)
        if parent_fd >= 0:
            os.close(parent_fd)
        # The base was created by this harness, but do not recursively delete it
        # if descriptor setup fails; preserving it is safer than a path walk.
        raise


def is_within(path: pathlib.Path, parent: pathlib.Path) -> bool:
    return path == parent or parent in path.parents


def physical_existing_directory(path: pathlib.Path, description: str) -> pathlib.Path:
    resolved = path.resolve(strict=True)
    if not resolved.is_dir():
        raise RuntimeError(f"{description} must name an existing directory: {path}")
    return resolved


def in_repository_probe_parent(repo: pathlib.Path, build_dir: pathlib.Path) -> pathlib.Path:
    repo_physical = physical_existing_directory(repo, "repository")
    build_physical = physical_existing_directory(build_dir, "build directory")
    if is_within(build_physical, repo_physical):
        return build_dir
    return repo


def prepare_workspace(requested_root: pathlib.Path, repo: pathlib.Path) -> PackageTestWorkspace:
    """Create a descriptor-owned private child while retaining the caller's logical root spelling.

    Resolve only to classify repository containment; never pass that physical
    spelling to the package or model-smoke processes. Refuse a final symlink
    and preserve the caller-owned parent directory during cleanup.
    """
    logical_root = requested_root.absolute()
    existing = checked_final_directory(logical_root, "package-test root")
    resolved_root = logical_root.resolve(strict=False)
    repo_physical = physical_existing_directory(repo, "repository")
    redirected = is_within(resolved_root, repo_physical)

    if redirected:
        base_path, base_fd, base_identity, parent_fd = create_private_base()
        try:
            child_name, child_identity = create_unique_owned_child(base_fd, base_path)
        except BaseException:
            os.close(base_fd)
            os.close(parent_fd)
            raise
        return PackageTestWorkspace(
            base_path=base_path,
            base_fd=base_fd,
            base_identity=base_identity,
            child_name=child_name,
            child_identity=child_identity,
            owns_base=True,
            base_parent_fd=parent_fd,
        )

    if existing is None:
        try:
            os.mkdir(logical_root, 0o700)
        except FileExistsError:
            pass
        except OSError as exc:
            raise RuntimeError(f"unable to create package-test root {logical_root}: {exc}") from exc

    base_fd, opened = open_final_directory(logical_root, "package-test root")
    if existing is not None and directory_identity(existing) != directory_identity(opened):
        os.close(base_fd)
        raise RuntimeError(f"package-test root changed while opening: {logical_root}")
    if opened.st_uid != os.geteuid():
        os.close(base_fd)
        raise RuntimeError(f"package-test root is not owned by effective user {os.geteuid()}: {logical_root}")
    if stat.S_IMODE(opened.st_mode) & 0o022:
        os.close(base_fd)
        raise RuntimeError(f"package-test root must not be group- or other-writable: {logical_root}")
    try:
        child_name, child_identity = create_unique_owned_child(base_fd, logical_root)
    except BaseException:
        os.close(base_fd)
        raise
    return PackageTestWorkspace(
        base_path=logical_root,
        base_fd=base_fd,
        base_identity=owned_directory_identity(opened),
        child_name=child_name,
        child_identity=child_identity,
        owns_base=False,
    )


def write_fd_text(directory_fd: int, name: str, content: bytes) -> None:
    file_fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600, dir_fd=directory_fd)
    try:
        os.write(file_fd, content)
    finally:
        os.close(file_fd)


def require_workspace_cleanup(workspace: PackageTestWorkspace, context: str, **kwargs: object) -> None:
    error = workspace.cleanup(**kwargs)
    if error is not None:
        raise RuntimeError(f"{context}: {error}")


def cleanup_workspace_after_regression(workspace: PackageTestWorkspace, context: str) -> None:
    if workspace.base_fd >= 0:
        require_workspace_cleanup(workspace, context)


def run_workspace_safety_regressions(repo: pathlib.Path, build_dir: pathlib.Path) -> None:
    """Exercise root ownership and cleanup races without touching caller roots."""
    in_repo_parent = in_repository_probe_parent(repo, build_dir)
    in_repo_request = in_repo_parent / f".ava-package-linux-tests-in-repo-root.{secrets.token_hex(12)}"
    in_repo_workspace = prepare_workspace(in_repo_request, repo)
    try:
        if not in_repo_workspace.owns_base or in_repo_workspace.root.parent == in_repo_request.parent:
            raise RuntimeError("in-repository CTest root was not redirected to a private system-temporary base")
        private_base = in_repo_workspace.base_path
        require_workspace_cleanup(in_repo_workspace, "in-repository workspace cleanup")
        if private_base.exists() or in_repo_request.exists():
            raise RuntimeError("in-repository workspace cleanup changed the requested root or retained its private base")
    finally:
        cleanup_workspace_after_regression(in_repo_workspace, "in-repository workspace cleanup after failure")

    with tempfile.TemporaryDirectory(prefix="ava-package-linux-tests-regressions-") as temporary:
        sandbox = pathlib.Path(temporary)

        source_physical = sandbox / "source"
        source_physical.mkdir(mode=0o700)
        source_sentinel = source_physical / "preserve"
        source_sentinel.write_text("preserve\n", encoding="utf-8")
        source_logical = sandbox / "source-logical"
        source_logical.symlink_to(source_physical, target_is_directory=True)
        external_build = sandbox / "external-build"
        external_build.mkdir(mode=0o700)
        nonstandard_build = source_physical / "nonstandard-build"
        nonstandard_build.mkdir(mode=0o700)
        for selected_build, expected_parent in (
            (external_build, source_logical),
            (source_logical / "nonstandard-build", source_logical / "nonstandard-build"),
        ):
            selected_parent = in_repository_probe_parent(source_logical, selected_build)
            if selected_parent != expected_parent:
                raise RuntimeError(
                    f"package-test in-repository probe selected {selected_parent}, expected {expected_parent}"
                )
            requested = selected_parent / f".ava-package-linux-tests-build-dir.{secrets.token_hex(12)}"
            selected_workspace = prepare_workspace(requested, source_logical)
            try:
                if not selected_workspace.owns_base or requested.exists():
                    raise RuntimeError("in-repository build-dir probe was not redirected safely")
            finally:
                cleanup_workspace_after_regression(
                    selected_workspace, "in-repository build-dir workspace cleanup"
                )
        if (source_physical / "build").exists() or source_sentinel.read_text(encoding="utf-8") != "preserve\n":
            raise RuntimeError("build-dir safety regression created source/build or modified source content")

        symlink_target = sandbox / "symlink-target"
        symlink_target.mkdir(mode=0o700)
        target_sentinel = symlink_target / "preserve"
        target_sentinel.write_text("preserve\n", encoding="utf-8")
        symlink_root = sandbox / "symlink-root"
        symlink_root.symlink_to(symlink_target, target_is_directory=True)
        try:
            prepare_workspace(symlink_root, repo)
        except RuntimeError as exc:
            if "must not be a symlink" not in str(exc):
                raise RuntimeError(f"final root symlink was refused for the wrong reason: {exc}") from exc
        else:
            raise RuntimeError("final root symlink was accepted")
        if target_sentinel.read_text(encoding="utf-8") != "preserve\n" or not symlink_root.is_symlink():
            raise RuntimeError("final root symlink refusal modified its target or link")

        non_directory_root = sandbox / "non-directory-root"
        non_directory_root.write_text("preserve\n", encoding="utf-8")
        try:
            prepare_workspace(non_directory_root, repo)
        except RuntimeError as exc:
            if "must be a directory" not in str(exc):
                raise RuntimeError(f"final non-directory root was refused for the wrong reason: {exc}") from exc
        else:
            raise RuntimeError("final non-directory root was accepted")
        if non_directory_root.read_text(encoding="utf-8") != "preserve\n":
            raise RuntimeError("final non-directory root refusal modified caller content")

        external_base = sandbox / "external-base"
        external_base.mkdir(mode=0o700)
        external_sentinel = external_base / "preexisting"
        external_sentinel.write_text("preserve\n", encoding="utf-8")
        external_workspace = prepare_workspace(external_base, repo)
        try:
            (external_workspace.root / "work").write_text("private\n", encoding="utf-8")
            require_workspace_cleanup(external_workspace, "external root cleanup")
            if external_sentinel.read_text(encoding="utf-8") != "preserve\n":
                raise RuntimeError("external caller root content was removed or modified")
        finally:
            cleanup_workspace_after_regression(external_workspace, "external root cleanup after failure")

        # The physical target is needed to decide whether to redirect an
        # in-repository root, but must not replace the path passed to AVA.
        external_logical = sandbox / "external-logical"
        external_logical.symlink_to(external_base, target_is_directory=True)
        logical_root = external_logical / "new-root"
        logical_workspace = prepare_workspace(logical_root, repo)
        try:
            if logical_workspace.base_path != logical_root or not logical_workspace.root.is_dir():
                raise RuntimeError("external workspace lost its logical symlink-ancestor spelling")
            require_workspace_cleanup(logical_workspace, "logical external root cleanup")
            if not logical_root.is_dir() or not external_logical.is_symlink():
                raise RuntimeError("logical external root cleanup changed the caller's parent")
        finally:
            cleanup_workspace_after_regression(logical_workspace, "logical external root cleanup after failure")

        swap_base = sandbox / "top-level-swap"
        swap_base.mkdir(mode=0o700)
        swap_workspace = prepare_workspace(swap_base, repo)
        try:
            saved_child = swap_base / "expected-child"
            os.rename(swap_workspace.child_name, saved_child.name, src_dir_fd=swap_workspace.base_fd, dst_dir_fd=swap_workspace.base_fd)
            os.mkdir(swap_workspace.child_name, 0o700, dir_fd=swap_workspace.base_fd)
            replacement_fd = os.open(
                swap_workspace.child_name,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                dir_fd=swap_workspace.base_fd,
            )
            try:
                write_fd_text(replacement_fd, "replacement", b"preserve\n")
            finally:
                os.close(replacement_fd)
            swap_error = swap_workspace.cleanup()
            if swap_error is None or "restored" not in swap_error:
                raise RuntimeError(f"top-level swap cleanup did not conservatively restore the replacement: {swap_error}")
            if not (swap_base / swap_workspace.child_name / "replacement").is_file() or not saved_child.is_dir():
                raise RuntimeError("top-level swap cleanup removed a replacement or the original child")
        finally:
            cleanup_workspace_after_regression(swap_workspace, "top-level swap workspace cleanup after failure")

        quarantine_base = sandbox / "quarantine-mismatch"
        quarantine_base.mkdir(mode=0o700)
        quarantine_workspace = prepare_workspace(quarantine_base, repo)
        try:
            def replace_quarantine(base_fd: int, quarantine_name: str) -> None:
                os.rename(quarantine_name, "stolen-expected-child", src_dir_fd=base_fd, dst_dir_fd=base_fd)
                os.mkdir(quarantine_name, 0o700, dir_fd=base_fd)
                replacement_fd = os.open(
                    quarantine_name,
                    os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=base_fd,
                )
                try:
                    write_fd_text(replacement_fd, "replacement", b"preserve\n")
                finally:
                    os.close(replacement_fd)

            quarantine_error = quarantine_workspace.cleanup(after_detach=replace_quarantine)
            if quarantine_error is None or "restored" not in quarantine_error:
                raise RuntimeError(f"quarantine mismatch was not restored safely: {quarantine_error}")
            if not (quarantine_base / quarantine_workspace.child_name / "replacement").is_file():
                raise RuntimeError("quarantine mismatch cleanup removed the replacement")
            if not (quarantine_base / "stolen-expected-child").is_dir():
                raise RuntimeError("quarantine mismatch cleanup removed the expected child after it was moved")
        finally:
            cleanup_workspace_after_regression(quarantine_workspace, "quarantine workspace cleanup after failure")

        nested_base = sandbox / "nested-replacement"
        nested_base.mkdir(mode=0o700)
        nested_workspace = prepare_workspace(nested_base, repo)
        try:
            nested = nested_workspace.root / "nested"
            nested.mkdir(mode=0o700)
            (nested / "original").write_text("remove only the original\n", encoding="utf-8")

            def replace_nested(parent_fd: int, name: str, _expected: DirectoryIdentity) -> None:
                if name != "nested":
                    return
                os.rename(name, "nested-original", src_dir_fd=parent_fd, dst_dir_fd=parent_fd)
                os.mkdir(name, 0o700, dir_fd=parent_fd)
                replacement_fd = os.open(
                    name,
                    os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=parent_fd,
                )
                try:
                    write_fd_text(replacement_fd, "replacement", b"preserve\n")
                finally:
                    os.close(replacement_fd)

            nested_error = nested_workspace.cleanup(before_rmdir=replace_nested)
            if nested_error is None or "changed directory entry before rmdir" not in nested_error:
                raise RuntimeError(f"nested replacement cleanup did not refuse the changed entry: {nested_error}")
            if nested_workspace.quarantine_name is None:
                raise RuntimeError("nested replacement cleanup did not retain a quarantine name")
            retained_nested = nested_base / nested_workspace.quarantine_name / "nested" / "replacement"
            if retained_nested.read_bytes() != b"preserve\n":
                raise RuntimeError("nested replacement cleanup removed or traversed the replacement")
        finally:
            cleanup_workspace_after_regression(nested_workspace, "nested workspace cleanup after failure")

        link_base = sandbox / "symlink-cleanup"
        link_base.mkdir(mode=0o700)
        link_workspace = prepare_workspace(link_base, repo)
        try:
            external_target = sandbox / "outside-link-target"
            external_target.write_text("preserve\n", encoding="utf-8")
            (link_workspace.root / "link").symlink_to(external_target)
            require_workspace_cleanup(link_workspace, "symlink cleanup")
            if external_target.read_text(encoding="utf-8") != "preserve\n":
                raise RuntimeError("cleanup followed a symbolic link target")
        finally:
            cleanup_workspace_after_regression(link_workspace, "symlink workspace cleanup after failure")


def run_package_tests(
    args: argparse.Namespace,
    workspace: PackageTestWorkspace,
    repo: pathlib.Path,
) -> int:
    root = workspace.root
    script = args.script.absolute()
    publisher = script.parent / "publish-linux-artifacts.py"
    version = project_version(repo)
    package_name = f"ava-{version}-linux-{package_architecture()}"
    archive_name = f"{package_name}.tar.gz"
    checksum_name = f"{archive_name}.sha256"
    temp = root / "tmp"
    temp.mkdir(mode=0o700)
    temp_identity = private_workspace_directory_identity(workspace, temp, "package-test TMPDIR")
    env = os.environ.copy()
    env["TMPDIR"] = str(temp)
    run_process_cleanup_regressions(root, env)

    # Exercise the real built CLI and fake provider once for the complete model smoke.
    output = root / "accepted-output"
    output.mkdir(mode=0o700)
    success = run(
        package_command(script, args.ava.absolute(), output, args.fake_provider.absolute()),
        env=env,
    )
    artifact = parse_path(success.stdout, "artifact")
    checksum = parse_path(success.stdout, "checksum")
    if artifact != output / archive_name or checksum != output / checksum_name:
        raise RuntimeError(f"package did not use deterministic artifact names: {artifact}, {checksum}")
    if not artifact.is_file() or not checksum.is_file():
        raise RuntimeError("accepted-binary package did not publish the expected archive pair")
    digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
    checksum_parts = checksum.read_text(encoding="utf-8").split()
    if checksum_parts != [digest, artifact.name]:
        raise RuntimeError(f"checksum file does not describe the adjacent archive: {checksum_parts}")
    with tarfile.open(artifact, "r:gz") as archive:
        members = archive.getmembers()
        regular_files = {member.name for member in members if member.isfile()}
        if any(member.issym() or member.islnk() for member in members):
            raise RuntimeError("package archive contains a link")
        if regular_files != expected_files(package_name):
            raise RuntimeError(
                f"archive allowlist mismatch\nactual={sorted(regular_files)}\n"
                f"expected={sorted(expected_files(package_name))}"
            )
        notices = archive.extractfile(f"{package_name}/share/doc/ava/THIRD_PARTY_NOTICES.md")
        provenance = archive.extractfile(f"{package_name}/share/doc/ava/PROVENANCE.json")
        if notices is None or "MIT_LICENSE_ONLY" not in notices.read().decode("utf-8"):
            raise RuntimeError("package third-party notices are missing the guarded-utils boundary")
        if provenance is None:
            raise RuntimeError("package provenance is missing")
        provenance_data = json.loads(provenance.read().decode("utf-8"))
        if provenance_data.get("schema_version") != 3 or provenance_data.get("build_mode") != "supplied-binary":
            raise RuntimeError("package provenance has unexpected accepted-binary content")
        expected_architecture = {"x64": "x86_64", "arm64": "aarch64"}.get(package_architecture())
        if expected_architecture is not None and (
            provenance_data.get("architecture") != expected_architecture
            or provenance_data.get("host_architecture") != expected_architecture
            or provenance_data.get("host_architecture_matches_binary") is not True
        ):
            raise RuntimeError("package provenance does not record native host/ELF architecture agreement")
        if provenance_data.get("release_qualified") is not False:
            raise RuntimeError("accepted-binary package must remain unqualified")
        extract = root / "independent-extract"
        extract.mkdir(mode=0o700)
        archive.extractall(extract, filter="data")
    extracted_ava = extract / package_name / "bin" / "ava"
    extracted_version = run([str(extracted_ava), "--version"], env=env).stdout.strip()
    if extracted_version != f"ava {version}":
        raise RuntimeError(f"independently extracted CLI smoke returned unexpected version: {extracted_version}")
    if "Usage" not in run([str(extracted_ava), "--help"], env=env).stdout:
        raise RuntimeError("independently extracted CLI help smoke failed")

    fixture = root / "fixture-ava"
    write_ava_fixture(fixture, version)
    in_repo_parent = in_repository_probe_parent(repo, args.build_dir)

    default_result = run(package_command(script, fixture), env=env)
    default_artifact = parse_path(default_result.stdout, "artifact")
    default_checksum = parse_path(default_result.stdout, "checksum")
    default_output = validate_default_package_output(
        workspace,
        temp,
        temp_identity,
        default_artifact,
        default_checksum,
        archive_name,
        checksum_name,
    )
    if default_output == output:
        raise RuntimeError(f"default output was not distinct from the requested output: {default_output}")
    # The descriptor-anchored workspace cleanup owns this directory.  Never
    # recursively delete a pathname whose spelling arrived via child stdout.

    insecure = root / "insecure-output"
    insecure.mkdir(mode=0o700)
    insecure.chmod(0o755)
    insecure_result = run(package_command(script, fixture, insecure), env=env, check=False)
    require_failure(insecure_result, "exact mode 0700", "non-0700 output was not rejected")

    in_repo = in_repo_parent / f".ava-package-linux-in-repo-negative.{secrets.token_hex(12)}"
    in_repo_result = run(package_command(script, fixture, in_repo), env=env, check=False)
    require_failure(in_repo_result, "outside the repository", "in-repository output was not rejected")

    publisher_fake_repo = root / f"publisher-check-repository-{secrets.token_hex(8)}"
    publisher_fake_repo.mkdir(mode=0o700)
    opened_in_repo = publisher_fake_repo / f"publisher-opened-in-repo-{secrets.token_hex(8)}"
    opened_in_repo.mkdir(mode=0o700)
    try:
        opened_in_repo_result = run(
            [
                sys.executable,
                str(publisher),
                "--check",
                str(opened_in_repo),
                "--repository-root",
                str(publisher_fake_repo),
            ],
            env=env,
            check=False,
        )
        require_failure(
            opened_in_repo_result,
            "outside the repository",
            "publisher --check did not classify its already-opened output descriptor ancestry",
        )
        assert_output_empty(opened_in_repo, "rejected publisher --check modified an in-repository directory")
    finally:
        opened_in_repo.rmdir()
        publisher_fake_repo.rmdir()

    symlink_checkout = root / "symlinked-checkout"
    symlink_checkout.symlink_to(repo, target_is_directory=True)
    symlinked_script = symlink_checkout / "scripts" / "package-linux.sh"
    symlinked_in_repo_output = in_repo_parent / f"package-linux-symlinked-in-repo-{secrets.token_hex(8)}"
    if symlinked_in_repo_output.exists() or symlinked_in_repo_output.is_symlink():
        raise RuntimeError("symlinked-checkout output rejection fixture unexpectedly exists")
    symlinked_output_result = run(package_command(symlinked_script, fixture, symlinked_in_repo_output), env=env, check=False)
    require_failure(
        symlinked_output_result,
        "outside the repository",
        "symlinked checkout did not reject a physically in-repository output",
    )
    if symlinked_in_repo_output.exists() or symlinked_in_repo_output.is_symlink():
        raise RuntimeError("rejected symlinked-checkout output mutated the repository")

    symlinked_external_output = root / "symlinked-checkout-external-output"
    symlinked_external_output.mkdir(mode=0o700)
    in_repo_temp_entries_before = set(in_repo_parent.glob("ava-package-linux.*"))
    symlinked_env = dict(env)
    symlinked_env["TMPDIR"] = str(in_repo_parent)
    symlinked_external_result = run(package_command(symlinked_script, fixture, symlinked_external_output), env=symlinked_env)
    parse_path(symlinked_external_result.stdout, "artifact")
    in_repo_temp_entries_after = set(in_repo_parent.glob("ava-package-linux.*"))
    if in_repo_temp_entries_after != in_repo_temp_entries_before:
        raise RuntimeError("symlinked checkout used a physically in-repository TMPDIR")

    symlink_directory_target = root / "symlink-directory-target"
    symlink_directory_target.mkdir(mode=0o700)
    symlink_directory = root / "symlink-directory"
    symlink_directory.symlink_to(symlink_directory_target, target_is_directory=True)
    symlink_directory_result = run(package_command(script, fixture, symlink_directory), env=env, check=False)
    require_failure(symlink_directory_result, "must not be a symlink", "symlink output directory was not rejected")

    mismatch = root / "ava-mismatched-version"
    write_ava_fixture(mismatch, "9.9.9")
    mismatch_output = root / "mismatch-output"
    mismatch_output.mkdir(mode=0o700)
    mismatch_result = run(package_command(script, mismatch, mismatch_output), env=env, check=False)
    require_failure(mismatch_result, "does not match current checkout", "mismatched accepted binary version was not rejected")

    non_elf = root / "non-elf-ava"
    write_executable(
        non_elf,
        f"#!/bin/sh\nprintf 'ava {version}\\n'\n",
    )
    non_elf_output = root / "non-elf-output"
    non_elf_output.mkdir(mode=0o700)
    non_elf_result = run(package_command(script, non_elf, non_elf_output), env=env, check=False)
    require_failure(
        non_elf_result,
        "does not contain a detected ELF architecture",
        "non-ELF accepted binary received a misleading architecture name",
    )
    assert_output_empty(non_elf_output, "non-ELF accepted binary published an artifact")

    binary_symlink = root / "binary-symlink"
    binary_symlink.symlink_to(fixture)
    binary_symlink_output = root / "binary-symlink-output"
    binary_symlink_output.mkdir(mode=0o700)
    binary_symlink_result = run(package_command(script, binary_symlink, binary_symlink_output), env=env, check=False)
    require_failure(
        binary_symlink_result,
        "must not be a symlink",
        "accepted --binary final symlink was not rejected",
    )

    non_executable = root / "non-executable-binary"
    non_executable.write_bytes(fixture.read_bytes())
    non_executable.chmod(0o600)
    non_executable_output = root / "non-executable-output"
    non_executable_output.mkdir(mode=0o700)
    non_executable_result = run(
        package_command(script, non_executable, non_executable_output), env=env, check=False
    )
    require_failure(
        non_executable_result,
        "must name an executable regular file",
        "non-executable accepted --binary was not rejected",
    )

    # Both publisher source paths must classify a FIFO without opening it for
    # reading. Run direct Python children with finite timeouts (never a shell)
    # so a regression cannot leave a blocked descendant behind.
    fifo_source = root / "publisher-source-fifo"
    os.mkfifo(fifo_source, 0o600)
    fifo_snapshot_destination = root / "publisher-source-fifo-snapshot"
    try:
        fifo_snapshot_result = run(
            [
                sys.executable,
                str(publisher),
                "--snapshot-executable",
                str(fifo_source),
                str(fifo_snapshot_destination),
            ],
            env=env,
            check=False,
            timeout=5.0,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("FIFO snapshot-source classification blocked instead of failing promptly") from exc
    require_failure(
        fifo_snapshot_result,
        "must name an executable regular file",
        "FIFO snapshot-source classification did not fail promptly",
    )
    if fifo_snapshot_destination.exists():
        raise RuntimeError("FIFO snapshot-source classification created a destination")

    fifo_publication_output = root / "publisher-fifo-output"
    fifo_publication_output.mkdir(mode=0o700)
    fifo_output_identity = run(
        [sys.executable, str(publisher), "--check", str(fifo_publication_output)], env=env
    ).stdout.strip()
    try:
        fifo_publication_result = run(
            [
                sys.executable,
                str(publisher),
                "--repository-root",
                str(repo),
                "--output",
                str(fifo_publication_output),
                "--expected-directory-identity",
                fifo_output_identity,
                "--file",
                str(fifo_source),
                "fifo.bin",
            ],
            env=env,
            check=False,
            timeout=5.0,
        )
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError("FIFO publication-source classification blocked instead of failing promptly") from exc
    require_failure(
        fifo_publication_result,
        "publication source is not a regular file",
        "FIFO publication-source classification did not fail promptly",
    )
    assert_output_empty(fifo_publication_output, "FIFO publication-source classification created output")

    # Mutating an already-open source inode during its copy must invalidate and
    # remove the private snapshot, even when the source pathname never changes.
    publisher_module = load_publisher(publisher)
    changing_source = root / "changing-snapshot-source"
    write_executable(changing_source, "#!/bin/sh\nexit 0\n")
    changing_destination_root = root / "changing-snapshot-destination"
    changing_destination_root.mkdir(mode=0o700)
    changing_destination = changing_destination_root / "ava"
    original_descriptor_copy = publisher_module.copy_file_descriptors

    def copy_then_mutate(source_fd: int, target_fd: int, source_path: pathlib.Path) -> None:
        original_descriptor_copy(source_fd, target_fd, source_path)
        mutation_fd = os.open(source_path, os.O_WRONLY | os.O_APPEND)
        try:
            os.write(mutation_fd, b"# changed in place\n")
            os.fsync(mutation_fd)
        finally:
            os.close(mutation_fd)

    publisher_module.copy_file_descriptors = copy_then_mutate
    try:
        try:
            publisher_module.snapshot_executable(changing_source, changing_destination)
        except RuntimeError as exc:
            if "changed while copying" not in str(exc):
                raise RuntimeError(f"source-mutation snapshot failed for the wrong reason: {exc}") from exc
        else:
            raise RuntimeError("in-place source mutation did not invalidate the executable snapshot")
    finally:
        publisher_module.copy_file_descriptors = original_descriptor_copy
    if changing_destination.exists():
        raise RuntimeError("invalid executable snapshot remained after in-place source mutation")

    fake_target = root / "fake-provider-fixture"
    write_executable(fake_target, "#!/bin/sh\nexit 0\n")
    fake_symlink = root / "fake-provider-symlink"
    fake_symlink.symlink_to(fake_target)
    fake_symlink_output = root / "fake-symlink-output"
    fake_symlink_output.mkdir(mode=0o700)
    fake_symlink_result = run(
        package_command(script, fixture, fake_symlink_output, fake_symlink), env=env, check=False
    )
    require_failure(
        fake_symlink_result,
        "must not be a symlink",
        "accepted --fake-provider final symlink was not rejected",
    )

    # Block the snapshotted CLI after both accepted inputs have been copied, replace
    # both original pathnames, and prove packaging/model smoke still use old bytes.
    snapshot_ava = root / "snapshot-ava"
    write_ava_fixture(snapshot_ava, version)
    accepted_ava_bytes = snapshot_ava.read_bytes()
    snapshot_fake = root / "snapshot-fake-provider"
    write_executable(snapshot_fake, "#!/bin/sh\nprintf 'accepted fake provider\\n'\n")
    accepted_fake_digest = hashlib.sha256(snapshot_fake.read_bytes()).hexdigest()
    snapshot_marker = root / "snapshot-version-started"
    snapshot_gate = root / "snapshot-version-release"
    fake_smoke_marker = root / "snapshot-fake-smoked"
    wrapper_dir = root / "cmake-wrapper"
    wrapper_dir.mkdir(mode=0o700)
    write_executable(
        wrapper_dir / "cmake",
        """#!/usr/bin/env python3
import hashlib
import os
import pathlib
import sys

prefix = "-DAVA_FAKE_PROVIDER_EXE="
values = [argument[len(prefix):] for argument in sys.argv[1:] if argument.startswith(prefix)]
if len(values) != 1:
    print("expected exactly one fake-provider CMake definition", file=sys.stderr)
    raise SystemExit(2)
snapshot = pathlib.Path(values[0])
original = pathlib.Path(os.environ["AVA_PACKAGE_TEST_FAKE_ORIGINAL"])
if snapshot == original or snapshot.name != "fake-provider" or snapshot.parent.name != "inputs":
    print(f"fake provider was not a private input snapshot: {snapshot}", file=sys.stderr)
    raise SystemExit(3)
actual = hashlib.sha256(snapshot.read_bytes()).hexdigest()
if actual != os.environ["AVA_PACKAGE_TEST_FAKE_DIGEST"]:
    print(f"fake-provider snapshot digest changed: {actual}", file=sys.stderr)
    raise SystemExit(4)
pathlib.Path(os.environ["AVA_PACKAGE_TEST_FAKE_SMOKE_MARKER"]).write_text("smoked\\n", encoding="utf-8")
""",
    )
    snapshot_output = root / "snapshot-output"
    snapshot_output.mkdir(mode=0o700)
    snapshot_env = env.copy()
    snapshot_env["PATH"] = f"{wrapper_dir}{os.pathsep}{env['PATH']}"
    snapshot_env["AVA_PACKAGE_TEST_MARKER"] = str(snapshot_marker)
    snapshot_env["AVA_PACKAGE_TEST_GATE"] = str(snapshot_gate)
    snapshot_env["AVA_PACKAGE_TEST_FAKE_ORIGINAL"] = str(snapshot_fake)
    snapshot_env["AVA_PACKAGE_TEST_FAKE_DIGEST"] = accepted_fake_digest
    snapshot_env["AVA_PACKAGE_TEST_FAKE_SMOKE_MARKER"] = str(fake_smoke_marker)
    snapshot_process = start(
        package_command(script, snapshot_ava, snapshot_output, snapshot_fake),
        env=snapshot_env,
    )
    wait_for_path(snapshot_marker, snapshot_process)
    replacement_ava = root / "snapshot-ava-replacement"
    write_ava_fixture(replacement_ava, "9.9.9")
    os.replace(replacement_ava, snapshot_ava)
    replacement_fake = root / "snapshot-fake-replacement"
    write_executable(replacement_fake, "#!/bin/sh\nprintf 'replacement fake provider\\n'\n")
    os.replace(replacement_fake, snapshot_fake)
    snapshot_gate.touch()
    snapshot_result = finish(snapshot_process)
    if snapshot_result.returncode != 0:
        raise RuntimeError(
            f"snapshot-pinning package failed\nstdout:\n{snapshot_result.stdout}\nstderr:\n{snapshot_result.stderr}"
        )
    if not fake_smoke_marker.is_file():
        raise RuntimeError("snapshot-pinning test did not exercise the snapshotted fake provider")
    snapshot_artifact = parse_path(snapshot_result.stdout, "artifact")
    with tarfile.open(snapshot_artifact, "r:gz") as archive:
        packaged_ava = archive.extractfile(f"{package_name}/bin/ava")
        if packaged_ava is None or packaged_ava.read() != accepted_ava_bytes:
            raise RuntimeError("archive did not contain the one-time accepted --binary snapshot")
    if snapshot_ava.read_bytes() == accepted_ava_bytes:
        raise RuntimeError("snapshot race did not replace the original --binary pathname")
    if hashlib.sha256(snapshot_fake.read_bytes()).hexdigest() == accepted_fake_digest:
        raise RuntimeError("snapshot race did not replace the original --fake-provider pathname")

    # Exercise build mode without recursively building AVA. Fake cmake creates
    # both build outputs; the snapshotted AVA replaces those mutable paths on
    # its first --version call. Fake install deliberately stages the replaced
    # AVA, so only an unconditional overwrite from the snapshot can pass.
    build_ava_template = root / "build-mode-ava-template"
    write_mutating_ava_fixture(build_ava_template, version)
    build_ava_bytes = build_ava_template.read_bytes()
    build_ava_digest = hashlib.sha256(build_ava_bytes).hexdigest()
    build_fake_template = root / "build-mode-fake-template"
    write_executable(build_fake_template, "#!/bin/sh\nprintf 'snapshotted build fake provider\\n'\n")
    build_fake_digest = hashlib.sha256(build_fake_template.read_bytes()).hexdigest()
    build_ava_replacement = root / "build-mode-ava-replacement"
    write_ava_fixture(build_ava_replacement, "9.9.9")
    build_fake_replacement = root / "build-mode-fake-replacement"
    write_executable(build_fake_replacement, "#!/bin/sh\nprintf 'mutable replacement fake provider\\n'\n")
    fake_build_repo, build_wrapper_dir = create_fake_build_repository(
        repo,
        script,
        publisher,
        root,
        package_name,
        version,
    )
    build_mutation_marker = root / "build-mode-mutated"
    build_model_marker = root / "build-mode-model-smoked"
    build_output = root / "build-mode-output"
    build_output.mkdir(mode=0o700)
    build_env = env.copy()
    build_env["PATH"] = f"{build_wrapper_dir}{os.pathsep}{env['PATH']}"
    build_env["AVA_PACKAGE_TEST_BUILD_REPO"] = str(fake_build_repo)
    build_env["AVA_PACKAGE_TEST_BUILD_AVA_TEMPLATE"] = str(build_ava_template)
    build_env["AVA_PACKAGE_TEST_BUILD_FAKE_TEMPLATE"] = str(build_fake_template)
    build_env["AVA_PACKAGE_TEST_BUILD_AVA_REPLACEMENT"] = str(build_ava_replacement)
    build_env["AVA_PACKAGE_TEST_BUILD_FAKE_REPLACEMENT"] = str(build_fake_replacement)
    build_env["AVA_PACKAGE_TEST_BUILD_MUTATION_MARKER"] = str(build_mutation_marker)
    build_env["AVA_PACKAGE_TEST_BUILD_MODEL_MARKER"] = str(build_model_marker)
    build_env["AVA_PACKAGE_TEST_BUILD_AVA_DIGEST"] = build_ava_digest
    build_env["AVA_PACKAGE_TEST_BUILD_FAKE_DIGEST"] = build_fake_digest
    build_result = run(
        build_package_command(fake_build_repo / "scripts" / "package-linux.sh", build_output),
        env=build_env,
    )
    if not build_mutation_marker.is_file() or not build_model_marker.is_file():
        raise RuntimeError("fake build-mode harness did not exercise mutation and deterministic model smoke")
    build_location = pathlib.Path((fake_build_repo / "build-location").read_text(encoding="utf-8"))
    if build_location.exists():
        raise RuntimeError("private build-mode tree was retained after packaging")
    build_artifact = parse_path(build_result.stdout, "artifact")
    with tarfile.open(build_artifact, "r:gz") as archive:
        packaged_build_ava = archive.extractfile(f"{package_name}/bin/ava")
        if packaged_build_ava is None or packaged_build_ava.read() != build_ava_bytes:
            raise RuntimeError("build-mode archive did not contain the exact private AVA snapshot")

    # Replace the approved output pathname while packaging is blocked. A fresh,
    # owner-owned 0700 directory at the same path must not inherit approval.
    identity_ava = root / "identity-ava"
    write_ava_fixture(identity_ava, version)
    identity_marker = root / "identity-version-started"
    identity_gate = root / "identity-version-release"
    identity_output = root / "identity-output"
    identity_output.mkdir(mode=0o700)
    identity_env = env.copy()
    identity_env["AVA_PACKAGE_TEST_MARKER"] = str(identity_marker)
    identity_env["AVA_PACKAGE_TEST_GATE"] = str(identity_gate)
    identity_process = start(package_command(script, identity_ava, identity_output), env=identity_env)
    wait_for_path(identity_marker, identity_process)
    detached_approved_output = root / "identity-output-approved-original"
    identity_output.rename(detached_approved_output)
    identity_output.mkdir(mode=0o700)
    identity_gate.touch()
    identity_result = finish(identity_process)
    require_failure(
        identity_result,
        "identity changed since approval",
        "replacement output directory incorrectly inherited initial approval",
    )
    assert_output_empty(identity_output, "replacement output directory received publication files")
    assert_output_empty(detached_approved_output, "detached approved output directory received publication files")

    first_source = root / "pair-first"
    second_source = root / "pair-second"
    first_source.write_bytes(b"first\n")
    second_source.write_bytes(b"second\n")

    # Deliver SIGTERM immediately after publishing the checksum (the first
    # final rename). The helper must restore handlers and roll back that final
    # plus the archive temporary, leaving no half-pair or private temporary.
    cancellation_output = root / "cancellation-rollback-output"
    cancellation_output.mkdir(mode=0o700)
    cancellation_identity = run(
        [sys.executable, str(publisher), "--check", str(cancellation_output)], env=env
    ).stdout.strip()
    cancellation_driver = root / "interrupt-publication.py"
    cancellation_driver.write_text(
        """import importlib.util
import os
import pathlib
import signal
import sys

publisher_path, repository_path, output_path, identity, checksum_source, archive_source = sys.argv[1:]
spec = importlib.util.spec_from_file_location("ava_publish_interruption_test", publisher_path)
if spec is None or spec.loader is None:
    raise SystemExit("could not load publisher")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
original_rename = module.rename_no_replace
rename_count = [0]


def rename_then_interrupt(directory_fd, temporary_name, final_name):
    original_rename(directory_fd, temporary_name, final_name)
    rename_count[0] += 1
    if rename_count[0] == 1:
        if not final_name.endswith(".sha256"):
            raise RuntimeError(f"archive was published before checksum: {final_name}")
        os.kill(os.getpid(), signal.SIGTERM)


module.rename_no_replace = rename_then_interrupt
initial_sigint = signal.getsignal(signal.SIGINT)
initial_sigterm = signal.getsignal(signal.SIGTERM)
try:
    module.publish(
        pathlib.Path(output_path),
        [
            (pathlib.Path(checksum_source), "pair.tar.gz.sha256"),
            (pathlib.Path(archive_source), "pair.tar.gz"),
        ],
        module.parse_identity(identity),
        pathlib.Path(repository_path),
    )
except module.PublicationCancelled as exc:
    if exc.signal_number != signal.SIGTERM:
        raise RuntimeError(f"unexpected cancellation signal: {exc.signal_number}") from exc
else:
    raise RuntimeError("SIGTERM after first final rename did not cancel publication")
if signal.getsignal(signal.SIGINT) != initial_sigint or signal.getsignal(signal.SIGTERM) != initial_sigterm:
    raise RuntimeError("publication did not restore Python signal handlers")
raise SystemExit(91)
""",
        encoding="utf-8",
    )
    cancellation_result = run(
        [
            sys.executable,
            str(cancellation_driver),
            str(publisher),
            str(repo),
            str(cancellation_output),
            cancellation_identity,
            str(first_source),
            str(second_source),
        ],
        env=env,
        check=False,
    )
    if cancellation_result.returncode != 91:
        raise RuntimeError(
            "publication interruption regression did not reach the expected cancellation path\n"
            f"return code: {cancellation_result.returncode}\n"
            f"stdout:\n{cancellation_result.stdout}\nstderr:\n{cancellation_result.stderr}"
        )
    assert_output_empty(cancellation_output, "cancelled publication left a final or temporary")

    # Keep the ordinary second-rename failure regression: a duplicate second
    # final must roll back the first final and all private temporaries.
    pair_output = root / "pair-rollback-output"
    pair_output.mkdir(mode=0o700)
    approved_identity = run([sys.executable, str(publisher), "--check", str(pair_output)], env=env).stdout.strip()
    pair_result = run(
        [
            sys.executable,
            str(publisher),
            "--repository-root",
            str(repo),
            "--output",
            str(pair_output),
            "--expected-directory-identity",
            approved_identity,
            "--file",
            str(first_source),
            "duplicate.bin",
            "--file",
            str(second_source),
            "duplicate.bin",
        ],
        env=env,
        check=False,
    )
    require_failure(pair_result, "refusing to overwrite", "duplicate second publication did not fail")
    assert_output_empty(pair_output, "transactional pair rollback left an orphan or temporary")

    # Existing regular files and symlinks are never removed or overwritten.
    existing_output = root / "existing-output"
    existing_output.mkdir(mode=0o700)
    existing_identity = run([sys.executable, str(publisher), "--check", str(existing_output)], env=env).stdout.strip()
    existing = existing_output / "existing.bin"
    existing.write_bytes(b"preexisting\n")
    existing_result = run(
        [
            sys.executable,
            str(publisher),
            "--repository-root",
            str(repo),
            "--output",
            str(existing_output),
            "--expected-directory-identity",
            existing_identity,
            "--file",
            str(first_source),
            existing.name,
        ],
        env=env,
        check=False,
    )
    require_failure(existing_result, "must be empty", "nonempty publication directory was not rejected")
    if existing.read_bytes() != b"preexisting\n":
        raise RuntimeError("publication modified or removed an existing regular destination")

    symlink_output = root / "symlink-output"
    symlink_output.mkdir(mode=0o700)
    sentinel = root / "sentinel"
    sentinel.write_text("do not modify\n", encoding="utf-8")
    (symlink_output / archive_name).symlink_to(sentinel)
    symlink_result = run(package_command(script, fixture, symlink_output), env=env, check=False)
    require_failure(symlink_result, "must be empty", "nonempty output with a symlink was not rejected")
    if sentinel.read_text(encoding="utf-8") != "do not modify\n":
        raise RuntimeError("publication followed and modified an existing symlink target")
    if not (symlink_output / archive_name).is_symlink():
        raise RuntimeError("publication removed an existing destination symlink")

    print("Linux package snapshot, identity, rollback, allowlist, checksum, and smoke tests passed")
    return 0


def cleanup_main_resources(workspace: PackageTestWorkspace) -> None:
    errors: list[str] = []
    cleanup_failure: RuntimeError | None = None
    # Preserve the descriptor-anchored workspace cleanup even when a second
    # termination request arrives while owned groups are being drained.
    with PackageTestTerminationDeferral():
        try:
            cleanup_verified_owned_processes()
        except RuntimeError as exc:
            errors.append(str(exc))
        cleanup_error = workspace.cleanup()
        if cleanup_error is not None:
            errors.append(f"package-test workspace cleanup failed: {cleanup_error}")

    if errors:
        message = "\n".join(errors)
        active_error = sys.exc_info()[1]
        if active_error is not None:
            active_error.add_note(message)
        else:
            cleanup_failure = RuntimeError(message)
    raise_deferred_package_test_termination()
    if cleanup_failure is not None:
        raise cleanup_failure


def run_main_package_tests(args: argparse.Namespace) -> int:
    if platform.system() != "Linux":
        print("skipping Linux package tests on non-Linux host")
        return 77

    workspace: PackageTestWorkspace | None = None
    with PackageTestTerminationHandlers():
        try:
            repo = args.repo.absolute()
            # Every safety regression creates and then immediately cleans a private
            # workspace. Defer terminal delivery across the complete sequence so a
            # signal cannot land between creation and that local cleanup.
            with PackageTestTerminationDeferral():
                run_workspace_safety_regressions(repo, args.build_dir)
            raise_deferred_package_test_termination()
            # Keep a terminal signal from landing after prepare_workspace()
            # creates its private child but before this finally block can own it.
            with PackageTestTerminationDeferral():
                workspace = prepare_workspace(args.root, repo)
            raise_deferred_package_test_termination()
            return run_package_tests(args, workspace, repo)
        finally:
            if workspace is None:
                cleanup_verified_owned_processes()
            else:
                cleanup_main_resources(workspace)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path, required=True)
    parser.add_argument("--ava", type=pathlib.Path, required=True)
    parser.add_argument("--fake-provider", type=pathlib.Path, required=True)
    parser.add_argument("--repo", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--signal-cleanup-regression-child", action="store_true")
    parser.add_argument("--signal-cleanup-fixture", type=pathlib.Path)
    parser.add_argument("--signal-cleanup-descendant-ready", type=pathlib.Path)
    parser.add_argument("--signal-cleanup-leader", type=pathlib.Path)
    parser.add_argument("--signal-cleanup-ready", type=pathlib.Path)
    args = parser.parse_args()

    try:
        if args.signal_cleanup_regression_child:
            return run_signal_cleanup_regression_child(args)
        return run_main_package_tests(args)
    except PackageTestTermination as cancellation:
        reraise_package_test_termination(cancellation)


if __name__ == "__main__":
    raise SystemExit(main())
