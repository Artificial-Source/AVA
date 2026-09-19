"""The tmux TUI smoke scenario for main startup trust keybinds."""

from __future__ import annotations

import re

from tui_smoke_helpers import (
    ACTIVE_CONTEXT_STATUS_PATTERN,
    SmokeContext,
    assert_screen_absent_for,
    capture,
    capture_styled,
    pane_cursor_position,
    save_evidence,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
    wait_for_screen_change,
    wait_for_screen_state,
)
from .common import _finish_main, _main_session, clear_settings_filter, close_settings, open_settings_section


def _assert_drawer_frame(screen: str, width: int, height: int, label: str) -> list[str]:
    lines = screen.splitlines()
    if len(lines) != height:
        raise RuntimeError(f"{label} had {len(lines)} rows, expected {height}\nscreen:\n{screen}")
    if "Session overview" not in screen:
        raise RuntimeError(f"{label} did not show the session overview title\nscreen:\n{screen}")
    if "live session" in screen or screen.count("Activity") > 1:
        raise RuntimeError(f"{label} duplicated the automatic side rail\nscreen:\n{screen}")
    if not lines[height - 2].startswith("│  Type a message...") or not lines[height - 1].startswith("│  GPT-5.5 · ctx "):
        raise RuntimeError(f"{label} did not retain the full-width quiet composer on rows {height - 2}/{height - 1}\nscreen:\n{screen}")
    if any(len(line) > width for line in lines):
        raise RuntimeError(f"{label} exceeded the {width}-column capture bound\nscreen:\n{screen}")
    unexpected_controls = [character for character in screen if ord(character) < 32 and character != "\n"]
    if unexpected_controls or "\x1b" in screen:
        raise RuntimeError(f"{label} contained terminal control bytes\nscreen:\n{screen}")
    return lines


def _wait_for_drawer_reflow(tmux_exe: object, session: str, width: int, height: int, label: str) -> str:
    """Wait for a fully valid drawer frame at the target geometry after a resize.

    capture() trims trailing empty columns, so a width-only resize can leave the
    trimmed pane text byte-identical even though the reflow already completed
    (capture_idle_shell documents the same width-only text identity). A
    text-change wait therefore times out on an already-valid frame, so
    synchronize on tmux's authoritative dimensions first and then poll for one
    frame that satisfies every _assert_drawer_frame condition at that geometry.
    """

    dimensions = tmux(
        tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}"
    ).stdout.strip()
    if dimensions != f"{width},{height}":
        raise RuntimeError(f"{label} dimensions were {dimensions}, expected {width},{height}")

    def valid_drawer_frame(screen: str) -> bool:
        try:
            _assert_drawer_frame(screen, width, height, label)
        except RuntimeError:
            # Blank or transient mid-reflow frames are polled past within the
            # bounded wait, never accepted and never failed immediately.
            return False
        return True

    return wait_for_screen_state(tmux_exe, session, valid_drawer_frame, f"{label} valid {width}x{height} drawer frame")


def _normalize_context_section(screen: str) -> str:
    """Return the /context freshness section with modal wrap joins repaired.

    The /context modal wraps rows at the captured width, so a short workspace
    path can split a required field across rows (at 84 columns loaded_bytes=19
    arrived as a row trailing "loade" plus an indented "d_bytes=19" row, and a
    shorter baseline split status=current as "sta"/"tus=current"). Qualifying a
    valid short path must not depend on where the wrap lands, so join the
    captured rows after the last "Context freshness:" marker, removing only
    each row's leading/trailing padding and the newlines. Spaces inside a row
    are kept, so multi-token rows stay intact and split forbidden tokens remain
    detectable by the negative assertions.
    """

    section = screen.rsplit("Context freshness:", 1)[-1]
    return "".join(line.strip() for line in section.splitlines())


def scenario_main_startup_trust_keybinds(ctx: SmokeContext) -> None:
    # Preserve the original precedence assertion without depending on the
    # theme-persistence scenario: NO_COLOR must override a stored light theme.
    ctx.ava_config.joinpath("display.json").write_text('{"theme":"light"}\n', encoding="utf-8")
    tmux_exe, root, workspace, ava_config, env_prefix, session = _main_session(ctx)
    import_keybinds_content = ctx.import_keybinds_content
    initial = wait_for(tmux_exe, session, r"Type a message", "fresh startup composer")
    if "Type a message" not in initial or "First-run setup" in initial:
        raise RuntimeError(f"fresh startup did not enter the normal composer directly\nscreen:\n{initial}")
    if "! OpenAI not connected · /connect" not in initial or "auth.json" in initial or "OPENAI_API_KEY" in initial:
        raise RuntimeError(f"first-run auth guidance was not one actionable path-free row\nscreen:\n{initial}")
    onboarding_marker = ctx.state / "ava" / "onboarding.json"
    if onboarding_marker.exists():
        raise RuntimeError(f"fresh TUI startup created an onboarding marker: {onboarding_marker}")
    footer_lines = [line for line in initial.splitlines() if "GPT-5.5" in line and "ctx " in line]
    footer_text = footer_lines[-1].removeprefix("│  ").strip() if footer_lines else ""
    if not re.fullmatch(rf"GPT-5\.5 · ctx {ACTIVE_CONTEXT_STATUS_PATTERN}", footer_text) or any(
        marker in footer_text for marker in ("Build", "OpenAI", "cwd ", "git ", "entries ")
    ):
        raise RuntimeError(
            "composer footer did not contain only the model name and active context usage\n"
            f"screen:\n{initial}"
        )
    save_evidence(root, "startup-ready-composer", initial)
    styled_initial = capture_styled(tmux_exe, session)
    if "\x1b[" in styled_initial:
        raise RuntimeError(f"NO_COLOR=1 TUI frame still captured ANSI style escapes\nscreen:\n{styled_initial}")

    def wait_for_idle_composer_reflow(width: int, height: int, label: str, *, sidebar_expected: bool = False) -> tuple[str, list[str]]:
        input_row = height - 2
        footer_row = height - 1
        canvas_left = 0 if sidebar_expected or width <= 120 else (width - 120) // 2
        inset = " " * canvas_left
        settled = wait_for(
            tmux_exe,
            session,
            rf"(?m)\A(?:[^\n]*\n){{{input_row}}}{inset}│  Type a message\.\.\.[^\n]*\n{inset}│  GPT-5\.5 · ctx {ACTIVE_CONTEXT_STATUS_PATTERN}[^\n]*(?:\n|\Z)",
            f"{label} target composer/footer rows {input_row}/{footer_row}",
        )
        settled_lines = settled.splitlines()
        if len(settled_lines) <= footer_row:
            raise RuntimeError(
                f"{label} did not contain target composer/footer rows {input_row}/{footer_row}\nscreen:\n{settled}"
            )
        if not settled_lines[input_row].startswith(inset + "│  Type a message..."):
            raise RuntimeError(
                f"{label} input row did not start with the quiet composer prefix at row {input_row}\nscreen:\n{settled}"
            )
        if not settled_lines[footer_row].startswith(inset + "│  "):
            raise RuntimeError(
                f"{label} footer did not start with the quiet composer prefix at row {footer_row}\nscreen:\n{settled}"
            )
        if "❯" in settled:
            raise RuntimeError(f"{label} retained the removed composer prompt glyph\nscreen:\n{settled}")
        return settled, settled_lines

    def capture_idle_shell(width: int, height: int, name: str, sidebar_expected: bool) -> None:
        tmux(tmux_exe, "resize-window", "-t", session, "-x", str(width), "-y", str(height))
        # A width-only resize can be textually identical when the short-height
        # layout intentionally hides the sidebar, so synchronize on tmux's
        # authoritative dimensions and the settled composer rows below.
        dimensions = tmux(
            tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}"
        ).stdout.strip()
        if dimensions != f"{width},{height}":
            raise RuntimeError(f"{name} dimensions were {dimensions}, expected {width},{height}")
        rail_divider = rf"(?m)^.{{{width - 39}}}│"
        if sidebar_expected:
            wait_for(tmux_exe, session, rail_divider, f"{name} automatic rail redraw")
        elif width == 160:
            # Exercise the delayed negative check only at the two meaningful
            # boundaries: just below the width threshold and above it with a
            # height that suppresses the rail. The settled-frame assertions
            # below still verify every narrower layout immediately.
            assert_screen_absent_for(
                tmux_exe,
                session,
                rail_divider,
                f"{name} full-width redraw without automatic rail",
            )
        settled, settled_lines = wait_for_idle_composer_reflow(width, height, name, sidebar_expected=sidebar_expected)
        if re.search(r"traceback|assert(?:ion)?|failure", settled, re.IGNORECASE):
            raise RuntimeError(f"{name} idle frame shows failure text\nscreen:\n{settled}")
        canvas_left = 0 if sidebar_expected or width <= 120 else (width - 120) // 2
        canvas_width = width - 39 if sidebar_expected else min(width, 120)
        settled_footer = settled_lines[height - 1][canvas_left + 3 : canvas_left + canvas_width]
        settled_footer = settled_footer.strip()
        if not re.fullmatch(rf"GPT-5\.5 · ctx {ACTIVE_CONTEXT_STATUS_PATTERN}", settled_footer):
            raise RuntimeError(
                f"{name} footer did not contain only the idle model name and active context usage\nscreen:\n{settled}"
            )
        if sidebar_expected:
            main_width = width - 39
            if any(len(line) <= main_width or line[main_width] != "│" for line in settled_lines):
                raise RuntimeError(f"{name} did not keep one rail divider at main width {main_width}\nscreen:\n{settled}")
            rail_lines = [line[main_width + 1 :] for line in settled_lines]
            rail_text = "\n".join(rail_lines)
            if not any(line.startswith("  Session") for line in rail_lines):
                raise RuntimeError(f"{name} did not show the two-cell-inset Session title\nscreen:\n{settled}")
            if "build · openai/GPT-5.5" not in rail_text:
                raise RuntimeError(f"{name} did not show compact mode/provider/model metadata\nscreen:\n{settled}")
            omitted = (
                "AVA",
                "live session",
                "Activity",
                "Modified Files",
                "idle",
                "no file changes",
                "unknown",
                "session ",
                "path ",
                "entries ",
                "cwd ",
                "workspace ",
                "version ",
            )
            if any(value in rail_text for value in omitted):
                raise RuntimeError(f"{name} automatic rail retained branding, placeholders, or raw metadata\nscreen:\n{settled}")
            if any("│" in line for line in rail_lines):
                raise RuntimeError(f"{name} automatic rail contained a duplicate divider\nscreen:\n{settled}")
        elif "live session" in settled or "Activity" in settled or "  Session" in settled:
            raise RuntimeError(f"{name} unexpectedly showed the automatic rail\nscreen:\n{settled}")
        if width == 160 and not sidebar_expected:
            expected_prefix = " " * 20 + "│  "
            if not settled_lines[height - 2].startswith(expected_prefix) or not settled_lines[height - 1].startswith(expected_prefix):
                raise RuntimeError(f"{name} did not retain the exact 20-column centered canvas inset\nscreen:\n{settled}")
        unexpected_controls = [
            character for character in settled if ord(character) < 32 and character != "\n"
        ]
        if unexpected_controls:
            raise RuntimeError(f"{name} saved frame contains unexpected C0 controls\nscreen:\n{settled}")
        save_evidence(root, name, settled)

    capture_idle_shell(176, 48, "frontend-f1-roomy-idle-composer", sidebar_expected=True)
    capture_idle_shell(160, 48, "frontend-f1-wide-idle-composer", sidebar_expected=False)
    capture_idle_shell(120, 36, "frontend-f1-ordinary-idle-composer", sidebar_expected=False)
    capture_idle_shell(80, 24, "frontend-f1-narrow-idle-composer", sidebar_expected=False)
    capture_idle_shell(100, 12, "frontend-f1-short-idle-composer", sidebar_expected=False)
    capture_idle_shell(160, 12, "frontend-f1-short-wide-auto-sidebar-hidden", sidebar_expected=False)

    def open_sidebar_drawer(width: int, height: int, label: str) -> tuple[str, str]:
        previous = capture(tmux_exe, session)
        tmux(tmux_exe, "resize-window", "-t", session, "-x", str(width), "-y", str(height))
        if capture(tmux_exe, session) == previous:
            wait_for_screen_change(tmux_exe, session, previous, f"{label} resize redraw")
        wait_for_idle_composer_reflow(width, height, f"{label} idle before drawer")
        dimensions = tmux(
            tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}"
        ).stdout.strip()
        if dimensions != f"{width},{height}":
            raise RuntimeError(f"{label} dimensions were {dimensions}, expected {width},{height}")
        cursor_before = pane_cursor_position(tmux_exe, session)
        send_literal(tmux_exe, session, "/sidebar")
        wait_for(tmux_exe, session, r"/sidebar", f"{label} command draft")
        send_keys(tmux_exe, session, "Enter")
        drawer = wait_for(tmux_exe, session, r"(?s)Session overview.*│  Type a message\.\.\.", f"{label} opened")
        _assert_drawer_frame(drawer, width, height, label)
        if drawer.count("Activity") != 1:
            raise RuntimeError(f"{label} did not show exactly one Activity section\nscreen:\n{drawer}")
        cursor_flag = tmux(tmux_exe, "display-message", "-p", "-t", session, "#{cursor_flag}").stdout.strip()
        if cursor_flag in ("0", "1") and cursor_flag != "0":
            raise RuntimeError(f"{label} left the composer cursor visible while drawer-focused")
        return drawer, cursor_before

    narrow_drawer, narrow_cursor_before = open_sidebar_drawer(80, 24, "narrow sidebar drawer")
    save_evidence(root, "frontend-f1-narrow-sidebar-drawer", narrow_drawer)
    scrolled_drawer = narrow_drawer
    for page in range(16):
        if "context sources" in scrolled_drawer and "version AVA " in scrolled_drawer:
            break
        previous = scrolled_drawer
        send_keys(tmux_exe, session, "PageDown")
        scrolled_drawer = wait_for_screen_change(tmux_exe, session, previous, f"narrow sidebar drawer page {page + 1}")
        _assert_drawer_frame(scrolled_drawer, 80, 24, "narrow sidebar drawer scrolled")
    if "context sources" not in scrolled_drawer or "version AVA " not in scrolled_drawer:
        raise RuntimeError(f"narrow sidebar drawer could not reach lower context/version fields\nscreen:\n{scrolled_drawer}")
    save_evidence(root, "frontend-f1-narrow-sidebar-drawer-scrolled", scrolled_drawer)
    send_keys(tmux_exe, session, "Escape")
    closed_narrow = wait_for_absent(tmux_exe, session, r"Session overview", "narrow sidebar drawer closed")
    if "live session" in closed_narrow or pane_cursor_position(tmux_exe, session) != narrow_cursor_before:
        raise RuntimeError(f"narrow sidebar drawer did not restore full-width empty composer focus\nscreen:\n{closed_narrow}")

    short_drawer, _ = open_sidebar_drawer(100, 12, "short sidebar drawer")
    send_keys(tmux_exe, session, "End")
    short_drawer_end = wait_for(tmux_exe, session, r"context sources|version AVA ", "short sidebar drawer end")
    if "context sources" not in short_drawer_end or "version AVA " not in short_drawer_end:
        previous = short_drawer_end
        send_keys(tmux_exe, session, "PageDown")
        short_drawer_end = wait_for_screen_change(tmux_exe, session, previous, "short sidebar drawer page down after End")
    _assert_drawer_frame(short_drawer_end, 100, 12, "short sidebar drawer")
    if "context sources" not in short_drawer_end or "version AVA " not in short_drawer_end:
        raise RuntimeError(f"short sidebar drawer could not reach lower context/version fields\nscreen:\n{short_drawer_end}")
    save_evidence(root, "frontend-f1-short-sidebar-drawer", short_drawer_end)

    tmux(tmux_exe, "resize-window", "-t", session, "-x", "160", "-y", "12")
    # A width-only resize can leave the trimmed capture byte-identical, so a
    # text-change wait times out on an already-valid reflow; synchronize on
    # tmux's authoritative dimensions and a fully valid drawer frame instead.
    reflowed_drawer = _wait_for_drawer_reflow(tmux_exe, session, 160, 12, "open sidebar drawer 160x12 reflow")
    _assert_drawer_frame(reflowed_drawer, 160, 12, "reflowed short-wide sidebar drawer")
    send_keys(tmux_exe, session, "Escape")
    short_wide_closed = wait_for_absent(tmux_exe, session, r"Session overview", "short-wide sidebar drawer closed")
    if "live session" in short_wide_closed or "Activity" in short_wide_closed:
        raise RuntimeError(f"160x12 automatic sidebar appeared after drawer closed\nscreen:\n{short_wide_closed}")

    restore_previous = capture(tmux_exe, session)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "32")
    wait_for_screen_change(tmux_exe, session, restore_previous, "startup baseline restore redraw")
    restored_dimensions = tmux(
        tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}"
    ).stdout.strip()
    if restored_dimensions != "120,32":
        raise RuntimeError(f"startup baseline restore dimensions were {restored_dimensions}, expected 120,32")
    restored_startup, _ = wait_for_idle_composer_reflow(120, 32, "startup baseline restored")
    if "live session" in restored_startup or "Activity" in restored_startup or "  Session" in restored_startup:
        raise RuntimeError(f"startup baseline restore unexpectedly showed the automatic rail\nscreen:\n{restored_startup}")

    send_literal(tmux_exe, session, "/copy")
    wait_for(tmux_exe, session, r"/copy", "empty copy command draft")
    send_keys(tmux_exe, session, "Enter")
    empty_copy = wait_for(tmux_exe, session, r"no AVA messages to copy", "empty /copy status")
    if "no AVA messages to copy" not in empty_copy:
        raise RuntimeError(f"/copy without prior AVA messages did not report the empty copy state\nscreen:\n{empty_copy}")
    send_keys(tmux_exe, session, "C-u")

    settings_modal = open_settings_section(
        tmux_exe,
        session,
        "Theme",
        r"Settings › Theme|Plain",
        "settings theme section for NO_COLOR",
    )
    if "Plain" not in settings_modal or "current" not in settings_modal:
        raise RuntimeError(f"settings modal did not report the active NO_COLOR plain mode\nscreen:\n{settings_modal}")
    save_evidence(root, "settings-plain-no-color", settings_modal)
    styled_settings = capture_styled(tmux_exe, session)
    if "\x1b[" in styled_settings:
        raise RuntimeError(f"NO_COLOR=1 settings modal still captured ANSI style escapes\nscreen:\n{styled_settings}")
    close_settings(tmux_exe, session, "settings closed after plain display check")

    open_settings_section(
        tmux_exe,
        session,
        "Workspace",
        r"Settings › Workspace|Trust project",
        "settings sessions section for trust rows",
    )
    # The Workspace modal header carries the path-free trust summary directly; rows are
    # mutations only. Detailed diagnostics stay with explicit /trust status below.
    if not re.search(r"trust unknown · skipped · 3 protected", capture(tmux_exe, session)):
        raise RuntimeError(
            f"settings modal did not show the workspace trust summary\nscreen:\n{capture(tmux_exe, session)}"
        )
    send_literal(tmux_exe, session, "trust")
    settings_trust_rows = wait_for(
        tmux_exe, session, r"Search\s{2}trust", "settings trust filtered rows"
    )
    if (
        "Trust project" not in settings_trust_rows
        or "Deny project" not in settings_trust_rows
        or "Clear trust decision" not in settings_trust_rows
    ):
        raise RuntimeError(
            f"settings modal did not expose project trust mutation rows\nscreen:\n{settings_trust_rows}"
        )
    close_settings(tmux_exe, session, "settings modal closed after trust rows")

    open_settings_section(
        tmux_exe,
        session,
        "Input",
        r"Settings › Input|Keybindings|Edit config",
        "settings input section before keybinding rows",
    )
    send_literal(tmux_exe, session, "Keybindings")
    wait_for(tmux_exe, session, r"Search\s{2}Keybindings", "settings keybinding filter state")
    settings_keybinding_rows = clear_settings_filter(tmux_exe, session, "settings keybinding filter cleared for mouse rows")
    keybindings_row = next(
        (
            (index + 1, line)
            for index, line in enumerate(settings_keybinding_rows.splitlines())
            if "Keybindings" in line
            and "Edit config" not in line
            and "Reload" not in line
        ),
        None,
    )
    if keybindings_row is None:
        raise RuntimeError(
            f"settings keybinding rows did not expose a clickable open row\nscreen:\n{settings_keybinding_rows}"
        )
    keybindings_row_number, keybindings_row_text = keybindings_row
    keybindings_column = max(1, len(keybindings_row_text) - len(keybindings_row_text.lstrip()) + 4)
    # Click a non-selected sibling first so the later open-row click must move selection.
    edit_row = next(
        (
            (index + 1, line)
            for index, line in enumerate(settings_keybinding_rows.splitlines())
            if "Edit config" in line
        ),
        None,
    )
    if edit_row is not None:
        edit_row_number, edit_row_text = edit_row
        edit_column = max(1, len(edit_row_text) - len(edit_row_text.lstrip()) + 4)
        before_file = capture(tmux_exe, session)
        send_literal(tmux_exe, session, f"\x1b[<0;{edit_column};{edit_row_number}M")
        wait_for_screen_change(tmux_exe, session, before_file, "settings keybindings edit mouse select")
    before_mouse = capture(tmux_exe, session)
    send_literal(tmux_exe, session, f"\x1b[<0;{keybindings_column};{keybindings_row_number}M")
    # Settings mouse clicks select/highlight only; Enter still confirms the route.
    mouse_selected = wait_for_screen_change(
        tmux_exe, session, before_mouse, "settings keybindings mouse selection redraw"
    )
    if "Search keybindings" in mouse_selected:
        raise RuntimeError(
            f"settings mouse click persisted/opened keybindings instead of selecting only\nscreen:\n{mouse_selected}"
        )
    if "Settings › Input" not in mouse_selected and "Keybindings" not in mouse_selected:
        raise RuntimeError(
            f"settings mouse click left the input settings section unexpectedly\nscreen:\n{mouse_selected}"
        )
    send_keys(tmux_exe, session, "Enter")
    settings_opened_hotkeys = wait_for(
        tmux_exe, session, r"Search keybindings|Toggle build/plan mode|mode_toggle", "settings Enter opens keybindings view"
    )
    if "Search keybindings" not in settings_opened_hotkeys:
        raise RuntimeError(
            f"settings keybindings row Enter did not open the active keybindings view\nscreen:\n{settings_opened_hotkeys}"
        )
    send_literal(tmux_exe, session, "cursor_left")
    filtered_cursor_left = wait_for(
        tmux_exe, session, r"Move cursor left|cursor_left", "settings-opened keybindings filtered action"
    )
    if "Move cursor left" not in filtered_cursor_left or "cursor_left" not in filtered_cursor_left:
        raise RuntimeError(
            f"settings-opened keybindings view did not show human label with machine id for cursor_left\nscreen:\n{filtered_cursor_left}"
        )
    send_keys(tmux_exe, session, "Enter")
    hotkeys_edit_draft = wait_for(
        tmux_exe, session, r"/keybindings set cursor_left", "settings-opened keybindings drafts selected action"
    )
    if "/keybindings set cursor_left" not in hotkeys_edit_draft:
        raise RuntimeError(
            f"settings-opened keybindings view did not draft the selected action edit command\nscreen:\n{hotkeys_edit_draft}"
        )

    send_keys(tmux_exe, session, "C-u")
    open_settings_section(
        tmux_exe,
        session,
        "Input",
        r"Settings › Input|Edit config",
        "settings input section before keybinding edit",
    )
    send_literal(tmux_exe, session, "edit config")
    wait_for(tmux_exe, session, r"Edit config", "settings keybinding edit row")
    send_keys(tmux_exe, session, "Enter")
    # Anchor on the composer draft row. Bare "/keybindings set" already appears in the
    # settings row description, so an unanchored wait races into open_settings_root while
    # the modal is still closing and can lose the leading "/s" under ESC coalescing.
    settings_edit_draft = wait_for(
        tmux_exe,
        session,
        r"(?m)^\s*│\s+/keybindings set(?:\s|$)",
        "settings keybinding edit drafts command",
    )
    if not re.search(r"(?m)^\s*│\s+/keybindings set(?:\s|$)", settings_edit_draft):
        raise RuntimeError(f"settings keybinding edit row did not draft /keybindings set\nscreen:\n{settings_edit_draft}")

    open_settings_section(
        tmux_exe,
        session,
        "Input",
        r"Settings › Input|Reload",
        "settings input section before keybinding reload",
    )
    send_literal(tmux_exe, session, "reload")
    settings_reload_row = wait_for(
        tmux_exe, session, r"Search\s{2}reload[^\n]*\n(?:[^\n]*\n)*[^\n]*Reload", "settings keybinding reload row"
    )
    if "Reload" not in settings_reload_row:
        raise RuntimeError(
            f"settings modal did not expose keybinding reload guidance when filtered\nscreen:\n{settings_reload_row}"
        )
    send_keys(tmux_exe, session, "Enter")
    # Successful reloads add no chat receipt and return to the composer; success statuses
    # never render in the alert-only status dock, so any visible confirmation text would
    # be an administrative chat receipt.
    settings_reload = wait_for_absent(tmux_exe, session, r"Settings ›", "settings keybinding reload action")
    if "keybindings reloaded" in settings_reload:
        raise RuntimeError(f"settings keybinding reload leaked an administrative chat receipt\nscreen:\n{settings_reload}")

    send_keys(tmux_exe, session, "C-u")
    open_settings_section(
        tmux_exe,
        session,
        "Model",
        r"Settings › Model|Model|Reasoning",
        "settings models section before model selector",
    )
    send_literal(tmux_exe, session, "Model")
    settings_model_selector_row = wait_for(
        tmux_exe, session, r"Search\s{2}Model", "settings model selector row"
    )
    if "Model" not in settings_model_selector_row or "openai/GPT" not in settings_model_selector_row:
        raise RuntimeError(
            f"settings modal did not expose the model selector action\nscreen:\n{settings_model_selector_row}"
        )
    send_keys(tmux_exe, session, "Enter")
    settings_opened_model_selector = wait_for(
        tmux_exe, session, r"Select model|Search models", "settings opens model selector"
    )
    if "Select model" not in settings_opened_model_selector and "Search models" not in settings_opened_model_selector:
        raise RuntimeError(
            f"settings model selector row did not open the model selector\nscreen:\n{settings_opened_model_selector}"
        )
    save_evidence(root, "settings-model-selector", settings_opened_model_selector)
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Select model|Search models", "settings-opened model selector canceled")

    send_keys(tmux_exe, session, "C-u")
    open_settings_section(
        tmux_exe,
        session,
        "Model",
        r"Settings › Model|Cycle scope",
        "settings models section before scoped models",
    )
    send_literal(tmux_exe, session, "cycle scope")
    settings_scoped_model_row = wait_for(
        tmux_exe, session, r"Search\s{2}cycle scope", "settings scoped model row"
    )
    if (
        "Cycle scope" not in settings_scoped_model_row
        or "Ctrl+P" not in settings_scoped_model_row
    ):
        raise RuntimeError(
            f"settings modal did not expose scoped model-cycle persistence guidance\nscreen:\n{settings_scoped_model_row}"
        )
    send_keys(tmux_exe, session, "Enter")
    settings_opened_scoped_model_selector = wait_for(
        tmux_exe, session, r"Scoped model cycle|Search models", "settings opens scoped model selector"
    )
    if "Scoped model cycle" not in settings_opened_scoped_model_selector:
        raise RuntimeError(
            f"settings scoped model row did not open the scoped cycle selector\nscreen:\n{settings_opened_scoped_model_selector}"
        )
    save_evidence(root, "settings-scoped-model-selector", settings_opened_scoped_model_selector)
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(
        tmux_exe, session, r"Scoped model cycle|Search models", "settings-opened scoped model selector canceled"
    )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/context")
    wait_for(tmux_exe, session, r"› /context|> /context", "context command palette")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"› /context|> /context", "context palette dismissed")
    send_keys(tmux_exe, session, "Enter")
    context_freshness = wait_for(
        tmux_exe, session, r"(?s)Context freshness:.*context_sources=1.*Enter/Esc close", "context freshness command"
    )
    context_freshness_section = _normalize_context_section(context_freshness)
    if (
        "prompt=builtin" not in context_freshness_section
        or "context_sources=1" not in context_freshness_section
        or "loaded_bytes=19" not in context_freshness_section
        or "status=current" not in context_freshness_section
        or "project_resources=skipped" not in context_freshness_section
        or "system_prompt_sources=0" not in context_freshness_section
    ):
        raise RuntimeError(f"/context did not report prompt and context freshness visibly\nscreen:\n{context_freshness}")
    if "trust-smoke" in context_freshness_section:
        raise RuntimeError(f"/context listed an untrusted project prompt command\nscreen:\n{context_freshness}")
    if "APPEND_SYSTEM" in context_freshness_section:
        raise RuntimeError(f"/context listed an untrusted project append-system prompt\nscreen:\n{context_freshness}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/trust status")
    wait_for(tmux_exe, session, r"/trust status", "trust status draft")
    send_keys(tmux_exe, session, "Enter")
    trust_status = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /trust.*Project trust:.*decision=unknown.*project_resources=skipped.*protected_resources=3.*prompt_commands.*plugins.*system_prompt.*Enter/Esc close",
        "complete trust status command modal",
    )
    if (
        "protected_resources=3" not in trust_status
        or "prompt_commands" not in trust_status
        or "plugins" not in trust_status
        or "system_prompt" not in trust_status
    ):
        raise RuntimeError(f"/trust status did not list protected project resources\nscreen:\n{trust_status}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/trust project")
    wait_for(tmux_exe, session, r"/trust project", "trust project draft")
    send_keys(tmux_exe, session, "Enter")
    trust_project = wait_for(
        tmux_exe, session, r"(?s)trusted project resources.*project_resources=enabled", "trust project command"
    )
    if "decision=trusted" not in trust_project:
        raise RuntimeError(f"/trust project did not persist a trusted decision\nscreen:\n{trust_project}")
    trust_project = wait_for(
        tmux_exe,
        session,
        rf"GPT-5\.5\s+·\s+ctx {ACTIVE_CONTEXT_STATUS_PATTERN}",
        "composer footer active context meter after project trust reload",
    )
    save_evidence(root, "footer-active-context-meter-refreshed", trust_project)

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/context trust-smoke ")
    wait_for(tmux_exe, session, r"│  /context trust-smoke", "trusted context query draft")
    send_keys(tmux_exe, session, "Enter")
    trusted_context = wait_for(
        tmux_exe,
        session,
        r"(?s)project_trust=trusted project_resources=enabled.*prompt_command\s+project\s+trust-smoke",
        "trusted project prompt command freshness",
    )
    trusted_context_compact = re.sub(r"\s+", "", trusted_context)
    if "status=current" not in trusted_context_compact or "context_sources=2" not in trusted_context_compact:
        raise RuntimeError(f"/context did not retain trusted instruction-source diagnostics\nscreen:\n{trusted_context}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/context APPEND_SYSTEM ")
    wait_for(tmux_exe, session, r"│  /context APPEND_SYSTEM", "trusted append-system context query draft")
    send_keys(tmux_exe, session, "Enter")
    trusted_prompt_context = wait_for(
        tmux_exe,
        session,
        r"(?s)project_trust=trusted project_resources=enabled.*append_system_prompt\s+project\s+APPEND_SYSTEM\.md",
        "trusted project append-system freshness",
    )
    if "status=current" not in re.sub(r"\s+", "", trusted_prompt_context):
        raise RuntimeError(f"/context did not report trusted append-system prompt freshness\nscreen:\n{trusted_prompt_context}")

    (ava_config / "keybinds.json").unlink(missing_ok=True)
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings init")
    wait_for(tmux_exe, session, r"│  /keybindings init(?:\s|$)", "keybindings init command draft")
    wait_for(tmux_exe, session, r"Create \$XDG_CONFIG_HOME/ava/keybinds\.json", "keybindings init completion row")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Create \$XDG_CONFIG_HOME/ava/keybinds\.json", "keybindings init palette dismissed")
    send_keys(tmux_exe, session, "Enter")
    keybindings_init = wait_for(
        tmux_exe, session, r"Created keybindings starter file", "keybindings starter init command"
    )
    if "Created keybindings starter file" not in keybindings_init:
        raise RuntimeError(f"/keybindings init did not report starter-file creation\nscreen:\n{keybindings_init}")
    keybinds_content = (ava_config / "keybinds.json").read_text(encoding="utf-8")
    if '"tui.input.submit"' not in keybinds_content or '"tui.editor.cursorLeft"' not in keybinds_content:
        raise RuntimeError(f"/keybindings init wrote an unexpected starter file\ncontent:\n{keybinds_content}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings init")
    wait_for(tmux_exe, session, r"│  /keybindings init(?:\s|$)", "keybindings existing init command draft")
    wait_for(tmux_exe, session, r"Create \$XDG_CONFIG_HOME/ava/keybinds\.json", "keybindings existing init completion row")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Create \$XDG_CONFIG_HOME/ava/keybinds\.json", "keybindings existing init palette dismissed")
    send_keys(tmux_exe, session, "Enter")
    keybindings_init_existing = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /keybindings.*keybindings file already exists.*Use /keybindings init --force.*Enter/Esc close",
        "complete keybindings starter overwrite refusal modal",
    )
    if "--force" not in keybindings_init_existing:
        raise RuntimeError(
            f"/keybindings init did not explain the explicit overwrite path\nscreen:\n{keybindings_init_existing}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings init --force")
    wait_for(tmux_exe, session, r"/keybindings init --force", "keybindings force init draft")
    send_keys(tmux_exe, session, "Enter")
    keybindings_init_force = wait_for(
        tmux_exe, session, r"Replaced keybindings starter file", "keybindings starter force command"
    )
    if "Replaced keybindings starter file" not in keybindings_init_force:
        raise RuntimeError(f"/keybindings init --force did not replace the starter file\nscreen:\n{keybindings_init_force}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings validate")
    wait_for(tmux_exe, session, r"/keybindings validate|Validate \$XDG_CONFIG_HOME/ava/keybinds", "keybindings validate palette row")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Validate \$XDG_CONFIG_HOME/ava/keybinds", "keybindings validate palette dismissed")
    send_keys(tmux_exe, session, "Enter", "Enter")
    keybindings_validate = wait_for(
        tmux_exe, session, r"keybindings file is valid", "keybindings validate command"
    )
    if "keybindings file is valid" not in keybindings_validate:
        raise RuntimeError(f"/keybindings validate did not report a valid starter file\nscreen:\n{keybindings_validate}")

    (ava_config / "keybinds.json").write_text(
        '{"submit":"Ctrl+P","model_cycle_forward":"Ctrl+P"}\n', encoding="utf-8"
    )
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings validate")
    wait_for(tmux_exe, session, r"/keybindings validate|Validate \$XDG_CONFIG_HOME/ava/keybinds", "invalid keybindings validate palette row")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Validate \$XDG_CONFIG_HOME/ava/keybinds", "invalid keybindings validate palette dismissed")
    send_keys(tmux_exe, session, "Enter", "Enter")
    invalid_keybindings_validate = wait_for(
        tmux_exe, session, r"keybindings file is invalid|conflicting TUI keybinding|Ctrl\+P", "invalid keybindings validate command"
    )
    if "keybindings file is invalid" not in invalid_keybindings_validate or "Ctrl+P" not in invalid_keybindings_validate:
        raise RuntimeError(
            f"/keybindings validate did not report the conflicting keybinding\nscreen:\n{invalid_keybindings_validate}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings import import-keybinds.json --force")
    wait_for(tmux_exe, session, r"/keybindings import import-keybinds\.json --force", "keybindings import draft")
    send_keys(tmux_exe, session, "Enter")
    keybindings_import = wait_for(tmux_exe, session, r"Imported keybindings file", "keybindings import command")
    if "Imported keybindings file" not in keybindings_import or "/reload keybindings" not in keybindings_import:
        raise RuntimeError(f"/keybindings import did not report an installed file\nscreen:\n{keybindings_import}")
    imported_keybinds = (ava_config / "keybinds.json").read_text(encoding="utf-8")
    if imported_keybinds != import_keybinds_content:
        raise RuntimeError(
            f"/keybindings import did not install the expected keybindings file\ncontent:\n{imported_keybinds}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings set cursor_left Alt+H")
    wait_for(tmux_exe, session, r"/keybindings set cursor_left Alt\+H", "keybindings set draft")
    send_keys(tmux_exe, session, "Enter")
    keybindings_set = wait_for(tmux_exe, session, r"Set keybinding", "keybindings set command")
    if "Set keybinding" not in keybindings_set or "/reload keybindings" not in keybindings_set:
        raise RuntimeError(f"/keybindings set did not report an edited keybinding\nscreen:\n{keybindings_set}")
    edited_keybinds = (ava_config / "keybinds.json").read_text(encoding="utf-8")
    if '"tui.editor.cursorLeft": "Alt+H"' not in edited_keybinds or '"cursor_left"' in edited_keybinds:
        raise RuntimeError(
            f"/keybindings set did not edit the keybindings file as expected\ncontent:\n{edited_keybinds}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/keybindings reset cursor_left")
    wait_for(tmux_exe, session, r"/keybindings reset cursor_left", "keybindings reset draft")
    send_keys(tmux_exe, session, "Enter")
    keybindings_reset = wait_for(tmux_exe, session, r"Reset keybinding override", "keybindings reset command")
    if "Reset keybinding override" not in keybindings_reset or "/reload keybindings" not in keybindings_reset:
        raise RuntimeError(f"/keybindings reset did not report a reset override\nscreen:\n{keybindings_reset}")
    reset_keybinds = (ava_config / "keybinds.json").read_text(encoding="utf-8")
    if "tui.editor.cursorLeft" in reset_keybinds or "cursor_left" in reset_keybinds:
        raise RuntimeError(
            f"/keybindings reset did not remove the cursor-left override\ncontent:\n{reset_keybinds}"
        )
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Command /keybindings", "keybindings reset output closed")

    send_keys(tmux_exe, session, "BTab")
    shift_tab_reasoning = wait_for(tmux_exe, session, r"reasoning set to low|reasoning low", "shift-tab reasoning cycle")
    if "reasoning set to low" not in shift_tab_reasoning and "reasoning low" not in shift_tab_reasoning:
        raise RuntimeError(f"Shift+Tab did not cycle the visible reasoning state\nscreen:\n{shift_tab_reasoning}")
    send_keys(tmux_exe, session, "C-t")
    thinking_selector = wait_for(tmux_exe, session, r"Select thinking mode", "ctrl-t thinking-mode selector")
    if "Low" not in thinking_selector or "Esc cancel" not in thinking_selector:
        raise RuntimeError(f"Ctrl+T did not open the direct thinking-mode selector\nscreen:\n{thinking_selector}")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Select thinking mode", "ctrl-t thinking-mode selector canceled")
    send_literal(tmux_exe, session, "/thinking")
    wait_for(tmux_exe, session, r"/thinking", "thinking command hide draft")
    send_keys(tmux_exe, session, "Enter")
    thinking_hidden_command = wait_for(tmux_exe, session, r"thinking hidden", "thinking command hides blocks")
    if "thinking hidden" not in thinking_hidden_command:
        raise RuntimeError(f"/thinking no longer controls thinking-block visibility\nscreen:\n{thinking_hidden_command}")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/thinking")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"thinking visible", "thinking command restores visible blocks")

    _finish_main(tmux_exe, session)
