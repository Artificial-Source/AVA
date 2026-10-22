#!/usr/bin/env python3
"""Run AVA's offline pre-modernization backend benchmark suites.

The harness is dependency-free, isolates process state, records every requested
M0 family, and writes both machine-readable JSON and an optional Markdown view.
It does not make cross-machine performance claims.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import http.server
import json
import math
import os
import platform
import re
import resource
import shutil
import signal
import stat
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path, PurePosixPath
from typing import Any, Sequence

SCHEMA_VERSION = "ava.backend-benchmark.v2"
TRUSTED_PATH = "/usr/local/bin:/usr/bin:/bin"
EXPECTED_RESULT_IDS = (
    "cold_startup",
    "warm_startup",
    "idle_rss",
    "builtin_noop_dispatch",
    "native_file_read_dispatch",
    "native_registry_lookup",
    "plugin_first_call",
    "plugin_repeated_calls",
    "unused_plugin_manifests_100",
    "catalog_entries_100",
    "catalog_entries_500",
    "catalog_entries_1000",
    "short_calls_10000",
    "canceled_calls_10000",
    "session_open_1000",
    "session_open_10000",
    "session_open_100000",
    "metadata_browsing",
    "cancellation_acknowledgement",
    "child_cleanup",
    "repeated_call_memory",
    "selective_router",
)

PROCESS_SCHEMA_VERSION = "ava.backend-benchmark.v3"
PROCESS_HELPER_SCHEMA_VERSION = "ava.backend-benchmark-helper.v2"
COMPARISON_SCHEMA_VERSION = "ava.backend-benchmark-comparison.v1"
PROCESS_CONTRACT_VERSION = "m1_process_supervision_v1"
PROCESS_EXPECTED_RESULT_IDS = (
    "application_warm_startup",
    "application_idle_rss",
    "supervisor_idle_scope_startup",
    "supervisor_first_spawn_commit",
    "supervisor_warm_sequential_spawn_commit",
    "supervisor_concurrent_records_1",
    "supervisor_concurrent_records_8",
    "supervisor_concurrent_records_64",
    "supervisor_natural_exit_settlement",
    "supervisor_leader_first_descendant_cleanup",
    "supervisor_term_refusal_escalation",
    "supervisor_shared_budget_shutdown_64",
    "monitor_idle_pidfd_1",
    "monitor_idle_pidfd_8",
    "monitor_idle_pidfd_64",
    "monitor_idle_posix_fallback_1",
    "monitor_idle_posix_fallback_8",
    "monitor_idle_posix_fallback_64",
    "family_curl_lifecycle",
    "family_plugin_lifecycle",
    "family_mcp_lifecycle",
    "family_lsp_lifecycle",
    "family_bash_lifecycle",
)
PROCESS_REASON_TEXT = {
    "source_architecture_absent": "Process-supervision source architecture is absent from this build.",
    "caller_not_migrated": "The family authority flag requests supervision, but this benchmark driver has not been adapted to verify a finished Supervisor record.",
    "pidfd_unavailable": "Automatic monitoring did not select pidfd on this host.",
    "fixture_unavailable": "A required repository-owned benchmark fixture is unavailable.",
    "platform_unsupported": "The process-supervision backend is unavailable on this platform.",
    "procfs_unavailable": "Application idle RSS requires Linux procfs.",
}
PROCESS_CLOSED_LABELS = {
    "legacy_local",
    "supervised",
    "neutral_supervisor",
    "immediate_children_only",
    "managed_group",
    "posix",
    "unsupported",
    "event_driven_posix",
    "automatic_pidfd",
    "automatic",
    "posix_fallback",
    PROCESS_CONTRACT_VERSION,
}
PROCESS_PRODUCTION_SOURCE_PATHS = ("src", "CMakeLists.txt", "cmake", "config.h.in")
PROCESS_FAMILY_SOURCE_SCOPES = {
    "curl": {
        "scope_kind": "recursive_git_pathspec",
        "canonical_paths": (),
        "canonical_pathspecs": (":(glob)src/ava/http/curl_transport*",),
    },
    "plugin": {
        "scope_kind": "recursive_tree",
        "canonical_paths": ("src/ava/plugin",),
        "canonical_pathspecs": (),
    },
    "mcp": {
        "scope_kind": "recursive_tree",
        "canonical_paths": ("src/ava/mcp",),
        "canonical_pathspecs": (),
    },
    "lsp": {
        "scope_kind": "recursive_tree",
        "canonical_paths": ("src/ava/lsp",),
        "canonical_pathspecs": (),
    },
    "bash": {
        "scope_kind": "recursive_git_pathspec",
        "canonical_paths": (),
        "canonical_pathspecs": (":(glob)src/ava/tools/bash_tool*",),
    },
}
PROCESS_SHARED_SOURCE_SCOPE = {
    "scope_kind": "recursive_git_pathspec",
    "canonical_paths": ("CMakeLists.txt", "config.h.in"),
    "canonical_pathspecs": (
        ":(glob)src/**",
        ":(glob)cmake/**",
        ":(exclude)src/ava/plugin",
        ":(exclude)src/ava/mcp",
        ":(exclude)src/ava/lsp",
        ":(exclude,glob)src/ava/http/curl_transport*",
        ":(exclude,glob)src/ava/tools/bash_tool*",
    ),
}
PROCESS_FAMILY_NAMES = tuple(PROCESS_FAMILY_SOURCE_SCOPES)
PROCESS_AUTHORITY_SOURCE_PATH = "tests/backend_benchmark_authorities.cmake"
PROCESS_PROVENANCE_SPLIT_FIELDS = (
    ("measured_checkout", "repository"),
    ("harness", "repository"),
    ("harness", "benchmark_script_sha256"),
)
FULL_GIT_OBJECT_ID = re.compile(r"^[0-9a-f]{40}$")
FULL_SHA256 = re.compile(r"^[0-9a-f]{64}$")
GIT_ENTRY_MODE = re.compile(r"^[0-7]{6}$")


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run reproducible offline AVA backend benchmarks.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  scripts/benchmark-backend.py --ava build/ava --benchmark-helper build/tests/ava_backend_benchmark_helper --suite smoke --runs 1 --output /tmp/ava-smoke.json
  scripts/benchmark-backend.py --ava build-release/ava --benchmark-helper build-release/tests/ava_backend_benchmark_helper --suite baseline --runs 5 --output baseline.json --report baseline.md
  scripts/benchmark-backend.py --ava build-release/ava --benchmark-helper build-release/tests/ava_backend_benchmark_helper --suite stress --runs 10 --output stress.json
""",
    )
    parser.add_argument("--ava", type=Path, required=True, help="path to the AVA executable")
    parser.add_argument("--benchmark-helper", type=Path, help="test-only deterministic C++ benchmark helper")
    parser.add_argument("--fake-provider", type=Path, help="optional local fake provider executable (recorded; no network provider is used)")
    parser.add_argument("--fake-process-child", type=Path, help="repository-owned process-supervisor child fixture")
    parser.add_argument("--fake-mcp-server", type=Path, help="repository-owned fake MCP server fixture")
    parser.add_argument("--fake-lsp-server", type=Path, help="repository-owned fake LSP server fixture")
    parser.add_argument(
        "--memory-helper",
        type=Path,
        default=Path(__file__).with_name("benchmark-memory.py"),
        help="existing idle-memory helper (default: scripts/benchmark-memory.py)",
    )
    parser.add_argument(
        "--sample-plugin",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "examples" / "plugins" / "todo",
        help="sample todo plugin fixture",
    )
    parser.add_argument("--suite", choices=("smoke", "baseline", "stress", "process-smoke", "process-baseline"), required=True)
    parser.add_argument("--runs", type=positive_int, required=True, help="measured repetitions")
    parser.add_argument("--output", type=Path, required=True, help="machine-readable JSON output")
    parser.add_argument("--report", type=Path, help="optional readable Markdown report")
    parser.add_argument("--runtime-reference", default="HEAD", help="Git revision used for production-path equality provenance")
    parser.add_argument(
        "--measured-source-root",
        type=Path,
        help="Git worktree containing the production source measured by a process suite (default: benchmark script repository)",
    )
    parser.add_argument("--run-order", choices=("standalone", "before_then_after", "after_then_before"), default="standalone")
    parser.add_argument("--compare-to", type=Path, help="validated v3 legacy cohort to compare with this process-baseline result")
    parser.add_argument("--comparison-output", type=Path, help="optional v1 comparison JSON output (requires --compare-to)")
    return parser


def resolve_file(path: Path, description: str, executable: bool = False) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise ValueError(f"{description} is not a file: {resolved}")
    if executable and not os.access(resolved, os.X_OK):
        raise ValueError(f"{description} is not executable: {resolved}")
    return resolved


def resolve_measured_source_root(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_dir():
        raise ValueError(f"--measured-source-root is not a directory: {resolved}")
    required_paths = {
        "src": "directory",
        "CMakeLists.txt": "file",
        "cmake": "directory",
        "config.h.in": "file",
    }
    missing = [
        name
        for name, kind in required_paths.items()
        if not ((resolved / name).is_dir() if kind == "directory" else (resolved / name).is_file())
    ]
    if missing:
        raise ValueError(f"--measured-source-root lacks required production paths: {', '.join(missing)}")
    try:
        completed = subprocess.run(
            ["git", "-C", str(resolved), "rev-parse", "--show-toplevel", "--is-inside-work-tree"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"--measured-source-root is not a Git worktree: {resolved}") from error
    lines = completed.stdout.splitlines()
    if completed.returncode != 0 or len(lines) != 2 or Path(lines[0]).resolve() != resolved or lines[1] != "true":
        raise ValueError(f"--measured-source-root is not a Git worktree root: {resolved}")
    return resolved


def validate_arguments(args: argparse.Namespace) -> None:
    process_suite = args.suite.startswith("process-")
    if args.measured_source_root is not None and not process_suite:
        raise ValueError("--measured-source-root is available only with process suites")
    if process_suite:
        source_root = args.measured_source_root or Path(__file__).resolve().parents[1]
        args.measured_source_root = resolve_measured_source_root(source_root)

    args.ava = resolve_file(args.ava, "--ava", executable=True)
    if args.suite == "smoke" and args.benchmark_helper is None:
        raise ValueError("--suite smoke requires an executable --benchmark-helper")
    if args.suite.startswith("process-") and args.benchmark_helper is None:
        raise ValueError(f"--suite {args.suite} requires an executable --benchmark-helper")
    if args.benchmark_helper is not None:
        args.benchmark_helper = resolve_file(args.benchmark_helper, "--benchmark-helper", executable=True)
    if args.fake_provider is not None:
        args.fake_provider = resolve_file(args.fake_provider, "--fake-provider", executable=True)
    for attribute, option in (
        ("fake_process_child", "--fake-process-child"),
        ("fake_mcp_server", "--fake-mcp-server"),
        ("fake_lsp_server", "--fake-lsp-server"),
    ):
        value = getattr(args, attribute)
        if value is not None:
            setattr(args, attribute, resolve_file(value, option, executable=True))
    args.memory_helper = resolve_file(args.memory_helper, "--memory-helper")
    args.sample_plugin = args.sample_plugin.expanduser().resolve()
    if not (args.sample_plugin / "plugin.json").is_file() or not (args.sample_plugin / "plugin.sh").is_file():
        raise ValueError(f"--sample-plugin must contain plugin.json and plugin.sh: {args.sample_plugin}")
    args.output = args.output.expanduser().resolve()
    if args.report is not None:
        args.report = args.report.expanduser().resolve()
    if args.report == args.output:
        raise ValueError("--report and --output must be different paths")
    if args.compare_to is not None:
        args.compare_to = resolve_file(args.compare_to, "--compare-to")
    if (args.compare_to is None) != (args.comparison_output is None):
        raise ValueError("--compare-to and --comparison-output must be supplied together")
    if args.compare_to is not None and args.suite != "process-baseline":
        raise ValueError("comparison output is available only with --suite process-baseline")
    if args.comparison_output is not None:
        args.comparison_output = args.comparison_output.expanduser().resolve()
        if args.comparison_output in (args.output, args.report):
            raise ValueError("--comparison-output must be different from result and report paths")


def isolated_environment(home: Path) -> dict[str, str]:
    """Return the complete allowlisted environment for benchmark children."""
    return {
        "PATH": TRUSTED_PATH,
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "HOME": str(home),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_DATA_HOME": str(home / ".local" / "share"),
        "XDG_STATE_HOME": str(home / ".local" / "state"),
        "TMPDIR": str(home / "tmp"),
        "TERM": "dumb",
        "NO_COLOR": "1",
        "GIT_TERMINAL_PROMPT": "0",
        "AVA_BENCHMARK_OFFLINE": "1",
        "AVA_NO_DEBUG_OUTPUT": "1",
        "LIBCWD_NO_STARTUP_MSGS": "1",
        "AVA_SESSION_TITLES": "off",
    }


def percentile_95(values: Sequence[float]) -> float:
    if not values:
        raise ValueError("cannot summarize no samples")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def summarize(values: Sequence[float]) -> dict[str, float]:
    if not values:
        raise ValueError("cannot summarize no samples")
    return {
        "median": float(statistics.median(values)),
        "p95": float(percentile_95(values)),
        "maximum": float(max(values)),
    }


def unsupported(result_id: str, family: str, reason: str, reason_code: str, closest: str | None = None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "id": result_id,
        "family": family,
        "status": "unsupported",
        "reason_code": reason_code,
        "reason": reason,
        "repetitions": 0,
        "samples": [],
        "statistics": None,
    }
    if closest:
        result["closest_current_result"] = closest
    return result


def measured_result(
    result_id: str,
    family: str,
    status: str,
    unit: str,
    samples: Sequence[dict[str, Any]],
    command: Sequence[str],
    note: str = "",
) -> dict[str, Any]:
    values = [float(sample["value"]) for sample in samples]
    result = {
        "id": result_id,
        "family": family,
        "status": status,
        "unit": unit,
        "exact_command": list(command),
        "repetitions": len(samples),
        "samples": list(samples),
        "statistics": summarize(values),
    }
    if note:
        result["note"] = note
    return result


def _owned_children() -> list[int]:
    """Return kernel-reported direct children of this single-threaded owner."""
    return [int(pid) for pid in Path(f"/proc/self/task/{os.getpid()}/children").read_text().split()]


def _reap_owned_descendants() -> None:
    """Kill/reap only direct children; subreaper adoption brings escaped sessions here."""
    deadline = time.monotonic() + 5
    while True:
        children = _owned_children()
        for pid in children:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        while True:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return
            if pid == 0:
                break
        if time.monotonic() >= deadline:
            raise RuntimeError(f"benchmark owner could not reap children: {_owned_children()}")
        time.sleep(0.005)


def _owned_process_main(result_fd: int, command: Sequence[str]) -> int:
    """Private Linux child owner; no unrelated process can become our child."""
    import ctypes

    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), "cannot establish benchmark child ownership")
    canceled = False
    launching = True

    def cancel(_number, _frame):
        nonlocal canceled
        canceled = True
        if not launching:
            raise SystemExit(143)

    for number in (signal.SIGTERM, signal.SIGINT):
        signal.signal(number, cancel)
    started = time.monotonic_ns()
    process = None
    try:
        if canceled:
            return 143
        process = subprocess.Popen(list(command), stdin=subprocess.DEVNULL)
        launching = False
        if canceled:
            return 143
        process.wait()
        elapsed = time.monotonic_ns() - started
        os.write(result_fd, json.dumps([elapsed, process.returncode]).encode())
        return 0
    finally:
        for number in (signal.SIGTERM, signal.SIGINT):
            signal.signal(number, signal.SIG_IGN)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
        # Never wait for inherited output pipes before killing adopted children.
        _reap_owned_descendants()
        if process is not None:
            process.wait(timeout=1)
        os.close(result_fd)


def run_process(command: Sequence[str], cwd: Path, environment: dict[str, str], timeout: float) -> tuple[float, subprocess.CompletedProcess[str]]:
    """Run with bounded output collection and descendant cleanup on every exit.

    Linux uses a dedicated subreaper (including descendants calling setsid).
    The reported interval starts inside that owner, excluding its Python startup.
    Other POSIX hosts use a private process group. Cancellation unwinds ownership
    before propagating; cleanup grace is deliberately not a measurement sample.
    """
    linux = sys.platform.startswith("linux")
    read_fd, write_fd = os.pipe() if linux else (-1, -1)
    invocation = ([sys.executable, str(Path(__file__).resolve()), "--owned-process", str(write_fd), *command]
                  if linux else list(command))
    previous = {}
    process = None
    started = time.monotonic_ns()
    launching = True
    canceled_signal = None

    def cancel(number, _frame):
        nonlocal canceled_signal
        if launching:
            canceled_signal = number
        else:
            raise SystemExit(128 + number)

    if threading.current_thread() is threading.main_thread():
        for number in (signal.SIGTERM, signal.SIGINT):
            previous[number] = signal.getsignal(number)
            signal.signal(number, cancel)
    try:
        # Defer cancellation across launch-to-registration; no child can escape
        # between Popen returning and assignment of its ownership handle.
        process = subprocess.Popen(
            invocation, cwd=cwd, env=environment, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            start_new_session=True, pass_fds=(write_fd,) if linux else (),
        )
        launching = False
        if canceled_signal is not None:
            raise SystemExit(128 + canceled_signal)
        if linux:
            os.close(write_fd)
            write_fd = -1
        stdout, stderr = process.communicate(timeout=timeout)
        if linux:
            payload = os.read(read_fd, 4096)
            if process.returncode != 0 or not payload:
                raise RuntimeError(f"benchmark process owner failed ({process.returncode}): {stderr[-2000:]}")
            elapsed, returncode = json.loads(payload)
        else:
            elapsed, returncode = time.monotonic_ns() - started, process.returncode
        return float(elapsed), subprocess.CompletedProcess(list(command), returncode, stdout, stderr)
    finally:
        for number in previous:
            signal.signal(number, signal.SIG_IGN)
        try:
            if process is not None:
                if process.poll() is None:
                    process.terminate()
                if not linux:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                try:
                    process.communicate(timeout=7)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=1)
                    raise RuntimeError("benchmark child owner exceeded its cleanup deadline")
                finally:
                    for stream in (process.stdout, process.stderr):
                        if stream is not None:
                            stream.close()
        finally:
            for fd in (read_fd, write_fd):
                if fd >= 0:
                    os.close(fd)
            for number, handler in previous.items():
                signal.signal(number, handler)


def run_startup(args: argparse.Namespace, root: Path, project: Path) -> dict[str, Any]:
    command = [str(args.ava), "--rpc", "--offline", "--no-session"]
    samples: list[dict[str, Any]] = []
    # One unrecorded run makes the measured population explicitly warm.
    warm_home = root / "startup-warmup"
    prepare_home(warm_home)
    _, warmup = run_process(command, project, isolated_environment(warm_home), 20.0)
    if warmup.returncode != 0:
        raise RuntimeError(f"AVA startup warmup failed ({warmup.returncode}): {warmup.stderr[-1000:]}")
    for index in range(args.runs):
        home = root / f"startup-{index}"
        prepare_home(home)
        elapsed, completed = run_process(command, project, isolated_environment(home), 20.0)
        if completed.returncode != 0:
            raise RuntimeError(f"AVA startup failed ({completed.returncode}): {completed.stderr[-1000:]}")
        samples.append({"run": index + 1, "value": elapsed, "return_code": completed.returncode})
    return measured_result(
        "warm_startup",
        "startup",
        "measured",
        "ns",
        samples,
        command,
        "Full offline RPC initialization and clean EOF shutdown after one warm-up; it performs no provider request.",
    )


def prepare_home(home: Path) -> None:
    for relative in ("tmp", ".config", ".cache", ".local/share", ".local/state"):
        (home / relative).mkdir(parents=True, exist_ok=True)


def run_helper(
    args: argparse.Namespace,
    root: Path,
    project: Path,
    result_id: str,
    family: str,
    benchmark_case: str,
    extra: Sequence[str],
    status: str = "measured",
    note: str = "",
) -> dict[str, Any]:
    if args.benchmark_helper is None:
        return unsupported(result_id, family, "The test-only C++ benchmark helper was not supplied.", "missing_benchmark_helper")
    command = [str(args.benchmark_helper), "--case", benchmark_case, *extra]
    samples: list[dict[str, Any]] = []
    unit: str | None = None
    for index in range(args.runs):
        home = root / f"helper-{result_id}-{index}"
        prepare_home(home)
        wall_ns, completed = run_process(command, project, isolated_environment(home), 60.0 if args.suite != "stress" else 300.0)
        if completed.returncode != 0:
            raise RuntimeError(f"benchmark helper {result_id} failed ({completed.returncode}): {completed.stderr[-2000:]}")
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"benchmark helper {result_id} emitted invalid JSON: {completed.stdout[-1000:]}") from error
        if not isinstance(payload, dict):
            raise RuntimeError(f"benchmark helper {result_id} emitted a non-object JSON payload")
        helper_status = payload.get("status", "measured")
        if helper_status == "unsupported":
            reason = payload.get("reason")
            reason_code = payload.get("reason_code")
            if not isinstance(reason, str) or not reason or not isinstance(reason_code, str) or not reason_code:
                raise RuntimeError(f"benchmark helper {result_id} emitted an invalid unsupported result")
            result = unsupported(result_id, family, reason, reason_code)
            result["exact_command"] = command
            result["details"] = payload.get("details", {})
            return result
        if helper_status != "measured":
            raise RuntimeError(f"benchmark helper {result_id} emitted unknown status: {helper_status}")
        try:
            value = float(payload["value"])
            current_unit = payload["unit"]
        except (KeyError, TypeError, ValueError) as error:
            raise RuntimeError(f"benchmark helper {result_id} emitted invalid measurement JSON: {completed.stdout[-1000:]}") from error
        if not isinstance(current_unit, str) or not current_unit:
            raise RuntimeError(f"benchmark helper {result_id} emitted an invalid unit")
        if unit is not None and unit != current_unit:
            raise RuntimeError(f"benchmark helper {result_id} changed units")
        unit = current_unit
        samples.append(
            {
                "run": index + 1,
                "value": value,
                "wall_time_ns": wall_ns,
                "details": payload.get("details", {}),
            }
        )
    if unit is None or not samples:
        raise RuntimeError(f"benchmark helper {result_id} produced no measured samples")
    return measured_result(result_id, family, status, unit, samples, command, note)


def idle_memory_samples(runs: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    for index, run in enumerate(runs):
        raw_snapshots = run.get("samples")
        if not isinstance(raw_snapshots, list) or not raw_snapshots:
            raise RuntimeError(f"idle memory run {index + 1} has no raw snapshots")
        try:
            maximum_rss_kib = max(float(snapshot["rss_kib"]) for snapshot in raw_snapshots)
        except (KeyError, TypeError, ValueError) as error:
            raise RuntimeError(f"idle memory run {index + 1} has an invalid RSS snapshot") from error
        samples.append(
            {
                "run": index + 1,
                "value": maximum_rss_kib,
                "details": {
                    "rss_aggregation": "maximum_observed_snapshot",
                    "raw_memory_helper_run": run,
                },
            }
        )
    return samples


def run_idle_memory(args: argparse.Namespace, root: Path, project: Path) -> dict[str, Any]:
    if sys.platform != "linux" or not Path("/proc/self/smaps_rollup").is_file():
        return unsupported("idle_rss", "memory", "Idle process-tree RSS requires Linux procfs smaps_rollup.", "procfs_unavailable")
    output = root / "idle-memory.json"
    settle = {"smoke": "0.1", "baseline": "1.0", "stress": "3.0"}[args.suite]
    sample_count = "1" if args.suite == "smoke" else "3"
    command = [
        sys.executable,
        str(args.memory_helper),
        "--apps",
        "ava",
        "--ava",
        str(args.ava),
        "--runs",
        str(args.runs),
        "--settle",
        settle,
        "--samples",
        sample_count,
        "--output",
        str(output),
    ]
    home = root / "memory-driver"
    prepare_home(home)
    _, completed = run_process(command, project, isolated_environment(home), 120.0 if args.suite == "smoke" else 600.0)
    if completed.returncode != 0:
        raise RuntimeError(f"idle memory helper failed ({completed.returncode}): {completed.stderr[-2000:]}")
    payload = json.loads(output.read_text(encoding="utf-8"))
    try:
        runs = payload["apps"]["ava"]["runs"]
    except (KeyError, TypeError) as error:
        raise RuntimeError("idle memory helper output lacks AVA runs") from error
    if not isinstance(runs, list):
        raise RuntimeError("idle memory helper AVA runs must be an array")
    result = measured_result(
        "idle_rss",
        "memory",
        "measured",
        "KiB",
        idle_memory_samples(runs),
        command,
        "Maximum observed Linux process-tree RSS snapshot per run from the procfs/PTY seam; includes AVA descendants.",
    )
    result["details"] = {
        "per_run_value": "maximum rss_kib across that run's raw /proc snapshots",
        "raw_memory_helper_output": payload,
    }
    return result


def not_selected(result_id: str, family: str, suite: str, closest: str | None = None) -> dict[str, Any]:
    return unsupported(
        result_id,
        family,
        f"The {suite} suite intentionally excludes this scale point; use --suite baseline or stress.",
        "not_selected_by_suite",
        closest,
    )


def git_identity(repository: Path) -> dict[str, Any]:
    def git(*arguments: str) -> str:
        try:
            completed = subprocess.run(
                ["git", "-C", str(repository), *arguments],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=10,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            return "unknown"
        if completed.returncode != 0:
            return "unknown"
        return completed.stdout.strip()

    commit = git("rev-parse", "HEAD")
    tree = git("rev-parse", "HEAD^{tree}")
    status = git("status", "--porcelain", "--untracked-files=normal")
    dirty = None if status == "unknown" else bool(status)
    if dirty is None:
        commit_with_state = f"{commit}-state-unknown"
    else:
        commit_with_state = f"{commit}{'-dirty' if dirty else ''}"
    return {
        "repository": str(repository.resolve()),
        "commit": commit,
        "tree": tree,
        "dirty": dirty,
        "commit_with_state": commit_with_state,
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as artifact:
        for chunk in iter(lambda: artifact.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_identity(path: Path) -> dict[str, Any]:
    resolved = path.resolve()
    before = resolved.stat()
    sha256 = sha256_file(resolved)
    after = resolved.stat()
    if before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns:
        raise RuntimeError(f"artifact changed while hashing: {resolved}")
    return {
        "path": str(resolved),
        "size_bytes": before.st_size,
        "sha256": sha256,
        "mtime_ns": before.st_mtime_ns,
        "executable": os.access(resolved, os.X_OK),
    }


def executable_version_identity(binary: Path, root: Path, project: Path) -> dict[str, Any]:
    command = [str(binary), "--version"]
    home = root / "version-probe"
    prepare_home(home)
    try:
        _, completed = run_process(command, project, isolated_environment(home), 5.0)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {
            "status": "unavailable",
            "exact_command": command,
            "error": type(error).__name__,
        }
    return {
        "status": "recorded",
        "exact_command": command,
        "return_code": completed.returncode,
        "stdout": completed.stdout[:4096].strip(),
        "stderr": completed.stderr[:4096].strip(),
    }


def artifact_inventory(args: argparse.Namespace, repository: Path, root: Path, project: Path) -> dict[str, Any]:
    ava = file_identity(args.ava)
    ava["version_probe"] = executable_version_identity(args.ava, root, project)

    manifest_path = args.sample_plugin / "plugin.json"
    entrypoint_path = args.sample_plugin / "plugin.sh"
    manifest_payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest_payload, dict):
        raise RuntimeError("sample plugin manifest must be a JSON object")
    entrypoint = manifest_payload.get("entrypoint")
    if not isinstance(entrypoint, dict):
        raise RuntimeError("sample plugin manifest lacks entrypoint identity")

    return {
        "ava": ava,
        "benchmark_helper": file_identity(args.benchmark_helper) if args.benchmark_helper is not None else None,
        "fake_provider": file_identity(args.fake_provider) if args.fake_provider is not None else None,
        "memory_helper": file_identity(args.memory_helper),
        "sample_plugin": {
            "root": str(args.sample_plugin),
            "manifest": file_identity(manifest_path),
            "entrypoint": file_identity(entrypoint_path),
            "manifest_identity": {
                "schema_version": manifest_payload.get("schema_version"),
                "id": manifest_payload.get("id"),
                "name": manifest_payload.get("name"),
                "version": manifest_payload.get("version"),
                "api_version": manifest_payload.get("api_version"),
                "entrypoint_command": entrypoint.get("command"),
                "entrypoint_args": entrypoint.get("args"),
            },
        },
        "benchmark_script": file_identity(repository / "scripts" / "benchmark-backend.py"),
    }


def total_ram_bytes() -> int | None:
    try:
        if sys.platform == "linux":
            for line in Path("/proc/meminfo").read_text(encoding="utf-8").splitlines():
                if line.startswith("MemTotal:"):
                    return int(line.split()[1]) * 1024
        page_size = os.sysconf("SC_PAGE_SIZE")
        page_count = os.sysconf("SC_PHYS_PAGES")
        return int(page_size) * int(page_count)
    except (OSError, ValueError, KeyError, AttributeError):
        return None


def cpu_model() -> str:
    try:
        if sys.platform == "linux":
            for line in Path("/proc/cpuinfo").read_text(encoding="utf-8", errors="replace").splitlines():
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def find_cmake_cache(binary: Path) -> Path | None:
    for parent in binary.resolve().parents:
        candidate = parent / "CMakeCache.txt"
        if candidate.is_file():
            return candidate
    return None


def read_cmake_cache(cache: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        values[key_and_type.split(":", 1)[0]] = value
    return values


def cmake_boolean(values: dict[str, str], names: Sequence[str]) -> bool | None:
    for name in names:
        value = values.get(name, "").upper()
        if value in ("1", "ON", "TRUE", "YES", "Y"):
            return True
        if value in ("0", "OFF", "FALSE", "NO", "N"):
            return False
    return None


def generated_compiler_identity(build_directory: Path) -> tuple[str, str]:
    candidates = sorted((build_directory / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    if not candidates:
        return "unknown", "unknown"
    text = candidates[-1].read_text(encoding="utf-8", errors="replace")
    compiler_id = re.search(r'^set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text, re.MULTILINE)
    compiler_version = re.search(r'^set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)', text, re.MULTILINE)
    return (
        compiler_id.group(1) if compiler_id else "unknown",
        compiler_version.group(1) if compiler_version else "unknown",
    )


def build_metadata(binary: Path, repository: Path) -> dict[str, Any]:
    cache = find_cmake_cache(binary)
    values = read_cmake_cache(cache) if cache is not None else {}
    source_root = values.get("CMAKE_HOME_DIRECTORY")
    compiler_path = values.get("CMAKE_CXX_COMPILER", "unknown")
    compiler_id, configured_compiler_version = (
        generated_compiler_identity(cache.parent) if cache is not None else ("unknown", "unknown")
    )
    compiler_version_output = "unknown"
    if compiler_path != "unknown":
        try:
            completed = subprocess.run(
                [compiler_path, "--version"],
                env={"PATH": TRUSTED_PATH, "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"},
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=5,
                check=False,
            )
            compiler_version_output = completed.stdout.splitlines()[0] if completed.stdout else "unknown"
        except (OSError, subprocess.TimeoutExpired):
            compiler_version_output = "unavailable"

    flag_names = (
        "AVA_ENABLE_SANITIZERS",
        "EnableAvaSanitizers",
        "OptionEnableAvaSanitizers",
        "AVA_ENABLE_TSAN",
        "EnableDebug",
        "OptionEnableDebug",
        "EnableLibcwd",
        "OptionEnableLibcwd",
    )
    cmake_flags = {name: values[name] for name in flag_names if name in values}
    cxx_flags = {
        name: value
        for name, value in values.items()
        if (name == "CMAKE_CXX_FLAGS" or name.startswith("CMAKE_CXX_FLAGS_")) and not name.endswith("-ADVANCED")
    }

    resolved_binary = binary.resolve()
    resolved_repository = repository.resolve()
    resolved_source = Path(source_root).resolve() if source_root else None
    binary_in_build_tree: bool | None = None
    binary_not_older_than_cache: bool | None = None
    cache_mtime_ns: int | None = None
    if cache is not None:
        try:
            resolved_binary.relative_to(cache.parent.resolve())
            binary_in_build_tree = True
        except ValueError:
            binary_in_build_tree = False
        cache_mtime_ns = cache.stat().st_mtime_ns
        binary_not_older_than_cache = resolved_binary.stat().st_mtime_ns >= cache_mtime_ns

    provenance = {
        "assessment": "best_effort_unverified",
        "statement": (
            "The nearest CMake cache, source-root match, and mtimes are recorded as best-effort provenance only. "
            "They do not verify build freshness or claim that the executable embeds or was produced from the recorded Git commit."
        ),
        "cmake_source_root_matches_recorded_source": resolved_source == resolved_repository if resolved_source is not None else None,
        "binary_is_within_cmake_build_tree": binary_in_build_tree,
        "binary_mtime_ns": resolved_binary.stat().st_mtime_ns,
        "cmake_cache_mtime_ns": cache_mtime_ns,
        "binary_not_older_than_cmake_cache": binary_not_older_than_cache,
        "git_commit_embedding_verified": False,
    }

    return {
        "cmake_cache": str(cache) if cache is not None else None,
        "cmake_source_root": source_root,
        "build_type": values.get("CMAKE_BUILD_TYPE", "unknown"),
        "compiler": {
            "path": compiler_path,
            "id": compiler_id,
            "configured_version": configured_compiler_version,
            "version_output": compiler_version_output,
        },
        "features": {
            "sanitizers": cmake_boolean(values, ("OptionEnableAvaSanitizers", "AVA_ENABLE_SANITIZERS", "EnableAvaSanitizers")),
            "tsan": cmake_boolean(values, ("AVA_ENABLE_TSAN",)),
            "debug": cmake_boolean(values, ("OptionEnableDebug", "EnableDebug")),
            "libcwd": cmake_boolean(values, ("OptionEnableLibcwd", "EnableLibcwd")),
        },
        "cmake_flags": cmake_flags,
        "cxx_flags": cxx_flags,
        "provenance": provenance,
    }


def smoke_checks(results: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    by_id = {result["id"]: result for result in results}
    checks: list[dict[str, Any]] = []

    def maximum(result_id: str) -> float | None:
        statistics_value = by_id[result_id].get("statistics")
        return float(statistics_value["maximum"]) if statistics_value else None

    def measured(result_id: str) -> bool:
        result = by_id[result_id]
        return result.get("status") == "measured" and bool(result.get("samples"))

    def add(name: str, passed: bool, ceiling: str) -> None:
        checks.append({"name": name, "passed": passed, "ceiling": ceiling})

    startup = maximum("warm_startup")
    add("startup_not_catastrophic", startup is not None and startup < 30_000_000_000, "< 30 seconds")
    rss = maximum("idle_rss")
    add("idle_rss_not_catastrophic", rss is None or rss < 4 * 1024 * 1024, "< 4 GiB when supported")

    add("native_dispatch_exercised", measured("native_file_read_dispatch"), "helper-backed dispatch measurement required")
    add(
        "cancellation_acknowledgement_exercised",
        measured("cancellation_acknowledgement"),
        "helper-backed cancellation measurement required",
    )
    add("session_open_exercised", measured("session_open_1000"), "helper-backed session-open measurement required")
    add("repeated_memory_exercised", measured("repeated_call_memory"), "helper-backed repeated-memory measurement required")

    growth = maximum("repeated_call_memory")
    add(
        "repeated_memory_not_catastrophic",
        measured("repeated_call_memory") and growth is not None and growth < 512 * 1024,
        "< 512 MiB current-RSS delta (or labeled platform fallback)",
    )
    helper_wall_times = [
        float(sample["wall_time_ns"])
        for result in results
        for sample in result["samples"]
        if "wall_time_ns" in sample
    ]
    add(
        "helper_timing_not_catastrophic",
        bool(helper_wall_times) and max(helper_wall_times) < 30_000_000_000,
        "each helper repetition < 30 seconds",
    )
    cleanup = by_id["child_cleanup"]
    cleanup_ok = measured("child_cleanup") and all(
        sample.get("details", {}).get("children_after") == 0 for sample in cleanup["samples"]
    )
    add("plugin_children_reaped", cleanup_ok, "helper-backed cleanup leaves zero waitable children")
    manifests = by_id["unused_plugin_manifests_100"]
    manifest_ok = measured("unused_plugin_manifests_100") and all(
        sample.get("details", {}).get("children_before") == 0 and sample.get("details", {}).get("children_after") == 0
        for sample in manifests["samples"]
    )
    add("manifest_discovery_starts_no_children", manifest_ok, "helper-backed discovery starts zero children")
    return checks


def validate_file_identity(identity: Any, label: str) -> None:
    if not isinstance(identity, dict):
        raise ValueError(f"benchmark artifact {label} must be an object")
    for key in ("path", "size_bytes", "sha256", "mtime_ns", "executable"):
        if key not in identity:
            raise ValueError(f"benchmark artifact {label} lacks {key}")
    if not isinstance(identity["path"], str) or not identity["path"]:
        raise ValueError(f"benchmark artifact {label} has an invalid path")
    if not isinstance(identity["size_bytes"], int) or identity["size_bytes"] < 0:
        raise ValueError(f"benchmark artifact {label} has an invalid size")
    if not isinstance(identity["mtime_ns"], int) or not isinstance(identity["executable"], bool):
        raise ValueError(f"benchmark artifact {label} has invalid file metadata")
    sha256 = identity["sha256"]
    if not isinstance(sha256, str) or len(sha256) != 64 or any(character not in "0123456789abcdef" for character in sha256):
        raise ValueError(f"benchmark artifact {label} has an invalid SHA-256")


def validate_document(document: dict[str, Any]) -> None:
    if document.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unexpected benchmark schema version")
    required_top_level = ("generated_at_utc", "git", "host", "build", "artifacts", "parameters", "results", "checks")
    for key in required_top_level:
        if key not in document:
            raise ValueError(f"benchmark document lacks {key}")
    for key in ("repository", "commit", "tree", "dirty", "commit_with_state"):
        if key not in document["git"]:
            raise ValueError(f"benchmark git identity lacks {key}")
    for key in ("os", "kernel", "cpu", "ram_bytes"):
        if key not in document["host"]:
            raise ValueError(f"benchmark host identity lacks {key}")

    build = document["build"]
    for key in ("cmake_cache", "cmake_source_root", "build_type", "compiler", "features", "cmake_flags", "cxx_flags", "provenance"):
        if key not in build:
            raise ValueError(f"benchmark build identity lacks {key}")
    for key in ("path", "id", "configured_version", "version_output"):
        if key not in build["compiler"]:
            raise ValueError(f"benchmark compiler identity lacks {key}")
    for key in ("sanitizers", "tsan", "debug", "libcwd"):
        if key not in build["features"]:
            raise ValueError(f"benchmark build features lack {key}")
    for key in (
        "assessment",
        "statement",
        "cmake_source_root_matches_recorded_source",
        "binary_is_within_cmake_build_tree",
        "binary_mtime_ns",
        "cmake_cache_mtime_ns",
        "binary_not_older_than_cmake_cache",
        "git_commit_embedding_verified",
    ):
        if key not in build["provenance"]:
            raise ValueError(f"benchmark provenance lacks {key}")
    if build["provenance"]["git_commit_embedding_verified"] is not False:
        raise ValueError("benchmark provenance must not claim verified Git commit embedding")

    artifacts = document["artifacts"]
    if not isinstance(artifacts, dict):
        raise ValueError("benchmark artifacts must be an object")
    for key in ("ava", "benchmark_helper", "fake_provider", "memory_helper", "sample_plugin", "benchmark_script"):
        if key not in artifacts:
            raise ValueError(f"benchmark artifacts lack {key}")
    for key in ("ava", "memory_helper", "benchmark_script"):
        validate_file_identity(artifacts[key], key)
    if not isinstance(artifacts["ava"].get("version_probe"), dict):
        raise ValueError("benchmark AVA identity lacks version_probe")
    for key in ("benchmark_helper", "fake_provider"):
        if artifacts[key] is not None:
            validate_file_identity(artifacts[key], key)
    if document["parameters"].get("suite") == "smoke" and artifacts["benchmark_helper"] is None:
        raise ValueError("smoke benchmark artifact identity lacks benchmark_helper")
    sample_plugin = artifacts["sample_plugin"]
    if not isinstance(sample_plugin, dict):
        raise ValueError("benchmark sample plugin identity must be an object")
    for key in ("root", "manifest", "entrypoint", "manifest_identity"):
        if key not in sample_plugin:
            raise ValueError(f"benchmark sample plugin identity lacks {key}")
    validate_file_identity(sample_plugin["manifest"], "sample_plugin.manifest")
    validate_file_identity(sample_plugin["entrypoint"], "sample_plugin.entrypoint")
    manifest_identity = sample_plugin["manifest_identity"]
    if not isinstance(manifest_identity, dict):
        raise ValueError("benchmark sample plugin manifest identity must be an object")
    for key in ("schema_version", "id", "name", "version", "api_version", "entrypoint_command", "entrypoint_args"):
        if key not in manifest_identity:
            raise ValueError(f"benchmark sample plugin manifest identity lacks {key}")
    for key in ("id", "name", "version", "api_version", "entrypoint_command"):
        if not isinstance(manifest_identity[key], str) or not manifest_identity[key]:
            raise ValueError(f"benchmark sample plugin manifest identity has invalid {key}")
    if not isinstance(manifest_identity["entrypoint_args"], list):
        raise ValueError("benchmark sample plugin manifest identity has invalid entrypoint_args")

    for key in ("suite", "runs", "exact_command", "clock", "environment"):
        if key not in document["parameters"]:
            raise ValueError(f"benchmark parameters lack {key}")
    results = document.get("results")
    if not isinstance(results, list):
        raise ValueError("benchmark results must be an array")
    ids = [result.get("id") for result in results]
    if ids != list(EXPECTED_RESULT_IDS):
        raise ValueError("benchmark result families are missing, duplicated, or out of order")
    for result in results:
        if not isinstance(result.get("samples"), list) or "repetitions" not in result or "statistics" not in result:
            raise ValueError(f"invalid result record: {result.get('id')}")
        if result["repetitions"] != len(result["samples"]):
            raise ValueError(f"sample count mismatch: {result['id']}")
        if result["status"] in ("measured", "current_one_shot"):
            if not result["samples"] or not isinstance(result["statistics"], dict):
                raise ValueError(f"measured result has no samples: {result['id']}")
            if not isinstance(result.get("unit"), str) or not result["unit"]:
                raise ValueError(f"measured result lacks unit: {result['id']}")
            for key in ("median", "p95", "maximum"):
                if key not in result["statistics"]:
                    raise ValueError(f"measured result lacks {key}: {result['id']}")
        elif result["status"] == "unsupported":
            if not isinstance(result.get("reason"), str) or not isinstance(result.get("reason_code"), str):
                raise ValueError(f"unsupported result lacks reason identity: {result['id']}")
        else:
            raise ValueError(f"unknown result status: {result['status']}")

    idle_rss = results[EXPECTED_RESULT_IDS.index("idle_rss")]
    if idle_rss["status"] == "measured":
        if not isinstance(idle_rss.get("details", {}).get("raw_memory_helper_output"), dict):
            raise ValueError("idle RSS result lacks raw memory-helper output")
        for sample in idle_rss["samples"]:
            details = sample.get("details", {})
            raw_run = details.get("raw_memory_helper_run")
            if details.get("rss_aggregation") != "maximum_observed_snapshot" or not isinstance(raw_run, dict):
                raise ValueError("idle RSS sample lacks raw snapshot details")
            snapshots = raw_run.get("samples")
            if not isinstance(snapshots, list) or not snapshots:
                raise ValueError("idle RSS sample lacks raw snapshots")
            expected = max(float(snapshot["rss_kib"]) for snapshot in snapshots)
            if float(sample["value"]) != expected:
                raise ValueError("idle RSS sample is not the maximum observed snapshot")

    native_registry = results[EXPECTED_RESULT_IDS.index("native_registry_lookup")]
    if native_registry["status"] == "measured":
        if native_registry["unit"] != "ns_per_lookup":
            raise ValueError("native registry lookup has an invalid unit")
        for sample in native_registry["samples"]:
            details = sample.get("details", {})
            if not isinstance(details.get("target"), str) or not isinstance(details.get("entry_count"), int):
                raise ValueError("native registry lookup lacks target identity")

    if not isinstance(document["checks"], list):
        raise ValueError("benchmark checks must be an array")


def markdown_report(document: dict[str, Any]) -> str:
    lines = [
        "# AVA Backend Benchmark Report",
        "",
        f"Generated: `{document['generated_at_utc']}`  ",
        f"Suite: `{document['parameters']['suite']}` with `{document['parameters']['runs']}` measured repetition(s)  ",
        f"Git: `{document['git']['commit_with_state']}`  ",
        f"AVA: `{document['artifacts']['ava']['path']}` (`{document['artifacts']['ava']['size_bytes']}` bytes; "
        f"SHA-256 `{document['artifacts']['ava']['sha256']}`)",
        "",
        "> Results are machine/build-specific observations, not portable performance claims.",
        "",
        "| Measurement | Status | Median | p95 | Maximum |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for result in document["results"]:
        stats = result["statistics"]
        if stats:
            unit = result["unit"]
            values = (f"{stats['median']:.3f} {unit}", f"{stats['p95']:.3f} {unit}", f"{stats['maximum']:.3f} {unit}")
        else:
            values = ("—", "—", "—")
        lines.append(f"| `{result['id']}` | {result['status']} | {values[0]} | {values[1]} | {values[2]} |")
    lines.extend(["", "## Unsupported and compatibility notes", ""])
    for result in document["results"]:
        if result["status"] == "unsupported":
            lines.append(f"- `{result['id']}`: {result['reason']}")
        elif result["status"] == "current_one_shot":
            lines.append(f"- `{result['id']}`: {result.get('note', 'Measured through the current one-shot path.')}")
    lines.extend(["", "## Smoke invariants", ""])
    for check in document["checks"]:
        lines.append(f"- {'PASS' if check['passed'] else 'FAIL'} `{check['name']}` ({check['ceiling']})")
    return "\n".join(lines) + "\n"


def execute(args: argparse.Namespace) -> dict[str, Any]:
    repository = Path(__file__).resolve().parents[1]
    root = Path(tempfile.mkdtemp(prefix="ava-backend-benchmark-"))
    project = root / "project"
    project.mkdir()
    (project / ".git").mkdir()
    results: list[dict[str, Any]] = []
    try:
        warm_startup = run_startup(args, root, project)
        results.append(
            unsupported(
                "cold_startup",
                "startup",
                "A reproducible cold start requires privileged filesystem/page-cache controls; the harness does not mislabel a fresh HOME as a cold machine.",
                "cold_cache_control_unavailable",
                "warm_startup",
            )
        )
        results.append(warm_startup)
        results.append(run_idle_memory(args, root, project))
        results.append(
            unsupported(
                "builtin_noop_dispatch",
                "dispatch",
                "The pre-modernization built-in registry has no no-op tool. The closest test-only no-op path is recorded by short_calls_10000.",
                "no_builtin_noop",
                "short_calls_10000",
            )
        )
        file_iterations = {"smoke": 10, "baseline": 1000, "stress": 10000}[args.suite]
        results.append(
            run_helper(args, root, project, "native_file_read_dispatch", "dispatch", "file-dispatch", ["--iterations", str(file_iterations)])
        )
        results.append(
            run_helper(
                args,
                root,
                project,
                "native_registry_lookup",
                "registry",
                "native-registry",
                ["--iterations", str(file_iterations)],
                note="Exact lookup of the final current built-in registry entry; registry construction is outside the timed interval.",
            )
        )
        plugin_note = (
            "The current application path starts and shuts down a plugin process for each call; this is not persistent warm-worker performance."
        )
        results.append(
            run_helper(
                args,
                root,
                project,
                "plugin_first_call",
                "plugin",
                "plugin-call",
                ["--iterations", "1", "--sample-plugin", str(args.sample_plugin)],
                "current_one_shot",
                plugin_note,
            )
        )
        repeated_plugins = {"smoke": 2, "baseline": 10, "stress": 100}[args.suite]
        results.append(
            run_helper(
                args,
                root,
                project,
                "plugin_repeated_calls",
                "plugin",
                "plugin-call",
                ["--iterations", str(repeated_plugins), "--sample-plugin", str(args.sample_plugin)],
                "current_one_shot",
                plugin_note,
            )
        )
        results.append(
            run_helper(
                args,
                root,
                project,
                "unused_plugin_manifests_100",
                "plugin",
                "manifest-discovery",
                ["--entries", "100", "--sample-plugin", str(args.sample_plugin)],
                note="Manifest parsing only, with a zero-waitable-child assertion before and after discovery.",
            )
        )
        for count in (100, 500, 1000):
            result_id = f"catalog_entries_{count}"
            if args.suite == "smoke" and count != 100:
                results.append(not_selected(result_id, "catalog", args.suite, "catalog_entries_100"))
            else:
                results.append(
                    run_helper(
                        args,
                        root,
                        project,
                        result_id,
                        "catalog",
                        "catalog",
                        ["--entries", str(count)],
                        note=(
                            "Composite timed boundary: synthetic registry construction, full schema materialization, and final-entry linear lookup; "
                            "not a selective router."
                        ),
                    )
                )
        if args.suite == "smoke":
            results.append(not_selected("short_calls_10000", "dispatch", args.suite, "native_file_read_dispatch"))
            results.append(not_selected("canceled_calls_10000", "cancellation", args.suite, "cancellation_acknowledgement"))
        else:
            results.append(run_helper(args, root, project, "short_calls_10000", "dispatch", "short-calls", ["--iterations", "10000"]))
            results.append(run_helper(args, root, project, "canceled_calls_10000", "cancellation", "canceled-calls", ["--iterations", "10000"]))
        for records in (1000, 10000, 100000):
            result_id = f"session_open_{records}"
            if args.suite == "smoke" and records != 1000:
                results.append(not_selected(result_id, "session", args.suite, "session_open_1000"))
            else:
                results.append(
                    run_helper(args, root, project, result_id, "session", "session-open", ["--records", str(records)])
                )
        metadata_count = {"smoke": 10, "baseline": 100, "stress": 1000}[args.suite]
        results.append(
            run_helper(args, root, project, "metadata_browsing", "session", "metadata", ["--entries", str(metadata_count)])
        )
        results.append(
            run_helper(args, root, project, "cancellation_acknowledgement", "cancellation", "cancellation-ack", ["--iterations", "10"])
        )
        results.append(
            run_helper(
                args,
                root,
                project,
                "child_cleanup",
                "process",
                "plugin-cleanup",
                ["--iterations", "2", "--sample-plugin", str(args.sample_plugin)],
            )
        )
        memory_calls = 1000 if args.suite == "smoke" else 10000
        results.append(
            run_helper(
                args,
                root,
                project,
                "repeated_call_memory",
                "memory",
                "repeated-memory",
                ["--iterations", str(memory_calls)],
                note=(
                    "Narrow direct-call memory seam: Linux current-RSS delta with peak high-water details; macOS uses an explicitly labeled "
                    "peak high-water fallback. This is not product-wide memory-stability evidence."
                ),
            )
        )
        results.append(
            unsupported(
                "selective_router",
                "catalog",
                "The pre-modernization architecture materializes the full catalog and performs linear registry lookup; no selective router exists yet.",
                "architecture_not_implemented",
                "catalog_entries_1000" if args.suite != "smoke" else "catalog_entries_100",
            )
        )

        document: dict[str, Any] = {
            "schema_version": SCHEMA_VERSION,
            "generated_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "git": git_identity(repository),
            "host": {
                "os": platform.system(),
                "platform": platform.platform(),
                "kernel": platform.release(),
                "machine": platform.machine(),
                "cpu": cpu_model(),
                "cpu_count": os.cpu_count(),
                "ram_bytes": total_ram_bytes(),
                "python": platform.python_version(),
            },
            "build": build_metadata(args.ava, repository),
            "artifacts": artifact_inventory(args, repository, root, project),
            "parameters": {
                "suite": args.suite,
                "runs": args.runs,
                "exact_command": list(sys.argv),
                "benchmark_helper": str(args.benchmark_helper) if args.benchmark_helper else None,
                "fake_provider": str(args.fake_provider) if args.fake_provider else None,
                "memory_helper": str(args.memory_helper),
                "sample_plugin": str(args.sample_plugin),
                "environment": (
                    "fixed allowlist: trusted PATH, C.UTF-8 locale, isolated HOME/XDG/TMPDIR, dumb terminal/no color, "
                    "offline/debug/session-title controls; no host variables inherited"
                ),
                "clock": "time.monotonic_ns / C++ steady_clock",
            },
            "results": results,
        }
        document["checks"] = smoke_checks(results) if args.suite == "smoke" else []
        validate_document(document)
        return document
    finally:
        shutil.rmtree(root, ignore_errors=True)


PROCESS_RESULT_SPECS: dict[str, dict[str, Any]] = {
    "application_warm_startup": {"family": "application", "primary_metric": "startup_ns", "unit": "ns", "boundary": "offline_rpc_start_through_clean_eof"},
    "application_idle_rss": {"family": "application", "primary_metric": "idle_rss_kib", "unit": "KiB", "boundary": "maximum_process_tree_rss_after_idle_settle"},
    "supervisor_idle_scope_startup": {"family": "supervisor", "case": "process-idle-scope", "primary_metric": "scope_construction_ns", "unit": "ns", "boundary": "supervisor_and_application_scope_construction"},
    "supervisor_first_spawn_commit": {"family": "supervisor", "case": "process-first-spawn", "primary_metric": "spawn_commit_ns", "unit": "ns", "boundary": "exact_environment_mint_reservation_spawn_through_exec_commit"},
    "supervisor_warm_sequential_spawn_commit": {"family": "supervisor", "case": "process-warm-sequential", "primary_metric": "spawn_commit_ns", "unit": "ns", "boundary": "individual_post_warmup_spawn_through_exec_commit"},
    "supervisor_concurrent_records_1": {"family": "supervisor", "case": "process-concurrent", "primary_metric": "spawn_commit_ns", "unit": "ns", "boundary": "barrier_release_through_each_spawn_commit", "records": 1},
    "supervisor_concurrent_records_8": {"family": "supervisor", "case": "process-concurrent", "primary_metric": "spawn_commit_ns", "unit": "ns", "boundary": "barrier_release_through_each_spawn_commit", "records": 8},
    "supervisor_concurrent_records_64": {"family": "supervisor", "case": "process-concurrent", "primary_metric": "spawn_commit_ns", "unit": "ns", "boundary": "barrier_release_through_each_spawn_commit", "records": 64},
    "supervisor_natural_exit_settlement": {"family": "supervisor", "case": "process-natural-exit", "primary_metric": "settlement_ns", "unit": "ns", "boundary": "spawn_start_through_complete_natural_exit"},
    "supervisor_leader_first_descendant_cleanup": {"family": "supervisor", "case": "process-leader-first-descendant", "primary_metric": "cleanup_settlement_ns", "unit": "ns", "boundary": "leader_exit_phase_through_complete_cleanup_and_eof"},
    "supervisor_term_refusal_escalation": {"family": "supervisor", "case": "process-term-refusal", "primary_metric": "stop_settlement_ns", "unit": "ns", "boundary": "stop_request_through_complete_escalation"},
    "supervisor_shared_budget_shutdown_64": {"family": "supervisor", "case": "process-shared-shutdown", "primary_metric": "shutdown_ns", "unit": "ns", "boundary": "shutdown_request_through_shared_budget_settlement", "records": 64},
    "monitor_idle_pidfd_1": {"family": "monitor", "case": "process-monitor-pidfd", "primary_metric": "cpu_ns_per_wall_second", "unit": "cpu_ns_per_wall_second", "boundary": "ready_idle_children_fixed_hold_process_cpu", "records": 1},
    "monitor_idle_pidfd_8": {"family": "monitor", "case": "process-monitor-pidfd", "primary_metric": "cpu_ns_per_wall_second", "unit": "cpu_ns_per_wall_second", "boundary": "ready_idle_children_fixed_hold_process_cpu", "records": 8},
    "monitor_idle_pidfd_64": {"family": "monitor", "case": "process-monitor-pidfd", "primary_metric": "cpu_ns_per_wall_second", "unit": "cpu_ns_per_wall_second", "boundary": "ready_idle_children_fixed_hold_process_cpu", "records": 64},
    "monitor_idle_posix_fallback_1": {"family": "monitor", "case": "process-monitor-fallback", "primary_metric": "cpu_ns_per_wall_second", "unit": "cpu_ns_per_wall_second", "boundary": "stabilized_fallback_ready_idle_children_fixed_hold_process_cpu", "records": 1},
    "monitor_idle_posix_fallback_8": {"family": "monitor", "case": "process-monitor-fallback", "primary_metric": "cpu_ns_per_wall_second", "unit": "cpu_ns_per_wall_second", "boundary": "stabilized_fallback_ready_idle_children_fixed_hold_process_cpu", "records": 8},
    "monitor_idle_posix_fallback_64": {"family": "monitor", "case": "process-monitor-fallback", "primary_metric": "cpu_ns_per_wall_second", "unit": "cpu_ns_per_wall_second", "boundary": "stabilized_fallback_ready_idle_children_fixed_hold_process_cpu", "records": 64},
    "family_curl_lifecycle": {"family": "curl", "case": "family-curl-lifecycle", "primary_metric": "lifecycle_ns", "unit": "ns", "boundary": "one_loopback_request_parse_and_cleanup"},
    "family_plugin_lifecycle": {"family": "plugin", "case": "family-plugin-lifecycle", "primary_metric": "lifecycle_ns", "unit": "ns", "boundary": "sample_plugin_start_initialize_call_shutdown"},
    "family_mcp_lifecycle": {"family": "mcp", "case": "family-mcp-lifecycle", "primary_metric": "lifecycle_ns", "unit": "ns", "boundary": "fake_mcp_initialize_tools_list_shutdown"},
    "family_lsp_lifecycle": {"family": "lsp", "case": "family-lsp-lifecycle", "primary_metric": "lifecycle_ns", "unit": "ns", "boundary": "fake_lsp_initialize_diagnostics_and_client_destruction"},
    "family_bash_lifecycle": {"family": "bash", "case": "family-bash-lifecycle", "primary_metric": "lifecycle_ns", "unit": "ns", "boundary": "sealed_direct_argv_planning_execution_and_cleanup"},
}

PROHIBITED_SAMPLE_KEY_TOKENS = {
    "pid",
    "pgid",
    "id",
    "fd",
    "argv",
    "command",
    "executable",
    "cwd",
    "path",
    "url",
    "uri",
    "environment",
    "env",
    "output",
    "frame",
    "prompt",
    "content",
}


def _sample_key_is_prohibited(name: Any) -> bool:
    if not isinstance(name, str) or not name:
        return True
    separated = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name)
    tokens = [token for token in re.split(r"[^A-Za-z0-9]+", separated.lower()) if token]
    return not tokens or any(token in PROHIBITED_SAMPLE_KEY_TOKENS for token in tokens)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def process_statistics(samples: Sequence[dict[str, Any]]) -> dict[str, Any]:
    if not samples:
        raise ValueError("cannot summarize process result without raw samples")
    primary = summarize([float(sample["value"]) for sample in samples])
    metric_names = sorted(
        {
            name
            for sample in samples
            for name, value in sample.get("metrics", {}).items()
            if _is_number(value)
        }
    )
    metrics = {
        name: summarize(
            [float(sample["metrics"][name]) for sample in samples if name in sample.get("metrics", {}) and _is_number(sample["metrics"][name])]
        )
        for name in metric_names
    }
    return {"primary": primary, "metrics": metrics}


def process_unsupported_result(result_id: str, reason_code: str, reason: str | None = None) -> dict[str, Any]:
    spec = PROCESS_RESULT_SPECS[result_id]
    expected_reason = PROCESS_REASON_TEXT.get(reason_code)
    if expected_reason is None:
        raise ValueError(f"unknown process unsupported reason code: {reason_code}")
    if reason is not None and reason != expected_reason:
        raise ValueError(f"non-static process unsupported reason for {result_id}")
    return {
        "id": result_id,
        "family": spec["family"],
        "status": "unsupported",
        "reason_code": reason_code,
        "reason": expected_reason,
        "primary_metric": spec["primary_metric"],
        "unit": spec["unit"],
        "boundary": spec["boundary"],
        "repetitions": 0,
        "observation_count": 0,
        "samples": [],
        "statistics": None,
    }


def process_measured_result(
    result_id: str,
    samples: Sequence[dict[str, Any]],
    repetitions: int,
    metadata: dict[str, Any] | None = None,
) -> dict[str, Any]:
    spec = PROCESS_RESULT_SPECS[result_id]
    result: dict[str, Any] = {
        "id": result_id,
        "family": spec["family"],
        "status": "measured",
        "primary_metric": spec["primary_metric"],
        "unit": spec["unit"],
        "boundary": spec["boundary"],
        "repetitions": repetitions,
        "observation_count": len(samples),
        "samples": list(samples),
        "statistics": process_statistics(samples),
    }
    if metadata:
        result["metadata"] = metadata
    if result_id.startswith("family_"):
        compatibility_names = ("protocol_compatible", "expected_response")
        result["compatibility_checks"] = {
            name: all(sample.get("checks", {}).get(name) is True for sample in samples) for name in compatibility_names
        }
        result["authority"] = (metadata or {}).get("authority", "legacy_local")
    return result


def _validate_closed_scalar(value: Any, label: str) -> None:
    if isinstance(value, bool):
        return
    if _is_number(value):
        return
    if isinstance(value, str) and value in PROCESS_CLOSED_LABELS:
        return
    raise RuntimeError(f"benchmark helper emitted non-closed {label}")


def validate_helper_payload(payload: Any, expected_case: str) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise RuntimeError("benchmark helper emitted a non-object JSON payload")
    if payload.get("helper_schema_version") != PROCESS_HELPER_SCHEMA_VERSION:
        raise RuntimeError("benchmark helper emitted an unexpected schema version")
    if payload.get("case") != expected_case:
        raise RuntimeError("benchmark helper changed the requested case identity")
    if payload.get("status") not in ("measured", "unsupported"):
        raise RuntimeError("benchmark helper emitted an unknown status")
    for key in ("primary_metric", "unit"):
        if not isinstance(payload.get(key), str) or not payload[key]:
            raise RuntimeError(f"benchmark helper omitted {key}")
    observations = payload.get("observations")
    case_metrics = payload.get("case_metrics")
    if not isinstance(observations, list) or not isinstance(case_metrics, dict):
        raise RuntimeError("benchmark helper omitted observations or case metrics")
    for name, value in case_metrics.items():
        if _sample_key_is_prohibited(name):
            raise RuntimeError("benchmark helper emitted a prohibited case metric")
        _validate_closed_scalar(value, "case metric")
    if payload["status"] == "unsupported":
        reason_code = payload.get("reason_code")
        if reason_code not in PROCESS_REASON_TEXT or payload.get("reason") != PROCESS_REASON_TEXT[reason_code]:
            raise RuntimeError("benchmark helper emitted an invalid unsupported reason")
        if observations:
            raise RuntimeError("unsupported benchmark helper payload contains observations")
        return payload
    if not observations:
        raise RuntimeError("measured benchmark helper payload contains no observations")
    for expected_ordinal, observation in enumerate(observations, 1):
        if not isinstance(observation, dict) or observation.get("ordinal") != expected_ordinal:
            raise RuntimeError("benchmark helper observations have invalid ordinals")
        if not _is_number(observation.get("value")) or float(observation["value"]) < 0:
            raise RuntimeError("benchmark helper observation has an invalid value")
        for field in ("metrics", "checks"):
            values = observation.get(field)
            if not isinstance(values, dict):
                raise RuntimeError(f"benchmark helper observation lacks {field}")
            for name, value in values.items():
                if _sample_key_is_prohibited(name):
                    raise RuntimeError("benchmark helper observation contains prohibited data")
                _validate_closed_scalar(value, field)
    return payload


def _validate_safe_sample(sample: Any) -> None:
    if not isinstance(sample, dict):
        raise ValueError("process sample must be an object")
    if set(sample) != {"run", "observation", "value", "metrics", "checks"}:
        raise ValueError("process sample has an invalid shape")
    if not isinstance(sample["run"], int) or sample["run"] <= 0 or not isinstance(sample["observation"], int) or sample["observation"] <= 0:
        raise ValueError("process sample has an invalid correlation identity")
    if not _is_number(sample["value"]) or float(sample["value"]) < 0:
        raise ValueError("process sample value must be non-negative finite numeric data")
    for field in ("metrics", "checks"):
        if not isinstance(sample[field], dict):
            raise ValueError(f"process sample {field} must be an object")
        for key, value in sample[field].items():
            if _sample_key_is_prohibited(key):
                raise ValueError("process sample contains prohibited data")
            _validate_closed_scalar(value, f"sample {field}")


def _helper_fixture_arguments(args: argparse.Namespace) -> list[str]:
    arguments = ["--sample-plugin", str(args.sample_plugin)]
    for option, value in (
        ("--fake-process-child", args.fake_process_child),
        ("--fake-mcp-server", args.fake_mcp_server),
        ("--fake-lsp-server", args.fake_lsp_server),
    ):
        if value is not None:
            arguments.extend((option, str(value)))
    return arguments


def run_process_helper(
    args: argparse.Namespace,
    root: Path,
    project: Path,
    result_id: str,
    extra: Sequence[str],
    driver_commands: list[dict[str, Any]],
) -> dict[str, Any]:
    if args.benchmark_helper is None:
        return process_unsupported_result(result_id, "fixture_unavailable")
    spec = PROCESS_RESULT_SPECS[result_id]
    benchmark_case = spec["case"]
    command = [str(args.benchmark_helper), "--case", benchmark_case, *extra, *_helper_fixture_arguments(args)]
    samples: list[dict[str, Any]] = []
    metadata: dict[str, Any] = {}
    for run_index in range(args.runs):
        home = root / f"process-helper-{result_id}-{run_index}"
        prepare_home(home)
        driver_commands.append({"result_id": result_id, "run": run_index + 1, "command": list(command)})
        wall_ns, completed = run_process(command, project, isolated_environment(home), 30.0)
        if completed.returncode != 0:
            raise RuntimeError(f"benchmark helper {result_id} failed with status {completed.returncode}")
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"benchmark helper {result_id} emitted malformed or truncated JSON") from error
        payload = validate_helper_payload(payload, benchmark_case)
        if payload["status"] == "unsupported":
            if samples:
                raise RuntimeError(f"benchmark helper {result_id} changed from measured to unsupported")
            return process_unsupported_result(result_id, payload["reason_code"], payload["reason"])
        if payload["primary_metric"] != spec["primary_metric"] or payload["unit"] != spec["unit"]:
            raise RuntimeError(f"benchmark helper {result_id} changed its metric contract")
        stable_metadata = {name: value for name, value in payload["case_metrics"].items() if isinstance(value, (str, bool))}
        if metadata and stable_metadata != metadata:
            raise RuntimeError(f"benchmark helper {result_id} changed closed case metadata")
        metadata = stable_metadata
        numeric_case_metrics = {
            name: value for name, value in payload["case_metrics"].items() if _is_number(value)
        }
        for observation in payload["observations"]:
            metrics = dict(observation["metrics"])
            for name, value in numeric_case_metrics.items():
                metrics.setdefault(name, value)
            metrics["helper_invocation_ns"] = wall_ns
            samples.append(
                {
                    "run": run_index + 1,
                    "observation": observation["ordinal"],
                    "value": float(observation["value"]),
                    "metrics": metrics,
                    "checks": dict(observation["checks"]),
                }
            )
    if not samples:
        raise RuntimeError(f"benchmark helper {result_id} produced no observations")
    return process_measured_result(result_id, samples, args.runs, metadata)


class _FixedLoopbackHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self) -> None:  # noqa: N802 - stdlib callback spelling
        if self.path != "/benchmark":
            self.send_error(404)
            return
        body = b"ava-backend-benchmark\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, _format: str, *_arguments: Any) -> None:
        return


class FixedLoopbackFixture:
    def __init__(self) -> None:
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _FixedLoopbackHandler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, name="ava-benchmark-loopback", daemon=True)

    @property
    def port(self) -> int:
        return int(self.server.server_address[1])

    def __enter__(self) -> "FixedLoopbackFixture":
        self.thread.start()
        return self

    def __exit__(self, _type: Any, _value: Any, _traceback: Any) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2.0)


def run_application_startup_v3(
    args: argparse.Namespace,
    root: Path,
    project: Path,
    driver_commands: list[dict[str, Any]],
) -> dict[str, Any]:
    command = [str(args.ava), "--rpc", "--offline", "--no-session"]
    warm_home = root / "process-startup-warmup"
    prepare_home(warm_home)
    driver_commands.append({"result_id": "application_warm_startup", "run": 0, "command": list(command), "warmup": True})
    _, warmup = run_process(command, project, isolated_environment(warm_home), 30.0)
    if warmup.returncode != 0:
        raise RuntimeError("AVA process benchmark startup warmup failed")
    samples: list[dict[str, Any]] = []
    for run_index in range(args.runs):
        home = root / f"process-startup-{run_index}"
        prepare_home(home)
        driver_commands.append({"result_id": "application_warm_startup", "run": run_index + 1, "command": list(command), "warmup": False})
        elapsed, completed = run_process(command, project, isolated_environment(home), 30.0)
        if completed.returncode != 0:
            raise RuntimeError("AVA process benchmark startup failed")
        samples.append(
            {
                "run": run_index + 1,
                "observation": 1,
                "value": elapsed,
                "metrics": {},
                "checks": {"return_code_zero": True},
            }
        )
    return process_measured_result("application_warm_startup", samples, args.runs, {"warmup_count": 1})


def run_application_idle_rss_v3(
    args: argparse.Namespace,
    root: Path,
    project: Path,
    driver_commands: list[dict[str, Any]],
) -> dict[str, Any]:
    if sys.platform != "linux" or not Path("/proc/self/smaps_rollup").is_file():
        return process_unsupported_result("application_idle_rss", "procfs_unavailable")
    output = root / "process-idle-memory.json"
    settle = "0.1" if args.suite == "process-smoke" else "1.0"
    sample_count = "1" if args.suite == "process-smoke" else "3"
    command = [
        sys.executable,
        str(args.memory_helper),
        "--apps",
        "ava",
        "--ava",
        str(args.ava),
        "--runs",
        str(args.runs),
        "--settle",
        settle,
        "--samples",
        sample_count,
        "--output",
        str(output),
    ]
    home = root / "process-memory-driver"
    prepare_home(home)
    driver_commands.append({"result_id": "application_idle_rss", "run": 1, "command": list(command)})
    wall_ns, completed = run_process(command, project, isolated_environment(home), 120.0)
    if completed.returncode != 0:
        raise RuntimeError("application idle RSS helper failed")
    payload = json.loads(output.read_text(encoding="utf-8"))
    try:
        runs = payload["apps"]["ava"]["runs"]
    except (KeyError, TypeError) as error:
        raise RuntimeError("application idle RSS output lacks measured runs") from error
    if not isinstance(runs, list) or len(runs) != args.runs:
        raise RuntimeError("application idle RSS output has an invalid run count")
    samples: list[dict[str, Any]] = []
    for run_index, run in enumerate(runs, 1):
        snapshots = run.get("samples") if isinstance(run, dict) else None
        if not isinstance(snapshots, list) or not snapshots:
            raise RuntimeError("application idle RSS run lacks snapshots")
        try:
            maximum = max(float(snapshot["rss_kib"]) for snapshot in snapshots)
        except (KeyError, TypeError, ValueError) as error:
            raise RuntimeError("application idle RSS run has an invalid snapshot") from error
        samples.append(
            {
                "run": run_index,
                "observation": 1,
                "value": maximum,
                "metrics": {"driver_invocation_ns": wall_ns},
                "checks": {"snapshots_present": True},
            }
        )
    return process_measured_result("application_idle_rss", samples, args.runs, {"aggregation": "maximum_observed_snapshot"})


def _git(repository: Path, *arguments: str) -> str:
    try:
        completed = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=15,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    if completed.returncode != 0:
        return "unknown"
    return completed.stdout.strip()


def _read_only_git_environment() -> dict[str, str]:
    environment = dict(os.environ)
    for name in (
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_COMMON_DIR",
        "GIT_DIR",
        "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY",
        "GIT_PREFIX",
        "GIT_WORK_TREE",
    ):
        environment.pop(name, None)
    environment.update(
        {
            "GIT_NO_LAZY_FETCH": "1",
            "GIT_NO_REPLACE_OBJECTS": "1",
            "GIT_OPTIONAL_LOCKS": "0",
            "GIT_PAGER": "cat",
            "GIT_TERMINAL_PROMPT": "0",
        }
    )
    return environment


def _read_only_git_command(repository: Path, *arguments: str) -> list[str]:
    return [
        "git",
        "-c",
        "core.fsmonitor=false",
        "-c",
        "core.hooksPath=",
        "-C",
        str(repository),
        *arguments,
    ]


def process_source_provenance(
    measured_source_root: Path, harness_repository: Path, runtime_reference: str
) -> dict[str, Any]:
    checkout = git_identity(measured_source_root)
    harness = git_identity(harness_repository)
    harness_script = harness_repository / "scripts" / "benchmark-backend.py"
    production_paths = PROCESS_PRODUCTION_SOURCE_PATHS
    reference_commit = _git(measured_source_root, "rev-parse", f"{runtime_reference}^{{commit}}")
    reference_tree = (
        _git(measured_source_root, "rev-parse", f"{runtime_reference}^{{tree}}")
        if reference_commit != "unknown"
        else "unknown"
    )
    if reference_commit == "unknown":
        production_equal: bool | None = None
    else:
        try:
            compared = subprocess.run(
                ["git", "-C", str(measured_source_root), "diff", "--quiet", reference_commit, "--", *production_paths],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=15,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            production_equal = None
        else:
            production_equal = compared.returncode == 0 if compared.returncode in (0, 1) else None
    production_status = _git(measured_source_root, "status", "--porcelain", "--", *production_paths)
    production_dirty = None if production_status == "unknown" else bool(production_status)
    return {
        "measured_checkout": checkout,
        "runtime_reference": {
            "requested_revision": runtime_reference,
            "commit": reference_commit,
            "tree": reference_tree,
            "production_paths": list(production_paths),
            "exact_production_path_equality": production_equal,
            "measured_production_paths_dirty": production_dirty,
        },
        "harness": {
            "repository": harness["repository"],
            "commit": harness["commit"],
            "tree": harness["tree"],
            "dirty": harness["dirty"],
            "benchmark_script_sha256": sha256_file(harness_script),
            "contract_version": PROCESS_CONTRACT_VERSION,
        },
    }


def _scope_entries_from_git(
    repository: Path,
    revision: str,
    canonical_paths: Sequence[str],
    canonical_pathspecs: Sequence[str],
) -> list[dict[str, str]]:
    if revision != "HEAD" and not _is_full_git_object_id(revision):
        raise RuntimeError("source scope revision must be HEAD or a full commit ID")
    with tempfile.TemporaryDirectory(prefix="ava-benchmark-git-index-") as temporary:
        index_path = Path(temporary) / "index"
        environment = _read_only_git_environment()
        environment["GIT_INDEX_FILE"] = str(index_path)
        try:
            read_tree = subprocess.run(
                _read_only_git_command(repository, "read-tree", revision),
                env=environment,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=15,
                check=False,
            )
            if read_tree.returncode != 0:
                raise RuntimeError(f"cannot resolve source scope revision {revision}")
            listed = subprocess.run(
                _read_only_git_command(
                    repository,
                    "ls-files",
                    "--stage",
                    "-z",
                    "--",
                    *canonical_paths,
                    *canonical_pathspecs,
                ),
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=15,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise RuntimeError("cannot resolve source scope entries") from error
    if listed.returncode != 0:
        raise RuntimeError("cannot resolve source scope entries")

    entries: list[dict[str, str]] = []
    for record in listed.stdout.split(b"\0"):
        if not record:
            continue
        try:
            metadata, raw_path = record.split(b"\t", 1)
            mode, object_id, stage = metadata.decode("ascii").split()
            path = raw_path.decode("utf-8")
        except (UnicodeDecodeError, ValueError) as error:
            raise RuntimeError("Git returned an invalid source scope entry") from error
        if stage != "0":
            raise RuntimeError("Git returned an unmerged source scope entry")
        entries.append(
            {
                "mode": mode,
                "type": "commit" if mode == "160000" else "blob",
                "object": object_id,
                "path": path,
            }
        )
    entries.sort(key=lambda entry: entry["path"])
    if len({entry["path"] for entry in entries}) != len(entries):
        raise RuntimeError("Git returned duplicate source scope entries")
    return entries


def _source_scope_digest(
    scope_kind: str,
    canonical_paths: Sequence[str],
    canonical_pathspecs: Sequence[str],
    entries: Sequence[dict[str, str]],
) -> str:
    payload = {
        "scope_kind": scope_kind,
        "canonical_paths": list(canonical_paths),
        "canonical_pathspecs": list(canonical_pathspecs),
        "entries": list(entries),
    }
    encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def source_scope_identity(
    repository: Path,
    declaration: dict[str, Any],
    revision: str = "HEAD",
) -> dict[str, Any]:
    scope_kind = declaration["scope_kind"]
    canonical_paths = declaration["canonical_paths"]
    canonical_pathspecs = declaration["canonical_pathspecs"]
    entries = _scope_entries_from_git(repository, revision, canonical_paths, canonical_pathspecs)
    if not entries:
        raise RuntimeError(f"source scope {scope_kind} resolved no entries")
    return {
        "scope_kind": scope_kind,
        "canonical_paths": list(canonical_paths),
        "canonical_pathspecs": list(canonical_pathspecs),
        "entry_count": len(entries),
        "entries": entries,
        "scope_digest_sha256": _source_scope_digest(
            scope_kind, canonical_paths, canonical_pathspecs, entries
        ),
    }


def family_source_identities(repository: Path, revision: str = "HEAD") -> dict[str, Any]:
    return {
        family: source_scope_identity(repository, declaration, revision)
        for family, declaration in PROCESS_FAMILY_SOURCE_SCOPES.items()
    }


def shared_process_source_identity(repository: Path, revision: str = "HEAD") -> dict[str, Any]:
    return source_scope_identity(repository, PROCESS_SHARED_SOURCE_SCOPE, revision)


def process_source_scope_identities(
    repository: Path, revision: str = "HEAD"
) -> tuple[dict[str, Any], dict[str, Any]]:
    families = family_source_identities(repository, revision)
    shared = shared_process_source_identity(repository, revision)
    scopes = [*families.values(), shared]
    selected_paths = [entry["path"] for scope in scopes for entry in scope["entries"]]
    if len(set(selected_paths)) != len(selected_paths):
        raise RuntimeError("process source ownership scopes overlap")

    production_entries = _scope_entries_from_git(
        repository, revision, PROCESS_PRODUCTION_SOURCE_PATHS, ()
    )
    if {entry["path"] for entry in production_entries} != set(selected_paths):
        raise RuntimeError("process source ownership scopes do not cover the production source paths")
    return families, shared


def _git_blob_at_path(repository: Path, revision: str, path: str) -> tuple[str, bytes]:
    if revision != "HEAD" and not _is_full_git_object_id(revision):
        raise RuntimeError("Git blob revision must be HEAD or a full commit ID")
    environment = _read_only_git_environment()
    try:
        listed = subprocess.run(
            _read_only_git_command(repository, "ls-tree", "-z", revision, "--", path),
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=15,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError("cannot resolve benchmark family authority source") from error
    records = [record for record in listed.stdout.split(b"\0") if record]
    if listed.returncode != 0 or len(records) != 1:
        raise RuntimeError("cannot resolve benchmark family authority source")
    try:
        metadata, raw_path = records[0].split(b"\t", 1)
        _mode, object_type, raw_object = metadata.decode("ascii").split()
        resolved_path = raw_path.decode("utf-8")
    except (UnicodeDecodeError, ValueError) as error:
        raise RuntimeError("cannot resolve benchmark family authority source") from error
    if object_type != "blob" or resolved_path != path or not _is_full_git_object_id(raw_object):
        raise RuntimeError("cannot resolve benchmark family authority source")
    try:
        loaded = subprocess.run(
            _read_only_git_command(repository, "cat-file", "blob", raw_object),
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=15,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise RuntimeError("cannot resolve benchmark family authority source") from error
    if loaded.returncode != 0:
        raise RuntimeError("cannot resolve benchmark family authority source")
    return raw_object, loaded.stdout


def family_authority_source_identity(repository: Path, revision: str = "HEAD") -> dict[str, Any]:
    source_object, committed_bytes = _git_blob_at_path(
        repository, revision, PROCESS_AUTHORITY_SOURCE_PATH
    )
    if revision == "HEAD":
        source = repository / PROCESS_AUTHORITY_SOURCE_PATH
        text = source.read_text(encoding="utf-8")
        source_sha256 = sha256_file(source)
    else:
        try:
            text = committed_bytes.decode("utf-8")
        except UnicodeDecodeError as error:
            raise RuntimeError("benchmark family authority source is not UTF-8") from error
        source_sha256 = hashlib.sha256(committed_bytes).hexdigest()
    matches = re.findall(
        r"^set\(AVA_BENCHMARK_(CURL|PLUGIN|MCP|LSP|BASH)_AUTHORITY +(legacy_local|supervised)\)$",
        text,
        re.MULTILINE,
    )
    expected_names = tuple(name.upper() for name in PROCESS_FAMILY_NAMES)
    if tuple(name for name, _authority in matches) != expected_names:
        raise RuntimeError("benchmark family authority source must declare each family exactly once in canonical order")
    return {
        "path": PROCESS_AUTHORITY_SOURCE_PATH,
        "object": source_object,
        "sha256": source_sha256,
        "authorities": {name.lower(): authority for name, authority in matches},
    }


def file_identity_v3(path: Path) -> dict[str, Any]:
    identity = file_identity(path)
    metadata = path.resolve().stat()
    identity["mode"] = stat.S_IMODE(metadata.st_mode)
    identity["mtime_ns"] = metadata.st_mtime_ns
    return identity


def optional_file_identity_v3(path: Path | None) -> dict[str, Any] | None:
    return file_identity_v3(path) if path is not None else None


def process_artifact_inventory(args: argparse.Namespace, repository: Path) -> dict[str, Any]:
    curl_path_text = shutil.which("curl", path=TRUSTED_PATH)
    pwd_path_text = shutil.which("pwd", path=TRUSTED_PATH)
    manifest = args.sample_plugin / "plugin.json"
    entrypoint = args.sample_plugin / "plugin.sh"
    artifacts: dict[str, Any] = {
        "ava": file_identity_v3(args.ava),
        "benchmark_helper": file_identity_v3(args.benchmark_helper),
        "benchmark_script": file_identity_v3(repository / "scripts" / "benchmark-backend.py"),
        "memory_helper": file_identity_v3(args.memory_helper),
        "python": file_identity_v3(Path(sys.executable)),
        "fake_process_child": optional_file_identity_v3(args.fake_process_child),
        "fake_mcp_server": optional_file_identity_v3(args.fake_mcp_server),
        "fake_lsp_server": optional_file_identity_v3(args.fake_lsp_server),
        "fake_provider": optional_file_identity_v3(args.fake_provider),
        "curl": file_identity_v3(Path(curl_path_text)) if curl_path_text else None,
        "bash_direct_argv_executable": file_identity_v3(Path(pwd_path_text)) if pwd_path_text else None,
        "sample_plugin": {
            "root": str(args.sample_plugin),
            "manifest": file_identity_v3(manifest),
            "entrypoint": file_identity_v3(entrypoint),
        },
    }
    return artifacts


def _command_first_line(command: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            list(command),
            env={"PATH": TRUSTED_PATH, "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"},
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return "unavailable"
    return completed.stdout.splitlines()[0] if completed.stdout else "unknown"


def process_build_provenance(
    binary: Path,
    repository: Path,
    capabilities: dict[str, Any],
    family_authorities: dict[str, str],
) -> dict[str, Any]:
    base = build_metadata(binary, repository)
    cache = find_cmake_cache(binary)
    values = read_cmake_cache(cache) if cache is not None else {}
    compiler_path_text = base["compiler"]["path"]
    compiler_path = Path(compiler_path_text) if compiler_path_text not in ("", "unknown") else None
    compiler_artifact = None
    if compiler_path is not None and compiler_path.is_file():
        compiler_artifact = file_identity_v3(compiler_path)
    configured_cmake = values.get("CMAKE_COMMAND")
    cmake_path_text = configured_cmake if configured_cmake and Path(configured_cmake).is_file() else shutil.which(
        "cmake", path=TRUSTED_PATH
    )
    cmake_artifact = file_identity_v3(Path(cmake_path_text)) if cmake_path_text else None
    authority_features = {
        name: family_authorities.get(name, "unknown") for name in PROCESS_FAMILY_NAMES
    }
    features = dict(base["features"])
    features.update(
        {
            "process_supervisor": capabilities.get("process_supervisor"),
            "process_fixture": capabilities.get("process_fixture"),
            "platform_backend": capabilities.get("platform_backend"),
            "family_authorities": authority_features,
        }
    )
    cache_identity = file_identity_v3(cache) if cache is not None else None
    recipe = {
        "generator": values.get("CMAKE_GENERATOR", "unknown"),
        "cmake_version": _command_first_line([cmake_path_text, "--version"]) if cmake_path_text else "unavailable",
        "build_type": base["build_type"],
        "cmake_flags": base["cmake_flags"],
        "cxx_flags": base["cxx_flags"],
    }
    return {
        "generator": recipe["generator"],
        "cmake_version": recipe["cmake_version"],
        "cmake": cmake_artifact,
        "cmake_cache": cache_identity,
        "cmake_source_root": base["cmake_source_root"],
        "build_type": base["build_type"],
        "features": features,
        "compiler": {
            "path": compiler_path_text,
            "artifact": compiler_artifact,
            "id": base["compiler"]["id"],
            "configured_version": base["compiler"]["configured_version"],
            "version_output": base["compiler"]["version_output"],
            "flags": base["cxx_flags"],
        },
        "recipe": recipe,
        "best_effort_provenance": {
            "assessment": "best_effort_unverified",
            "statement": "This binary does not embed a verified source commit; cache, source-root, byte identity, and timestamps are evidence only.",
            "git_commit_embedding_verified": False,
            "cmake_source_root_matches_recorded_source": base["provenance"]["cmake_source_root_matches_recorded_source"],
            "binary_is_within_cmake_build_tree": base["provenance"]["binary_is_within_cmake_build_tree"],
            "binary_not_older_than_cmake_cache": base["provenance"]["binary_not_older_than_cmake_cache"],
        },
    }


def _build_configuration_identity(build: dict[str, Any]) -> dict[str, Any]:
    return {
        "generator": build.get("generator"),
        "cmake_version": build.get("cmake_version"),
        "cmake_sha256": _artifact_hash(build.get("cmake")),
        "build_type": build.get("build_type"),
        "features": build.get("features"),
        "recipe": build.get("recipe"),
        "compiler": {
            "path": build.get("compiler", {}).get("path"),
            "sha256": _artifact_hash(build.get("compiler", {}).get("artifact")),
            "id": build.get("compiler", {}).get("id"),
            "configured_version": build.get("compiler", {}).get("configured_version"),
            "version_output": build.get("compiler", {}).get("version_output"),
            "flags": build.get("compiler", {}).get("flags"),
        },
    }


def process_binary_build_binding(
    measured_ava_build: dict[str, Any], benchmark_helper_build: dict[str, Any]
) -> dict[str, Any]:
    ava_cache = measured_ava_build.get("cmake_cache")
    helper_cache = benchmark_helper_build.get("cmake_cache")
    same_cache = (
        isinstance(ava_cache, dict)
        and isinstance(helper_cache, dict)
        and ava_cache.get("path") == helper_cache.get("path")
        and ava_cache.get("sha256") == helper_cache.get("sha256")
    )
    equivalent = _build_configuration_identity(measured_ava_build) == _build_configuration_identity(
        benchmark_helper_build
    )
    if same_cache:
        relationship = "same_cmake_cache"
    elif equivalent:
        relationship = "equivalent_recorded_configuration"
    else:
        relationship = "inconsistent"
    return {
        "assessment": "best_effort_unverified",
        "statement": (
            "Neither the measured AVA binary nor the benchmark helper embeds a verified source commit; "
            "their independent cache and artifact provenance is best-effort evidence only."
        ),
        "relationship": relationship,
        "recorded_configuration_equivalent": equivalent,
        "measured_ava_git_commit_embedding_verified": False,
        "benchmark_helper_git_commit_embedding_verified": False,
    }


def _limit_value(value: int) -> int | str:
    return "infinity" if value == resource.RLIM_INFINITY else int(value)


def host_provenance(load_at_start: Sequence[float]) -> dict[str, Any]:
    boot_hash: str | None = None
    try:
        boot_value = Path("/proc/sys/kernel/random/boot_id").read_text(encoding="utf-8").strip()
        if boot_value:
            boot_hash = hashlib.sha256(boot_value.encode("utf-8")).hexdigest()
    except OSError:
        pass
    limits: dict[str, Any] = {}
    for name in ("RLIMIT_NOFILE", "RLIMIT_NPROC", "RLIMIT_AS", "RLIMIT_CORE", "RLIMIT_STACK"):
        selector = getattr(resource, name, None)
        if selector is None:
            continue
        try:
            soft, hard = resource.getrlimit(selector)
            limits[name] = {"soft": _limit_value(soft), "hard": _limit_value(hard)}
        except (OSError, ValueError):
            limits[name] = {"soft": "unavailable", "hard": "unavailable"}
    try:
        page_size = int(os.sysconf("SC_PAGE_SIZE"))
    except (OSError, ValueError, KeyError):
        page_size = 0
    return {
        "os": platform.system(),
        "platform": platform.platform(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "cpu": cpu_model(),
        "cpu_count": os.cpu_count(),
        "ram_bytes": total_ram_bytes(),
        "page_size_bytes": page_size,
        "python_version": platform.python_version(),
        "python_implementation": platform.python_implementation(),
        "boot_id_sha256": boot_hash,
        "limits": limits,
        "monotonic_clock_resolution_ns": time.get_clock_info("monotonic").resolution * 1_000_000_000.0,
        "load_at_start": list(load_at_start),
        "load_at_end": None,
    }


def run_capability_probe(
    args: argparse.Namespace,
    root: Path,
    project: Path,
    driver_commands: list[dict[str, Any]],
) -> dict[str, Any]:
    command = [str(args.benchmark_helper), "--case", "process-capabilities", *_helper_fixture_arguments(args)]
    home = root / "process-capabilities"
    prepare_home(home)
    driver_commands.append({"result_id": "capabilities", "run": 1, "command": list(command)})
    _, completed = run_process(command, project, isolated_environment(home), 30.0)
    if completed.returncode != 0:
        raise RuntimeError("benchmark helper capability probe failed")
    try:
        payload = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError("benchmark helper capability probe emitted malformed or truncated JSON") from error
    payload = validate_helper_payload(payload, "process-capabilities")
    if payload["status"] != "measured" or payload["primary_metric"] != "capability_probe" or payload["unit"] != "count":
        raise RuntimeError("benchmark helper capability probe is not measured metadata")
    metrics = payload["case_metrics"]
    required = {
        "process_supervisor",
        "process_fixture",
        "platform_backend",
        "process_backend",
        "helper_contract",
        "curl_authority",
        "plugin_authority",
        "mcp_authority",
        "lsp_authority",
        "bash_authority",
    }
    if set(metrics) != required or metrics["helper_contract"] != PROCESS_CONTRACT_VERSION:
        raise RuntimeError("benchmark helper capability metadata is incomplete")
    return dict(metrics)


def process_smoke_checks(results: Sequence[dict[str, Any]], capabilities: dict[str, Any]) -> list[dict[str, Any]]:
    by_id = {result["id"]: result for result in results}
    checks: list[dict[str, Any]] = []

    def add(name: str, passed: bool, ceiling: str) -> None:
        checks.append({"name": name, "passed": bool(passed), "ceiling": ceiling})

    def maximum(result_id: str, metric: str | None = None) -> float | None:
        result = by_id[result_id]
        statistics_value = result.get("statistics")
        if not statistics_value:
            return None
        selected = statistics_value["primary"] if metric is None else statistics_value["metrics"].get(metric)
        return float(selected["maximum"]) if selected else None

    def measured(result_id: str) -> bool:
        return by_id[result_id]["status"] == "measured" and bool(by_id[result_id]["samples"])

    startup = maximum("application_warm_startup")
    add("application_startup_not_catastrophic", startup is not None and startup < 30_000_000_000, "< 30 seconds")
    rss = maximum("application_idle_rss")
    add("application_idle_rss_not_catastrophic", rss is not None and rss < 4 * 1024 * 1024, "< 4 GiB")

    supervisor_ids = [result_id for result_id in PROCESS_EXPECTED_RESULT_IDS if result_id.startswith(("supervisor_", "monitor_"))]
    process_present = capabilities.get("process_supervisor") is True and capabilities.get("platform_backend") == "posix"
    if not process_present:
        structured = all(
            by_id[result_id]["status"] == "unsupported" and by_id[result_id].get("reason_code") == "source_architecture_absent"
            for result_id in supervisor_ids
        )
        add("source_architecture_absence_is_structured", structured, "every supervisor result is source_architecture_absent")
    else:
        required_measured = [
            result_id
            for result_id in supervisor_ids
            if not result_id.startswith("monitor_idle_pidfd_")
        ]
        add("required_process_cases_measured", all(measured(result_id) for result_id in required_measured), "all required M1 process seams measured")
        pidfd_valid = all(
            measured(result_id)
            or (by_id[result_id]["status"] == "unsupported" and by_id[result_id].get("reason_code") == "pidfd_unavailable")
            for result_id in supervisor_ids
            if result_id.startswith("monitor_idle_pidfd_")
        )
        add("pidfd_cases_honest", pidfd_valid, "measured automatic pidfd or pidfd_unavailable")
        scope = maximum("supervisor_idle_scope_startup")
        scope_rss = maximum("supervisor_idle_scope_startup", "rss_delta_kib")
        add("idle_scope_not_catastrophic", scope is not None and scope < 5_000_000_000, "< 5 seconds")
        add("idle_scope_rss_not_catastrophic", scope_rss is not None and abs(scope_rss) < 256 * 1024, "absolute RSS delta < 256 MiB")
        concurrent = maximum("supervisor_concurrent_records_64", "batch_spawn_commit_ns")
        add("concurrent_64_not_catastrophic", concurrent is not None and concurrent < 30_000_000_000, "batch < 30 seconds")
        for result_id in ("supervisor_leader_first_descendant_cleanup", "supervisor_term_refusal_escalation"):
            value = maximum(result_id)
            add(f"{result_id}_not_catastrophic", value is not None and value < 5_000_000_000, "< 5 seconds")
        shutdown = maximum("supervisor_shared_budget_shutdown_64")
        add("shutdown_64_not_catastrophic", shutdown is not None and shutdown < 15_000_000_000, "< 15 seconds")
        for prefix in ("monitor_idle_pidfd_", "monitor_idle_posix_fallback_"):
            result_id = prefix + "64"
            value = maximum(result_id)
            if prefix == "monitor_idle_pidfd_" and by_id[result_id]["status"] == "unsupported":
                continue
            add(f"{result_id}_cpu_not_catastrophic", value is not None and value < 500_000_000, "< 0.5 core")
        fallback = by_id["monitor_idle_posix_fallback_64"]
        fallback_probes = maximum("monitor_idle_posix_fallback_64", "fallback_probes_delta")
        add(
            "fallback_probe_bound",
            fallback["status"] == "measured" and fallback_probes is not None and fallback_probes <= 12 * 64 + 8,
            "500ms probe delta <= 12*records+8",
        )

    for result_id in PROCESS_EXPECTED_RESULT_IDS[-5:]:
        value = maximum(result_id)
        add(f"{result_id}_not_catastrophic", value is not None and value < 10_000_000_000, "one lifecycle < 10 seconds")

    helper_times = [
        float(sample["metrics"]["helper_invocation_ns"])
        for result in results
        for sample in result.get("samples", [])
        if "helper_invocation_ns" in sample.get("metrics", {})
    ]
    add("helper_invocations_not_catastrophic", bool(helper_times) and max(helper_times) < 30_000_000_000, "each helper invocation < 30 seconds")
    measured_checks = [
        value
        for result in results
        if result["status"] == "measured"
        for sample in result["samples"]
        for value in sample["checks"].values()
        if isinstance(value, bool)
    ]
    add("all_measured_correctness_checks", bool(measured_checks) and all(measured_checks), "every emitted correctness and cleanup check is true")
    family_measured = all(by_id[result_id]["status"] == "measured" for result_id in PROCESS_EXPECTED_RESULT_IDS[-5:])
    add("all_legacy_family_lifecycles_measured", family_measured, "all five current-family drivers measured")
    return checks


def execute_process(args: argparse.Namespace) -> dict[str, Any]:
    harness_repository = Path(__file__).resolve().parents[1]
    measured_source_root = args.measured_source_root
    root = Path(tempfile.mkdtemp(prefix="ava-process-benchmark-"))
    project = root / "project"
    project.mkdir()
    (project / ".git").mkdir()
    driver_commands: list[dict[str, Any]] = []
    started_at = dt.datetime.now(dt.timezone.utc).isoformat()
    try:
        try:
            load_start = os.getloadavg()
        except OSError:
            load_start = (0.0, 0.0, 0.0)
        capabilities = run_capability_probe(args, root, project, driver_commands)
        results: list[dict[str, Any]] = [
            run_application_startup_v3(args, root, project, driver_commands),
            run_application_idle_rss_v3(args, root, project, driver_commands),
        ]
        common = ["--deadline-ms", "2000", "--grace-ms", "75"]
        results.append(run_process_helper(args, root, project, "supervisor_idle_scope_startup", common, driver_commands))
        results.append(run_process_helper(args, root, project, "supervisor_first_spawn_commit", common, driver_commands))
        sequential_iterations = 1 if args.suite == "process-smoke" else 20
        results.append(
            run_process_helper(
                args,
                root,
                project,
                "supervisor_warm_sequential_spawn_commit",
                ["--iterations", str(sequential_iterations), *common],
                driver_commands,
            )
        )
        for records in (1, 8, 64):
            results.append(
                run_process_helper(
                    args,
                    root,
                    project,
                    f"supervisor_concurrent_records_{records}",
                    ["--records", str(records), *common],
                    driver_commands,
                )
            )
        results.append(run_process_helper(args, root, project, "supervisor_natural_exit_settlement", common, driver_commands))
        results.append(run_process_helper(args, root, project, "supervisor_leader_first_descendant_cleanup", common, driver_commands))
        results.append(run_process_helper(args, root, project, "supervisor_term_refusal_escalation", common, driver_commands))
        results.append(
            run_process_helper(
                args,
                root,
                project,
                "supervisor_shared_budget_shutdown_64",
                ["--records", "64", *common],
                driver_commands,
            )
        )
        hold_milliseconds = 500 if args.suite == "process-smoke" else 2000
        for backend in ("pidfd", "posix_fallback"):
            for records in (1, 8, 64):
                results.append(
                    run_process_helper(
                        args,
                        root,
                        project,
                        f"monitor_idle_{backend}_{records}",
                        ["--records", str(records), "--hold-ms", str(hold_milliseconds), *common],
                        driver_commands,
                    )
                )
        with FixedLoopbackFixture() as loopback:
            results.append(
                run_process_helper(
                    args,
                    root,
                    project,
                    "family_curl_lifecycle",
                    ["--loopback-port", str(loopback.port)],
                    driver_commands,
                )
            )
        for result_id in (
            "family_plugin_lifecycle",
            "family_mcp_lifecycle",
            "family_lsp_lifecycle",
            "family_bash_lifecycle",
        ):
            results.append(run_process_helper(args, root, project, result_id, [], driver_commands))

        host = host_provenance(load_start)
        try:
            host["load_at_end"] = list(os.getloadavg())
        except OSError:
            host["load_at_end"] = [0.0, 0.0, 0.0]
        source = process_source_provenance(measured_source_root, harness_repository, args.runtime_reference)
        family_sources, shared_process_source = process_source_scope_identities(measured_source_root)
        source["family_sources"] = family_sources
        source["shared_process_source"] = shared_process_source
        authority_source = family_authority_source_identity(measured_source_root)
        source["family_authorities"] = authority_source
        source["build"] = process_build_provenance(
            args.ava, measured_source_root, capabilities, authority_source["authorities"]
        )
        source["benchmark_helper_build"] = process_build_provenance(
            args.benchmark_helper, measured_source_root, capabilities, authority_source["authorities"]
        )
        source["binary_build_binding"] = process_binary_build_binding(
            source["build"], source["benchmark_helper_build"]
        )
        source["host"] = host
        source["driver"] = {
            "exact_command": list(args._exact_command),
            "run_order_label": args.run_order,
            "run_order": list(PROCESS_EXPECTED_RESULT_IDS),
            "invocations": driver_commands,
            "warmup": {"application": 1, "warm_sequential": 1},
            "records": [1, 8, 64],
            "iterations": {"warm_sequential": sequential_iterations},
            "hold_milliseconds": hold_milliseconds,
            "termination_grace_milliseconds": 75,
            "shutdown_deadline_milliseconds": 2000,
            "measured_repetitions": args.runs,
            "clock": "monotonic_ns_and_steady_clock",
            "environment_policy": "fixed_allowlist_no_host_environment_inheritance",
        }
        document: dict[str, Any] = {
            "schema_version": PROCESS_SCHEMA_VERSION,
            "contract_version": PROCESS_CONTRACT_VERSION,
            "generated_at_utc": started_at,
            "completed_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "suite": args.suite,
            "provenance": source,
            "artifacts": process_artifact_inventory(args, harness_repository),
            "capabilities": capabilities,
            "results": results,
            "checks": [],
        }
        document["checks"] = process_smoke_checks(results, capabilities) if args.suite == "process-smoke" else []
        validate_process_document(document)
        return document
    finally:
        shutil.rmtree(root, ignore_errors=True)


def _statistics_equal(actual: Any, expected: Any) -> bool:
    if not isinstance(actual, dict) or actual.keys() != expected.keys():
        return False
    for key, expected_value in expected.items():
        actual_value = actual[key]
        if isinstance(expected_value, dict):
            if not _statistics_equal(actual_value, expected_value):
                return False
        elif not _is_number(actual_value) or float(actual_value) != float(expected_value):
            return False
    return True


def _is_resolved_text(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip()) and value.strip().lower() != "unknown"


def _is_full_git_object_id(value: Any) -> bool:
    return isinstance(value, str) and FULL_GIT_OBJECT_ID.fullmatch(value) is not None


def _is_full_sha256(value: Any) -> bool:
    return isinstance(value, str) and FULL_SHA256.fullmatch(value) is not None


def _has_process_provenance_split(provenance: dict[str, Any]) -> bool:
    return all(field in provenance.get(section, {}) for section, field in PROCESS_PROVENANCE_SPLIT_FIELDS)


def _has_complete_process_source_scopes(provenance: dict[str, Any]) -> bool:
    families = provenance.get("family_sources")
    shared = provenance.get("shared_process_source")
    return (
        isinstance(families, dict)
        and set(families) == set(PROCESS_FAMILY_NAMES)
        and all(
            isinstance(families.get(family), dict)
            and "scope_kind" in families[family]
            for family in PROCESS_FAMILY_NAMES
        )
        and isinstance(shared, dict)
        and "scope_kind" in shared
    )


def _canonical_scope_entry_path(value: Any) -> bool:
    if not isinstance(value, str) or not value:
        return False
    path = PurePosixPath(value)
    return (
        not path.is_absolute()
        and path.as_posix() == value
        and value != "."
        and all(part not in ("", ".", "..") for part in path.parts)
    )


def _validate_source_scope_identity(
    identity: Any,
    declaration: dict[str, Any],
    description: str,
) -> None:
    expected_fields = {
        "scope_kind",
        "canonical_paths",
        "canonical_pathspecs",
        "entry_count",
        "entries",
        "scope_digest_sha256",
    }
    if not isinstance(identity, dict) or set(identity) != expected_fields:
        raise ValueError(f"process {description} source scope must be a complete identity object")
    scope_kind = declaration["scope_kind"]
    canonical_paths = list(declaration["canonical_paths"])
    canonical_pathspecs = list(declaration["canonical_pathspecs"])
    if identity.get("scope_kind") != scope_kind:
        raise ValueError(f"process {description} source scope has the wrong kind")
    if identity.get("canonical_paths") != canonical_paths:
        raise ValueError(f"process {description} source scope has noncanonical paths")
    if identity.get("canonical_pathspecs") != canonical_pathspecs:
        raise ValueError(f"process {description} source scope has noncanonical pathspecs")

    entries = identity.get("entries")
    if not isinstance(entries, list) or not entries:
        raise ValueError(f"process {description} source scope has no entries")
    if identity.get("entry_count") != len(entries):
        raise ValueError(f"process {description} source scope has the wrong entry count")
    previous_path: str | None = None
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != {"mode", "type", "object", "path"}:
            raise ValueError(f"process {description} source scope entry {index} is invalid")
        mode = entry["mode"]
        entry_type = entry["type"]
        path = entry["path"]
        if not isinstance(mode, str) or GIT_ENTRY_MODE.fullmatch(mode) is None:
            raise ValueError(f"process {description} source scope entry {index} has an invalid mode")
        if entry_type != ("commit" if mode == "160000" else "blob"):
            raise ValueError(f"process {description} source scope entry {index} has an invalid type")
        if not _is_full_git_object_id(entry["object"]):
            raise ValueError(f"process {description} source scope entry {index} has an invalid object")
        if not _canonical_scope_entry_path(path):
            raise ValueError(f"process {description} source scope entry {index} has a noncanonical path")
        if previous_path is not None and path <= previous_path:
            raise ValueError(f"process {description} source scope entries are not uniquely sorted")
        previous_path = path
        if scope_kind == "recursive_tree" and not any(
            path == root or path.startswith(f"{root}/") for root in canonical_paths
        ):
            raise ValueError(f"process {description} source scope entry {index} is outside its tree")

    expected_digest = _source_scope_digest(
        scope_kind, canonical_paths, canonical_pathspecs, entries
    )
    if identity.get("scope_digest_sha256") != expected_digest:
        raise ValueError(f"process {description} source scope digest does not match its entries")


def _validate_process_source_scopes(provenance: dict[str, Any]) -> None:
    families = provenance.get("family_sources")
    shared_present = "shared_process_source" in provenance
    family_scope_markers = [
        isinstance(families, dict)
        and isinstance(families.get(family), dict)
        and "scope_kind" in families[family]
        for family in PROCESS_FAMILY_NAMES
    ]
    if not shared_present and not any(family_scope_markers):
        return
    if not _has_complete_process_source_scopes(provenance):
        raise ValueError("process source ownership scopes must be present atomically")

    for family, declaration in PROCESS_FAMILY_SOURCE_SCOPES.items():
        _validate_source_scope_identity(families[family], declaration, family)
    shared = provenance["shared_process_source"]
    _validate_source_scope_identity(shared, PROCESS_SHARED_SOURCE_SCOPE, "shared process")
    selected_paths = [
        entry["path"]
        for identity in [*(families[family] for family in PROCESS_FAMILY_NAMES), shared]
        for entry in identity["entries"]
    ]
    if len(set(selected_paths)) != len(selected_paths):
        raise ValueError("process source ownership scopes overlap")


def validate_process_document(document: dict[str, Any]) -> None:
    if document.get("schema_version") != PROCESS_SCHEMA_VERSION or document.get("contract_version") != PROCESS_CONTRACT_VERSION:
        raise ValueError("unexpected process benchmark schema or contract version")
    for key in ("generated_at_utc", "completed_at_utc", "suite", "provenance", "artifacts", "capabilities", "results", "checks"):
        if key not in document:
            raise ValueError(f"process benchmark document lacks {key}")
    if document["suite"] not in ("process-smoke", "process-baseline"):
        raise ValueError("process benchmark document has an invalid suite")
    provenance = document["provenance"]
    for key in ("measured_checkout", "runtime_reference", "harness", "family_sources", "build", "host", "driver"):
        if key not in provenance:
            raise ValueError(f"process benchmark provenance lacks {key}")
    measured_checkout = provenance["measured_checkout"]
    for key in ("commit", "tree", "dirty"):
        if key not in measured_checkout:
            raise ValueError(f"process measured checkout lacks {key}")
    for key in ("commit", "tree", "exact_production_path_equality"):
        if key not in provenance["runtime_reference"]:
            raise ValueError(f"process runtime reference lacks {key}")
    harness = provenance["harness"]
    for key in ("commit", "tree", "dirty"):
        if key not in harness:
            raise ValueError(f"process harness provenance lacks {key}")
    split_field_presence = [field in provenance[section] for section, field in PROCESS_PROVENANCE_SPLIT_FIELDS]
    harness_split_present = any(split_field_presence[1:])
    if harness_split_present and not all(split_field_presence):
        raise ValueError("process provenance split fields must be present atomically")
    if all(split_field_presence):
        for section, field in PROCESS_PROVENANCE_SPLIT_FIELDS:
            if not isinstance(provenance[section][field], str):
                raise ValueError(f"process provenance split field {section}.{field} must be a string")
        script_hash = harness["benchmark_script_sha256"]
        if script_hash != "unknown" and not _is_full_sha256(script_hash):
            raise ValueError("process harness provenance has an invalid benchmark script SHA-256")
    if harness.get("contract_version") != PROCESS_CONTRACT_VERSION:
        raise ValueError("process harness provenance has the wrong contract")
    if provenance["build"].get("best_effort_provenance", {}).get("git_commit_embedding_verified") is not False:
        raise ValueError("process build provenance must not claim verified source embedding")
    _validate_process_source_scopes(provenance)

    build_binding_fields = ("family_authorities", "benchmark_helper_build", "binary_build_binding")
    build_binding_presence = [field in provenance for field in build_binding_fields]
    if any(build_binding_presence) and not all(build_binding_presence):
        raise ValueError("process helper build-binding fields must be present atomically")
    if all(build_binding_presence):
        authority_source = provenance["family_authorities"]
        if not isinstance(authority_source, dict):
            raise ValueError("process family authority source identity must be an object")
        if authority_source.get("path") != PROCESS_AUTHORITY_SOURCE_PATH:
            raise ValueError("process family authority source has the wrong path")
        if not isinstance(authority_source.get("object"), str):
            raise ValueError("process family authority source has an invalid object")
        if not _is_full_sha256(authority_source.get("sha256")):
            raise ValueError("process family authority source has an invalid SHA-256")
        authorities = authority_source.get("authorities")
        if not isinstance(authorities, dict) or set(authorities) != set(PROCESS_FAMILY_NAMES):
            raise ValueError("process family authority source has an invalid map")
        if any(authority not in ("legacy_local", "supervised") for authority in authorities.values()):
            raise ValueError("process family authority source has an invalid authority")
        helper_build = provenance["benchmark_helper_build"]
        if not isinstance(helper_build, dict):
            raise ValueError("process benchmark helper build provenance must be an object")
        if helper_build.get("best_effort_provenance", {}).get("git_commit_embedding_verified") is not False:
            raise ValueError("process benchmark helper build must not claim verified source embedding")
        binding = provenance["binary_build_binding"]
        if not isinstance(binding, dict) or not isinstance(binding.get("recorded_configuration_equivalent"), bool):
            raise ValueError("process binary build binding is invalid")
        if binding.get("relationship") not in (
            "same_cmake_cache",
            "equivalent_recorded_configuration",
            "inconsistent",
        ):
            raise ValueError("process binary build binding has an invalid relationship")
        if (
            binding.get("measured_ava_git_commit_embedding_verified") is not False
            or binding.get("benchmark_helper_git_commit_embedding_verified") is not False
        ):
            raise ValueError("process binary build binding must not claim verified source embedding")
    host = provenance["host"]
    for key in (
        "os",
        "kernel",
        "machine",
        "cpu",
        "cpu_count",
        "ram_bytes",
        "page_size_bytes",
        "python_version",
        "boot_id_sha256",
        "limits",
        "monotonic_clock_resolution_ns",
        "load_at_start",
        "load_at_end",
    ):
        if key not in host:
            raise ValueError(f"process host provenance lacks {key}")
    artifacts = document["artifacts"]
    for key in ("ava", "benchmark_helper", "benchmark_script", "memory_helper", "python"):
        validate_file_identity(artifacts.get(key), key)
        if not isinstance(artifacts[key].get("mode"), int):
            raise ValueError(f"process artifact {key} lacks mode")
    if _has_process_provenance_split(provenance):
        if _is_resolved_text(harness["repository"]):
            expected_script_path = str(
                Path(harness["repository"]) / "scripts" / "benchmark-backend.py"
            )
            if not _resolved_paths_equal(
                artifacts["benchmark_script"]["path"], expected_script_path
            ):
                raise ValueError("process benchmark script artifact is not from the harness repository")
        if _is_full_sha256(harness["benchmark_script_sha256"]):
            if artifacts["benchmark_script"]["sha256"] != harness["benchmark_script_sha256"]:
                raise ValueError("process benchmark script artifact does not match harness content")
    for key in ("fake_process_child", "fake_mcp_server", "fake_lsp_server", "fake_provider", "curl", "bash_direct_argv_executable"):
        if artifacts.get(key) is not None:
            validate_file_identity(artifacts[key], key)
            if not isinstance(artifacts[key].get("mode"), int):
                raise ValueError(f"process artifact {key} lacks mode")
    sample_plugin = artifacts.get("sample_plugin")
    if not isinstance(sample_plugin, dict) or "root" not in sample_plugin:
        raise ValueError("process artifacts lack sample plugin identity")
    for key in ("manifest", "entrypoint"):
        validate_file_identity(sample_plugin.get(key), f"sample_plugin.{key}")
        if not isinstance(sample_plugin[key].get("mode"), int):
            raise ValueError(f"process sample plugin {key} lacks mode")

    capabilities = document["capabilities"]
    if capabilities.get("helper_contract") != PROCESS_CONTRACT_VERSION:
        raise ValueError("process capabilities have the wrong helper contract")
    results = document["results"]
    if not isinstance(results, list) or [result.get("id") for result in results] != list(PROCESS_EXPECTED_RESULT_IDS):
        raise ValueError("process benchmark results are missing, duplicated, or out of order")
    for result in results:
        result_id = result["id"]
        spec = PROCESS_RESULT_SPECS[result_id]
        for key in ("family", "status", "primary_metric", "unit", "boundary", "repetitions", "observation_count", "samples", "statistics"):
            if key not in result:
                raise ValueError(f"process result {result_id} lacks {key}")
        if any(result[key] != spec[key] for key in ("family", "primary_metric", "unit", "boundary")):
            raise ValueError(f"process result {result_id} changed its metric contract")
        if not isinstance(result["samples"], list) or result["observation_count"] != len(result["samples"]):
            raise ValueError(f"process result {result_id} has an invalid sample count")
        serialized_result = json.dumps(result, sort_keys=True)
        if "://" in serialized_result or "CANARY_REDACTION" in serialized_result:
            raise ValueError(f"process result {result_id} contains prohibited content")
        if result["status"] == "unsupported":
            reason_code = result.get("reason_code")
            if reason_code not in PROCESS_REASON_TEXT or result.get("reason") != PROCESS_REASON_TEXT[reason_code]:
                raise ValueError(f"process result {result_id} has an invalid unsupported reason")
            if result["samples"] or result["statistics"] is not None or result["repetitions"] != 0:
                raise ValueError(f"unsupported process result {result_id} contains measurements")
            continue
        if result["status"] != "measured" or not result["samples"] or result["repetitions"] <= 0:
            raise ValueError(f"process result {result_id} is not a valid measurement")
        ordinals_by_run: dict[int, list[int]] = {}
        for sample in result["samples"]:
            _validate_safe_sample(sample)
            failed_checks = [
                name
                for name, value in sample["checks"].items()
                if isinstance(value, bool)
                and not value
                and not (
                    result_id.startswith("family_")
                    and name in ("protocol_compatible", "expected_response")
                )
            ]
            if failed_checks:
                raise ValueError(f"process result {result_id} contains a failed correctness check")
            ordinals_by_run.setdefault(sample["run"], []).append(sample["observation"])
        if sorted(ordinals_by_run) != list(range(1, result["repetitions"] + 1)):
            raise ValueError(f"process result {result_id} lost run correlation")
        if any(ordinals != list(range(1, len(ordinals) + 1)) for ordinals in ordinals_by_run.values()):
            raise ValueError(f"process result {result_id} lost observation correlation")
        expected_statistics = process_statistics(result["samples"])
        if not _statistics_equal(result["statistics"], expected_statistics):
            raise ValueError(f"process result {result_id} statistics do not match raw samples")
        if result_id.startswith("family_"):
            if result.get("authority") not in ("legacy_local", "supervised"):
                raise ValueError(f"family result {result_id} has an invalid authority")
            if result.get("authority") == "supervised" and not all(
                sample["checks"].get("supervisor_record_finished") is True and sample["checks"].get("supervisor_settlement_once") is True
                for sample in result["samples"]
            ):
                raise ValueError(f"family result {result_id} makes a false supervised claim")
            if result.get("metadata", {}).get("authority") != result.get("authority"):
                raise ValueError(f"family result {result_id} has inconsistent authority metadata")
            compatibility = result.get("compatibility_checks")
            if not isinstance(compatibility, dict) or set(compatibility.values()) - {True, False}:
                raise ValueError(f"family result {result_id} lacks compatibility checks")
            expected_compatibility = {
                name: all(sample["checks"].get(name) is True for sample in result["samples"])
                for name in ("protocol_compatible", "expected_response")
            }
            if compatibility != expected_compatibility:
                raise ValueError(f"family result {result_id} compatibility checks do not match raw samples")
    if not isinstance(document["checks"], list):
        raise ValueError("process benchmark checks must be an array")


def _artifact_hash(identity: Any) -> str | None:
    return identity.get("sha256") if isinstance(identity, dict) else None


def _resolved_artifact_identity(identity: Any) -> bool:
    return (
        isinstance(identity, dict)
        and _is_resolved_text(identity.get("path"))
        and _is_full_sha256(identity.get("sha256"))
        and isinstance(identity.get("size_bytes"), int)
        and isinstance(identity.get("mtime_ns"), int)
        and isinstance(identity.get("mode"), int)
    )


def _resolve_recorded_path(value: Any) -> Path | None:
    """Resolve a recorded path while rejecting loops on every supported Python.

    Missing synthetic paths remain comparable for offline document validation.
    Other resolution failures, including symlink loops, fail closed.
    """
    if not _is_resolved_text(value):
        return None
    try:
        return Path(value).resolve(strict=True)
    except FileNotFoundError:
        try:
            return Path(value).resolve(strict=False)
        except (OSError, RuntimeError, ValueError):
            return None
    except (OSError, RuntimeError, ValueError):
        return None


def _resolved_paths_equal(first: Any, second: Any) -> bool:
    first_path = _resolve_recorded_path(first)
    second_path = _resolve_recorded_path(second)
    if first_path is None or second_path is None:
        return False
    return first_path == second_path


def _path_is_within(path: Any, directory: Any) -> bool:
    resolved_path = _resolve_recorded_path(path)
    resolved_directory = _resolve_recorded_path(directory)
    if resolved_path is None or resolved_directory is None:
        return False
    try:
        resolved_path.relative_to(resolved_directory)
    except ValueError:
        return False
    return True


def _build_record_mismatches(
    document: dict[str, Any], cohort: str, provenance_key: str, artifact_key: str
) -> list[str]:
    provenance = document["provenance"]
    build = provenance.get(provenance_key)
    prefix = f"{cohort}.{provenance_key}"
    if not isinstance(build, dict):
        return [prefix]
    mismatches: list[str] = []
    measured_repository = provenance["measured_checkout"].get("repository")
    source_root = build.get("cmake_source_root")
    if not _is_resolved_text(source_root):
        mismatches.append(f"{prefix}.cmake_source_root")
    elif _is_resolved_text(measured_repository) and not _resolved_paths_equal(
        source_root, measured_repository
    ):
        mismatches.append(f"{prefix}.cmake_source_root")

    for field in ("generator", "cmake_version", "build_type"):
        if not _is_resolved_text(build.get(field)):
            mismatches.append(f"{prefix}.{field}")
    for field in ("cmake", "cmake_cache"):
        if not _resolved_artifact_identity(build.get(field)):
            mismatches.append(f"{prefix}.{field}")

    compiler = build.get("compiler")
    if not isinstance(compiler, dict):
        mismatches.append(f"{prefix}.compiler")
    else:
        for field in ("path", "id", "configured_version", "version_output"):
            if not _is_resolved_text(compiler.get(field)):
                mismatches.append(f"{prefix}.compiler.{field}")
        if not _resolved_artifact_identity(compiler.get("artifact")):
            mismatches.append(f"{prefix}.compiler.artifact")
        if not isinstance(compiler.get("flags"), dict):
            mismatches.append(f"{prefix}.compiler.flags")

    recipe = build.get("recipe")
    if not isinstance(recipe, dict):
        mismatches.append(f"{prefix}.recipe")
    else:
        recipe_consistent = (
            recipe.get("generator") == build.get("generator")
            and recipe.get("cmake_version") == build.get("cmake_version")
            and recipe.get("build_type") == build.get("build_type")
            and isinstance(recipe.get("cmake_flags"), dict)
            and isinstance(recipe.get("cxx_flags"), dict)
            and isinstance(compiler, dict)
            and compiler.get("flags") == recipe.get("cxx_flags")
        )
        if not recipe_consistent:
            mismatches.append(f"{prefix}.recipe_internal_consistency")

    features = build.get("features")
    if not isinstance(features, dict):
        mismatches.append(f"{prefix}.features")
    else:
        boolean_features = ("sanitizers", "tsan", "debug", "libcwd", "process_supervisor", "process_fixture")
        if any(not isinstance(features.get(name), bool) for name in boolean_features) or not _is_resolved_text(
            features.get("platform_backend")
        ):
            mismatches.append(f"{prefix}.features")
        authorities = features.get("family_authorities")
        if (
            not isinstance(authorities, dict)
            or set(authorities) != set(PROCESS_FAMILY_NAMES)
            or any(authority not in ("legacy_local", "supervised") for authority in authorities.values())
        ):
            mismatches.append(f"{prefix}.features.family_authorities")

    best_effort = build.get("best_effort_provenance")
    if not isinstance(best_effort, dict):
        mismatches.append(f"{prefix}.best_effort_provenance")
    else:
        if best_effort.get("cmake_source_root_matches_recorded_source") is not True:
            mismatches.append(f"{prefix}.best_effort_provenance.cmake_source_root_matches_recorded_source")
        if best_effort.get("binary_is_within_cmake_build_tree") is not True:
            mismatches.append(f"{prefix}.best_effort_provenance.binary_is_within_cmake_build_tree")
        if best_effort.get("git_commit_embedding_verified") is not False:
            mismatches.append(f"{prefix}.best_effort_provenance.git_commit_embedding_verified")

    cache = build.get("cmake_cache")
    binary = document["artifacts"].get(artifact_key)
    if _resolved_artifact_identity(cache) and _resolved_artifact_identity(binary):
        if not _path_is_within(binary["path"], str(Path(cache["path"]).parent)):
            mismatch = f"{prefix}.binary_path_within_cmake_build_tree"
            if mismatch not in mismatches:
                mismatches.append(mismatch)
    return mismatches


def _helper_build_binding_mismatches(document: dict[str, Any], cohort: str) -> list[str]:
    provenance = document["provenance"]
    helper_build = provenance.get("benchmark_helper_build")
    binding = provenance.get("binary_build_binding")
    if not isinstance(helper_build, dict):
        return [f"{cohort}.benchmark_helper_build"]
    mismatches = _build_record_mismatches(document, cohort, "benchmark_helper_build", "benchmark_helper")
    if not isinstance(binding, dict):
        mismatches.append(f"{cohort}.binary_build_binding")
        return mismatches
    equivalent = _build_configuration_identity(provenance["build"]) == _build_configuration_identity(helper_build)
    expected_binding = process_binary_build_binding(provenance["build"], helper_build)
    if not equivalent or binding.get("recorded_configuration_equivalent") is not True:
        mismatches.append(f"{cohort}.binary_build_binding.recorded_configuration_equivalent")
    if binding.get("relationship") != expected_binding["relationship"]:
        mismatches.append(f"{cohort}.binary_build_binding.relationship")
    return mismatches


def _comparison_git(repository: Path, *arguments: str) -> tuple[int, str] | None:
    try:
        completed = subprocess.run(
            _read_only_git_command(repository, *arguments),
            env=_read_only_git_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=15,
            check=False,
        )
    except (OSError, UnicodeError, subprocess.TimeoutExpired):
        return None
    return completed.returncode, completed.stdout.strip()


def _comparison_worktree(repository_value: Any) -> Path | None:
    if not _is_resolved_text(repository_value):
        return None
    try:
        repository = Path(repository_value).resolve()
        if not repository.is_dir():
            return None
    except (OSError, RuntimeError, ValueError):
        return None
    identity = _comparison_git(
        repository, "rev-parse", "--show-toplevel", "--is-inside-work-tree"
    )
    if identity is None or identity[0] != 0:
        return None
    lines = identity[1].splitlines()
    if len(lines) != 2 or lines[1] != "true":
        return None
    try:
        if Path(lines[0]).resolve() != repository:
            return None
    except (OSError, RuntimeError, ValueError):
        return None
    return repository


def _comparison_resolved_revision(repository: Path, revision: Any) -> tuple[str, str] | None:
    if not _is_full_git_object_id(revision):
        return None
    resolved_commit = _comparison_git(
        repository,
        "rev-parse",
        "--verify",
        "--end-of-options",
        f"{revision}^{{commit}}",
    )
    if (
        resolved_commit is None
        or resolved_commit[0] != 0
        or resolved_commit[1] != revision
    ):
        return None
    resolved_tree = _comparison_git(
        repository,
        "rev-parse",
        "--verify",
        "--end-of-options",
        f"{revision}^{{tree}}",
    )
    if (
        resolved_tree is None
        or resolved_tree[0] != 0
        or not _is_full_git_object_id(resolved_tree[1])
    ):
        return None
    return resolved_commit[1], resolved_tree[1]


def _exact_json_identity_equal(first: Any, second: Any) -> bool:
    try:
        return json.dumps(
            first, ensure_ascii=False, separators=(",", ":"), sort_keys=True
        ) == json.dumps(
            second, ensure_ascii=False, separators=(",", ":"), sort_keys=True
        )
    except (TypeError, ValueError):
        return False


def _live_comparison_provenance_mismatches(
    document: dict[str, Any], cohort: str
) -> list[str]:
    provenance = document["provenance"]
    mismatches: list[str] = []

    def mismatch(path: str) -> None:
        qualified = f"{cohort}.{path}"
        if qualified not in mismatches:
            mismatches.append(qualified)

    measured = provenance["measured_checkout"]
    repository = _comparison_worktree(measured.get("repository"))
    if repository is None:
        mismatch("measured_checkout.repository_worktree")
        return mismatches

    measured_revision = _comparison_resolved_revision(repository, measured.get("commit"))
    if measured_revision is None:
        mismatch("measured_checkout.commit_resolution")
    elif measured_revision[1] != measured.get("tree"):
        mismatch("measured_checkout.tree_matches_commit")

    current_head = _comparison_git(
        repository, "rev-parse", "--verify", "HEAD^{commit}"
    )
    current_revision = (
        _comparison_resolved_revision(repository, current_head[1])
        if current_head is not None and current_head[0] == 0
        else None
    )
    if current_revision is None or current_revision[0] != measured.get("commit"):
        mismatch("measured_checkout.current_commit")
    if current_revision is None or current_revision[1] != measured.get("tree"):
        mismatch("measured_checkout.current_tree")
    current_status = _comparison_git(
        repository,
        "status",
        "--porcelain",
        "--untracked-files=normal",
        "--ignore-submodules=none",
    )
    if current_status is None or current_status[0] != 0 or current_status[1]:
        mismatch("measured_checkout.current_dirty")

    if measured_revision is not None:
        try:
            regenerated_families, regenerated_shared = process_source_scope_identities(
                repository, measured_revision[0]
            )
        except Exception:
            mismatch("source_ownership_scopes.regeneration")
        else:
            recorded_families = provenance["family_sources"]
            for family in PROCESS_FAMILY_NAMES:
                if not _exact_json_identity_equal(
                    regenerated_families[family], recorded_families[family]
                ):
                    mismatch(f"family_sources.{family}")
            if not _exact_json_identity_equal(
                regenerated_shared, provenance["shared_process_source"]
            ):
                mismatch("shared_process_source")

        try:
            regenerated_authority = family_authority_source_identity(
                repository, measured_revision[0]
            )
        except Exception:
            mismatch("family_authorities.source_identity")
        else:
            if not _exact_json_identity_equal(
                regenerated_authority, provenance["family_authorities"]
            ):
                mismatch("family_authorities.source_identity")

    runtime_reference = provenance["runtime_reference"]
    runtime_revision = _comparison_resolved_revision(
        repository, runtime_reference.get("commit")
    )
    if runtime_revision is None:
        mismatch("runtime_reference.commit_resolution")
    elif runtime_revision[1] != runtime_reference.get("tree"):
        mismatch("runtime_reference.tree_matches_commit")

    if runtime_revision is not None:
        compared = _comparison_git(
            repository,
            "diff",
            "--quiet",
            "--no-ext-diff",
            "--no-textconv",
            "--ignore-submodules=none",
            runtime_revision[0],
            "--",
            *PROCESS_PRODUCTION_SOURCE_PATHS,
        )
        if compared is None or compared[0] != 0:
            mismatch("runtime_reference.current_production_path_equality")
    production_status = _comparison_git(
        repository,
        "status",
        "--porcelain",
        "--untracked-files=normal",
        "--ignore-submodules=none",
        "--",
        *PROCESS_PRODUCTION_SOURCE_PATHS,
    )
    if production_status is None or production_status[0] != 0 or production_status[1]:
        mismatch("runtime_reference.current_production_paths_dirty")

    harness = provenance["harness"]
    harness_repository = _comparison_worktree(harness.get("repository"))
    if harness_repository is None:
        mismatch("harness.repository_worktree")
        return mismatches

    harness_revision = _comparison_resolved_revision(
        harness_repository, harness.get("commit")
    )
    if harness_revision is None:
        mismatch("harness.commit_resolution")
    elif harness_revision[1] != harness.get("tree"):
        mismatch("harness.tree_matches_commit")

    harness_head = _comparison_git(
        harness_repository, "rev-parse", "--verify", "HEAD^{commit}"
    )
    current_harness_revision = (
        _comparison_resolved_revision(harness_repository, harness_head[1])
        if harness_head is not None and harness_head[0] == 0
        else None
    )
    if current_harness_revision is None or current_harness_revision[0] != harness.get("commit"):
        mismatch("harness.current_commit")
    if current_harness_revision is None or current_harness_revision[1] != harness.get("tree"):
        mismatch("harness.current_tree")
    harness_status = _comparison_git(
        harness_repository,
        "status",
        "--porcelain",
        "--untracked-files=normal",
        "--ignore-submodules=none",
    )
    if harness_status is None or harness_status[0] != 0 or harness_status[1]:
        mismatch("harness.current_dirty")

    harness_script = harness_repository / "scripts" / "benchmark-backend.py"
    try:
        current_script_hash = sha256_file(harness_script)
    except Exception:
        mismatch("harness.current_benchmark_script_sha256")
    else:
        if current_script_hash != harness.get("benchmark_script_sha256"):
            mismatch("harness.current_benchmark_script_sha256")
    return mismatches


def _comparison_provenance_mismatches(document: dict[str, Any], cohort: str) -> list[str]:
    provenance = document["provenance"]
    mismatches: list[str] = []

    def mismatch(path: str) -> None:
        mismatches.append(f"{cohort}.{path}")

    measured = provenance["measured_checkout"]
    if not _is_resolved_text(measured.get("repository")):
        mismatch("measured_checkout.repository")
    for field in ("commit", "tree"):
        if not _is_full_git_object_id(measured.get(field)):
            mismatch(f"measured_checkout.{field}")
    if measured.get("dirty") is not False:
        mismatch("measured_checkout.dirty")

    runtime_reference = provenance["runtime_reference"]
    for field in ("commit", "tree"):
        if not _is_full_git_object_id(runtime_reference.get(field)):
            mismatch(f"runtime_reference.{field}")
    if runtime_reference.get("exact_production_path_equality") is not True:
        mismatch("runtime_reference.exact_production_path_equality")
    if runtime_reference.get("measured_production_paths_dirty") is not False:
        mismatch("runtime_reference.measured_production_paths_dirty")

    if not _has_complete_process_source_scopes(provenance):
        mismatch("source_ownership_scopes")

    authority_source = provenance.get("family_authorities")
    if not isinstance(authority_source, dict):
        mismatch("family_authorities")
    else:
        if authority_source.get("path") != PROCESS_AUTHORITY_SOURCE_PATH:
            mismatch("family_authorities.path")
        if not _is_full_git_object_id(authority_source.get("object")):
            mismatch("family_authorities.object")
        if not _is_full_sha256(authority_source.get("sha256")):
            mismatch("family_authorities.sha256")
        authorities = authority_source.get("authorities")
        if (
            not isinstance(authorities, dict)
            or set(authorities) != set(PROCESS_FAMILY_NAMES)
            or any(authority not in ("legacy_local", "supervised") for authority in authorities.values())
        ):
            mismatch("family_authorities.authorities")

    mismatches.extend(_build_record_mismatches(document, cohort, "build", "ava"))
    mismatches.extend(_helper_build_binding_mismatches(document, cohort))

    harness = provenance["harness"]
    if not _is_resolved_text(harness.get("repository")):
        mismatch("harness.repository")
    for field in ("commit", "tree"):
        if not _is_full_git_object_id(harness.get(field)):
            mismatch(f"harness.{field}")
    if not _is_full_sha256(harness.get("benchmark_script_sha256")):
        mismatch("harness.benchmark_script_sha256")
    if harness.get("dirty") is not False:
        mismatch("harness.dirty")

    script_artifact = document["artifacts"].get("benchmark_script")
    if isinstance(script_artifact, dict):
        if _is_resolved_text(harness.get("repository")):
            expected_path = str(
                Path(harness["repository"]) / "scripts" / "benchmark-backend.py"
            )
            if not _resolved_paths_equal(script_artifact.get("path"), expected_path):
                mismatch("harness.benchmark_script_artifact_path")
        if script_artifact.get("sha256") != harness.get("benchmark_script_sha256"):
            mismatch("harness.benchmark_script_artifact_sha256")
    else:
        mismatch("harness.benchmark_script_artifact")
    return mismatches


def _comparison_identity(document: dict[str, Any]) -> dict[str, Any]:
    provenance = document["provenance"]
    host = provenance["host"]
    build = provenance["build"]
    features = dict(build["features"])
    for migration_dimension in ("family_authorities", "process_supervisor", "process_fixture", "platform_backend"):
        features.pop(migration_dimension, None)
    artifacts = document["artifacts"]
    plugin = artifacts["sample_plugin"]
    fixture_hashes = {
        "benchmark_script": _artifact_hash(artifacts["benchmark_script"]),
        "memory_helper": _artifact_hash(artifacts["memory_helper"]),
        "python": _artifact_hash(artifacts["python"]),
        "fake_process_child": _artifact_hash(artifacts.get("fake_process_child")),
        "fake_mcp_server": _artifact_hash(artifacts.get("fake_mcp_server")),
        "fake_lsp_server": _artifact_hash(artifacts.get("fake_lsp_server")),
        "fake_provider": _artifact_hash(artifacts.get("fake_provider")),
        "curl": _artifact_hash(artifacts.get("curl")),
        "bash_direct_argv_executable": _artifact_hash(artifacts.get("bash_direct_argv_executable")),
        "sample_plugin_manifest": _artifact_hash(plugin["manifest"]),
        "sample_plugin_entrypoint": _artifact_hash(plugin["entrypoint"]),
    }
    boundaries = {
        result["id"]: {"unit": result["unit"], "primary_metric": result["primary_metric"], "boundary": result["boundary"]}
        for result in document["results"]
    }
    harness = provenance["harness"]
    return {
        "contract": document["contract_version"],
        "harness": {
            "commit": harness["commit"],
            "tree": harness["tree"],
            "benchmark_script_sha256": harness["benchmark_script_sha256"],
        },
        "host": {
            key: host.get(key)
            for key in ("os", "kernel", "machine", "cpu", "cpu_count", "ram_bytes", "page_size_bytes", "boot_id_sha256")
        },
        "build_recipe": build["recipe"],
        "compiler": {
            "path": build["compiler"]["path"],
            "sha256": _artifact_hash(build["compiler"].get("artifact")),
            "id": build["compiler"]["id"],
            "configured_version": build["compiler"]["configured_version"],
            "version_output": build["compiler"]["version_output"],
            "flags": build["compiler"]["flags"],
        },
        "features": features,
        "fixture_hashes": fixture_hashes,
        "boundaries": boundaries,
    }


def comparison_unsupported(reason_code: str, reason: str, mismatches: Sequence[str] = ()) -> dict[str, Any]:
    return {
        "schema_version": COMPARISON_SCHEMA_VERSION,
        "contract_version": PROCESS_CONTRACT_VERSION,
        "status": "unsupported",
        "reason_code": reason_code,
        "reason": reason,
        "mismatches": list(mismatches),
        "comparisons": [],
        "repeatable_claim": False,
        "confirmation": "reversed_order_required",
    }


def _family_authority_mismatches(document: dict[str, Any], cohort: str) -> list[str]:
    provenance = document["provenance"]
    source_authorities = provenance.get("family_authorities", {}).get("authorities")
    build_authorities = provenance["build"].get("features", {}).get("family_authorities")
    helper_authorities = provenance.get("benchmark_helper_build", {}).get("features", {}).get("family_authorities")
    if not isinstance(source_authorities, dict) or set(source_authorities) != set(PROCESS_FAMILY_NAMES):
        return [f"{cohort}.family_authorities.source_map"]
    mismatches: list[str] = []
    results = {result["id"]: result for result in document["results"]}
    for family in PROCESS_FAMILY_NAMES:
        source_authority = source_authorities.get(family)
        if source_authority not in ("legacy_local", "supervised"):
            mismatches.append(f"{cohort}.family_authorities.{family}.source")
            continue
        if not isinstance(build_authorities, dict) or build_authorities.get(family) != source_authority:
            mismatches.append(f"{cohort}.family_authorities.{family}.build")
        if not isinstance(helper_authorities, dict) or helper_authorities.get(family) != source_authority:
            mismatches.append(f"{cohort}.family_authorities.{family}.helper_build")
        if document["capabilities"].get(f"{family}_authority") != source_authority:
            mismatches.append(f"{cohort}.family_authorities.{family}.capability")
        result = results[f"family_{family}_lifecycle"]
        if result["status"] == "measured" and result.get("authority") != source_authority:
            mismatches.append(f"{cohort}.family_authorities.{family}.result")
    return mismatches


def _source_scope_signature(identity: dict[str, Any]) -> str:
    return json.dumps(identity, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def _family_scope_signature(document: dict[str, Any], family: str) -> str:
    return _source_scope_signature(document["provenance"]["family_sources"][family])


def _shared_process_scope_signature(document: dict[str, Any]) -> str:
    return _source_scope_signature(document["provenance"]["shared_process_source"])


def compare_process_documents(before: dict[str, Any], after: dict[str, Any]) -> dict[str, Any]:
    def provenance_required(mismatches: Sequence[str]) -> dict[str, Any]:
        return comparison_unsupported(
            "comparison_provenance_required",
            "Each cohort requires clean, resolved, internally consistent comparison provenance.",
            mismatches,
        )

    validation_mismatches: list[str] = []
    for cohort, document in (("before", before), ("after", after)):
        try:
            validate_process_document(document)
        except Exception:
            validation_mismatches.append(f"{cohort}.document_validation")
    if validation_mismatches:
        return provenance_required(validation_mismatches)

    split_mismatches: list[str] = []
    for cohort, document in (("before", before), ("after", after)):
        try:
            has_split = _has_process_provenance_split(document["provenance"])
        except Exception:
            return provenance_required(
                [*split_mismatches, f"{cohort}.static_provenance_evaluation"]
            )
        if not has_split:
            split_mismatches.append(f"{cohort}.provenance_split")
    if split_mismatches:
        return comparison_unsupported(
            "provenance_split_required",
            "Comparison requires v3 provenance with independent measured-source and harness identities.",
            split_mismatches,
        )

    provenance_mismatches: list[str] = []
    for cohort, document in (("before", before), ("after", after)):
        try:
            provenance_mismatches.extend(
                _comparison_provenance_mismatches(document, cohort)
            )
        except Exception:
            provenance_mismatches.append(f"{cohort}.static_provenance_evaluation")
            return provenance_required(provenance_mismatches)
    if not provenance_mismatches:
        for cohort, document in (("before", before), ("after", after)):
            try:
                provenance_mismatches.extend(
                    _live_comparison_provenance_mismatches(document, cohort)
                )
            except Exception:
                provenance_mismatches.append(f"{cohort}.live_provenance_evaluation")
                return provenance_required(provenance_mismatches)
    if provenance_mismatches:
        return provenance_required(provenance_mismatches)

    try:
        before_identity = _comparison_identity(before)
        after_identity = _comparison_identity(after)
        mismatch_names = [
            name
            for name in before_identity
            if before_identity[name] != after_identity[name]
        ]
    except Exception:
        return provenance_required(["identity_evaluation"])
    if mismatch_names:
        return comparison_unsupported(
            "incomparable_cohorts",
            "The cohorts differ in required host, boot, build, compiler, feature, fixture, boundary, or contract identity.",
            mismatch_names,
        )

    try:
        authority_mismatches = [
            *(_family_authority_mismatches(before, "before")),
            *(_family_authority_mismatches(after, "after")),
        ]
    except Exception:
        return provenance_required(["identity_evaluation"])
    if authority_mismatches:
        return comparison_unsupported(
            "authority_attribution_mismatch",
            "Family result authority must match the source-owned capability map in each cohort.",
            authority_mismatches,
        )

    try:
        before_authorities = before["provenance"]["family_authorities"]["authorities"]
        after_authorities = after["provenance"]["family_authorities"]["authorities"]
        changed_authorities = [
            family
            for family in PROCESS_FAMILY_NAMES
            if before_authorities[family] != after_authorities[family]
        ]
        unexpected_transitions = [
            family
            for family in changed_authorities
            if (before_authorities[family], after_authorities[family])
            != ("legacy_local", "supervised")
        ]
    except Exception:
        return provenance_required(["identity_evaluation"])
    if unexpected_transitions:
        return comparison_unsupported(
            "unexpected_authority_transition",
            "The pair contains an authority change other than legacy_local to supervised.",
            [f"family_authorities.{family}" for family in unexpected_transitions],
        )
    if not changed_authorities:
        return comparison_unsupported(
            "authority_transition_required",
            "No family has a validated legacy_local to supervised authority transition.",
        )
    if len(changed_authorities) != 1:
        return comparison_unsupported(
            "single_authority_transition_required",
            "A comparison pair must change exactly one family from legacy_local to supervised.",
            [f"family_authorities.{family}" for family in changed_authorities],
        )
    transitioned_family = changed_authorities[0]
    try:
        changed_family_scopes = [
            family
            for family in PROCESS_FAMILY_NAMES
            if _family_scope_signature(before, family)
            != _family_scope_signature(after, family)
        ]
        shared_process_scope_changed = (
            _shared_process_scope_signature(before)
            != _shared_process_scope_signature(after)
        )
        source_attribution = {
            "transitioned_family": transitioned_family,
            "changed_family_scopes": changed_family_scopes,
            "shared_process_scope_changed": shared_process_scope_changed,
        }
    except Exception:
        return provenance_required(["identity_evaluation"])
    if changed_family_scopes != [transitioned_family]:
        attribution_mismatches = [
            f"family_sources.{family}"
            for family in PROCESS_FAMILY_NAMES
            if (family == transitioned_family) != (family in changed_family_scopes)
        ]
        document = comparison_unsupported(
            "family_source_attribution_mismatch",
            "Complete family-owned scope changes must be attributable only to the one authority transition.",
            attribution_mismatches,
        )
        document["source_attribution"] = source_attribution
        return document
    before_results = {result["id"]: result for result in before["results"]}
    after_results = {result["id"]: result for result in after["results"]}
    comparisons: list[dict[str, Any]] = []
    transitioned = 0
    for result_id in PROCESS_EXPECTED_RESULT_IDS[-5:]:
        before_result = before_results[result_id]
        after_result = after_results[result_id]
        if before_result["status"] != "measured" or after_result["status"] != "measured":
            comparisons.append(
                {
                    "id": result_id,
                    "status": "unsupported",
                    "reason_code": "raw_samples_required",
                    "reason": "Both family cohorts must contain measured raw samples.",
                }
            )
            continue
        if before_result.get("authority") != "legacy_local" or after_result.get("authority") != "supervised":
            comparisons.append(
                {
                    "id": result_id,
                    "status": "unsupported",
                    "reason_code": "authority_transition_required",
                    "reason": "Family comparison requires a legacy_local to supervised authority transition.",
                }
            )
            continue
        required_compatibility = {"protocol_compatible": True, "expected_response": True}
        if (
            before_result.get("compatibility_checks") != required_compatibility
            or after_result.get("compatibility_checks") != required_compatibility
        ):
            comparisons.append(
                {
                    "id": result_id,
                    "status": "unsupported",
                    "reason_code": "compatibility_mismatch",
                    "reason": "Both family cohorts require all compatibility checks to be present and true.",
                }
            )
            continue
        before_stats = process_statistics(before_result["samples"])
        after_stats = process_statistics(after_result["samples"])
        before_median = before_stats["primary"]["median"]
        after_median = after_stats["primary"]["median"]
        delta = after_median - before_median
        percent = (delta / before_median * 100.0) if before_median != 0 else None
        trigger = delta > 100_000 and percent is not None and percent > 20.0
        comparisons.append(
            {
                "id": result_id,
                "status": "measured",
                "unit": before_result["unit"],
                "boundary": before_result["boundary"],
                "before_statistics": before_stats,
                "after_statistics": after_stats,
                "median_delta": delta,
                "median_delta_percent": percent,
                "investigation_trigger": trigger,
                "gating": False,
                "faster_required": False,
            }
        )
        transitioned += 1
    if transitioned == 0:
        document = comparison_unsupported(
            "authority_transition_required",
            "No family has a validated legacy_local to supervised authority transition.",
        )
        document["comparisons"] = comparisons
        document["source_attribution"] = source_attribution
        return document
    return {
        "schema_version": COMPARISON_SCHEMA_VERSION,
        "contract_version": PROCESS_CONTRACT_VERSION,
        "status": "measured",
        "comparisons": comparisons,
        "source_attribution": source_attribution,
        "investigation_thresholds": {
            "latency": {"relative_percent": 20.0, "absolute_ns": 100_000.0},
            "rss": {"relative_percent": 20.0, "absolute_kib": 4096.0},
            "monitor_cpu": {"relative_percent": 25.0, "absolute_cpu_ns_per_wall_second": 5_000_000.0},
        },
        "gating": False,
        "faster_required": False,
        "repeatable_claim": False,
        "confirmation": "reversed_order_required",
    }


def validate_comparison_document(document: dict[str, Any]) -> None:
    if document.get("schema_version") != COMPARISON_SCHEMA_VERSION or document.get("contract_version") != PROCESS_CONTRACT_VERSION:
        raise ValueError("unexpected process comparison schema or contract")
    if document.get("status") not in ("measured", "unsupported") or not isinstance(document.get("comparisons"), list):
        raise ValueError("invalid process comparison status")
    if document.get("repeatable_claim") is not False or document.get("confirmation") != "reversed_order_required":
        raise ValueError("a single paired comparison must require reversed-order confirmation")
    if document["status"] == "unsupported" and not isinstance(document.get("reason_code"), str):
        raise ValueError("unsupported process comparison lacks a reason code")
    source_attribution = document.get("source_attribution")
    if document["status"] == "measured" and source_attribution is None:
        raise ValueError("measured process comparison lacks source attribution")
    if source_attribution is not None:
        changed_scopes = (
            source_attribution.get("changed_family_scopes")
            if isinstance(source_attribution, dict)
            else None
        )
        invalid_attribution = (
            not isinstance(source_attribution, dict)
            or set(source_attribution)
            != {"transitioned_family", "changed_family_scopes", "shared_process_scope_changed"}
            or source_attribution.get("transitioned_family") not in PROCESS_FAMILY_NAMES
            or not isinstance(changed_scopes, list)
            or any(not isinstance(family, str) for family in changed_scopes)
            or len(set(changed_scopes)) != len(changed_scopes)
            or any(family not in PROCESS_FAMILY_NAMES for family in changed_scopes)
            or not isinstance(source_attribution.get("shared_process_scope_changed"), bool)
        )
        if document["status"] == "measured" and changed_scopes != [
            source_attribution.get("transitioned_family")
        ]:
            invalid_attribution = True
        if invalid_attribution:
            raise ValueError("process comparison has invalid source attribution")
    for comparison in document["comparisons"]:
        if comparison.get("status") == "measured":
            if comparison.get("gating") is not False or comparison.get("faster_required") is not False:
                raise ValueError("process comparison must remain non-gating")
            for key in ("before_statistics", "after_statistics"):
                if not isinstance(comparison.get(key), dict):
                    raise ValueError("measured process comparison lacks recomputed statistics")
        elif comparison.get("status") != "unsupported":
            raise ValueError("process comparison has an invalid result status")


def process_markdown_report(document: dict[str, Any]) -> str:
    lines = [
        "# AVA Process-Supervision Benchmark Report",
        "",
        f"Generated: `{document['generated_at_utc']}`  ",
        f"Suite: `{document['suite']}`  ",
        f"Contract: `{document['contract_version']}`",
        "",
        "> Machine/build-specific evidence only. Performance triggers are non-gating and require reversed-order confirmation.",
        "",
        "| Measurement | Status | Median | p95 | Maximum |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for result in document["results"]:
        statistics_value = result["statistics"]
        if statistics_value:
            primary = statistics_value["primary"]
            values = tuple(f"{primary[key]:.3f} {result['unit']}" for key in ("median", "p95", "maximum"))
        else:
            values = ("—", "—", "—")
        lines.append(f"| `{result['id']}` | {result['status']} | {values[0]} | {values[1]} | {values[2]} |")
    lines.extend(["", "## Unsupported cases", ""])
    unsupported_results = [result for result in document["results"] if result["status"] == "unsupported"]
    if unsupported_results:
        for result in unsupported_results:
            lines.append(f"- `{result['id']}`: `{result['reason_code']}` — {result['reason']}")
    else:
        lines.append("- None.")
    lines.extend(["", "## Catastrophic smoke checks", ""])
    if document["checks"]:
        for check in document["checks"]:
            lines.append(f"- {'PASS' if check['passed'] else 'FAIL'} `{check['name']}` ({check['ceiling']})")
    else:
        lines.append("- Not applied to this opt-in baseline suite.")
    return "\n".join(lines) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if argv is None:
        args._exact_command = [sys.executable, *sys.argv]
    else:
        args._exact_command = [sys.executable, str(Path(__file__).resolve()), *argv]
    try:
        validate_arguments(args)
        process_suite = args.suite.startswith("process-")
        document = execute_process(args) if process_suite else execute(args)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if args.report is not None:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            report_text = process_markdown_report(document) if process_suite else markdown_report(document)
            args.report.write_text(report_text, encoding="utf-8")
        comparison_failed = False
        if args.compare_to is not None and args.comparison_output is not None:
            before = json.loads(args.compare_to.read_text(encoding="utf-8"))
            comparison = compare_process_documents(before, document)
            comparison["provenance"] = {
                "before_commit": before["provenance"]["measured_checkout"]["commit"],
                "after_commit": document["provenance"]["measured_checkout"]["commit"],
                "run_order_confirmation": "reversed_order_required",
            }
            comparison["artifacts"] = {
                "before_document": file_identity_v3(args.compare_to),
                "after_document": file_identity_v3(args.output),
            }
            validate_comparison_document(comparison)
            args.comparison_output.parent.mkdir(parents=True, exist_ok=True)
            args.comparison_output.write_text(json.dumps(comparison, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            comparison_failed = comparison["status"] == "unsupported"
            print(f"Comparison: {args.comparison_output}")
        print(f"JSON: {args.output}")
        if args.report is not None:
            print(f"Markdown: {args.report}")
        failed = [check for check in document["checks"] if not check["passed"]]
        if failed:
            for check in failed:
                print(f"smoke invariant failed: {check['name']} ({check['ceiling']})", file=sys.stderr)
            return 1
        if comparison_failed:
            print("comparison unsupported: cohorts are not valid legacy_local to supervised evidence", file=sys.stderr)
            return 1
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
        print(f"benchmark-backend.py: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    if len(sys.argv) > 3 and sys.argv[1] == "--owned-process":
        sys.exit(_owned_process_main(int(sys.argv[2]), sys.argv[3:]))
    sys.exit(main())
