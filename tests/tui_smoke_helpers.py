#!/usr/bin/env python3
"""Shared isolated harness for the opt-in tmux TUI smoke scenarios.

The dispatcher keeps behavioural assertions in ``tui_tmux_smoke.py``.  This
module deliberately owns all process, environment, tmux, and filesystem
isolation so scenarios can run concurrently without using user state.
"""

from __future__ import annotations

import json
import os
import pathlib
import pwd
import re
import shlex
import shutil
import signal
import subprocess
import tempfile
import time
import uuid
from dataclasses import dataclass, field
from typing import Callable

from fake_provider import FakeProvider, launch_fake_provider
from test_timing_trace import timed_operation, timing_poll


SKIP = 77
# Poll terminal and fixture state frequently enough that one missed first capture
# does not impose a 100 ms floor on every scripted interaction. A 20 ms interval
# still bounds tmux subprocess churn while remaining well below AVA's 100 ms
# bare-Escape disambiguation delay.
POLL_INTERVAL = 0.02
# Footer value emitted by the frontend active-context token projection: unknown,
# a raw sub-1k count, or an integer-truncated compact count with lowercase k/m.
ACTIVE_CONTEXT_TOKEN_COUNT_PATTERN = r"(?:unknown|(?:0|[1-9]\d{0,2})|[1-9]\d{0,2}(?:\.[1-9])?k|[1-9]\d*(?:\.[1-9])?m)"


def enabled(value: str | None) -> bool:
    return value is not None and value.lower() in {"1", "true", "yes", "on"}


def _compatibility_environment(*, home: pathlib.Path, tmpdir: pathlib.Path) -> dict[str, str]:
    """Return the only parent-derived values allowed into test processes.

    The tmux server and fake provider receive this environment, rather than a
    copy of the developer's environment.  PATH is intentionally the portable
    system default (``os.defpath``), never the mutable host PATH: sealed command
    plans capture PATH metadata, so developer tool directories would make the
    fixture non-deterministic.  Individual pane HOME/XDG values and the two
    fake-provider settings are added explicitly by ``SmokeContext``.
    """

    environment = {
        "HOME": str(home),
        "PATH": os.defpath,
        "SHELL": "/bin/sh",
        "TERM": "xterm-256color",
        "TMPDIR": str(tmpdir),
        "TZ": "UTC",
        # Match the libcwd suppression applied to ava_tests.* so ava's debug
        # initialization cannot write to the pane streams this harness inspects.
        "LIBCWD_NO_STARTUP_MSGS": "1",
        "AVA_NO_DEBUG_OUTPUT": "1",
    }
    for name in ("LANG", "LC_ALL", "LC_CTYPE"):
        value = os.environ.get(name)
        if value:
            environment[name] = value
    # Explicit debug-routing opt-ins are safe exceptions to the sealed parent
    # environment: they identify the CTest owner and private output policy.
    for name in ("AVA_TEST_NAME", "AVA_DEBUG_OUTPUT_DIR", "LIBCWD_RCFILE_NAME", "LIBCWD_RCFILE_OVERRIDE_NAME"):
        value = os.environ.get(name)
        if value:
            environment[name] = value
    return environment


def _quoted_assignment(name: str, value: str | pathlib.Path) -> str:
    # Quote only the value: a quoted whole ``NAME=value`` word is a command,
    # not a POSIX shell assignment when the value contains whitespace.
    return f"{name}={shlex.quote(str(value))}"


class TmuxClient:
    """Client for one config-free, root-private tmux server."""

    def __init__(self, executable: str, socket_path: pathlib.Path, environment: dict[str, str]) -> None:
        self.executable = executable
        self.socket_path = socket_path
        self.environment = environment

    def run(self, args: list[str], *, check: bool = True) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            [self.executable, "-f", "/dev/null", "-S", str(self.socket_path), *args],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=self.environment,
        )
        if check and result.returncode != 0:
            raise RuntimeError(
                f"tmux command failed ({result.returncode}): {' '.join(args)}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        return result

    @timed_operation("cleanup", label_argument=None, default_label="tmux server cleanup")
    def close(self) -> None:
        self.run(["kill-server"], check=False)


def tmux(tmux_client: TmuxClient, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return tmux_client.run(list(args), check=check)


def capture(tmux_client: TmuxClient, session: str) -> str:
    result = tmux(tmux_client, "capture-pane", "-t", session, "-p")
    lines = [line.rstrip() for line in result.stdout.splitlines()]
    return "\n".join(lines)


def capture_styled(tmux_client: TmuxClient, session: str) -> str:
    return tmux(tmux_client, "capture-pane", "-e", "-t", session, "-p").stdout


def save_evidence(root: pathlib.Path, name: str, screen: str) -> None:
    evidence_dir = root / "evidence"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "-", name).strip("-").lower()
    (evidence_dir / f"{safe_name}.txt").write_text(screen.rstrip() + "\n", encoding="utf-8")


def pane_current_command(tmux_client: TmuxClient, session: str) -> str:
    return tmux(tmux_client, "display-message", "-p", "-t", session, "#{pane_current_command}").stdout.strip()


@timed_operation("wait", label_argument="label")
def wait_for(tmux_client: TmuxClient, session: str, pattern: str, label: str, timeout: float = 8.0) -> str:
    compiled = re.compile(pattern)
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast screen:\n{last}")
        last = capture(tmux_client, session)
        if compiled.search(last):
            return last
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(f"timed out waiting for {label}; expected /{pattern}/\nlast screen:\n{last}")


@timed_operation("wait", label_argument="label")
def wait_for_count(
    tmux_client: TmuxClient, session: str, pattern: str, expected_count: int, label: str, timeout: float = 8.0
) -> str:
    compiled = re.compile(pattern)
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast screen:\n{last}")
        last = capture(tmux_client, session)
        if len(compiled.findall(last)) >= expected_count:
            return last
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(
        f"timed out waiting for {label}; expected at least {expected_count} matches of /{pattern}/\nlast screen:\n{last}"
    )


@timed_operation("wait", label_argument="label")
def wait_for_absent(tmux_client: TmuxClient, session: str, pattern: str, label: str, timeout: float = 8.0) -> str:
    compiled = re.compile(pattern)
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast screen:\n{last}")
        last = capture(tmux_client, session)
        if not compiled.search(last):
            return last
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(f"timed out waiting for {label}; still matched /{pattern}/\nlast screen:\n{last}")


@timed_operation("wait", label_argument="label")
def wait_for_screen_change(
    tmux_client: TmuxClient, session: str, previous: str, label: str, timeout: float = 8.0
) -> str:
    """Wait for the event-driven screen change that replaces old fixed sleeps."""

    deadline = time.monotonic() + timeout
    last = previous
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast screen:\n{last}")
        last = capture(tmux_client, session)
        if last != previous:
            return last
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(f"timed out waiting for {label}; screen did not change\nlast screen:\n{last}")


@timed_operation("wait", label_argument="label")
def wait_for_styled_screen_change(
    tmux_client: TmuxClient, session: str, previous: str, label: str, timeout: float = 8.0
) -> str:
    """Wait for terminal style cells to change when visible text may remain identical.

    Theme watcher tests use this after editing a color definition. Timing traces
    retain only the caller-authored label and duration, never styled pane data.
    """

    deadline = time.monotonic() + timeout
    last = previous
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}")
        last = capture_styled(tmux_client, session)
        if last != previous:
            return last
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(f"timed out waiting for {label}; styled screen did not change")


@timed_operation("wait", label_argument="label")
def wait_for_screen_state(
    tmux_client: TmuxClient,
    session: str,
    predicate: Callable[[str], bool],
    label: str,
    timeout: float = 8.0,
) -> str:
    """Wait for a parsed behavioral screen state rather than presentation text."""

    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast screen:\n{last}")
        last = capture(tmux_client, session)
        if predicate(last):
            return last
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(f"timed out waiting for {label}\nlast screen:\n{last}")


@timed_operation("observation", label_argument="label")
def assert_screen_absent_for(
    tmux_client: TmuxClient, session: str, pattern: str, label: str, duration: float = 0.4
) -> str:
    """Prove a negative terminal assertion for a bounded observation window."""

    compiled = re.compile(pattern)
    deadline = time.monotonic() + duration
    last = ""
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast screen:\n{last}")
        last = capture(tmux_client, session)
        if compiled.search(last):
            raise RuntimeError(f"{label}; unexpectedly matched /{pattern}/\nscreen:\n{last}")
        time.sleep(POLL_INTERVAL)
    return last


@timed_operation("observation", label_argument="label")
def assert_screen_present_for(
    tmux_client: TmuxClient, session: str, pattern: str, label: str, duration: float = 0.4
) -> str:
    """Prove a required screen state remains visible for a bounded window."""

    compiled = re.compile(pattern)
    deadline = time.monotonic() + duration
    last = ""
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast screen:\n{last}")
        last = capture(tmux_client, session)
        if not compiled.search(last):
            raise RuntimeError(f"{label}; expected /{pattern}/\nscreen:\n{last}")
        time.sleep(POLL_INTERVAL)
    return last


def pane_cursor_position(tmux_client: TmuxClient, session: str) -> str:
    return tmux(tmux_client, "display-message", "-p", "-t", session, "#{cursor_x},#{cursor_y}").stdout.strip()


@timed_operation("wait", label_argument="label")
def wait_for_cursor_change(
    tmux_client: TmuxClient, session: str, previous: str, label: str, timeout: float = 8.0
) -> str:
    deadline = time.monotonic() + timeout
    last = previous
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast cursor position: {last}")
        last = pane_cursor_position(tmux_client, session)
        if last != previous:
            return last
        time.sleep(POLL_INTERVAL)
    screen = capture(tmux_client, session)
    raise RuntimeError(f"timed out waiting for {label}; cursor remained at {last}\nscreen:\n{screen}")


@timed_operation("wait", label_argument="label")
def wait_for_pane_command(
    tmux_client: TmuxClient, session: str, pattern: str, label: str, timeout: float = 8.0
) -> str:
    compiled = re.compile(pattern)
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        timing_poll()
        status = tmux(tmux_client, "has-session", "-t", session, check=False)
        if status.returncode != 0:
            raise RuntimeError(f"tmux session exited before {label}\nlast pane command:\n{last}")
        last = pane_current_command(tmux_client, session)
        if compiled.search(last):
            return last
        time.sleep(POLL_INTERVAL)
    screen = capture(tmux_client, session)
    raise RuntimeError(f"timed out waiting for {label}; expected pane command /{pattern}/, last {last}\nscreen:\n{screen}")


def send_keys(tmux_client: TmuxClient, session: str, *keys: str) -> None:
    tmux(tmux_client, "send-keys", "-t", session, *keys)


def send_literal(tmux_client: TmuxClient, session: str, text: str) -> None:
    tmux(tmux_client, "send-keys", "-t", session, "-l", text)


def selected_modal_row(screen: str) -> str:
    for raw_line in screen.splitlines():
        line = raw_line.strip()
        if line.startswith("› "):
            return line
    return ""


def selected_modal_identity(row: str) -> str:
    return re.sub(r"^›\s+(?:[●✓]\s+)?", "", row)


@timed_operation("wait", label_argument="label")
def wait_for_selected_modal_change(
    tmux_client: TmuxClient, session: str, previous: str, label: str, timeout: float = 8.0
) -> tuple[str, str]:
    deadline = time.monotonic() + timeout
    last_screen = ""
    last_row = ""
    while time.monotonic() < deadline:
        timing_poll()
        last_screen = capture(tmux_client, session)
        last_row = selected_modal_row(last_screen)
        if last_row and last_row != previous:
            return last_row, last_screen
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(
        f"timed out waiting for {label}; selected row did not change from {previous!r}\n"
        f"last selected row: {last_row!r}\nscreen:\n{last_screen}"
    )


@timed_operation("wait", label_argument=None, default_label="tmux session exit")
def wait_for_session_exit(tmux_client: TmuxClient, session: str, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        timing_poll()
        if tmux(tmux_client, "has-session", "-t", session, check=False).returncode != 0:
            return
        time.sleep(POLL_INTERVAL)
    screen = capture(tmux_client, session)
    raise RuntimeError(f"tmux session did not exit\nscreen:\n{screen}")


@timed_operation("wait", label_argument="label")
def wait_for_json_file(path: pathlib.Path, predicate: Callable[[object], bool], label: str, timeout: float = 8.0) -> str:
    deadline = time.monotonic() + timeout
    last = ""
    last_error = "file does not exist"
    while time.monotonic() < deadline:
        timing_poll()
        if path.exists():
            last = path.read_text(encoding="utf-8", errors="replace")
            try:
                value = json.loads(last)
                last_error = "predicate not satisfied"
                if predicate(value):
                    return last
            except json.JSONDecodeError as exc:
                last_error = str(exc)
        time.sleep(POLL_INTERVAL)
    raise RuntimeError(f"timed out waiting for {label}; {last_error}\nlast content:\n{last}")


@dataclass
class SmokeContext:
    scenario: str
    root: pathlib.Path
    ava_exe: pathlib.Path
    fake_provider_exe: pathlib.Path
    fake_mermaid_helper_exe: pathlib.Path
    tmux_exe: str
    tmux: TmuxClient = field(init=False)
    workspace: pathlib.Path = field(init=False)
    home: pathlib.Path = field(init=False)
    config: pathlib.Path = field(init=False)
    state: pathlib.Path = field(init=False)
    data: pathlib.Path = field(init=False)
    active_workspace: pathlib.Path = field(init=False)
    active_home: pathlib.Path = field(init=False)
    active_config: pathlib.Path = field(init=False)
    active_state: pathlib.Path = field(init=False)
    active_data: pathlib.Path = field(init=False)
    restore_workspace: pathlib.Path = field(init=False)
    restore_home: pathlib.Path = field(init=False)
    restore_config: pathlib.Path = field(init=False)
    restore_state: pathlib.Path = field(init=False)
    restore_data: pathlib.Path = field(init=False)
    ava_config: pathlib.Path = field(init=False)
    active_ava_config: pathlib.Path = field(init=False)
    restore_ava_config: pathlib.Path = field(init=False)
    import_keybinds_content: str = field(init=False)
    editor_command: str = field(init=False)
    _environment: dict[str, str] = field(init=False)
    _state_directory: tempfile.TemporaryDirectory[str] | None = field(default=None, init=False, repr=False)
    _providers: list[FakeProvider] = field(default_factory=list, init=False)
    _closed: bool = field(default=False, init=False)

    @timed_operation("setup", label_argument=None, default_label="smoke context setup")
    def __post_init__(self) -> None:
        try:
            self._prepare_root()
            tmpdir = self.root / "tmp"
            tmpdir.mkdir(mode=0o700)
            tmux_home = self.root / "tmux-home"
            tmux_home.mkdir(mode=0o700)
            self._environment = _compatibility_environment(home=tmux_home, tmpdir=tmpdir)
            self.tmux = TmuxClient(self.tmux_exe, self.root / "tmux.sock", self._environment)
            self._prepare_fixture()
            # The first new-session command starts the private server atomically.
            # A separate start-server races tmux's default exit-empty teardown
            # because no session exists yet.
        except BaseException:
            self.close()
            raise

    def _prepare_root(self) -> None:
        root = self.root.absolute()
        if root.name != self.scenario or root.parent.name != "tui-tmux-smoke":
            raise RuntimeError(
                "refusing unsafe tmux smoke root; expected "
                f".../tui-tmux-smoke/{self.scenario}, got {root}"
            )
        parent = root.parent
        parent.mkdir(parents=True, exist_ok=True)
        if root.exists() or root.is_symlink():
            if root.is_symlink() or not root.is_dir():
                raise RuntimeError(f"refusing to clear non-directory smoke leaf: {root}")
            # Deliberately remove only this scenario's guarded leaf, never its parent or siblings.
            shutil.rmtree(root)
        root.mkdir(mode=0o700)
        root.chmod(0o700)
        self.root = root

    def _prepare_fixture(self) -> None:
        # Workspaces, XDG config/data, and review artifacts stay under the
        # guarded build evidence root.  AVA pane trusted HOMEs join XDG state
        # under the lifecycle-owned TemporaryDirectory so concurrent sibling
        # scenario mkdir/rmtree cannot perturb shared build-root ancestors that
        # trusted-home freshness captures (HOME is outside the session AnchorSet).
        self.workspace = self.root / "workspace"
        self.config = self.root / "config"
        self.data = self.root / "data"
        self.active_workspace = self.root / "active-workspace"
        self.active_config = self.root / "active-config"
        self.active_data = self.root / "active-data"
        self.restore_workspace = self.root / "restore-workspace"
        self.restore_config = self.root / "restore-config"
        self.restore_data = self.root / "restore-data"

        safe_scenario = re.sub(r"[^A-Za-z0-9_.-]+", "-", self.scenario).strip("-") or "scenario"
        self._state_directory = tempfile.TemporaryDirectory(prefix=f"ava-tui-smoke-{safe_scenario}-")
        state_root = pathlib.Path(self._state_directory.name).resolve(strict=True)
        real_home = pathlib.Path(pwd.getpwuid(os.geteuid()).pw_dir).resolve(strict=True)
        workspace = self.workspace.resolve()
        if any(
            state_root == protected or state_root in protected.parents or protected in state_root.parents
            for protected in (real_home, workspace)
        ):
            raise RuntimeError(
                "refusing tmux smoke state root overlapping trusted paths: "
                f"state root={state_root}, real home={real_home}, workspace={workspace}"
            )
        self.home = state_root / "home"
        self.active_home = state_root / "active-home"
        self.restore_home = state_root / "restore-home"
        self.state = state_root / "state"
        self.active_state = state_root / "active-state"
        self.restore_state = state_root / "restore-state"
        for path in (
            self.workspace,
            self.home,
            self.config,
            self.state,
            self.data,
            self.active_workspace,
            self.active_home,
            self.active_config,
            self.active_state,
            self.active_data,
            self.restore_workspace,
            self.restore_home,
            self.restore_config,
            self.restore_state,
            self.restore_data,
        ):
            path.mkdir(parents=True, exist_ok=True, mode=0o700)
            # Keep scenario fixtures owner-only and deterministic across umask
            # values; private-primary-group behavior has focused C++ coverage.
            path.chmod(0o700)

        self.ava_config = self.config / "ava"
        self.active_ava_config = self.active_config / "ava"
        self.restore_ava_config = self.restore_config / "ava"
        for path in (self.ava_config, self.active_ava_config, self.restore_ava_config):
            path.mkdir(parents=True, exist_ok=True, mode=0o700)
            # These directories hold AVA configuration authority, so unlike a
            # private-group workspace they must stay strictly owner-only.
            path.chmod(0o700)

        self.ava_config.joinpath("models.json").write_text(
            '{"models":[{"provider":"openai","id":"diagnostic-local","name":"Diagnostic Local","supports_reasoning":true}]}\n',
            encoding="utf-8",
        )
        fake_models = (
            '{"default_provider":"moonshot","default_model":"ava-tui-fake",'
            '"models":[{"provider":"moonshot","id":"ava-tui-fake","name":"AVA TUI Fake","family":"fake",'
            '"context_window_tokens":8192,"max_output_tokens":1024,"supports_tools":false,'
            '"supports_streaming":false,"supports_reasoning":false,"reports_usage":true}]}\n'
        )
        self.active_ava_config.joinpath("models.json").write_text(fake_models, encoding="utf-8")
        self.restore_ava_config.joinpath("models.json").write_text(fake_models, encoding="utf-8")

        self.workspace.joinpath("src").mkdir(parents=True, exist_ok=True)
        self.workspace.joinpath("AGENTS.md").write_text("tmux smoke context\n", encoding="utf-8")
        self.workspace.joinpath("src", "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
        self.active_workspace.joinpath("AGENTS.md").write_text("active tmux smoke context\n", encoding="utf-8")
        self.restore_workspace.joinpath("AGENTS.md").write_text("restore tmux smoke context\n", encoding="utf-8")
        self.workspace.joinpath("screen.png").write_bytes(b"\x89PNG\r\n\x1a\nava-tui-image")
        self.import_keybinds_content = '{"tui.editor.cursorLeft":["Left","Alt+H"],"app.tools.expand":"Ctrl+O"}\n'
        self.workspace.joinpath("import-keybinds.json").write_text(self.import_keybinds_content, encoding="utf-8")
        fake_editor = self.root / "fake-editor.sh"
        fake_editor.write_text("#!/bin/sh\nprintf '%s\\n' 'external editor draft' > \"$1\"\n", encoding="utf-8")
        fake_editor.chmod(0o755)
        self.editor_command = f"/bin/sh {shlex.quote(str(fake_editor))}"
        self.workspace.joinpath("my folder").mkdir(parents=True, exist_ok=True)
        self.workspace.joinpath("my folder", "space file.txt").write_text("space path\n", encoding="utf-8")
        self.workspace.joinpath(".ava", "commands").mkdir(parents=True, exist_ok=True)
        self.workspace.joinpath(".ava", "commands", "trust-smoke.md").write_text(
            "---\ndescription: Trust smoke command\n---\nTrust smoke $1\n", encoding="utf-8"
        )
        self.workspace.joinpath(".ava", "APPEND_SYSTEM.md").write_text("tmux project append prompt\n", encoding="utf-8")
        footer_plugin = self.workspace / ".ava" / "plugins" / "com.example.footer"
        footer_plugin.joinpath("prompts").mkdir(parents=True, exist_ok=True)
        footer_plugin.joinpath("prompts", "footer.md").write_text("tmux footer context refresh prompt\n", encoding="utf-8")
        footer_plugin.joinpath("plugin.json").write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "id": "com.example.footer",
                    "name": "Footer Context Smoke",
                    "version": "0.1.0",
                    "api_version": "ava.plugin.v1",
                    "entrypoint": {"command": "/bin/true", "args": []},
                    "capabilities": ["prompts"],
                    "permissions": {"file": [], "shell": [], "network": [], "session": []},
                    "contributes": {
                        "tools": [],
                        "commands": [],
                        "prompts": [
                            {
                                "name": "footer-refresh",
                                "description": "Exercise composer context-count refresh",
                                "path": "prompts/footer.md",
                            }
                        ],
                        "skills": [],
                        "event_hooks": [],
                    },
                }
            )
            + "\n",
            encoding="utf-8",
        )
        plugin_state_dir = self.state / "ava"
        plugin_state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        plugin_state_dir.chmod(0o700)
        plugin_state_dir.joinpath("plugin-enablement.json").write_text(
            json.dumps(
                {
                    "workspaces": {
                        str(self.workspace.absolute()): {"project": {"com.example.footer": {"enabled": True}}}
                    }
                }
            )
            + "\n",
            encoding="utf-8",
        )
        themes = self.ava_config / "themes"
        themes.mkdir(parents=True, exist_ok=True)
        themes.joinpath("ocean.json").write_text(
            "{\n"
            '  "name": "ocean",\n'
            '  "vars": {"primary": "#0066cc", "paper": 255},\n'
            '  "colors": {\n'
            '    "text": "",\n'
            '    "muted": 242,\n'
            '    "success": 34,\n'
            '    "warning": "#ffaa00",\n'
            '    "error": "#ff0000",\n'
            '    "accent": "primary",\n'
            '    "screenBg": "paper",\n'
            '    "composerBg": 236\n'
            "  }\n"
            "}\n",
            encoding="utf-8",
        )

    def session_name(self, label: str) -> str:
        safe_label = re.sub(r"[^a-z0-9]+", "-", label.lower()).strip("-")
        return f"ava-tui-{safe_label}-{uuid.uuid4().hex[:10]}"

    def pane_command(
        self,
        *,
        home: pathlib.Path,
        config: pathlib.Path,
        state: pathlib.Path,
        data: pathlib.Path,
        extra: dict[str, str | pathlib.Path] | None = None,
        exec_ava: bool = True,
    ) -> str:
        # The server itself has only _compatibility_environment.  Build pane
        # variables explicitly from that allowlist plus scenario-owned paths.
        values: dict[str, str | pathlib.Path] = {
            "HOME": home,
            "XDG_CONFIG_HOME": config,
            "XDG_STATE_HOME": state,
            "XDG_DATA_HOME": data,
        }
        if extra:
            values.update(extra)
        assignments = " ".join(_quoted_assignment(name, value) for name, value in values.items())
        executable = shlex.quote(str(self.ava_exe))
        return f"{assignments} {'exec ' if exec_ava else ''}{executable}"

    def main_pane_command(self) -> str:
        return self.pane_command(
            home=self.home,
            config=self.config,
            state=self.state,
            data=self.data,
            extra={
                "NO_COLOR": "1",
                "COLORFGBG": "",
                "VISUAL": "",
                "EDITOR": self.editor_command,
                "AVA_CLIPBOARD_IMAGE_FILE": self.workspace / "screen.png",
            },
        )

    @timed_operation("setup", label_argument="session", default_label="AVA tmux launch")
    def launch_ava(
        self,
        session: str,
        *,
        workspace: pathlib.Path,
        command: str,
        width: int = 120,
        height: int = 32,
    ) -> None:
        tmux(
            self.tmux,
            "new-session",
            "-d",
            "-s",
            session,
            "-x",
            str(width),
            "-y",
            str(height),
            "-c",
            str(workspace),
            command,
        )

    @timed_operation("setup", label_argument="name", default_label="fake provider startup")
    def start_fake_provider(
        self,
        name: str,
        *,
        delay_ms: int,
        scenario: str = "text-three",
        target: str | pathlib.Path = "",
    ) -> FakeProvider:
        """Launch an isolated provider with one inherited process-gate endpoint.

        ``name`` selects private fixture artifacts, ``delay_ms`` paces scenarios
        that intentionally stream deltas, ``scenario`` configures responses and
        request barriers, and ``target`` supplies an optional fixture path.
        The returned owner exposes persistent bidirectional gates and guarantees
        that only the child retains its endpoint after ``Popen`` succeeds.
        """

        provider = launch_fake_provider(
            self.fake_provider_exe,
            self.root,
            prefix=f"{name}-provider",
            delay_ms=delay_ms,
            scenario=scenario,
            target=target,
            environment=self._environment,
        )
        self._providers.append(provider)
        return provider

    def fake_provider_command(
        self,
        provider: FakeProvider,
        *,
        home: pathlib.Path,
        config: pathlib.Path,
        state: pathlib.Path,
        data: pathlib.Path,
        no_color: bool = True,
    ) -> str:
        extra = {
            "COLORFGBG": "",
            "MOONSHOT_API_KEY": "test-key",
            "MOONSHOT_BASE_URL": f"http://127.0.0.1:{provider.port}",
        }
        if no_color:
            extra["NO_COLOR"] = "1"
        return self.pane_command(home=home, config=config, state=state, data=data, extra=extra)

    @timed_operation("cleanup", label_argument=None, default_label="smoke context cleanup")
    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        for provider in reversed(self._providers):
            try:
                provider.stop()
            except (OSError, subprocess.SubprocessError):
                # Continue to the tmux server: leaving AVA panes alive is worse
                # than losing a cleanup diagnostic during signal handling.
                pass
        self._providers.clear()
        tmux_client = getattr(self, "tmux", None)
        try:
            if tmux_client is not None:
                try:
                    tmux_client.close()
                finally:
                    # tmux may leave a dead custom socket pathname after kill-server. It is
                    # inside this scenario's guarded leaf; never touch the shared parent.
                    tmux_client.socket_path.unlink(missing_ok=True)
        finally:
            state_directory = self._state_directory
            if state_directory is not None:
                state_directory.cleanup()
                self._state_directory = None

    def __enter__(self) -> "SmokeContext":
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()
