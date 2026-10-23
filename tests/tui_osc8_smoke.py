#!/usr/bin/env python3
import argparse
import base64
import binascii
import fcntl
import os
import pathlib
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import termios
import time

from fake_provider import launch_fake_provider


SKIP = 77
MAX_CAPTURE_BYTES = 2 * 1024 * 1024
ENVIRONMENT_ALLOWLIST = (
    "PATH",
    "LANG",
    "LANGUAGE",
    "LC_ALL",
    "LC_CTYPE",
    "LC_MESSAGES",
    "LC_NUMERIC",
    "LC_TIME",
    "LC_COLLATE",
    "LC_MONETARY",
    "TERMINFO",
    "TERMINFO_DIRS",
    "LD_LIBRARY_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FALLBACK_LIBRARY_PATH",
    "ASAN_OPTIONS",
    "UBSAN_OPTIONS",
    "LSAN_OPTIONS",
    "TSAN_OPTIONS",
    "MSAN_OPTIONS",
    "HWASAN_OPTIONS",
    "ASAN_SYMBOLIZER_PATH",
    "UBSAN_SYMBOLIZER_PATH",
    "LSAN_SYMBOLIZER_PATH",
    "TSAN_SYMBOLIZER_PATH",
    "MSAN_SYMBOLIZER_PATH",
    "HWASAN_SYMBOLIZER_PATH",
    "LLVM_SYMBOLIZER_PATH",
    "TMPDIR",
    "TZ",
    "AVA_TEST_NAME",
    "AVA_DEBUG_OUTPUT_DIR",
    "LIBCWD_RCFILE_NAME",
    "LIBCWD_RCFILE_OVERRIDE_NAME",
    "AVA_SESSION_TITLES",
)


def enabled(value: str | None) -> bool:
    return value is not None and value.lower() in {"1", "true", "yes", "on"}


def set_winsize(fd: int, rows: int, cols: int) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def allowlisted_environment() -> dict[str, str]:
    env = {key: os.environ[key] for key in ENVIRONMENT_ALLOWLIST if key in os.environ}
    # Match the libcwd suppression applied to ava_tests.*: this sealed env does
    # not inherit os.environ, so set the pair explicitly to keep ava's debug
    # initialization from writing to the streams this smoke inspects.
    env["LIBCWD_NO_STARTUP_MSGS"] = "1"
    env["AVA_NO_DEBUG_OUTPUT"] = "1"
    return env


def strip_control_sequences(text: bytes) -> bytes:
    out = bytearray()
    index = 0
    while index < len(text):
        if text[index:index + 2] == b"\x1b[":
            index += 2
            while index < len(text) and not (0x40 <= text[index] <= 0x7E):
                index += 1
            if index < len(text):
                index += 1
            continue
        if text[index:index + 2] == b"\x1b]":
            index += 2
            while index < len(text):
                if text[index:index + 2] == b"\x1b\\":
                    index += 2
                    break
                if text[index] == 0x07:
                    index += 1
                    break
                index += 1
            continue
        out.append(text[index])
        index += 1
    return bytes(out)


def read_until(master_fd: int, process: subprocess.Popen[bytes], predicate, label: str, timeout: float = 10.0) -> bytes:
    deadline = time.monotonic() + timeout
    captured = bytearray()
    while time.monotonic() < deadline:
        if predicate(bytes(captured)):
            return bytes(captured)
        ready, _, _ = select.select([master_fd], [], [], 0.1)
        if ready:
            try:
                chunk = os.read(master_fd, 8192)
            except OSError:
                chunk = b""
            if chunk:
                captured.extend(chunk)
                if len(captured) > MAX_CAPTURE_BYTES:
                    raise RuntimeError(f"terminal output exceeded {MAX_CAPTURE_BYTES} bytes while waiting for {label}")
                continue
        if process.poll() is not None:
            raise RuntimeError(
                f"AVA exited before {label} with code {process.returncode}\n"
                f"captured:\n{strip_control_sequences(bytes(captured)).decode(errors='replace')}"
            )
    raise RuntimeError(
        f"timed out waiting for {label}\n"
        f"captured:\n{strip_control_sequences(bytes(captured)).decode(errors='replace')}"
    )


def process_group_exists(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def wait_for_no_process_group(pgid: int, timeout: float = 1.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not process_group_exists(pgid):
            return
        select.select([], [], [], min(0.05, max(0.0, deadline - time.monotonic())))
    raise RuntimeError(f"process group {pgid} remained after exit")


def terminate_process(process: subprocess.Popen[bytes], master_fd: int | None = None) -> None:
    if process.poll() is None and master_fd is not None:
        try:
            os.write(master_fd, b"\x04")
            process.wait(timeout=3)
        except Exception:
            pass
    if process_group_exists(process.pid):
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline and process_group_exists(process.pid):
            process.poll()
            select.select([], [], [], 0.05)
        if process_group_exists(process.pid):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True)
    parser.add_argument("--fake-provider", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    if not enabled(os.environ.get("AVA_TUI_OSC8_SMOKE")):
        print("skipping OSC 8 PTY smoke; set AVA_TUI_OSC8_SMOKE=1 to run")
        return SKIP

    ava_exe = pathlib.Path(args.ava).absolute()
    fake_provider_exe = pathlib.Path(args.fake_provider).absolute()
    if not ava_exe.exists():
        raise RuntimeError(f"AVA executable does not exist: {ava_exe}")
    if not fake_provider_exe.exists():
        raise RuntimeError(f"fake provider executable does not exist: {fake_provider_exe}")

    root = pathlib.Path(args.root).absolute()
    if root.exists():
        if root.name != "tui-osc8-smoke":
            raise RuntimeError(f"refusing to clear unexpected smoke root: {root}")
        shutil.rmtree(root)
    root.mkdir(parents=True, mode=0o700)
    workspace = root / "workspace"
    home = root / "home"
    config = root / "config"
    state = root / "state"
    data = root / "data"
    cache = root / "cache"
    runtime = root / "runtime"
    ava_config = config / "ava"
    for path in (workspace, home, ava_config, state, data, cache, runtime):
        path.mkdir(parents=True, exist_ok=True)
    runtime.chmod(0o700)
    (workspace / "AGENTS.md").write_text("osc8 smoke context\n", encoding="utf-8")
    (ava_config / "models.json").write_text(
        '{"default_provider":"moonshot","default_model":"ava-osc8-fake",'
        '"models":[{"provider":"moonshot","id":"ava-osc8-fake","name":"AVA OSC8 Fake","family":"fake",'
        '"context_window_tokens":8192,"max_output_tokens":1024,"supports_tools":false,'
        '"supports_streaming":false,"supports_reasoning":false,"reports_usage":true}]}\n',
        encoding="utf-8",
    )

    provider = launch_fake_provider(
        fake_provider_exe,
        root,
        prefix="provider",
        delay_ms=0,
        scenario="markdown-links",
        environment=allowlisted_environment(),
    )
    port = provider.port
    master_fd = -1
    try:

        env = allowlisted_environment()
        env.update(
            {
                "HOME": str(home),
                "XDG_CONFIG_HOME": str(config),
                "XDG_STATE_HOME": str(state),
                "XDG_DATA_HOME": str(data),
                "XDG_CACHE_HOME": str(cache),
                "XDG_RUNTIME_DIR": str(runtime),
                "TERM": "xterm-256color",
                "TERM_PROGRAM": "vscode",
                "COLORTERM": "truecolor",
                "MOONSHOT_API_KEY": "test-key",
                "MOONSHOT_BASE_URL": f"http://127.0.0.1:{port}",
            }
        )
        for key in (
            "TMUX",
            "TMUX_PANE",
            "NO_COLOR",
            "AVA_TUI_THEME",
            "KITTY_WINDOW_ID",
            "GHOSTTY_RESOURCES_DIR",
            "WEZTERM_PANE",
            "WARP_SESSION_ID",
            "WARP_TERMINAL_SESSION_UUID",
            "ITERM_SESSION_ID",
            "WT_SESSION",
        ):
            env.pop(key, None)

        master_fd, slave_fd = pty.openpty()
        set_winsize(slave_fd, 28, 100)
        process = subprocess.Popen(
            [str(ava_exe)],
            cwd=workspace,
            env=env,
            stdin=slave_fd,
            stdout=slave_fd,
            stderr=slave_fd,
            start_new_session=True,
        )
        os.close(slave_fd)

        try:
            read_until(master_fd, process, lambda data: b"Type a message" in data or b"live session" in data,
                       "initial TUI frame")
            os.write(master_fd, b"show markdown links\r")
            captured = read_until(
                master_fd,
                process,
                lambda data: (
                    b"\x1b]8;;https://e.test/d\x1b\\" in data
                    and b"\x1b]8;;https://e.test/b\x1b\\" in data
                    and b"\x1b]8;;mailto:u@e.test\x1b\\" in data
                    and b"\x1b]8;;\x1b\\" in data
                ),
                "OSC 8 assistant Markdown links",
                timeout=12.0,
            )
            visible = strip_control_sequences(captured)
            if b"Docs" not in visible or b"https://e.test/b" not in visible or b"u@e.test" not in visible:
                raise RuntimeError(
                    "assistant Markdown link text was not visible while OSC 8 links were emitted\n"
                    f"captured:\n{visible.decode(errors='replace')}"
                )
            if b"(https://e.test/d)" in visible:
                raise RuntimeError(
                    "OSC 8 hyperlink mode leaked the visible URL fallback for a descriptive Markdown link\n"
                    f"captured:\n{visible.decode(errors='replace')}"
                )

            completion_pattern = re.compile(rb"AVA OSC8 Fake[^\r\n]*\d+ms")
            if completion_pattern.search(visible) is None:
                read_until(
                    master_fd,
                    process,
                    lambda data: completion_pattern.search(strip_control_sequences(data)) is not None,
                    "completed assistant turn before OSC 52 copy",
                    timeout=12.0,
                )

            os.write(master_fd, b"\x1b[200~/copy\x1b[201~")
            read_until(
                master_fd,
                process,
                lambda data: b"/copy" in strip_control_sequences(data),
                "OSC 52 copy command draft",
            )
            os.write(master_fd, b"\r")
            copied = read_until(
                master_fd,
                process,
                lambda data: (
                    re.search(rb"\x1b\]52;c;[A-Za-z0-9+/]*={0,2}\x1b\\", data) is not None
                    and b"last AVA message copy request sent" in strip_control_sequences(data)
                ),
                "OSC 52 clipboard copy",
            )
            match = re.search(rb"\x1b\]52;c;([A-Za-z0-9+/]*={0,2})\x1b\\", copied)
            if match is None:
                raise RuntimeError(f"captured output did not contain one complete OSC 52 sequence: {copied!r}")
            try:
                clipboard_payload = base64.b64decode(match.group(1), validate=True)
            except (ValueError, binascii.Error) as error:
                raise RuntimeError("OSC 52 clipboard payload was not valid base64") from error
            assistant_response = b"[Docs](https://e.test/d) https://e.test/b u@e.test"
            if assistant_response not in clipboard_payload:
                raise RuntimeError(
                    "OSC 52 payload did not contain the actual assistant response; "
                    f"decoded={clipboard_payload!r}"
                )
            copied_visible = strip_control_sequences(copied)
            if b"last AVA message copy request sent" not in copied_visible:
                raise RuntimeError(
                    "OSC 52 emission did not retain visible request-sent status; "
                    f"captured={copied_visible.decode(errors='replace')}"
                )
            os.write(master_fd, b"\x04")
            process.wait(timeout=5)
            if process.returncode != 0:
                raise RuntimeError(f"AVA clean exit returned {process.returncode}")
            wait_for_no_process_group(process.pid)
            print(
                f"osc8+osc52: links=3 osc52_terminator=ST decoded_clipboard_bytes={len(clipboard_payload)} "
                f"exit={process.returncode}"
            )
            return 0
        finally:
            terminate_process(process, master_fd)
    finally:
        if master_fd >= 0:
            os.close(master_fd)
        provider.stop()


if __name__ == "__main__":
    sys.exit(main())
