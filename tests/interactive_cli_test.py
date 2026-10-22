#!/usr/bin/env python3
"""Credential-free CLI coverage for interactive TUI startup gates."""

from __future__ import annotations

import argparse
import os
from cli_test_support import isolated_environment, owned_popen, process_group_is_live, require, run_owned
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch
import cli_test_support

from process_gate import create_process_gate_pair

from timeout_support import test_timeout


TTY_REQUIRED = b"interactive TUI requires a terminal on both stdin and stdout"
PRINT_OR_RPC = b"--print or --rpc"


def run(
    ava: pathlib.Path,
    arguments: list[str],
    environment: dict[str, str],
    workspace: pathlib.Path,
    input_bytes: bytes = b"",
) -> subprocess.CompletedProcess[bytes]:
    return run_owned([str(ava), *arguments],
    input=input_bytes,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    cwd=workspace,
    env=environment,
    timeout=test_timeout(15),
    check=False,)


def assert_no_runtime_state(state_root: pathlib.Path, label: str) -> None:
    ava_state = state_root / "ava"
    for name in ("sessions", "diagnostics"):
        require(not (ava_state / name).exists(), f"{label} created {name} runtime state")


def assert_no_sessions(state_root: pathlib.Path, label: str) -> None:
    sessions = state_root / "ava" / "sessions"
    found = sorted(path for path in sessions.rglob("*") if path.is_file()) if sessions.exists() else []
    require(not found, f"{label} created session state: {found}")


def run_cases(ava: pathlib.Path, workspace: pathlib.Path, environment: dict[str, str]) -> int:
    state_root = pathlib.Path(environment["XDG_STATE_HOME"])

    help_result = run(ava, ["--help"], environment, workspace)
    require(help_result.returncode == 0, f"--help failed: rc={help_result.returncode} stderr={help_result.stderr!r}")
    require(b"--line-shell" not in help_result.stdout, f"--help still advertises --line-shell: {help_result.stdout!r}")

    unknown = run(ava, ["--line-shell"], environment, workspace)
    require(
        unknown.returncode == 2 and unknown.stdout == b"" and b"unknown argument: --line-shell" in unknown.stderr,
        f"--line-shell was not rejected as an unknown flag: rc={unknown.returncode} stdout={unknown.stdout!r} stderr={unknown.stderr!r}",
    )
    assert_no_runtime_state(state_root, "unknown --line-shell")

    acp_unknown = run(ava, ["--acp", "--line-shell"], environment, workspace)
    require(
        acp_unknown.returncode == 2
        and acp_unknown.stdout == b""
        and b"use either --line-shell or --acp" not in acp_unknown.stderr
        and b"--acp is a standalone mode" in acp_unknown.stderr,
        f"--acp --line-shell did not use ordinary ACP extra-flag rejection: rc={acp_unknown.returncode} stdout={acp_unknown.stdout!r} stderr={acp_unknown.stderr!r}",
    )
    assert_no_runtime_state(state_root, "ACP extra --line-shell")

    both_non_tty = run(ava, ["--offline"], environment, workspace, b"/exit\n")
    require(
        both_non_tty.returncode == 2
        and both_non_tty.stdout == b""
        and TTY_REQUIRED in both_non_tty.stderr
        and PRINT_OR_RPC in both_non_tty.stderr,
        f"piped stdin and stdout did not reject interactive startup: rc={both_non_tty.returncode} stdout={both_non_tty.stdout!r} stderr={both_non_tty.stderr!r}",
    )
    assert_no_runtime_state(state_root, "both-non-TTY interactive startup")

    print_smoke = run(ava, ["--print", "--no-session", "--offline", "hi"], environment, workspace)
    require(
        print_smoke.returncode == 1
        and print_smoke.stdout == b""
        and b"offline mode is enabled; provider model calls are disabled" in print_smoke.stderr
        and TTY_REQUIRED not in print_smoke.stderr,
        f"explicit --print --no-session did not reach the offline provider gate: rc={print_smoke.returncode} stdout={print_smoke.stdout!r} stderr={print_smoke.stderr!r}",
    )
    assert_no_sessions(state_root, "explicit --print --no-session")

    positional = run(ava, ["--offline", "--no-session", "hello"], environment, workspace)
    require(
        positional.returncode == 1
        and positional.stdout == b""
        and b"offline mode is enabled; provider model calls are disabled" in positional.stderr
        and TTY_REQUIRED not in positional.stderr,
        f"positional prompt did not reach the offline provider gate: rc={positional.returncode} stdout={positional.stdout!r} stderr={positional.stderr!r}",
    )
    assert_no_sessions(state_root, "positional print prompt")
    return 0


def assert_cli_helper_contracts(root: pathlib.Path) -> None:
    hostile = {
        name: "http://127.0.0.1:1" for name in
        ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY")
    }
    hostile.update({"no_proxy": "", "NO_PROXY": "", "OPENAI_API_KEY": "ambient-key-canary",
                    "ANTHROPIC_BASE_URL": "https://ambient-endpoint.invalid", "ASAN_OPTIONS": "detect_leaks=0"})
    with patch.dict(os.environ, hostile):
        environment = isolated_environment(root / "helper-environment")
    for name in hostile:
        if name in ("no_proxy", "NO_PROXY", "ASAN_OPTIONS"):
            continue
        require(name not in environment, f"isolated environment inherited {name}")
    require(environment["no_proxy"] == environment["NO_PROXY"] == "*", "loopback bypass must be explicit")
    require(environment["ASAN_OPTIONS"] == "detect_leaks=0", "sanitizer settings must survive isolation")

    fixture = """
import os
from pathlib import Path
import signal
import socket
import sys

if os.fork() == 0:
    if sys.argv[3] == "natural-adoption" and os.fork() != 0:
        os._exit(0)
    os.setsid()
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    Path(sys.argv[2]).write_text(str(os.getpid()))
    gate = socket.socket(fileno=int(sys.argv[1]))
    gate.sendall(bytes((0,)))
    if sys.argv[3] == "natural-adoption":
        os._exit(0)
    signal.pause()
else:
    os.close(int(sys.argv[1]))
    signal.pause()
"""
    class ExpectedFixtureFailure(Exception):
        pass

    for mode in ("killed-leader", "timeout", "natural-adoption"):
        gates = create_process_gate_pair()
        descendant_file = root / f"custody-{mode}.pid"
        process = None
        try:
            try:
                with owned_popen(
                    [sys.executable, "-c", fixture, str(gates.child_fd), str(descendant_file), mode],
                    pass_fds=(gates.child_fd,), stdout=subprocess.DEVNULL,
                    stderr=subprocess.PIPE, env=environment,
                ) as process:
                    gates.close_child_endpoint()
                    gates.gates.wait(0, test_timeout(5))
                    descendant = int(descendant_file.read_text())
                    if mode == "natural-adoption":
                        deadline = time.monotonic() + test_timeout(5)
                        while pathlib.Path(f"/proc/{descendant}").exists():
                            require(time.monotonic() < deadline,
                                    "custodian retained an adopted zombie while the leader was alive")
                            time.sleep(0.01)
                        require(process.poll() is None, "natural reaping terminated the still-running leader")
                    else:
                        require(os.getpgid(descendant) == descendant,
                                f"{mode}: acknowledged descendant did not establish its own group")
                    if mode == "killed-leader":
                        process.kill()
                        require(process.wait(timeout=test_timeout(5)) == -signal.SIGKILL,
                                "fixture leader was not killed")
                        require(process_group_is_live(descendant),
                                "wait() activated safety cleanup before product assertions")
                        raise ExpectedFixtureFailure()
                    if mode == "timeout":
                        process.communicate(timeout=test_timeout(0.05))
                        raise AssertionError("gated fixture unexpectedly completed instead of timing out")
            except ExpectedFixtureFailure:
                pass
            except subprocess.TimeoutExpired:
                if mode != "timeout":
                    raise
            require(descendant in process.reaped_descendants,
                    f"{mode}: custodian did not reap separately grouped descendant {descendant}")
            require(not pathlib.Path(f"/proc/{descendant}").exists(),
                    f"{mode}: descendant still has a procfs entry after custody ended")
        finally:
            gates.close()

    gates = create_process_gate_pair()
    try:
        with owned_popen(
            [sys.executable, "-c",
             "import signal,socket,sys; gate=socket.socket(fileno=int(sys.argv[1])); "
             "gate.sendall(bytes((0,))); signal.pause()", str(gates.child_fd)],
            pass_fds=(gates.child_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            env=environment,
        ) as process:
            gates.close_child_endpoint()
            gates.gates.wait(0, test_timeout(5))
            original_read = pathlib.Path.read_text

            def denied_stat(path, *arguments, **options):
                if str(path) == f"/proc/{process.pid}/stat":
                    raise PermissionError("injected unreadable procfs stat")
                return original_read(path, *arguments, **options)

            with patch.object(pathlib.Path, "read_text", denied_stat):
                with patch.object(cli_test_support.os, "killpg", wraps=os.killpg) as probe:
                    require(process_group_is_live(process.pid),
                            "unreadable procfs stat incorrectly proved an owned live group absent")
                    probe.assert_called_with(process.pid, 0)
    finally:
        gates.close()

    with patch.object(pathlib.Path, "read_text", side_effect=FileNotFoundError("missing procfs")):
        with patch.object(cli_test_support.subprocess, "Popen") as spawn:
            try:
                with owned_popen(["/bin/true"]):
                    raise AssertionError("missing procfs prerequisite was silently accepted")
            except RuntimeError as error:
                require("readable procfs" in str(error), f"wrong prerequisite failure: {error}")
            spawn.assert_not_called()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    ava = pathlib.Path(args.ava).resolve()
    require(ava.is_file(), f"AVA executable does not exist: {ava}")
    root = pathlib.Path(args.root).resolve()
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(prefix="ava-interactive-cli-", dir=root) as temporary:
        case = pathlib.Path(temporary)
        assert_cli_helper_contracts(case)
        workspace = case / "workspace"
        workspace.mkdir()
        return run_cases(ava, workspace, isolated_environment(case))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
