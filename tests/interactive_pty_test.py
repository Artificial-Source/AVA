#!/usr/bin/env python3
"""PTY proof that a TTY starts the TUI and mixed non-TTY stdio reject early."""

from __future__ import annotations

import argparse
import errno
import fcntl
import os
import pathlib
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


DEADLINE_SECONDS = 10.0
MAX_CAPTURE_BYTES = 256 * 1024
ENVIRONMENT_ALLOWLIST = (
    "PATH",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "LD_LIBRARY_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FALLBACK_LIBRARY_PATH",
    "ASAN_OPTIONS",
    "UBSAN_OPTIONS",
    "LSAN_OPTIONS",
    "TSAN_OPTIONS",
    "MSAN_OPTIONS",
    "ASAN_SYMBOLIZER_PATH",
    "LLVM_SYMBOLIZER_PATH",
    "TMPDIR",
    "TZ",
)

TTY_REQUIRED = b"interactive TUI requires a terminal on both stdin and stdout"
PRINT_OR_RPC = b"--print or --rpc"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def process_group_exists(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def terminate_group(process: subprocess.Popen[bytes], timeout: float = 2.0) -> None:
    pgid = process.pid
    if process.poll() is None and process_group_exists(pgid):
        try:
            os.killpg(pgid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process_group_exists(pgid):
        process.poll()
        select.select([], [], [], 0.05)
    if process_group_exists(pgid):
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass


def set_winsize(fd: int, rows: int = 24, columns: int = 80) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))


def isolated_environment(root: pathlib.Path, tmpdir: pathlib.Path) -> dict[str, str]:
    environment = {name: os.environ[name] for name in ENVIRONMENT_ALLOWLIST if name in os.environ}
    directories = {
        "HOME": root / "home",
        "XDG_CONFIG_HOME": root / "config",
        "XDG_STATE_HOME": root / "state",
        "XDG_DATA_HOME": root / "data",
        "XDG_CACHE_HOME": root / "cache",
        "XDG_RUNTIME_DIR": root / "runtime",
    }
    for name, directory in directories.items():
        directory.mkdir(parents=True, exist_ok=True)
        environment[name] = str(directory)
    directories["XDG_RUNTIME_DIR"].chmod(0o700)
    tmpdir.mkdir(parents=True, exist_ok=True)
    environment.update(
        {
            "TERM": "xterm-256color",
            "NO_COLOR": "1",
            "TMPDIR": str(tmpdir),
            "AVA_SESSION_TITLES": "off",
            "LIBCWD_NO_STARTUP_MSGS": "1",
            "AVA_NO_DEBUG_OUTPUT": "1",
        }
    )
    return environment


def assert_no_runtime_state(state_root: pathlib.Path, label: str) -> None:
    ava_state = state_root / "ava"
    for name in ("sessions", "diagnostics"):
        require(not (ava_state / name).exists(), f"{label} created {name} runtime state")


def wait_exit(process: subprocess.Popen[bytes], timeout: float = DEADLINE_SECONDS) -> int:
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        terminate_group(process)
        raise RuntimeError(f"AVA did not exit within {timeout:.1f}s") from error


def drain(fd: int, capture: bytearray) -> bool:
    try:
        chunk = os.read(fd, 16384)
    except OSError as error:
        if error.errno != errno.EIO:
            raise
        return False
    capture.extend(chunk)
    require(len(capture) <= MAX_CAPTURE_BYTES, f"PTY capture exceeded its byte limit: {bytes(capture)!r}")
    return bool(chunk)


def prove_tty_starts_tui(ava: pathlib.Path, workspace: pathlib.Path, environment: dict[str, str]) -> None:
    master_fd, slave_fd = pty.openpty()
    set_winsize(slave_fd)
    process = subprocess.Popen(
        [str(ava), "--offline", "--no-session"],
        cwd=workspace,
        env=environment,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        start_new_session=True,
    )
    os.close(slave_fd)
    capture = bytearray()
    try:
        deadline = time.monotonic() + DEADLINE_SECONDS
        while time.monotonic() < deadline:
            # The rendered empty composer is visible only after ncurses initialization.
            if b"Type a message..." in capture:
                break
            if process.poll() is not None:
                raise RuntimeError(f"AVA exited before TUI readiness: rc={process.returncode} capture={bytes(capture)!r}")
            ready, _, _ = select.select([master_fd], [], [], min(0.1, max(0.0, deadline - time.monotonic())))
            if ready:
                drain(master_fd, capture)
        else:
            raise RuntimeError(f"timed out waiting for TUI composer; capture={bytes(capture)!r}")
        os.write(master_fd, b"\x04")
        deadline = time.monotonic() + DEADLINE_SECONDS
        while process.poll() is None and time.monotonic() < deadline:
            ready, _, _ = select.select([master_fd], [], [], min(0.1, max(0.0, deadline - time.monotonic())))
            if ready:
                drain(master_fd, capture)
        if process.poll() is None:
            raise RuntimeError(f"AVA did not exit within {DEADLINE_SECONDS:.1f}s; capture={bytes(capture)!r}")
        returncode = process.wait()
        # The child can exit before its final terminal bytes are read from the master.
        while select.select([master_fd], [], [], 0)[0]:
            if not drain(master_fd, capture):
                break
        require(returncode == 0, f"TTY TUI exit failed: rc={returncode} capture={bytes(capture)!r}")
        require(b"Ready when you are" in capture, f"TTY TUI did not restore and print the farewell card: {bytes(capture)!r}")
        require(TTY_REQUIRED not in capture, f"TTY TUI was rejected as non-interactive: {bytes(capture)!r}")
    finally:
        terminate_group(process)
        os.close(master_fd)


def prove_piped_stdin_rejects(ava: pathlib.Path, workspace: pathlib.Path, environment: dict[str, str], state_root: pathlib.Path) -> None:
    master_fd, slave_fd = pty.openpty()
    set_winsize(slave_fd)
    process = subprocess.Popen(
        [str(ava), "--offline"],
        cwd=workspace,
        env=environment,
        stdin=subprocess.PIPE,
        stdout=slave_fd,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    os.close(slave_fd)
    assert process.stdin is not None
    process.stdin.close()
    try:
        returncode = wait_exit(process)
        stderr = process.stderr.read() if process.stderr is not None else b""
        require(
            returncode == 2 and TTY_REQUIRED in stderr and PRINT_OR_RPC in stderr,
            f"piped stdin with TTY stdout did not reject early: rc={returncode} stderr={stderr!r}",
        )
        assert_no_runtime_state(state_root, "piped-stdin interactive startup")
    finally:
        terminate_group(process)
        os.close(master_fd)


def prove_redirected_stdout_rejects(ava: pathlib.Path, workspace: pathlib.Path, environment: dict[str, str], state_root: pathlib.Path) -> None:
    master_fd, slave_fd = pty.openpty()
    set_winsize(slave_fd)
    process = subprocess.Popen(
        [str(ava), "--offline"],
        cwd=workspace,
        env=environment,
        stdin=slave_fd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    os.close(slave_fd)
    try:
        returncode = wait_exit(process)
        stdout = process.stdout.read() if process.stdout is not None else b""
        stderr = process.stderr.read() if process.stderr is not None else b""
        require(
            returncode == 2 and stdout == b"" and TTY_REQUIRED in stderr and PRINT_OR_RPC in stderr,
            f"redirected stdout with TTY stdin did not reject early: rc={returncode} stdout={stdout!r} stderr={stderr!r}",
        )
        assert_no_runtime_state(state_root, "redirected-stdout interactive startup")
    finally:
        terminate_group(process)
        os.close(master_fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    ava = pathlib.Path(args.ava).resolve()
    require(ava.is_file(), f"AVA executable does not exist: {ava}")
    root = pathlib.Path(args.root).resolve()
    if root.exists():
        require(root.name == "interactive-pty", f"refusing to clear unexpected PTY root: {root}")
        shutil.rmtree(root)
    root.mkdir(parents=True, mode=0o700)
    workspace = root / "workspace"
    workspace.mkdir()
    with tempfile.TemporaryDirectory(prefix="ava-interactive-pty-", dir=root) as tmp:
        environment = isolated_environment(root, pathlib.Path(tmp))
        state_root = pathlib.Path(environment["XDG_STATE_HOME"])
        prove_piped_stdin_rejects(ava, workspace, environment, state_root)
        prove_redirected_stdout_rejects(ava, workspace, environment, state_root)
        prove_tty_starts_tui(ava, workspace, environment)
        return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
