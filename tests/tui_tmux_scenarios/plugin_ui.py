"""Credential-free tmux coverage for host-owned foreground plugin UI."""

from __future__ import annotations

import json
import os
import pathlib
import re
import time

from tui_smoke_helpers import (
    SmokeContext,
    capture,
    save_evidence,
    send_keys,
    send_literal,
    wait_for,
    wait_for_absent,
    wait_for_session_exit,
)


def _write_plugin_fixture(ctx: SmokeContext) -> dict[str, pathlib.Path]:
    plugin_dir = ctx.workspace / ".ava" / "plugins" / "com.example.plugin-ui"
    plugin_dir.mkdir(parents=True, exist_ok=True)
    paths = {
        "plugin_dir": plugin_dir,
        "action_log": plugin_dir / "actions.log",
        "continue_demo": plugin_dir / "continue-demo",
        "continue_exit": plugin_dir / "continue-exit",
        "pid_log": plugin_dir / "child-pids.log",
        "environment_log": plugin_dir / "environment.log",
    }
    plugin_dir.joinpath("plugin.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "id": "com.example.plugin-ui",
                "name": "Plugin UI Smoke",
                "version": "0.1.0",
                "api_version": "ava.plugin.v1",
                "entrypoint": {"command": "/bin/sh", "args": ["plugin.sh"]},
                "capabilities": ["commands", "ui.status", "ui.widget", "ui.select", "ui.confirm"],
                "permissions": {"file": [], "shell": [], "network": [], "session": []},
                "contributes": {
                    "tools": [],
                    "commands": [
                        {"name": "demo", "description": "Exercise the complete host-owned plugin UI flow"},
                        {"name": "ctrlc", "description": "Block in a modal until Ctrl+C"},
                        {"name": "exit", "description": "Exit while a status surface is visible"},
                        {"name": "hostile", "description": "Emit hostile terminal text for containment coverage"},
                    ],
                    "prompts": [],
                    "skills": [],
                    "event_hooks": [],
                },
            }
        )
        + "\n",
        encoding="utf-8",
    )
    plugin_dir.joinpath("plugin.sh").write_text(
        "#!/bin/sh\n"
        "set -eu\n"
        f"printf '%s\\n' $$ >> {paths['pid_log']}\n"
        f"/usr/bin/env >> {paths['environment_log']}\n"
        "IFS= read -r initialize\n"
        "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
        "IFS= read -r command\n"
        "request_id=$(printf '%s' \"$command\" | /bin/sed -n 's/.*\"id\":\"\\([^\"]*\\)\".*/\\1/p')\n"
        "case \"$command\" in\n"
        "  *'\"command\":\"demo\"'*)\n"
        "    printf '%s\\n' '{\"id\":\"tmux-status\",\"type\":\"ui.status\",\"text\":\"TMUX plugin UI working\"}'\n"
        "    IFS= read -r action\n"
        f"    printf '%s\\n' \"$action\" >> {paths['action_log']}\n"
        "    printf '%s\\n' '{\"id\":\"tmux-widget-one\",\"type\":\"ui.widget\",\"title\":\"First bounded widget\",\"lines\":[\"phase one\",\"phase two\"]}'\n"
        "    IFS= read -r action\n"
        f"    printf '%s\\n' \"$action\" >> {paths['action_log']}\n"
        "    printf '%s\\n' '{\"id\":\"tmux-widget-two\",\"type\":\"ui.widget\",\"title\":\"Second bounded widget\",\"lines\":[\"phase three\",\"phase four\"]}'\n"
        "    IFS= read -r action\n"
        f"    printf '%s\\n' \"$action\" >> {paths['action_log']}\n"
        f"    while [ ! -f {paths['continue_demo']} ]; do /bin/sleep 0.02; done\n"
        "    printf '%s\\n' '{\"id\":\"TMUX_HIDDEN_REQUEST_SELECT\",\"type\":\"ui.select\",\"title\":\"Choose a channel\",\"description\":\"Visible labels only\",\"choices\":[{\"id\":\"TMUX_HIDDEN_OPTION_ALPHA\",\"label\":\"Alpha channel\"},{\"id\":\"TMUX_HIDDEN_OPTION_BETA\",\"label\":\"Beta channel\",\"description\":\"recommended\"}]}'\n"
        "    IFS= read -r action\n"
        f"    printf '%s\\n' \"$action\" >> {paths['action_log']}\n"
        "    printf '%s\\n' '{\"id\":\"TMUX_HIDDEN_REQUEST_CONFIRM\",\"type\":\"ui.confirm\",\"title\":\"Apply selection?\",\"description\":\"Host confirmation controls\"}'\n"
        "    IFS= read -r action\n"
        f"    printf '%s\\n' \"$action\" >> {paths['action_log']}\n"
        "    printf '%s\\n' '{\"id\":\"TMUX_HIDDEN_REQUEST_CANCEL\",\"type\":\"ui.confirm\",\"title\":\"Dismiss this step\",\"description\":\"Escape must cancel only this modal\"}'\n"
        "    IFS= read -r action\n"
        f"    printf '%s\\n' \"$action\" >> {paths['action_log']}\n"
        "    printf '%s\\n' \"{\\\"id\\\":\\\"$request_id\\\",\\\"type\\\":\\\"command.result\\\",\\\"ok\\\":true,\\\"content\\\":\\\"PLUGIN_UI_FIXED_RESULT\\\"}\"\n"
        "    ;;\n"
        "  *'\"command\":\"ctrlc\"'*)\n"
        "    printf '%s\\n' '{\"id\":\"tmux-ctrlc-modal\",\"type\":\"ui.confirm\",\"title\":\"Ctrl+C cleanup invocation\",\"description\":\"Stop this process now\"}'\n"
        "    IFS= read -r action\n"
        "    ;;\n"
        "  *'\"command\":\"exit\"'*)\n"
        "    printf '%s\\n' '{\"id\":\"tmux-exit-status\",\"type\":\"ui.status\",\"text\":\"Child exit cleanup surface\"}'\n"
        "    IFS= read -r action\n"
        f"    while [ ! -f {paths['continue_exit']} ]; do /bin/sleep 0.02; done\n"
        "    exit 23\n"
        "    ;;\n"
        "  *'\"command\":\"hostile\"'*)\n"
        "    printf '%s\\n' '{\"id\":\"tmux-hostile\",\"type\":\"ui.confirm\",\"title\":\"HOSTILE_RAW_CANARY_0f31\\u001b]52;c;bad\\u0007\\u009b31m\\u202e\\u0001\",\"description\":\"must be rejected\"}'\n"
        "    ;;\n"
        "esac\n",
        encoding="utf-8",
    )

    enablement_path = ctx.state / "ava" / "plugin-enablement.json"
    enablement = json.loads(enablement_path.read_text(encoding="utf-8"))
    project = enablement.setdefault("workspaces", {}).setdefault(str(ctx.workspace.absolute()), {}).setdefault("project", {})
    project["com.example.plugin-ui"] = {"enabled": True}
    enablement_path.write_text(json.dumps(enablement) + "\n", encoding="utf-8")
    ctx.state.joinpath("ava", "project-trust.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "decisions": [{"path": str(ctx.workspace.resolve()), "trusted": True}],
            }
        )
        + "\n",
        encoding="utf-8",
    )
    return paths


def _approve_permissions_until(ctx: SmokeContext, session: str, pattern: str, label: str) -> str:
    last = ""
    deadline = time.monotonic() + 12.0
    approvals = 0
    compiled = re.compile(pattern, re.DOTALL)
    while time.monotonic() < deadline:
        last = capture(ctx.tmux, session)
        if compiled.search(last):
            return last
        if "Permission required" in last:
            if approvals >= 5:
                raise RuntimeError(f"{label} requested too many permission prompts\nscreen:\n{last}")
            previous = last
            send_keys(ctx.tmux, session, "Tab", "Enter")
            approvals += 1
            transition_deadline = time.monotonic() + 3.0
            while time.monotonic() < transition_deadline:
                if capture(ctx.tmux, session) != previous:
                    break
                time.sleep(0.02)
            else:
                raise RuntimeError(f"timed out waiting for {label} permission approval {approvals} to transition\nscreen:\n{previous}")
            continue
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {label} after {approvals} finite approvals\nscreen:\n{last}")


def _wait_for_action_lines(path: pathlib.Path, count: int, label: str) -> list[str]:
    deadline = time.monotonic() + 4.0
    lines: list[str] = []
    while time.monotonic() < deadline:
        if path.exists():
            lines = [line for line in path.read_text(encoding="utf-8").splitlines() if line]
            if len(lines) >= count:
                return lines
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {label}; action lines={lines}")


def _wait_for_process_exit(pid_log: pathlib.Path, expected_count: int, label: str) -> None:
    deadline = time.monotonic() + 4.0
    observed: list[int] = []
    while time.monotonic() < deadline:
        if pid_log.exists():
            observed = [int(line) for line in pid_log.read_text(encoding="utf-8").splitlines() if line]
        if len(observed) >= expected_count:
            live: list[int] = []
            for pid in observed[:expected_count]:
                try:
                    os.kill(pid, 0)
                except ProcessLookupError:
                    continue
                except PermissionError:
                    live.append(pid)
                else:
                    live.append(pid)
            if not live:
                return
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {label}; observed child pids={observed}")


def _assert_no_ui_persistence(ctx: SmokeContext) -> None:
    forbidden = (
        "TMUX_HIDDEN_REQUEST",
        "TMUX_HIDDEN_OPTION",
        "TMUX plugin UI working",
        "First bounded widget",
        "Second bounded widget",
        "Child exit cleanup surface",
        "HOSTILE_RAW_CANARY_0f31",
    )
    roots = (ctx.home, ctx.config, ctx.state, ctx.data)
    leaked: list[str] = []
    for root in roots:
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            try:
                content = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            if any(token in content for token in forbidden):
                leaked.append(str(path))
    if leaked:
        raise RuntimeError(f"ephemeral plugin UI leaked into session or persistent state: {leaked}")


def _invoke(ctx: SmokeContext, session: str, command: str) -> None:
    send_literal(ctx.tmux, session, command)
    send_keys(ctx.tmux, session, "Enter")


def _dismiss_plugin_command_settlement(ctx: SmokeContext, session: str, command: str) -> None:
    # Modal teardown and child death can precede the host settlement overlay.
    # Typing the next slash during that gap leaves the command in the draft.
    wait_for(
        ctx.tmux,
        session,
        rf"(?s)Command /plugin.*com\.example\.plugin-ui:{command}.*Enter/Esc close",
        f"{command} plugin command settlement",
    )
    send_keys(ctx.tmux, session, "Escape")
    wait_for_absent(ctx.tmux, session, r"Command /plugin", f"{command} plugin command output closed")


def scenario_plugin_ui(ctx: SmokeContext) -> None:
    paths = _write_plugin_fixture(ctx)
    session = ctx.session_name("plugin-ui")
    ctx.launch_ava(session, workspace=ctx.workspace, command=ctx.main_pane_command(), width=80, height=18)
    wait_for(ctx.tmux, session, r"Type a message|live session", "plugin UI initial TUI")

    # Normal, credential-free invocation: status, exactly two widgets, a
    # non-default selection, host confirmation, Esc-only cancellation, and a
    # fixed command.result. The 120s absolute deadline is a bounded coordinator
    # test; this 50-second tmux smoke intentionally does not wait it out.
    _invoke(ctx, session, "/plugin run com.example.plugin-ui demo {}")
    dock = _approve_permissions_until(
        ctx,
        session,
        r"Plugin ID · com\.example\.plugin-ui.*Command · demo.*TMUX plugin UI working.*First bounded widget.*phase one.*phase two.*Second bounded widget.*phase three.*phase four.*Ctrl\+C stop · 120s max",
        "complete plugin UI dock",
    )
    if any(token in dock for token in ("TMUX_HIDDEN_REQUEST", "TMUX_HIDDEN_OPTION")):
        raise RuntimeError(f"dock exposed hidden protocol identifiers\nscreen:\n{dock}")
    save_evidence(ctx.root, "plugin-ui-dock-18-row", dock)
    paths["continue_demo"].write_text("continue\n", encoding="utf-8")

    select_screen = wait_for(
        ctx.tmux,
        session,
        r"(?s)Plugin ID · com\.example\.plugin-ui.*Command · demo.*Select · Choose a channel.*Alpha channel.*Beta channel.*Enter select · Esc cancel · Ctrl\+C stop · 120s max",
        "host-owned plugin selection modal",
    )
    if any(token in select_screen for token in ("TMUX_HIDDEN_REQUEST", "TMUX_HIDDEN_OPTION")):
        raise RuntimeError(f"selection modal exposed protocol identifiers\nscreen:\n{select_screen}")
    send_keys(ctx.tmux, session, "Down", "Enter")

    confirm_screen = wait_for(
        ctx.tmux,
        session,
        r"(?s)Confirm · Apply selection\?.*Confirm plugin action.*› Cancel.*Enter confirm · Esc cancel · Ctrl\+C stop · 120s max",
        "host-owned safe-default confirmation modal",
    )
    send_keys(ctx.tmux, session, "Up", "Enter")
    cancel_screen = wait_for(
        ctx.tmux,
        session,
        r"(?s)Confirm · Dismiss this step.*Escape must cancel only this modal.*Enter confirm · Esc cancel",
        "plugin Escape-only cancellation modal",
    )
    save_evidence(ctx.root, "plugin-ui-confirm-and-cancel", cancel_screen)
    send_keys(ctx.tmux, session, "Escape")
    completed = wait_for(ctx.tmux, session, r"PLUGIN_UI_FIXED_RESULT", "fixed plugin command result")
    cleaned = wait_for_absent(
        ctx.tmux,
        session,
        r"Choose a channel|Apply selection|Dismiss this step|TMUX plugin UI working|First bounded widget|Second bounded widget",
        "normal plugin presentation cleanup",
    )
    if "PLUGIN_UI_FIXED_RESULT" not in completed + cleaned:
        raise RuntimeError(f"fixed command result disappeared during cleanup\nscreen:\n{cleaned}")

    actions = _wait_for_action_lines(paths["action_log"], 6, "complete normal-invocation action sequence")
    decoded = [json.loads(line) for line in actions]
    expected = [
        {"id": "tmux-status", "type": "ui.action", "action": "ack"},
        {"id": "tmux-widget-one", "type": "ui.action", "action": "ack"},
        {"id": "tmux-widget-two", "type": "ui.action", "action": "ack"},
        {
            "id": "TMUX_HIDDEN_REQUEST_SELECT",
            "type": "ui.action",
            "action": "select",
            "option_id": "TMUX_HIDDEN_OPTION_BETA",
        },
        {"id": "TMUX_HIDDEN_REQUEST_CONFIRM", "type": "ui.action", "action": "confirm"},
        {"id": "TMUX_HIDDEN_REQUEST_CANCEL", "type": "ui.action", "action": "cancel"},
    ]
    if decoded != expected:
        raise RuntimeError(f"plugin received unexpected host action sequence: {decoded}")
    _wait_for_process_exit(paths["pid_log"], 1, "normal plugin child cleanup")

    # Ctrl+C cleanup is a separate invocation and must terminate the child.
    _invoke(ctx, session, "/plugin run com.example.plugin-ui ctrlc {}")
    ctrlc_screen = _approve_permissions_until(
        ctx,
        session,
        r"Ctrl\+C cleanup invocation.*Ctrl\+C stop",
        "Ctrl+C plugin modal",
    )
    if "› Cancel" not in ctrlc_screen:
        raise RuntimeError(f"Ctrl+C confirmation did not default to host-owned Cancel\nscreen:\n{ctrlc_screen}")
    send_keys(ctx.tmux, session, "C-c")
    wait_for_absent(ctx.tmux, session, r"Ctrl\+C cleanup invocation|Stop this process now", "plugin presentation cleanup after Ctrl+C")
    _wait_for_process_exit(paths["pid_log"], 2, "Ctrl+C plugin child cleanup")
    _dismiss_plugin_command_settlement(ctx, session, "ctrlc")

    # Child-exit cleanup is independent of Ctrl+C: expose a status, release the
    # deterministic gate, then require process-exit teardown to clear it.
    _invoke(ctx, session, "/plugin run com.example.plugin-ui exit {}")
    _approve_permissions_until(ctx, session, r"Child exit cleanup surface", "child-exit plugin status")
    paths["continue_exit"].write_text("exit\n", encoding="utf-8")
    wait_for_absent(ctx.tmux, session, r"Child exit cleanup surface", "plugin presentation cleanup after child exit")
    _wait_for_process_exit(paths["pid_log"], 3, "exited plugin child cleanup")
    _dismiss_plugin_command_settlement(ctx, session, "exit")

    # Escaped ESC/OSC, C1, bidi, and ordinary controls must fail before any
    # terminal bytes or raw UI fields become public. The TUI remains usable.
    _invoke(ctx, session, "/plugin run com.example.plugin-ui hostile {}")
    hostile = _approve_permissions_until(
        ctx,
        session,
        r"(?s)Command /plugin.*plugin UI (?:capability is unavailable|request is malformed|request is invalid or unauthorized)",
        "hostile plugin UI containment error",
    )
    forbidden_hostile = ("HOSTILE_RAW_CANARY_0f31", "tmux-hostile", "\\u001b", "\\u009b", "\\u202e", "\\u0001", "\u202e", "\x9b")
    if any(token in hostile for token in forbidden_hostile):
        raise RuntimeError(f"hostile plugin UI bytes escaped generic containment\nscreen:\n{hostile}")
    wait_for_absent(ctx.tmux, session, r"must be rejected", "hostile plugin UI cleanup")
    _wait_for_process_exit(paths["pid_log"], 4, "hostile plugin child cleanup")
    _invoke(ctx, session, "/plugins inspect com.example.plugin-ui")
    usable = wait_for(ctx.tmux, session, r"Exercise the complete host-owned plugin UI flow", "terminal usability after hostile plugin input")
    save_evidence(ctx.root, "plugin-ui-hostile-contained", usable)

    environment = paths["environment_log"].read_text(encoding="utf-8")
    forbidden_environment = re.compile(r"(?im)^[A-Z0-9_]*(?:API_?KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL)[A-Z0-9_]*=")
    if forbidden_environment.search(environment):
        raise RuntimeError(f"credential-like provider environment reached plugin process:\n{environment}")
    _assert_no_ui_persistence(ctx)
    save_evidence(ctx.root, "plugin-ui-cleanup", usable)

    send_keys(ctx.tmux, session, "Escape")
    wait_for_absent(ctx.tmux, session, r"Command /plugins", "plugin inspect command output closed")
    send_keys(ctx.tmux, session, "C-d")
    wait_for_session_exit(ctx.tmux, session)
