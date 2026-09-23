"""The tmux TUI smoke scenario for theme persisted."""

from __future__ import annotations

import re

from tui_smoke_helpers import (
    SmokeContext,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
)
from .common import close_settings, open_settings_section


def scenario_theme_persisted(ctx: SmokeContext) -> None:
    tmux_exe = ctx.tmux
    workspace = ctx.workspace
    ava_config = ctx.ava_config
    persisted_theme_session = ctx.session_name("theme-persist")
    display_config = ava_config / "display.json"
    persisted_theme_env_prefix = ctx.pane_command(
        home=ctx.home, config=ctx.config, state=ctx.state, data=ctx.data,
        extra={"NO_COLOR": "", "AVA_TUI_THEME": "", "COLORFGBG": ""},
    )
    (ava_config / "keybinds.json").write_text(
        '{"tui.editor.cursorLineStart":["Home","Ctrl+A"]}\n', encoding="utf-8"
    )
    tmux(
        tmux_exe,
        "new-session",
        "-d",
        "-s",
        persisted_theme_session,
        "-x",
        "100",
        "-y",
        "24",
        "-c",
        str(workspace),
        persisted_theme_env_prefix,
    )
    wait_for(tmux_exe, persisted_theme_session, r"Type a message|live session", "persisted-theme initial TUI frame")
    open_settings_section(
        tmux_exe,
        persisted_theme_session,
        "Theme",
        r"Settings › Theme|Dark|Light|Plain|ocean",
        "persisted-theme display settings",
    )
    send_literal(tmux_exe, persisted_theme_session, "light")
    wait_for(tmux_exe, persisted_theme_session, r"Search\s{2}light", "persisted-theme filtered theme row")
    send_keys(tmux_exe, persisted_theme_session, "Enter")
    # Successful settings actions add no chat receipts; synchronize on the refreshed
    # current-row state inside the still-open Theme section.
    applied_theme = wait_for(
        tmux_exe, persisted_theme_session, r"Light[^\n]*current", "settings theme selection applied"
    )
    if "Stored TUI theme" in applied_theme:
        raise RuntimeError(f"settings modal leaked a theme receipt\nscreen:\n{applied_theme}")
    if not display_config.exists() or '"theme": "light"' not in display_config.read_text(encoding="utf-8"):
        raise RuntimeError(f"settings theme selection did not write display.json\npath:\n{display_config}")
    tmux(tmux_exe, "kill-session", "-t", persisted_theme_session, check=False)

    tmux(
        tmux_exe,
        "new-session",
        "-d",
        "-s",
        persisted_theme_session,
        "-x",
        "100",
        "-y",
        "24",
        "-c",
        str(workspace),
        persisted_theme_env_prefix,
    )
    wait_for(tmux_exe, persisted_theme_session, r"Type a message|live session", "persisted-theme restart TUI frame")
    persisted_theme_modal = open_settings_section(
        tmux_exe,
        persisted_theme_session,
        "Theme",
        r"Settings › Theme|Dark|Light|Plain|ocean",
        "persisted-theme display settings after restart",
    )
    if "Light" not in persisted_theme_modal or "current" not in persisted_theme_modal:
        raise RuntimeError(f"settings modal did not report persisted display.json light theme\nscreen:\n{persisted_theme_modal}")
    close_settings(tmux_exe, persisted_theme_session, "persisted-theme settings modal canceled")
    send_keys(tmux_exe, persisted_theme_session, "C-u")
    open_settings_section(
        tmux_exe,
        persisted_theme_session,
        "Theme",
        r"Settings › Theme|Dark|Light|Plain|ocean",
        "custom-theme display settings",
    )
    send_literal(tmux_exe, persisted_theme_session, "ocean")
    wait_for(tmux_exe, persisted_theme_session, r"Search\s{2}ocean", "custom-theme filtered theme row")
    send_keys(tmux_exe, persisted_theme_session, "Enter")
    applied_custom_theme = wait_for(
        tmux_exe, persisted_theme_session, r"ocean[^\n]*current", "settings custom theme selection applied"
    )
    if "Stored TUI theme" in applied_custom_theme:
        raise RuntimeError(f"settings modal leaked a custom theme receipt\nscreen:\n{applied_custom_theme}")
    if '"theme": "ocean"' not in display_config.read_text(encoding="utf-8"):
        raise RuntimeError(f"settings custom theme selection did not write display.json\npath:\n{display_config}")
    display_config.write_text('{\n  "theme": "plain"\n}\n', encoding="utf-8")
    # Confirm keeps Display open; leave settings before drafting composer commands.
    close_settings(tmux_exe, persisted_theme_session, "closed settings before reload theme")
    send_keys(tmux_exe, persisted_theme_session, "C-u")
    send_literal(tmux_exe, persisted_theme_session, "/reload theme")
    wait_for(tmux_exe, persisted_theme_session, r"/reload theme", "display theme reload draft")
    send_keys(tmux_exe, persisted_theme_session, "Enter")
    reloaded_theme = wait_for(
        tmux_exe, persisted_theme_session, r"display theme reloaded: plain", "display theme reload command"
    )
    if "display theme reloaded: plain" not in reloaded_theme:
        raise RuntimeError(f"/reload theme did not report the externally edited plain theme\nscreen:\n{reloaded_theme}")
    send_keys(tmux_exe, persisted_theme_session, "C-u")
    reloaded_theme_modal = open_settings_section(
        tmux_exe,
        persisted_theme_session,
        "Theme",
        r"Settings › Theme|Dark|Light|Plain|ocean",
        "settings modal after display theme reload",
    )
    if "Plain" not in reloaded_theme_modal or "current" not in reloaded_theme_modal:
        raise RuntimeError(f"settings modal did not report reloaded display.json plain theme\nscreen:\n{reloaded_theme_modal}")
    close_settings(tmux_exe, persisted_theme_session, "reloaded-theme settings modal canceled")
    display_config.write_text('{\n  "theme": "light"\n}\n', encoding="utf-8")
    auto_reloaded_theme = wait_for(
        tmux_exe,
        persisted_theme_session,
        r"display theme auto-reloaded: ava-light",
        "automatic display theme reload",
    )
    if "display theme auto-reloaded: ava-light" not in auto_reloaded_theme:
        raise RuntimeError(f"display.json edit did not auto-reload the light theme\nscreen:\n{auto_reloaded_theme}")
    send_keys(tmux_exe, persisted_theme_session, "C-u")
    auto_reloaded_theme_modal = open_settings_section(
        tmux_exe,
        persisted_theme_session,
        "Theme",
        r"Settings › Theme|Dark|Light|Plain|ocean",
        "settings modal after automatic display theme reload",
    )
    if "Light" not in auto_reloaded_theme_modal or "current" not in auto_reloaded_theme_modal:
        raise RuntimeError(
            f"settings modal did not report auto-reloaded display.json light theme\nscreen:\n{auto_reloaded_theme_modal}"
        )
    close_settings(tmux_exe, persisted_theme_session, "closed settings before backend reload")
    for command, expected in (
        ("/reload models", r"models: loaded"),
        ("/reload permissions", r"permissions: restart-required"),
        ("/reload no-such", r"unsupported reload target: no-such"),
    ):
        send_keys(tmux_exe, persisted_theme_session, "C-u")
        send_literal(tmux_exe, persisted_theme_session, command)
        wait_for(tmux_exe, persisted_theme_session, rf"(?m)^\s*│\s+{re.escape(command)}\s*$", f"{command} draft")
        # Reload suggestions include local aliases: dismiss completion rather
        # than letting Enter replace a backend target with the selected alias.
        send_keys(tmux_exe, persisted_theme_session, "Escape")
        wait_for_absent(tmux_exe, persisted_theme_session, r"(?m)^\s*│\s+›\s+theme\s+Reload", "reload palette dismissed")
        send_keys(tmux_exe, persisted_theme_session, "Enter")
        wait_for(tmux_exe, persisted_theme_session, expected, f"{command} backend report")
    tmux(tmux_exe, "kill-session", "-t", persisted_theme_session, check=False)
