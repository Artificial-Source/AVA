"""Real-terminal streaming, detached scrollback, and input responsiveness coverage."""

from __future__ import annotations

import pathlib
import re
import time

from tui_smoke_helpers import (
    SmokeContext,
    TmuxClient,
    capture,
    capture_styled,
    save_evidence,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
    wait_for_screen_state,
    wait_for_session_exit,
)
from .common import _wait_for_normal_turn_request_count


_NUMBERED_LINE = re.compile(r"stream line (\d{3})")
_SGR_SEQUENCE = re.compile(r"\x1b\[[0-9;]*m")
_DELETED_SCROLLBACK_TEXT = ("scrollback detached", "updates below", "jump_to_bottom")
# Role-specific final chrome: footer uses the catalog display name; assistant turn meta may render
# either the display name or the bare model id depending on the meta assembly path.
_FOOTER_MODEL_LINE = re.compile(r"^│\s+AVA TUI Fake\b.*\bctx\b")
_ASSISTANT_META_LINE = re.compile(r"\*\s+Build · (?:AVA TUI Fake|ava-tui-fake)\b")


def _wait_for_path(path: pathlib.Path, label: str, timeout: float = 10.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {label}: {path}")


def _wait_for_session_text(state_root: pathlib.Path, needle: str, label: str, timeout: float = 10.0) -> pathlib.Path:
    deadline = time.monotonic() + timeout
    seen: list[pathlib.Path] = []
    while time.monotonic() < deadline:
        seen = list(state_root.rglob("*.jsonl"))
        for path in seen:
            if needle in path.read_text(encoding="utf-8", errors="replace"):
                return path
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {label}; inspected session files: {seen}")


def _numbered_window(screen: str, label: str) -> list[int]:
    numbers = [int(value) for value in _NUMBERED_LINE.findall(screen)]
    if len(numbers) < 10 or numbers != list(range(numbers[0], numbers[-1] + 1)):
        raise RuntimeError(f"{label} did not contain a contiguous numbered stream window\nnumbers: {numbers}\nscreen:\n{screen}")
    return numbers


def _numbered_window_moved_up(screen: str, before: list[int], draft: str) -> bool:
    if draft not in screen:
        return False
    numbers = [int(value) for value in _NUMBERED_LINE.findall(screen)]
    if len(numbers) < 10 or numbers != list(range(numbers[0], numbers[-1] + 1)):
        return False
    return numbers[0] < before[0]


def _wait_for_preparatory_up_steps(
    tmux_exe: TmuxClient,
    session: str,
    start_numbers: list[int],
    draft: str,
    *,
    steps: int,
    keyboard_scroll_rows: int,
) -> list[int]:
    numbers = start_numbers
    screen = ""
    for step in range(1, steps + 1):
        before = numbers
        send_keys(tmux_exe, session, "Up")
        screen = wait_for_screen_state(
            tmux_exe,
            session,
            lambda captured, current_before=before: _numbered_window_moved_up(captured, current_before, draft),
            f"streaming-scroll preparatory Up {step} of {steps} moved numbered window upward",
        )
        numbers = _numbered_window(screen, f"preparatory Up {step} of {steps} numbered window")
    if (
        draft not in screen
        or "STREAM COMPLETE" in screen
        or numbers[0] < keyboard_scroll_rows
        or numbers[-1] > 59 - keyboard_scroll_rows
    ):
        raise RuntimeError(
            "streaming-scroll detached numbered region before plain arrow scroll checks "
            "did not keep the draft and a contiguous window with room for a keyboard-step Up/Down\n"
            f"numbers: {numbers}\nscreen:\n{screen}"
        )
    return numbers


def _assert_no_deleted_scrollback_text(screen: str, label: str) -> None:
    surfaced = [text for text in _DELETED_SCROLLBACK_TEXT if text in screen]
    if surfaced:
        raise RuntimeError(f"{label} surfaced deleted scrollback chrome {surfaced}\nscreen:\n{screen}")


def _plain_styled_capture(styled: str) -> str:
    return "\n".join(_SGR_SEQUENCE.sub("", line).rstrip() for line in styled.splitlines())


def _right_edge_reverse_rows(styled: str, pane_width: int) -> list[int]:
    rows: list[int] = []
    marker = "\x1b[7m"
    for row, line in enumerate(styled.splitlines()):
        marker_at = line.rfind(marker)
        if marker_at < 0:
            continue
        prefix = _SGR_SEQUENCE.sub("", line[:marker_at])
        suffix = _SGR_SEQUENCE.sub("", line[marker_at + len(marker) :])
        # tmux trims the styled final blank cell but retains the SGR that starts
        # it, yielding a marker after exactly width - 1 visible columns.
        if len(prefix) == pane_width - 1 and not suffix:
            rows.append(row)
    return rows


def _wait_for_transient_scrollbar(
    ctx: SmokeContext,
    session: str,
    expected_numbers: list[int],
    draft: str,
    started: float,
    timeout: float = 1.0,
) -> tuple[str, str]:
    pane_width = int(tmux(ctx.tmux, "display-message", "-p", "-t", session, "#{pane_width}").stdout.strip())
    deadline = started + timeout
    last = ""
    while time.monotonic() < deadline:
        last = capture_styled(ctx.tmux, session)
        plain = _plain_styled_capture(last)
        thumb_rows = _right_edge_reverse_rows(last, pane_width)
        numbers = [int(value) for value in _NUMBERED_LINE.findall(plain)]
        if thumb_rows and numbers == expected_numbers and draft in plain:
            lines = plain.splitlines()
            if not all(row < len(lines) and _NUMBERED_LINE.search(lines[row]) for row in thumb_rows):
                raise RuntimeError(f"transient scrollbar replaced ordinary transcript cells\nstyled screen:\n{last!r}")
            return last, plain
        time.sleep(0.02)
    raise RuntimeError(f"timed out waiting for transient right-edge scrollbar and numbered window\nstyled screen:\n{last!r}")


def _wait_for_transient_scrollbar_expiry(
    ctx: SmokeContext,
    session: str,
    expected_numbers: list[int],
    draft: str,
    started: float,
    timeout: float = 2.5,
) -> tuple[str, str]:
    pane_width = int(tmux(ctx.tmux, "display-message", "-p", "-t", session, "#{pane_width}").stdout.strip())
    deadline = started + timeout
    last = ""
    while time.monotonic() < deadline:
        last = capture_styled(ctx.tmux, session)
        if _right_edge_reverse_rows(last, pane_width):
            time.sleep(0.02)
            continue
        elapsed = time.monotonic() - started
        if elapsed < 0.8:
            raise RuntimeError(f"transient scrollbar disappeared too early after {elapsed:.3f}s\nstyled screen:\n{last!r}")
        plain = _plain_styled_capture(last)
        numbers = [int(value) for value in _NUMBERED_LINE.findall(plain)]
        if numbers != expected_numbers or draft not in plain:
            raise RuntimeError(
                "numbered transcript cells or composer draft changed when the transient scrollbar expired\n"
                f"expected numbers: {expected_numbers}\nactual numbers: {numbers}\nstyled screen:\n{last!r}"
            )
        return last, plain
    raise RuntimeError(f"timed out waiting for transient right-edge scrollbar expiry\nstyled screen:\n{last!r}")


def scenario_streaming_scroll(ctx: SmokeContext) -> None:
    tmux_exe = ctx.tmux
    root = ctx.root
    session = ctx.session_name("streaming-scroll")
    controls = root / "streaming-controls"
    controls.mkdir(mode=0o700)
    controls.chmod(0o700)

    streaming_models = (
        '{"default_provider":"moonshot","default_model":"ava-tui-fake",'
        '"models":[{"provider":"moonshot","id":"ava-tui-fake","name":"AVA TUI Fake","family":"fake",'
        '"context_window_tokens":8192,"max_output_tokens":1024,"supports_tools":false,'
        '"supports_streaming":true,"supports_reasoning":false,"reports_usage":true}]}\n'
    )
    ctx.active_ava_config.joinpath("models.json").write_text(streaming_models, encoding="utf-8")
    ctx.active_ava_config.joinpath("keybinds.json").write_text(
        '{"app.transcript.halfPageUp":"F9","app.transcript.halfPageDown":"F10"}\n', encoding="utf-8"
    )

    provider = ctx.start_fake_provider("streaming-scroll", delay_ms=20, scenario="streaming-scroll", target=controls)
    command = ctx.fake_provider_command(
        provider,
        home=ctx.active_home,
        config=ctx.active_config,
        state=ctx.active_state,
        data=ctx.active_data,
        no_color=False,
    )
    ctx.launch_ava(session, workspace=ctx.active_workspace, command=command, width=120, height=32)
    wait_for(tmux_exe, session, r"Type a message|live session", "streaming-scroll initial frame")

    send_literal(tmux_exe, session, "exercise deterministic streaming scroll")
    wait_for(tmux_exe, session, r"exercise deterministic streaming scroll", "streaming-scroll prompt draft")
    send_keys(tmux_exe, session, "Enter")
    _wait_for_normal_turn_request_count(provider, 1, "streaming-scroll provider request")

    draft = "STREAM-DRAFT-RESPONSIVE"
    same_direction_suffix = "-WHEEL-SAME"
    alternating_suffix = "-WHEEL-ALT"
    active_prefix = "A" * 160
    same_direction_draft = draft + same_direction_suffix
    complete_draft = same_direction_draft + alternating_suffix
    active_started = time.monotonic()
    send_literal(tmux_exe, session, active_prefix + draft)
    responsive = wait_for(tmux_exe, session, re.escape(draft), "streaming-scroll responsive active draft", timeout=2.0)
    active_elapsed = time.monotonic() - active_started
    if "STREAM COMPLETE" in responsive:
        raise RuntimeError(f"active draft was not observed before stream completion\nscreen:\n{responsive}")

    _wait_for_path(controls / "paused", "streaming-scroll paused marker")
    initial_screen = wait_for(tmux_exe, session, r"stream line 029", "streaming-scroll initial numbered tail")
    initial_numbers = _numbered_window(initial_screen, "initial paused stream")
    if initial_numbers[-1] != 29 or initial_numbers[0] == 0 or draft not in initial_screen:
        raise RuntimeError(
            "initial paused stream did not expose a movable tail ending at line 029 with the draft intact\n"
            f"numbers: {initial_numbers}\nscreen:\n{initial_screen}"
        )
    _assert_no_deleted_scrollback_text(initial_screen, "initial paused stream")

    wheel_up = "\x1b[<64;4;6M"
    wheel_down = "\x1b[<65;4;6M"
    # One accepted transcript wheel event scrolls exactly three rendered rows.
    wheel_scroll_rows = 3
    same_direction_started = time.monotonic()
    send_literal(tmux_exe, session, wheel_up * 12 + same_direction_suffix)
    same_direction_screen = wait_for(
        tmux_exe, session, re.escape(same_direction_draft), "streaming-scroll same-direction wheel burst consumed before suffix", timeout=2.0
    )
    same_direction_elapsed = time.monotonic() - same_direction_started
    same_direction_numbers = _numbered_window(same_direction_screen, "same-direction wheel-burst detached stream")

    def _is_exact_wheel_up_step(before: list[int], after: list[int], step: int) -> bool:
        if len(after) < 10 or after != list(range(after[0], after[-1] + 1)):
            return False
        if after[0] != before[0] - step:
            return False
        # Same-height window shifted by the wheel step.
        if len(after) == len(before):
            return after[-1] == before[-1] - step
        # Detaching can free live-tail chrome rows; allow growth while keeping the step and bounds.
        return len(after) > len(before) and after[-1] <= before[-1]

    if not _is_exact_wheel_up_step(initial_numbers, same_direction_numbers, wheel_scroll_rows):
        raise RuntimeError(
            "raw same-direction wheel burst did not produce exactly three upward transcript rows\n"
            f"active typing elapsed: {active_elapsed:.3f}s; wheel elapsed: {same_direction_elapsed:.3f}s\n"
            f"before: {initial_numbers}\nafter: {same_direction_numbers}\nscreen:\n{same_direction_screen}"
        )
    if same_direction_draft not in same_direction_screen:
        raise RuntimeError(f"same-direction wheel burst changed the composer draft or cursor insertion point\nscreen:\n{same_direction_screen}")
    _assert_no_deleted_scrollback_text(same_direction_screen, "same-direction wheel-burst detached stream")

    burst_started = time.monotonic()
    send_literal(tmux_exe, session, (wheel_up + wheel_down) * 60 + alternating_suffix)
    burst_screen = wait_for(tmux_exe, session, re.escape(complete_draft), "streaming-scroll alternating wheel flood consumed before suffix", timeout=2.0)
    burst_elapsed = time.monotonic() - burst_started
    burst_numbers = _numbered_window(burst_screen, "alternating wheel-flood stream")
    if burst_numbers[0] < 0 or burst_numbers[-1] > initial_numbers[-1]:
        raise RuntimeError(
            "alternating wheel flood escaped the bounded paused transcript window\n"
            f"active typing elapsed: {active_elapsed:.3f}s; same-direction wheel elapsed: {same_direction_elapsed:.3f}s; "
            f"alternating wheel elapsed: {burst_elapsed:.3f}s\n"
            f"before: {same_direction_numbers}\nafter: {burst_numbers}\nscreen:\n{burst_screen}"
        )
    if complete_draft not in burst_screen:
        raise RuntimeError(f"alternating wheel flood changed the composer draft or cursor insertion point\nscreen:\n{burst_screen}")
    _assert_no_deleted_scrollback_text(burst_screen, "alternating wheel-flood stream")

    provider.release_request(0)
    _wait_for_path(controls / "completed", "streaming-scroll completed marker")
    _wait_for_session_text(ctx.active_state, "STREAM COMPLETE", "persisted streaming completion")

    completed_detached = capture(tmux_exe, session)
    completed_detached_numbers = _numbered_window(completed_detached, "completed detached stream")
    if completed_detached_numbers[: len(burst_numbers)] != burst_numbers or complete_draft not in completed_detached:
        raise RuntimeError(
            "detached transcript or draft moved while the stream completed below\n"
            f"before: {burst_numbers}\nafter: {completed_detached_numbers}\nscreen:\n{completed_detached}"
        )
    _assert_no_deleted_scrollback_text(completed_detached, "completed detached stream")

    live_stream_screen = completed_detached
    for step in range(32):
        previous_numbers = [int(value) for value in _NUMBERED_LINE.findall(live_stream_screen)]
        wheel_sent_at = time.monotonic()

        def reverse_step_synchronized(screen: str) -> bool:
            changed = "STREAM COMPLETE" in screen or [int(value) for value in _NUMBERED_LINE.findall(screen)] != previous_numbers
            # AVA intentionally drops repeated same-direction wheel events
            # within 40 ms. Synchronize on both the rendered step and expiry of
            # that documented governor interval before sending the next step;
            # relying on two poll captures became too short at a 20 ms cadence.
            return changed and time.monotonic() >= wheel_sent_at + 0.045

        send_literal(tmux_exe, session, wheel_down)
        live_stream_screen = wait_for_screen_state(
            tmux_exe,
            session,
            reverse_step_synchronized,
            f"streaming-scroll synchronized reverse wheel step {step + 1}",
            timeout=1.0,
        )
        # Only the footer/meta row mentions the model label; no startup chrome reserves rows.
        metadata_count = sum(
            1
            for line in live_stream_screen.splitlines()
            if "AVA TUI Fake" in line
        )
        if "STREAM COMPLETE" in live_stream_screen and metadata_count == 1:
            break
    if "STREAM COMPLETE" not in live_stream_screen or metadata_count != 1 or complete_draft not in live_stream_screen:
        raise RuntimeError(f"reverse-direction wheel events did not return to the completed live tail\nscreen:\n{live_stream_screen}")
    _assert_no_deleted_scrollback_text(live_stream_screen, "restored completed live tail")

    def restored_live_tail(screen: str) -> bool:
        numbers = [int(value) for value in _NUMBERED_LINE.findall(screen)]
        return "STREAM COMPLETE" in screen and complete_draft in screen and bool(numbers) and numbers[-1] == 59

    # Re-detach deterministically so Ctrl+End can prove live-tail restore independently of wheel.
    send_literal(tmux_exe, session, wheel_up * 16)
    ctrl_end_detached = wait_for_screen_state(
        tmux_exe,
        session,
        lambda screen: complete_draft in screen
        and "STREAM COMPLETE" not in screen
        and bool([int(value) for value in _NUMBERED_LINE.findall(screen)]),
        "streaming-scroll re-detached before Ctrl+End live-tail restore",
    )
    _assert_no_deleted_scrollback_text(ctrl_end_detached, "re-detached before Ctrl+End")

    send_literal(tmux_exe, session, "\x1b[1;5F")
    ctrl_end_live = wait_for_screen_state(
        tmux_exe,
        session,
        restored_live_tail,
        "streaming-scroll Ctrl+End returns detached stream to live tail without draft mutation",
    )
    ctrl_end_numbers = _numbered_window(ctrl_end_live, "Ctrl+End restored live tail")
    if ctrl_end_numbers[-1] != 59 or complete_draft not in ctrl_end_live:
        raise RuntimeError(
            "Ctrl+End did not restore the completed live tail with the composer draft intact\n"
            f"numbers: {ctrl_end_numbers}\nscreen:\n{ctrl_end_live}"
        )
    _assert_no_deleted_scrollback_text(ctrl_end_live, "Ctrl+End restored live tail")

    # Leave live-tail chrome so ordinary Up/Down can be measured as the keyboard scroll step
    # (3 transcript rows) without colliding with live-tail chrome or the oldest boundary.
    # Send each preparatory Up individually so a later exact 3-row check cannot snapshot a
    # 2/3-key intermediate window from a 4-key burst.
    keyboard_scroll_rows = 3
    before_up = _wait_for_preparatory_up_steps(
        tmux_exe,
        session,
        ctrl_end_numbers,
        complete_draft,
        steps=4,
        keyboard_scroll_rows=keyboard_scroll_rows,
    )

    def keyboard_step_up(screen: str) -> bool:
        numbers = [int(value) for value in _NUMBERED_LINE.findall(screen)]
        if len(numbers) < 10 or complete_draft not in screen:
            return False
        same_size_shift = list(range(before_up[0] - keyboard_scroll_rows, before_up[0] - keyboard_scroll_rows + len(before_up)))
        return numbers == same_size_shift

    send_keys(tmux_exe, session, "Up")
    up_scrolled = wait_for_screen_state(
        tmux_exe,
        session,
        keyboard_step_up,
        "streaming-scroll plain Up scrolls transcript by the keyboard step without draft mutation",
    )
    up_numbers = _numbered_window(up_scrolled, "plain Up keyboard-step scroll")
    if complete_draft not in up_scrolled:
        raise RuntimeError(f"plain Up altered the composer draft\nscreen:\n{up_scrolled}")
    _assert_no_deleted_scrollback_text(up_scrolled, "plain Up keyboard-step scroll")

    before_down = up_numbers

    def keyboard_step_down(screen: str) -> bool:
        numbers = [int(value) for value in _NUMBERED_LINE.findall(screen)]
        if complete_draft not in screen or len(numbers) < 10:
            return False
        same_size_shift = list(range(before_down[0] + keyboard_scroll_rows, before_down[0] + keyboard_scroll_rows + len(before_down)))
        return numbers == same_size_shift

    send_keys(tmux_exe, session, "Down")
    down_scrolled = wait_for_screen_state(
        tmux_exe,
        session,
        keyboard_step_down,
        "streaming-scroll plain Down scrolls transcript by the keyboard step without draft mutation",
    )
    down_numbers = _numbered_window(down_scrolled, "plain Down keyboard-step scroll")
    if complete_draft not in down_scrolled:
        raise RuntimeError(f"plain Down altered the composer draft\nscreen:\n{down_scrolled}")
    _assert_no_deleted_scrollback_text(down_scrolled, "plain Down keyboard-step scroll")

    # This detached middle window consists entirely of one-row numbered transcript
    # lines, so its observed size is the real transcript body height after the
    # wrapped composer has reserved its rows. Production halves that height with
    # floor division and keeps a one-row minimum.
    visible_transcript_rows = len(down_numbers)
    half_page_rows = max(1, visible_transcript_rows // 2)
    expected_half_up = list(range(down_numbers[0] - half_page_rows, down_numbers[0] - half_page_rows + visible_transcript_rows))
    half_page_started = time.monotonic()
    send_keys(tmux_exe, session, "F9")
    indicator_styled, half_page_up = _wait_for_transient_scrollbar(
        ctx, session, expected_half_up, complete_draft, half_page_started
    )
    half_page_up_numbers = _numbered_window(half_page_up, "configured F9 half-page upward scroll")
    expired_styled, expired_plain = _wait_for_transient_scrollbar_expiry(
        ctx, session, expected_half_up, complete_draft, half_page_started
    )
    if expired_plain != half_page_up:
        raise RuntimeError(
            "transient scrollbar expiry changed ordinary visible transcript or composer cells\n"
            f"before:\n{half_page_up}\nafter:\n{expired_plain}"
        )
    save_evidence(root, "streaming-scroll-indicator-immediate-styled", indicator_styled)
    save_evidence(root, "streaming-scroll-indicator-expired-styled", expired_styled)
    if complete_draft not in half_page_up:
        raise RuntimeError(f"configured F9 altered the composer draft\nscreen:\n{half_page_up}")
    _assert_no_deleted_scrollback_text(half_page_up, "configured F9 half-page upward scroll")

    expected_half_down = list(
        range(half_page_up_numbers[0] + half_page_rows, half_page_up_numbers[0] + half_page_rows + visible_transcript_rows)
    )
    send_keys(tmux_exe, session, "F10")
    half_page_down = wait_for_screen_state(
        tmux_exe,
        session,
        lambda screen: complete_draft in screen and [int(value) for value in _NUMBERED_LINE.findall(screen)] == expected_half_down,
        "streaming-scroll configured F10 moves exactly half the visible transcript body downward",
    )
    if expected_half_down != down_numbers or complete_draft not in half_page_down:
        raise RuntimeError(
            "configured F10 did not restore the pre-F9 numbered window with the composer draft intact\n"
            f"visible transcript rows: {visible_transcript_rows}; half-page rows: {half_page_rows}\n"
            f"expected: {down_numbers}\nactual: {expected_half_down}\nscreen:\n{half_page_down}"
        )
    _assert_no_deleted_scrollback_text(half_page_down, "configured F10 half-page downward scroll")

    half_page_live = half_page_down
    for step in range(4):
        if restored_live_tail(half_page_live):
            break
        send_keys(tmux_exe, session, "F10")
        half_page_live = wait_for_screen_state(
            tmux_exe,
            session,
            lambda screen: complete_draft in screen
            and (
                restored_live_tail(screen)
                or [int(value) for value in _NUMBERED_LINE.findall(screen)]
                != [int(value) for value in _NUMBERED_LINE.findall(half_page_live)]
            ),
            f"streaming-scroll configured F10 live-tail restore step {step + 1}",
        )
    if not restored_live_tail(half_page_live):
        raise RuntimeError(f"configured F10 did not restore live tail without changing the composer draft\nscreen:\n{half_page_live}")
    _assert_no_deleted_scrollback_text(half_page_live, "configured F10 restored live tail")

    send_keys(tmux_exe, session, "M-k")
    alt_k_screen = wait_for_screen_state(
        tmux_exe,
        session,
        lambda screen: complete_draft in screen
        and (
            "previous message" in screen
            or "oldest message visible" in screen
            or [int(value) for value in _NUMBERED_LINE.findall(screen)] != down_numbers
        ),
        "streaming-scroll Alt+K moves to a prior message boundary",
    )
    alt_k_numbers = [int(value) for value in _NUMBERED_LINE.findall(alt_k_screen)]
    if complete_draft not in alt_k_screen:
        raise RuntimeError(f"Alt+K altered the composer draft\nscreen:\n{alt_k_screen}")
    if (
        not alt_k_numbers
        and "oldest message visible" not in alt_k_screen
        and "previous message" not in alt_k_screen
    ):
        raise RuntimeError(f"Alt+K did not move message boundaries or report a boundary status\nscreen:\n{alt_k_screen}")
    _assert_no_deleted_scrollback_text(alt_k_screen, "Alt+K prior message boundary")

    send_keys(tmux_exe, session, "M-j")
    alt_j_screen = wait_for_screen_state(
        tmux_exe,
        session,
        lambda screen: complete_draft in screen
        and (
            "next message" in screen
            or "live tail" in screen
            or "STREAM COMPLETE" in screen
            or [int(value) for value in _NUMBERED_LINE.findall(screen)] != alt_k_numbers
        ),
        "streaming-scroll Alt+J moves to the next message boundary or live tail",
    )
    if complete_draft not in alt_j_screen:
        raise RuntimeError(f"Alt+J altered the composer draft\nscreen:\n{alt_j_screen}")
    _assert_no_deleted_scrollback_text(alt_j_screen, "Alt+J next message boundary")

    send_literal(tmux_exe, session, "\x1b[1;5F")
    live_after_nav = wait_for_screen_state(
        tmux_exe,
        session,
        restored_live_tail,
        "streaming-scroll Ctrl+End after message-boundary navigation restores live tail",
    )
    if complete_draft not in live_after_nav:
        raise RuntimeError(f"Ctrl+End after message navigation altered the composer draft\nscreen:\n{live_after_nav}")
    _assert_no_deleted_scrollback_text(live_after_nav, "Ctrl+End after message-boundary navigation")

    live_stream_screen = live_after_nav
    if complete_draft not in live_stream_screen or "STREAM COMPLETE" not in live_stream_screen:
        raise RuntimeError(f"navigation path did not finish on the completed live tail\nscreen:\n{live_stream_screen}")
    _assert_no_deleted_scrollback_text(live_stream_screen, "restored completed live tail after navigation defaults")

    send_keys(tmux_exe, session, "C-c")
    wait_for_absent(tmux_exe, session, re.escape(complete_draft), "streaming-scroll active draft clear")

    idle_draft = "I" * 72 + "IDLE-RESPONSIVE"
    final_draft = idle_draft + "XFINAL"
    idle_started = time.monotonic()
    send_literal(tmux_exe, session, idle_draft + (wheel_up + wheel_down) * 40 + "XFINAL")

    def _footer_model_lines(screen: str) -> list[str]:
        return [line for line in screen.splitlines() if _FOOTER_MODEL_LINE.search(line)]

    def _assistant_meta_lines(screen: str) -> list[str]:
        return [line for line in screen.splitlines() if _ASSISTANT_META_LINE.search(line)]

    def idle_burst_live_tail(screen: str) -> bool:
        # Converge on true live tail with role-aware chrome before hard assertions fire.
        numbers = [int(value) for value in _NUMBERED_LINE.findall(screen)]
        return (
            bool(numbers)
            and numbers[-1] == 59
            and "STREAM COMPLETE" in screen
            and final_draft in screen
            and len(_footer_model_lines(screen)) == 1
            and len(_assistant_meta_lines(screen)) == 1
        )

    final_screen = wait_for_screen_state(
        tmux_exe,
        session,
        idle_burst_live_tail,
        "streaming-scroll completed live tail and retained idle burst draft",
        timeout=2.0,
    )
    idle_elapsed = time.monotonic() - idle_started
    final_numbers = _numbered_window(final_screen, "streaming-scroll final live tail")
    lines = final_screen.splitlines()
    dimensions = tmux(tmux_exe, "display-message", "-p", "-t", session, "#{pane_width},#{pane_height}").stdout.strip()
    input_rows = [index for index, line in enumerate(lines) if final_draft in line]
    footer_lines = _footer_model_lines(final_screen)
    assistant_meta_lines = _assistant_meta_lines(final_screen)
    if final_numbers[-1] != 59 or final_draft not in final_screen or "STREAM COMPLETE" not in final_screen:
        raise RuntimeError(
            "idle typed/wheel burst did not retain the completed live tail with the exact draft/cursor result\n"
            f"active typing elapsed: {active_elapsed:.3f}s; active wheel elapsed: {burst_elapsed:.3f}s; idle elapsed: {idle_elapsed:.3f}s\n"
            f"screen:\n{final_screen}"
        )
    if len(footer_lines) != 1 or len(assistant_meta_lines) != 1:
        raise RuntimeError(
            "streaming final frame did not keep exactly one footer model line and one assistant metadata line\n"
            f"footer_model_lines={footer_lines!r}\nassistant_meta_lines={assistant_meta_lines!r}\nscreen:\n{final_screen}"
        )
    if dimensions != "120,32" or len(lines) != 32 or any(len(line) > 120 for line in lines):
        raise RuntimeError(f"streaming final evidence did not retain exact bounded 120x32 dimensions\nscreen:\n{final_screen}")
    if (
        len(input_rows) != 1
        or input_rows[0] < 2
        or lines[input_rows[0] - 2].strip()
        or lines[input_rows[0] - 1].strip() != "│"
    ):
        raise RuntimeError(
            f"streaming final frame did not retain the plain breathing gap and elevated guttered composer row immediately above the input\nscreen:\n{final_screen}"
        )
    if "\x1b" in final_screen or any(ord(character) < 32 and character != "\n" for character in final_screen):
        raise RuntimeError(f"streaming final evidence contained ESC or unexpected C0 controls\nscreen:\n{final_screen}")
    _assert_no_deleted_scrollback_text(final_screen, "streaming final live tail")
    save_evidence(root, "streaming-scroll-final", final_screen)

    send_keys(tmux_exe, session, "C-c")
    wait_for_absent(tmux_exe, session, re.escape(final_draft), "streaming-scroll draft clear")
    send_keys(tmux_exe, session, "C-d")
    wait_for_session_exit(tmux_exe, session)
    provider.stop()
    if provider.process.poll() is None:
        raise RuntimeError("streaming-scroll fake provider remained alive after established cleanup")
    provider_error = provider.stderr_path.read_text(encoding="utf-8", errors="replace")
    if provider_error:
        raise RuntimeError(f"streaming-scroll fake provider reported an error:\n{provider_error}")
