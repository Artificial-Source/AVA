#!/usr/bin/env python3
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
import termios
import time


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
)
BRACKETED_PASTE_ENABLE = b"\x1b[?2004h"
BRACKETED_PASTE_DISABLE = b"\x1b[?2004l"
KITTY_KEYBOARD_PUSH_FLAGS_1 = b"\x1b[>1u"
KITTY_KEYBOARD_QUERY = b"\x1b[?u"
DEVICE_ATTRIBUTES_QUERY = b"\x1b[c"
MODIFY_OTHER_KEYS_ENABLE = b"\x1b[>4;2m"
MODIFY_OTHER_KEYS_DISABLE = b"\x1b[>4;0m"
KITTY_KEYBOARD_POP = b"\x1b[<u"
ALT_SCREEN_ENTER = b"\x1b[?1049h"
ALT_SCREEN_EXIT = b"\x1b[?1049l"
CURSOR_HIDE = b"\x1b[?25l"
CURSOR_SHOW = b"\x1b[?25h"
CURSOR_STEADY_BAR = b"\x1b[6 q"
CURSOR_STYLE_RESET = b"\x1b[0 q"
CURSOR_STYLE_SEQUENCES = tuple(f"\x1b[{parameter} q".encode() for parameter in range(7))
CONTROL_CHARACTER_NAMES = {}
for control_name in (
    "VINTR", "VQUIT", "VERASE", "VKILL", "VEOF", "VTIME", "VMIN", "VSWTC", "VSTART", "VSTOP",
    "VSUSP", "VEOL", "VREPRINT", "VDISCARD", "VWERASE", "VLNEXT", "VEOL2",
):
    if hasattr(termios, control_name):
        CONTROL_CHARACTER_NAMES.setdefault(getattr(termios, control_name), control_name)


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


def read_until(master_fd: int, process: subprocess.Popen[bytes], predicate, label: str, timeout: float = 8.0) -> bytes:
    deadline = time.monotonic() + timeout
    captured = bytearray()
    while time.monotonic() < deadline:
        if predicate(bytes(captured)):
            return bytes(captured)
        ready, _, _ = select.select([master_fd], [], [], min(0.1, max(0.0, deadline - time.monotonic())))
        if ready:
            try:
                chunk = os.read(master_fd, 16384)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                chunk = b""
            if chunk:
                captured.extend(chunk)
                if len(captured) > MAX_CAPTURE_BYTES:
                    raise RuntimeError(f"terminal output exceeded {MAX_CAPTURE_BYTES} bytes while waiting for {label}")
                continue
        if process.poll() is not None:
            raise RuntimeError(f"AVA exited before {label} with code {process.returncode}; captured={bytes(captured)!r}")
    raise RuntimeError(f"timed out waiting for {label}; captured={bytes(captured)!r}")


def drain_exit(master_fd: int, process: subprocess.Popen[bytes], timeout: float = 6.0) -> tuple[int, bytes]:
    deadline = time.monotonic() + timeout
    captured = bytearray()
    eof = False
    while time.monotonic() < deadline:
        ready, _, _ = select.select([master_fd], [], [], min(0.1, max(0.0, deadline - time.monotonic())))
        if ready:
            try:
                chunk = os.read(master_fd, 16384)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                chunk = b""
            if chunk:
                captured.extend(chunk)
                if len(captured) > MAX_CAPTURE_BYTES:
                    raise RuntimeError(f"terminal teardown exceeded {MAX_CAPTURE_BYTES} bytes")
            else:
                eof = True
        returncode = process.poll()
        if returncode is not None and eof:
            return returncode, bytes(captured)
        if returncode is not None and not ready:
            # This driver intentionally retains the slave descriptor for post-exit
            # termios inspection, so the master cannot report EOF here. A completed
            # select cycle with no readable bytes is the bounded drain condition.
            return returncode, bytes(captured)
    raise RuntimeError(f"AVA did not exit and drain within {timeout:.1f}s; captured={bytes(captured)!r}")


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
    raise RuntimeError(f"AVA process group {pgid} remained after exit")


def terminate_group(process: subprocess.Popen[bytes], pgid: int) -> None:
    if not process_group_exists(pgid):
        return
    try:
        os.killpg(pgid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + 2.0
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


def canonical_control_character(value: int | bytes) -> int | tuple[int, ...]:
    if isinstance(value, int):
        return value
    if len(value) == 1:
        return value[0]
    return tuple(value)


def normalized_termios(attrs: list) -> tuple[int, int, int, int, int, int, tuple[int | tuple[int, ...], ...]]:
    return (
        int(attrs[0]),
        int(attrs[1]),
        int(attrs[2]),
        int(attrs[3]),
        int(attrs[4]),
        int(attrs[5]),
        tuple(canonical_control_character(value) for value in attrs[6]),
    )


def termios_differences(before: tuple, after: tuple) -> list[str]:
    names = ("iflag", "oflag", "cflag", "lflag", "ispeed", "ospeed")
    differences = [f"{name}: before={before[index]!r} after={after[index]!r}" for index, name in enumerate(names) if before[index] != after[index]]
    before_cc = before[6]
    after_cc = after[6]
    for index in range(max(len(before_cc), len(after_cc))):
        old = before_cc[index] if index < len(before_cc) else "<missing>"
        new = after_cc[index] if index < len(after_cc) else "<missing>"
        if old != new:
            label = CONTROL_CHARACTER_NAMES.get(index, f"index {index}")
            differences.append(f"cc[{index}/{label}]: before={old!r} after={new!r}")
    return differences


def require_order(data: bytes, sequences: list[tuple[str, bytes]]) -> None:
    previous = -1
    for label, sequence in sequences:
        position = data.find(sequence)
        if position < 0:
            raise RuntimeError(f"missing {label} sequence {sequence!r}; teardown={data!r}")
        if position <= previous:
            raise RuntimeError(f"{label} sequence was out of teardown order at {position}; teardown={data!r}")
        previous = position


def cursor_visibility(output: bytes) -> bool | None:
    """Return the last explicit cursor visibility in accumulated xterm PTY output.

    Shape/blink changes do not affect visibility. Return None when no show/hide
    sequence was observed; callers must accumulate chunks before checking state.
    """

    show_at = output.rfind(CURSOR_SHOW)
    hide_at = output.rfind(CURSOR_HIDE)
    if show_at < 0 and hide_at < 0:
        return None
    return show_at > hide_at


def hide_cursor_for_teardown(master_fd: int, process: subprocess.Popen[bytes], label: str) -> bytes:
    """Open AVA's local model selector and wait for its ncurses-owned hidden cursor.

    Ctrl+L requests an actual UI redraw which calls curs_set(0) inside AVA. Sending
    a cursor-hide escape into the PTY instead would only feed AVA input, not alter
    its ncurses state. Leave the selector open for signal-driven teardown.
    """

    os.write(master_fd, b"\x0c")
    return read_until(
        master_fd,
        process,
        lambda output: b"Select model" in output and b"Esc close" in output and cursor_visibility(output) is False,
        f"{label} model selector with hidden cursor",
    )


def require_cursor_restored(before_teardown: bytes, teardown: bytes, label: str) -> None:
    """Require a visible final cursor, allowing no-op restoration when already visible.

    A hidden pre-teardown cursor still requires a later show. Inspect both phases
    together so a missing or subsequently reversed restoration cannot pass.
    """

    if cursor_visibility(before_teardown + teardown) is not True:
        raise RuntimeError(
            f"{label} did not leave the cursor visible; "
            f"before_teardown_visible={cursor_visibility(before_teardown)!r}; teardown={teardown!r}"
        )


def run_case(
    ava_exe: pathlib.Path,
    case_root: pathlib.Path,
    case: str,
    teardown_method: str,
    *,
    direct_alacritty: bool = False,
    forced_cursor: bool = False,
) -> None:
    workspace = case_root / "workspace"
    home = case_root / "home"
    config = case_root / "config"
    state = case_root / "state"
    data = case_root / "data"
    cache = case_root / "cache"
    runtime = case_root / "runtime"
    for path in (workspace, home, config, state, data, cache, runtime):
        path.mkdir(parents=True, exist_ok=True)
    runtime.chmod(0o700)
    (workspace / "AGENTS.md").write_text(f"terminal lifecycle {case}\n", encoding="utf-8")
    if forced_cursor:
        ava_config = config / "ava"
        ava_config.mkdir()
        (ava_config / "display.json").write_text(
            '{\n  "cursor_style": "bar",\n  "cursor_blink": false\n}\n',
            encoding="utf-8",
        )

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
            "TERM_PROGRAM": "Alacritty" if direct_alacritty else "xterm",
            "COLORTERM": "truecolor",
        }
    )
    for key in (
        "TMUX", "TMUX_PANE", "NO_COLOR", "AVA_TUI_THEME", "KITTY_WINDOW_ID", "GHOSTTY_RESOURCES_DIR",
        "WEZTERM_PANE", "WARP_SESSION_ID", "WARP_TERMINAL_SESSION_UUID", "ITERM_SESSION_ID", "WT_SESSION",
    ):
        env.pop(key, None)

    master_fd, slave_fd = pty.openpty()
    set_winsize(slave_fd, 28, 100)
    before = termios.tcgetattr(slave_fd)
    process = subprocess.Popen(
        [str(ava_exe)],
        cwd=workspace,
        env=env,
        stdin=slave_fd,
        stdout=slave_fd,
        stderr=slave_fd,
        start_new_session=True,
    )
    pgid = process.pid
    try:
        initial_kitty_push = KITTY_KEYBOARD_PUSH_FLAGS_1
        startup = read_until(
            master_fd,
            process,
            lambda output: (
                (b"Type a message" in output or b"live session" in output)
                and BRACKETED_PASTE_ENABLE in output
                and initial_kitty_push in output
                and KITTY_KEYBOARD_QUERY in output
                and DEVICE_ATTRIBUTES_QUERY in output
                and MODIFY_OTHER_KEYS_ENABLE in output
                and (not forced_cursor or CURSOR_STEADY_BAR in output)
            ),
            f"{case} synchronized startup",
        )
        startup_protocols = [
            ("initial Kitty keyboard push", initial_kitty_push),
            ("Kitty keyboard query", KITTY_KEYBOARD_QUERY),
            ("device-attributes query", DEVICE_ATTRIBUTES_QUERY),
            ("modifyOtherKeys fallback enable", MODIFY_OTHER_KEYS_ENABLE),
        ]
        try:
            require_order(startup, startup_protocols)
        except RuntimeError as error:
            raise RuntimeError(f"{case} startup protocol order was invalid: {error}") from error
        if forced_cursor:
            # Context applies the requested shape, then repairs it after ncurses makes the initially hidden cursor visible.
            if startup.count(CURSOR_STEADY_BAR) != 2 or CURSOR_STYLE_RESET in startup:
                raise RuntimeError(f"{case} did not apply and restore one steady bar cursor without resetting it during entry; startup={startup!r}")
        else:
            for sequence in CURSOR_STYLE_SEQUENCES:
                if sequence in startup:
                    raise RuntimeError(f"{case} default cursor perturbed DECSCUSR during entry with {sequence!r}; startup={startup!r}")

        # Negotiation is bounded and completed before the first frame. A delayed
        # device-attributes reply is ordinary unsolicited input and must never
        # drive a second keyboard-protocol owner.
        negotiation = b""
        os.write(master_fd, b"\x1b[?1;2c")

        cursor_setup = b""
        if teardown_method == "ctrl_d":
            # The idle composer already shows the cursor. Cleanup may be a no-op.
            os.write(master_fd, b"\x04")
        elif teardown_method == "sigterm":
            # Reuse the existing signal cases to exercise real hidden-to-visible
            # restoration without another AVA launch. Ctrl+D would only dismiss
            # this selector, so its normal-exit cases stay on the idle composer.
            cursor_setup = hide_cursor_for_teardown(master_fd, process, case)
            os.kill(process.pid, signal.SIGTERM)
        else:
            raise RuntimeError(f"unsupported teardown method: {teardown_method}")
        returncode, teardown = drain_exit(master_fd, process)
        expected_returncode = 0 if teardown_method == "ctrl_d" else 130
        if returncode != expected_returncode:
            raise RuntimeError(f"{case} exited with {returncode}, expected {expected_returncode}; teardown={teardown!r}")

        teardown_protocols = []
        teardown_protocols.append(("modifyOtherKeys disable", MODIFY_OTHER_KEYS_DISABLE))
        teardown_protocols.extend(
            [
                ("Kitty keyboard pop", KITTY_KEYBOARD_POP),
                ("bracketed-paste disable", BRACKETED_PASTE_DISABLE),
            ]
        )
        if forced_cursor:
            teardown_protocols.append(("forced cursor reset", CURSOR_STYLE_RESET))
        require_order(teardown, teardown_protocols)

        if ALT_SCREEN_ENTER in startup and ALT_SCREEN_EXIT not in teardown:
            raise RuntimeError(f"xterm alternate-screen entry was not paired with exit; teardown={teardown!r}")
        cursor_show_at = teardown.find(CURSOR_SHOW)
        require_cursor_restored(startup + negotiation + cursor_setup, teardown, case)
        if forced_cursor:
            if teardown.count(CURSOR_STYLE_RESET) != 1:
                raise RuntimeError(f"{case} did not reset the forced cursor exactly once during teardown; teardown={teardown!r}")
        else:
            for phase, captured in (("negotiation", negotiation), ("cursor setup", cursor_setup), ("teardown", teardown)):
                for sequence in CURSOR_STYLE_SEQUENCES:
                    if sequence in captured:
                        raise RuntimeError(
                            f"{case} default cursor perturbed DECSCUSR during {phase} with {sequence!r}; captured={captured!r}"
                        )

        after = termios.tcgetattr(slave_fd)
        normalized_before = normalized_termios(before)
        normalized_after = normalized_termios(after)
        if normalized_after != normalized_before:
            differences = "; ".join(termios_differences(normalized_before, normalized_after))
            raise RuntimeError(f"{case} did not exactly restore normalized termios state: {differences}")
        wait_for_no_process_group(pgid)
        print(
            f"{case}: exit={returncode} startup={len(startup)} negotiation={len(negotiation)} teardown={len(teardown)} "
            f"disable_at={teardown.find(MODIFY_OTHER_KEYS_DISABLE)} pop_at={teardown.find(KITTY_KEYBOARD_POP)} "
            f"paste_disable_at={teardown.find(BRACKETED_PASTE_DISABLE)} cursor_reset_at={teardown.find(CURSOR_STYLE_RESET)} "
            f"cursor_show_at={cursor_show_at} alt_exit_at={teardown.find(ALT_SCREEN_EXIT)} termios_exact=True"
        )
    finally:
        terminate_group(process, pgid)
        os.close(master_fd)
        os.close(slave_fd)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    if not enabled(os.environ.get("AVA_TUI_TERMINAL_LIFECYCLE_SMOKE")):
        print("skipping terminal lifecycle PTY smoke; set AVA_TUI_TERMINAL_LIFECYCLE_SMOKE=1 to run")
        return SKIP

    ava_exe = pathlib.Path(args.ava).resolve()
    if not ava_exe.exists():
        raise RuntimeError(f"AVA executable does not exist: {ava_exe}")
    root = pathlib.Path(args.root).resolve()
    if root.exists():
        if root.name != "tui-terminal-lifecycle-smoke":
            raise RuntimeError(f"refusing to clear unexpected smoke root: {root}")
        shutil.rmtree(root)
    root.mkdir(parents=True, mode=0o700)

    run_case(ava_exe, root / "default-ctrl-d", "default_ctrl_d", "ctrl_d")
    run_case(ava_exe, root / "default-sigterm", "default_sigterm", "sigterm")
    run_case(ava_exe, root / "alacritty-ctrl-d", "alacritty_ctrl_d", "ctrl_d", direct_alacritty=True)
    run_case(ava_exe, root / "forced-cursor-ctrl-d", "forced_cursor_ctrl_d", "ctrl_d", forced_cursor=True)
    run_case(ava_exe, root / "forced-cursor-sigterm", "forced_cursor_sigterm", "sigterm", forced_cursor=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
