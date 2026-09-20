"""The tmux TUI smoke scenario for main session mgmt."""

from __future__ import annotations

import re

from tui_smoke_helpers import (
    SmokeContext,
    assert_screen_absent_for,
    assert_screen_present_for,
    capture,
    save_evidence,
    send_keys,
    send_literal,
    wait_for,
    wait_for_absent,
    wait_for_screen_change,
    wait_for_screen_state,
)
from .common import (
    _finish_main,
    _main_session,
    _wait_for_normal_turn_request_count,
    assert_title_first_new_receipt,
)

_KEYBOARD_SCROLL_ROWS = 3
_SESSION_SCROLL_LINE = re.compile(r"SESSION SCROLLBACK LINE (\d{2})")
_AUTH_RECEIPT_MARKER = "slash tool commands still work offline."
_COMPOSER_EMPTY = re.compile(r"(?m)^\s*│\s+Type a message\.\.\.")
_NAME_DRAFT = "│  /name TUI smoke"
_NAME_OR_MODAL = re.compile(r"Command /name|session name set|Enter(?:/Esc)? close")


def _ordinary_seed_live_tail_is_settled(screen: str) -> bool:
    """True when the no-credentials seed shows a completed receipt and idle composer."""

    if _AUTH_RECEIPT_MARKER not in screen:
        return False
    if _COMPOSER_EMPTY.search(screen) is None:
        return False
    if _NAME_OR_MODAL.search(screen) is not None or _NAME_DRAFT in screen:
        return False
    numbers = [int(value) for value in _SESSION_SCROLL_LINE.findall(screen)]
    return len(numbers) >= 10 and numbers[-1] == 30 and numbers == list(range(numbers[0], numbers[-1] + 1))


def _numbered_seed_window(screen: str, label: str) -> list[int]:
    numbers = [int(value) for value in _SESSION_SCROLL_LINE.findall(screen)]
    if len(numbers) < 10 or numbers != list(range(numbers[0], numbers[-1] + 1)):
        raise RuntimeError(
            f"{label} did not contain a contiguous numbered seed window\nnumbers: {numbers}\nscreen:\n{screen}"
        )
    return numbers


def _wait_for_ordinary_scrollback_seed(tmux_exe: object, session: str) -> list[int]:
    screen = wait_for_screen_state(
        tmux_exe,
        session,
        _ordinary_seed_live_tail_is_settled,
        "session-management ordinary scrollback seed",
    )
    return _numbered_seed_window(screen, "session-management ordinary scrollback seed")


def _wait_for_session_name_output_closed(tmux_exe: object, session: str) -> list[int]:
    screen = wait_for_screen_state(
        tmux_exe,
        session,
        _ordinary_seed_live_tail_is_settled,
        "session name output fully closed",
    )
    return _numbered_seed_window(screen, "session name output fully closed")


def _idle_up_finished_keyboard_step(screen: str, before: list[int]) -> bool:
    if _COMPOSER_EMPTY.search(screen) is None or _NAME_DRAFT in screen:
        return False
    numbers = [int(value) for value in _SESSION_SCROLL_LINE.findall(screen)]
    if len(numbers) < 10 or numbers != list(range(numbers[0], numbers[-1] + 1)):
        return False
    return numbers[0] == before[0] - _KEYBOARD_SCROLL_ROWS


def _idle_down_restored_live_tail(screen: str, live_numbers: list[int]) -> bool:
    if not _ordinary_seed_live_tail_is_settled(screen):
        return False
    return [int(value) for value in _SESSION_SCROLL_LINE.findall(screen)] == live_numbers


def _wait_for_idle_up_keyboard_step(tmux_exe: object, session: str, live_numbers: list[int]) -> str:
    send_keys(tmux_exe, session, "Up")
    return wait_for_screen_state(
        tmux_exe,
        session,
        lambda screen: _idle_up_finished_keyboard_step(screen, live_numbers),
        "idle Up arrow transcript movement without history recall",
    )


def _wait_for_idle_down_live_tail(tmux_exe: object, session: str, live_numbers: list[int]) -> str:
    send_keys(tmux_exe, session, "Down")
    return wait_for_screen_state(
        tmux_exe,
        session,
        lambda screen: _idle_down_restored_live_tail(screen, live_numbers),
        "idle Down arrow return to live tail",
    )


def scenario_main_session_mgmt(ctx: SmokeContext) -> None:
    tmux_exe, root, workspace, ava_config, env_prefix, session = _main_session(ctx)
    # This scenario cannot rely on prior scenarios for transcript rows. Seed
    # ordinary conversation rather than using local /help output as chat.
    scroll_seed = "session management transcript seed\n" + "\n".join(
        f"SESSION SCROLLBACK LINE {index:02d}" for index in range(1, 31)
    )
    send_literal(tmux_exe, session, f"\x1b[200~{scroll_seed}\x1b[201~")
    send_keys(tmux_exe, session, "Enter")
    # No-credentials ordinary turns finish with the offline auth receipt. Wait for
    # that completed receipt and an idle composer before /name so later scroll
    # gates are not baselined from a partial seed paint.
    _wait_for_ordinary_scrollback_seed(tmux_exe, session)
    send_literal(tmux_exe, session, "/name TUI smoke")
    send_keys(tmux_exe, session, "Enter")
    name_output = wait_for(tmux_exe, session, r"(?s)Command /name.*session name set: \"TUI smoke\"", "session name command output")
    if "/name TUI smoke" in name_output:
        raise RuntimeError(f"session name invocation leaked into transcript\nscreen:\n{name_output}")
    send_keys(tmux_exe, session, "Escape")
    # Tmux can observe doupdate after the /name title row clears but before the
    # composer and numbered seed window are restored. Require one settled live tail.
    live_numbers = _wait_for_session_name_output_closed(tmux_exe, session)
    arrow_scrollback = _wait_for_idle_up_keyboard_step(tmux_exe, session, live_numbers)
    # /help scrollback intentionally lists the jump_to_bottom keybinding name; only
    # treat the deleted detached-chrome phrases as failures here.
    if any(text in arrow_scrollback for text in ("scrollback detached", "updates below")):
        raise RuntimeError(f"idle Up arrow surfaced deleted detached chrome\nscreen:\n{arrow_scrollback}")
    _wait_for_idle_down_live_tail(tmux_exe, session, live_numbers)

    for index in range(1, 7):
        send_keys(tmux_exe, session, "C-u")
        send_literal(tmux_exe, session, f"/new Page {index}")
        send_keys(tmux_exe, session, "Enter")
        previous_title = "TUI smoke" if index == 1 else f"Page {index - 1}"
        new_receipt = wait_for(
            tmux_exe,
            session,
            rf'(?s)started session "Page {index}" · id.*?session_.*previous session "{previous_title}" · id.*?session_.*switched to "Page {index}"',
            f"seed page session {index}",
        )
        assert_title_first_new_receipt(new_receipt, f"Page {index}", previous_title, f"/new Page {index}")
        if index == 1:
            save_evidence(root, "session-new-title-first-receipt", new_receipt)

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/resume")
    wait_for(tmux_exe, session, r"/resume.*Resume a session", "resume palette row")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"/resume.*Resume a session", "resume palette dismissed")
    send_keys(tmux_exe, session, "Enter")
    selector = wait_for(tmux_exe, session, r"Select session|Session tree", "resume session selector")
    if "session selector opened" not in selector and "Select session" not in selector:
        raise RuntimeError(f"/resume did not open the session selector\nscreen:\n{selector}")
    save_evidence(root, "session-selector", selector)
    send_literal(tmux_exe, session, "\x1b[6~")
    page_down = wait_for(tmux_exe, session, r"›\s+Page 1", "session selector page down")
    if "Page 1" not in page_down:
        raise RuntimeError(f"session selector PageDown did not jump by a page\nscreen:\n{page_down}")
    send_literal(tmux_exe, session, "\x1b[5~")
    page_up = wait_for(tmux_exe, session, r"›\s+(?:●\s+)?Page 6", "session selector page up")
    if "Page 6" not in page_up:
        raise RuntimeError(f"session selector PageUp did not jump by a page\nscreen:\n{page_up}")
    send_literal(tmux_exe, session, "tui smoke")
    wait_for(tmux_exe, session, r"(?s)filter\s+tui smoke█.*›\s+TUI smoke", "session selector query after page navigation")
    send_keys(tmux_exe, session, "C-s")
    wait_for(tmux_exe, session, r"sort name|Ctrl\+S/Ctrl\+T sort \(name\)", "session selector sort cycle")
    send_keys(tmux_exe, session, "C-n")
    named_filter = wait_for(tmux_exe, session, r"sort name · named", "session selector named-only filter")
    if "TUI smoke" not in named_filter or "sort name · named" not in named_filter:
        raise RuntimeError(f"session selector named-only filter did not keep the named session visible\nscreen:\n{named_filter}")
    named_lines = named_filter.splitlines()
    named_start = next((index for index, line in enumerate(named_lines) if "Select session" in line), None)
    named_end = next((index for index, line in enumerate(named_lines) if index >= (named_start or 0) and "Ctrl+D archive" in line), None)
    named_modal = "\n".join(named_lines[named_start : named_end + 1]) if named_start is not None and named_end is not None else ""
    runtime_state_root = str(ctx.state.parent)
    if (
        not named_modal
        or "session_" in named_modal
        or ".jsonl" in named_modal
        or runtime_state_root in named_modal
        or "current current" in named_modal
    ):
        raise RuntimeError(f"default session selector rows exposed ids, paths, or duplicate current state\nscreen:\n{named_filter}")
    save_evidence(root, "session-selector-named-default-path-hidden", named_filter)
    send_keys(tmux_exe, session, "C-p")
    path_toggle = wait_for(tmux_exe, session, r"sort name · named · paths", "session selector path-display toggle")
    if "TUI smoke" not in path_toggle or "sort name · named · paths" not in path_toggle:
        raise RuntimeError(f"session selector path-display toggle did not keep the named session visible\nscreen:\n{path_toggle}")
    path_lines = path_toggle.splitlines()
    path_start = next((index for index, line in enumerate(path_lines) if "Select session" in line), None)
    path_end = next((index for index, line in enumerate(path_lines) if index >= (path_start or 0) and "Ctrl+D archive" in line), None)
    path_modal = "\n".join(path_lines[path_start : path_end + 1]) if path_start is not None and path_end is not None else ""
    if runtime_state_root not in path_modal and ".jsonl" not in path_modal:
        raise RuntimeError(f"Ctrl+P did not explicitly disclose the selected session path\nscreen:\n{path_toggle}")
    save_evidence(root, "session-selector-path-disclosed", path_toggle)
    send_keys(tmux_exe, session, "C-r")
    rename_draft = wait_for(tmux_exe, session, r"/sessions rename session_", "session selector rename draft")
    if "/sessions rename session_" not in rename_draft:
        raise RuntimeError(f"session selector Ctrl+R did not restore a rename command draft\nscreen:\n{rename_draft}")
    send_literal(tmux_exe, session, "Selector rename")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"session .* name set: \"Selector rename\"", "session selector rename command")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/resume")
    wait_for(tmux_exe, session, r"/resume.*Resume a session", "resume palette before label draft")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"/resume.*Resume a session", "resume palette dismissed before label draft")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Select session|Session tree", "resume selector before label draft")
    send_literal(tmux_exe, session, "Selector rename")
    wait_for(tmux_exe, session, r"›\s+Selector rename", "resume selector filtered before label draft")
    send_literal(tmux_exe, session, "L")
    labels_draft = wait_for(tmux_exe, session, r"/sessions labels session_", "session selector Shift+L labels draft")
    if "/sessions labels session_" not in labels_draft:
        raise RuntimeError(f"session selector Shift+L did not restore a labels command draft\nscreen:\n{labels_draft}")
    send_literal(tmux_exe, session, "picker bookmark")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"session .* labels set: picker,bookmark", "session selector labels command")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/sessions picker")
    # Post-submit state refreshes the dynamic session completion catalog. Wait
    # for the named row itself before dismissing it; visible rows intentionally
    # no longer expose the canonical session id.
    wait_for(tmux_exe, session, r"│\s+›\s+Selector rename", "literal sessions query completion active")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"│\s+›\s+Selector rename", "literal sessions query completion dismissed")
    send_keys(tmux_exe, session, "Enter")
    wait_for(
        tmux_exe,
        session,
        r"(?s)Sessions:.*Selector rename.*labels=picker,bookmark",
        "session selector labels visible in tree",
    )
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/resume")
    wait_for(tmux_exe, session, r"/resume.*Resume a session", "resume palette before label-time toggle")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"/resume.*Resume a session", "resume palette dismissed before label-time toggle")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Select session|Session tree", "resume selector before label-time toggle")
    send_literal(tmux_exe, session, "Selector rename")
    wait_for(tmux_exe, session, r"›\s+Selector rename", "resume selector filtered before label-time toggle")
    send_literal(tmux_exe, session, "T")
    label_time = wait_for(tmux_exe, session, r"label times", "session selector Shift+T label-time toggle")
    if "Selector rename" not in label_time:
        raise RuntimeError(f"session selector Shift+T lost the filtered labeled row\nscreen:\n{label_time}")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Select session|Session tree", "session selector closed after label-time toggle")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/new Archive current")
    send_keys(tmux_exe, session, "Enter")
    archive_new_receipt = wait_for(
        tmux_exe,
        session,
        r'(?s)started session "Archive current" · id.*?session_.*previous session "Page 6" · id.*?session_.*switched to "Archive current"',
        "new session before selector archive",
    )
    assert_title_first_new_receipt(archive_new_receipt, "Archive current", "Page 6", "archive setup /new")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/resume")
    wait_for(tmux_exe, session, r"/resume.*Resume a session", "resume palette before archive")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"/resume.*Resume a session", "resume palette dismissed before archive")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Select session|Session tree", "resume selector before archive")
    send_literal(tmux_exe, session, "Selector rename")
    wait_for(tmux_exe, session, r"›\s+Selector rename", "session selector non-current row selected")
    send_literal(tmux_exe, session, "\x1b[127;5u")
    ctrl_backspace_filtered = assert_screen_absent_for(
        tmux_exe,
        session,
        r"press Ctrl+Backspace again|press Ctrl+D again",
        "Ctrl+Backspace archive confirmation while the selector query was non-empty",
    )
    if (
        "press Ctrl+Backspace again" in ctrl_backspace_filtered
        or "press Ctrl+D again" in ctrl_backspace_filtered
    ):
        raise RuntimeError(
            "Ctrl+Backspace opened archive confirmation while the selector query was non-empty\n"
            f"screen:\n{ctrl_backspace_filtered}"
        )
    for _ in range(len("Selector rename")):
        send_literal(tmux_exe, session, "\x1b[127;2u")
    wait_for(tmux_exe, session, r"›\s+Selector rename", "session selector row selected after clearing query")
    send_literal(tmux_exe, session, "\x1b[127;5u")
    ctrl_backspace_confirmation = assert_screen_present_for(
        tmux_exe,
        session,
        r"Select session|Session tree",
        "Ctrl+Backspace first archive press closed the selector instead of waiting for confirmation",
    )
    if "Select session" not in ctrl_backspace_confirmation and "Session tree" not in ctrl_backspace_confirmation:
        raise RuntimeError(
            "Ctrl+Backspace first archive press closed the selector instead of waiting for confirmation\n"
            f"screen:\n{ctrl_backspace_confirmation}"
        )
    archived_selector_before = capture(tmux_exe, session)
    send_literal(tmux_exe, session, "\x1b[127;5u")
    wait_for_screen_change(tmux_exe, session, archived_selector_before, "selector archive completion")
    send_keys(tmux_exe, session, "C-c")
    wait_for_absent(tmux_exe, session, r"Select session|Session tree", "session selector closed after archive")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/sessions --archived Selector rename")
    send_keys(tmux_exe, session, "Enter")
    archived_sessions = wait_for(
        tmux_exe, session, r"(?s)Sessions \(including archived\):.*Selector rename", "archived session list"
    )
    if "archived" not in archived_sessions:
        raise RuntimeError(f"Archived session list did not mark the archived row\nscreen:\n{archived_sessions}")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/resume")
    wait_for(tmux_exe, session, r"/resume.*Resume a session", "resume palette before restore")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"/resume.*Resume a session", "resume palette dismissed before restore")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Select session|Session tree", "resume selector before restore")
    send_keys(tmux_exe, session, "C-a")
    archived_selector = wait_for(
        tmux_exe, session, r"Select session\s+sort recent · archived", "session selector archived toggle"
    )
    send_literal(tmux_exe, session, "Selector rename")
    archived_selector = wait_for(
        tmux_exe, session, r"(?m)^\s*›\s+Selector rename\s+archived", "archived row filtered in selector"
    )
    if "archived" not in archived_selector:
        raise RuntimeError(f"session selector did not show archived session state\nscreen:\n{archived_selector}")
    send_keys(tmux_exe, session, "C-d")
    assert_screen_present_for(
        tmux_exe,
        session,
        r"(?m)^\s*›\s+Selector rename\s+archived",
        "selector restore confirmation",
    )
    send_keys(tmux_exe, session, "C-d")
    wait_for_absent(
        tmux_exe,
        session,
        r"(?m)^\s*›\s+Selector rename\s+archived",
        "selector restore completion",
    )
    send_keys(tmux_exe, session, "C-c")
    wait_for_absent(tmux_exe, session, r"Select session|Session tree", "session selector closed after restore")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/sessions Selector rename")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"(?s)Sessions:.*Selector rename", "restored session visible in default list")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/name Branch parent")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"session name set: \"Branch parent\"", "branch parent session name")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/fork-from")
    send_keys(tmux_exe, session, "Enter")
    # Command-only sessions have no public user turns; the picker fails closed with status.
    empty_fork_from = wait_for(
        tmux_exe,
        session,
        r"no public user turns available|Fork from user turn",
        "fork-from empty-or-picker status on command-only session",
    )
    if "Fork from user turn" in empty_fork_from:
        send_keys(tmux_exe, session, "Escape")
        wait_for_absent(tmux_exe, session, r"Fork from user turn", "fork-from picker closed without mutation")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/fork Branch child")
    send_keys(tmux_exe, session, "Enter")
    wait_for(
        tmux_exe,
        session,
        r"(?s)forked session session_.*name=\"Branch child\".*switched to session_",
        "forked child session before selector branch navigation",
    )
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/resume")
    wait_for(tmux_exe, session, r"/resume.*Resume a session", "resume palette before parent branch navigation")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(
        tmux_exe,
        session,
        r"/resume.*Resume a session",
        "resume palette dismissed before parent branch navigation",
    )
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Select session|Session tree", "resume selector before parent branch navigation")
    send_literal(tmux_exe, session, "\x1b[1;3D")
    wait_for(tmux_exe, session, r"opened parent branch session_", "selector alt-left opened parent branch")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/sessions Branch parent")
    send_keys(tmux_exe, session, "Enter")
    parent_active = wait_for(
        tmux_exe,
        session,
        r"(?s)Sessions:.*\* Branch parent",
        "parent branch active after selector alt-left",
    )
    if "* Branch parent" not in parent_active:
        raise RuntimeError(f"selector Alt+Left did not make the parent branch current\nscreen:\n{parent_active}")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/resume")
    wait_for(tmux_exe, session, r"/resume.*Resume a session", "resume palette before child branch navigation")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(
        tmux_exe,
        session,
        r"/resume.*Resume a session",
        "resume palette dismissed before child branch navigation",
    )
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Select session|Session tree", "resume selector before child branch navigation")
    send_literal(tmux_exe, session, "\x1b[1;3C")
    wait_for(tmux_exe, session, r"opened child branch session_", "selector alt-right opened child branch")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/sessions Branch child")
    send_keys(tmux_exe, session, "Enter")
    child_active = wait_for(
        tmux_exe,
        session,
        r"(?s)Sessions:.*\* Branch child",
        "child branch active after selector alt-right",
    )
    if "* Branch child" not in child_active:
        raise RuntimeError(f"selector Alt+Right did not make the child branch current\nscreen:\n{child_active}")

    old_new_marker = "OLD-PRESENTATION-BEFORE-NEW-7E4C"
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, old_new_marker)
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, old_new_marker, "old transcript marker before /new")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/new Presentation reset new")
    wait_for(tmux_exe, session, r"│  /new Presentation reset new(?:\s|$)", "presentation reset new command draft")
    send_keys(tmux_exe, session, "Enter")
    reset_by_new = wait_for(
        tmux_exe,
        session,
        r'(?s)started session "Presentation reset new" · id.*?session_.*previous session "Branch child" · id.*?session_.*switched to "Presentation reset new"',
        "fresh visible presentation after /new",
    )
    assert_title_first_new_receipt(reset_by_new, "Presentation reset new", "Branch child", "presentation reset /new")
    if old_new_marker in reset_by_new:
        raise RuntimeError(f"/new retained an old transcript marker\nscreen:\n{reset_by_new}")

    old_clear_marker = "OLD-PRESENTATION-BEFORE-CLEAR-9A2D"
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, old_clear_marker)
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, old_clear_marker, "old transcript marker before /clear")
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/clear Presentation reset clear")
    wait_for(tmux_exe, session, r"│  /clear Presentation reset clear(?:\s|$)", "presentation reset clear command draft")
    send_keys(tmux_exe, session, "Enter")
    reset_by_clear = wait_for(
        tmux_exe,
        session,
        r'(?s)started session "Presentation reset clear" · id.*?session_.*previous session "Presentation reset new" · id.*?session_.*switched to "Presentation reset clear"',
        "fresh visible presentation after /clear",
    )
    assert_title_first_new_receipt(reset_by_clear, "Presentation reset clear", "Presentation reset new", "presentation reset /clear")
    if old_new_marker in reset_by_clear or old_clear_marker in reset_by_clear or 'started session "Presentation reset new"' in reset_by_clear:
        raise RuntimeError(f"/clear retained old presentation rows\nscreen:\n{reset_by_clear}")
    save_evidence(root, "session-new-clear-presentation-reset", reset_by_clear)

    _finish_main(tmux_exe, session)

    # Production-path evidence for /fork-from and /copy user over real public turns
    # using the fake provider (no paid live provider). Two distinct user turns are
    # required so fork-from can cut before the later transcript.
    fake_models = (
        '{"default_provider":"moonshot","default_model":"ava-tui-fake",'
        '"models":[{"provider":"moonshot","id":"ava-tui-fake","name":"AVA TUI Fake","family":"fake",'
        '"context_window_tokens":8192,"max_output_tokens":1024,"supports_tools":false,'
        '"supports_streaming":false,"supports_reasoning":false,"reports_usage":true}]}\n'
    )
    ctx.ava_config.joinpath("models.json").write_text(fake_models, encoding="utf-8")
    # text-three admits multiple ordinary turns (default "text" stops after one).
    provider = ctx.start_fake_provider("user-turn-pickers", delay_ms=0, scenario="text-three")
    picker_command = ctx.fake_provider_command(
        provider,
        home=ctx.home,
        config=ctx.config,
        state=ctx.state,
        data=ctx.data,
    )
    picker_session = ctx.session_name("user-turn-pickers")
    ctx.launch_ava(picker_session, workspace=workspace, command=picker_command, width=120, height=32)
    wait_for(tmux_exe, picker_session, r"Type a message|live session", "user-turn picker initial frame")

    send_keys(tmux_exe, picker_session, "C-u")
    send_literal(tmux_exe, picker_session, "alpha earlier unique user turn")
    wait_for(tmux_exe, picker_session, r"alpha earlier unique user turn", "user-turn picker first draft")
    send_keys(tmux_exe, picker_session, "Enter")
    _wait_for_normal_turn_request_count(provider, 1, "user-turn picker first provider request")
    wait_for(
        tmux_exe,
        picker_session,
        r"(?s)alpha earlier unique user turn.*headless active prompt complete",
        "user-turn picker first completed turn",
    )
    wait_for_absent(tmux_exe, picker_session, r"Esc stop|processing", "user-turn picker idle after first turn")

    send_keys(tmux_exe, picker_session, "C-u")
    send_literal(tmux_exe, picker_session, "beta later unique user turn")
    wait_for(tmux_exe, picker_session, r"beta later unique user turn", "user-turn picker second draft")
    send_keys(tmux_exe, picker_session, "Enter")
    _wait_for_normal_turn_request_count(provider, 2, "user-turn picker second provider request")
    both_turns = wait_for(
        tmux_exe,
        picker_session,
        r"(?s)alpha earlier unique user turn.*beta later unique user turn.*headless active prompt complete",
        "user-turn picker second completed turn",
    )
    if "beta later unique user turn" not in both_turns:
        raise RuntimeError(f"expected both user turns before fork-from\nscreen:\n{both_turns}")
    wait_for_absent(tmux_exe, picker_session, r"Esc stop|processing", "user-turn picker idle after second turn")

    send_keys(tmux_exe, picker_session, "C-u")
    send_literal(tmux_exe, picker_session, "/fork-from alpha earlier")
    send_keys(tmux_exe, picker_session, "Enter")
    fork_picker = wait_for(
        tmux_exe,
        picker_session,
        r"Fork from user turn",
        "fork-from filter opened picker on earlier turn",
    )
    if "alpha earlier unique user turn" not in fork_picker:
        raise RuntimeError(f"fork-from filter did not retain the earlier turn\nscreen:\n{fork_picker}")
    send_keys(tmux_exe, picker_session, "Enter")
    forked = wait_for(
        tmux_exe,
        picker_session,
        r"forked session session_\S+ from session_\S+",
        "fork-from earlier turn switched sessions",
    )
    wait_for_absent(tmux_exe, picker_session, r"Fork from user turn", "fork-from selector cleared after resolve")
    if "beta later unique user turn" in forked:
        raise RuntimeError(f"fork-from earlier turn retained later transcript\nscreen:\n{forked}")
    if "forked session" not in forked:
        raise RuntimeError(f"fork-from did not expose the new fork status\nscreen:\n{forked}")
    save_evidence(root, "user-turn-fork-from-earlier", forked)

    fork_match = re.search(
        r"forked session (session_\S+) from (session_\S+)",
        forked,
    )
    if fork_match is None:
        raise RuntimeError(f"fork-from status did not include source/new session ids\nscreen:\n{forked}")
    parent_session_id = fork_match.group(2)

    # Resume the parent so /copy user can target the later turn that the fork cut away.
    send_keys(tmux_exe, picker_session, "C-u")
    send_literal(tmux_exe, picker_session, f"/resume {parent_session_id}")
    send_keys(tmux_exe, picker_session, "Enter")
    wait_for(
        tmux_exe,
        picker_session,
        rf"resumed session {re.escape(parent_session_id)}|session already open",
        "resumed parent session before copy user",
    )

    send_keys(tmux_exe, picker_session, "C-u")
    send_literal(tmux_exe, picker_session, "/copy user beta later")
    send_keys(tmux_exe, picker_session, "Enter")
    copy_picker = wait_for(
        tmux_exe,
        picker_session,
        r"Copy user turn",
        "copy user filter opened picker on later turn",
    )
    if "beta later unique user turn" not in copy_picker:
        raise RuntimeError(f"copy user filter did not retain the later turn\nscreen:\n{copy_picker}")
    send_keys(tmux_exe, picker_session, "Enter")
    copy_status = wait_for(
        tmux_exe,
        picker_session,
        r"user turn copy request sent|clipboard copy failed",
        "copy user Enter status",
    )
    wait_for_absent(tmux_exe, picker_session, r"Copy user turn", "copy user selector cleared after resolve")
    if "user turn copy request sent" not in copy_status and "clipboard copy failed" not in copy_status:
        raise RuntimeError(f"copy user did not report a truthful status\nscreen:\n{copy_status}")
    save_evidence(root, "user-turn-copy-user", copy_status)

    _finish_main(tmux_exe, picker_session)
