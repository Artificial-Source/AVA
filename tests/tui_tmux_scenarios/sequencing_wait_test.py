#!/usr/bin/env python3
"""Behavioral regressions for plugin settlement, preparatory-Up, and idle-arrow sequencing.

Scripted pane captures and a fake monotonic clock model event-driven waits without
real sleeps.
"""

from __future__ import annotations

import pathlib
import sys
import types
import unittest
from contextlib import ExitStack
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import tui_smoke_helpers
from tui_tmux_scenarios import main_session_mgmt, plugin_ui, streaming_scroll

CLIENT = object()
SESSION = "unit-test-session"
CTX = types.SimpleNamespace(tmux=CLIENT)
DRAFT = "STREAM-DRAFT-RESPONSIVE-WHEEL-SAME-WHEEL-ALT"
KEYBOARD_SCROLL_ROWS = 3
COMPOSER = "Type a message\n"
CTRLC_OVERLAY = "Command /plugin\ncom.example.plugin-ui:ctrlc done\nEnter/Esc close\n"
EXIT_OVERLAY = "Command /plugin\ncom.example.plugin-ui:exit done\nEnter/Esc close\n"
EXIT_COMMAND = "/plugin run com.example.plugin-ui exit {}"


def _numbered_screen(start: int, end: int, *, draft: str = DRAFT, complete: bool = False) -> str:
    lines = [f"stream line {number:03d}" for number in range(start, end + 1)]
    if complete:
        lines.append("STREAM COMPLETE")
    lines.append(draft)
    return "\n".join(lines)


LIVE = _numbered_screen(33, 59, complete=True)
AFTER_1 = _numbered_screen(30, 56)
AFTER_2 = _numbered_screen(27, 53)
AFTER_3 = _numbered_screen(24, 50)
AFTER_4 = _numbered_screen(21, 47)
LIVE_NUMBERS = list(range(33, 60))


class FakePane:
    """Replay scripted captures, then keep returning the final capture."""

    def __init__(self, frames: list[str]) -> None:
        if not frames:
            raise ValueError("FakePane needs at least one frame")
        self._frames = list(frames)
        self.last = frames[0]

    def __call__(self, tmux_client: object, session: str) -> str:
        if len(self._frames) > 1:
            self.last = self._frames.pop(0)
        else:
            self.last = self._frames[0]
        return self.last


class FakeTime:
    """Advance the monotonic clock through polling sleeps only."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


class FakeTmux:
    """Answer session checks and record send-keys against the last capture."""

    def __init__(self, pane: object, on_send=None) -> None:
        self.pane = pane
        self.on_send = on_send
        self.sends: list[tuple[str, ...]] = []
        self.screen_at_send: list[str] = []

    def __call__(self, tmux_client: object, *args: str, check: bool = True) -> types.SimpleNamespace:
        if args and args[0] == "has-session":
            return types.SimpleNamespace(returncode=0, stdout="")
        if args and args[0] == "send-keys":
            payload = args[3:]
            self.sends.append(payload)
            self.screen_at_send.append(getattr(self.pane, "last", ""))
            if self.on_send is not None:
                self.on_send(payload)
            return types.SimpleNamespace(returncode=0, stdout="")
        raise AssertionError(f"unexpected tmux call: {args}")


class PluginSettlementScript:
    """Delay the target overlay and its disappearance after Escape."""

    def __init__(
        self,
        overlay: str,
        *,
        overlay_after: int = 2,
        absent_after_escape: int = 2,
        wrong_overlay: str | None = None,
    ) -> None:
        self.overlay = overlay
        self.overlay_after = overlay_after
        self.absent_after_escape = absent_after_escape
        self.wrong_overlay = wrong_overlay
        self.calls = 0
        self.escape_at: int | None = None
        self.last = COMPOSER

    def __call__(self, tmux_client: object, session: str) -> str:
        self.calls += 1
        if self.escape_at is None:
            if self.calls <= self.overlay_after:
                self.last = COMPOSER
            elif self.wrong_overlay is not None and self.calls == self.overlay_after + 1:
                self.last = self.wrong_overlay
            else:
                self.last = self.overlay
            return self.last
        self.last = COMPOSER if self.calls - self.escape_at > self.absent_after_escape else self.overlay
        return self.last

    def on_send(self, payload: tuple[str, ...]) -> None:
        if payload == ("Escape",) and self.escape_at is None:
            self.escape_at = self.calls


class UpCoupledPane:
    """Render one Up per send and reject a send before that move was captured."""

    def __init__(self, windows: list[str], *, stale_after_send: int) -> None:
        self.windows = list(windows)
        self.stale_after_send = stale_after_send
        self.rendered = 0
        self.pending_stale = 0
        self.ups_sent = 0
        self.awaiting_observation = False
        self.last = windows[0]

    def __call__(self, tmux_client: object, session: str) -> str:
        if self.pending_stale:
            self.pending_stale -= 1
            self.last = self.windows[self.rendered]
            if not self.pending_stale:
                self.rendered = min(self.ups_sent, len(self.windows) - 1)
            return self.last
        self.last = self.windows[self.rendered]
        if self.awaiting_observation and self.rendered == self.ups_sent:
            self.awaiting_observation = False
        return self.last

    def on_send(self, payload: tuple[str, ...]) -> None:
        if payload != ("Up",):
            raise AssertionError(f"unexpected keys: {payload}")
        if self.awaiting_observation:
            raise AssertionError("sent another Up before the prior rendered move was captured")
        self.ups_sent += 1
        self.awaiting_observation = True
        self.pending_stale = self.stale_after_send
        if not self.pending_stale:
            self.rendered = min(self.ups_sent, len(self.windows) - 1)


def _harness(pane: object, on_send=None) -> tuple[FakeTmux, FakeTime, ExitStack]:
    clock = FakeTime()
    fake_tmux = FakeTmux(pane, on_send=on_send)
    stack = ExitStack()
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "capture", side_effect=pane))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "tmux", side_effect=fake_tmux))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "time", clock))
    return fake_tmux, clock, stack


class PluginCommandSettlementTests(unittest.TestCase):
    def test_dismiss_waits_for_delayed_overlay_and_close_before_next_invoke(self) -> None:
        pane = PluginSettlementScript(CTRLC_OVERLAY)
        fake_tmux, _, stack = _harness(pane, on_send=pane.on_send)
        with stack:
            plugin_ui._dismiss_plugin_command_settlement(CTX, SESSION, "ctrlc")
            plugin_ui._invoke(CTX, SESSION, EXIT_COMMAND)
        self.assertEqual(fake_tmux.sends, [("Escape",), ("-l", EXIT_COMMAND), ("Enter",)])
        self.assertIn("com.example.plugin-ui:ctrlc", fake_tmux.screen_at_send[0])
        self.assertNotIn("Command /plugin", fake_tmux.screen_at_send[1])

    def test_wrong_command_overlay_does_not_satisfy_ctrlc_settlement(self) -> None:
        pane = PluginSettlementScript(CTRLC_OVERLAY, wrong_overlay=EXIT_OVERLAY)
        fake_tmux, _, stack = _harness(pane, on_send=pane.on_send)
        with stack:
            plugin_ui._dismiss_plugin_command_settlement(CTX, SESSION, "ctrlc")
        self.assertEqual(fake_tmux.sends, [("Escape",)])
        self.assertIn("com.example.plugin-ui:ctrlc", fake_tmux.screen_at_send[0])
        self.assertGreater(pane.escape_at or 0, pane.overlay_after + 1)
        self.assertEqual(pane.last, COMPOSER)

    def test_exit_settlement_overlay_is_accepted(self) -> None:
        pane = PluginSettlementScript(EXIT_OVERLAY, overlay_after=1, absent_after_escape=1)
        fake_tmux, _, stack = _harness(pane, on_send=pane.on_send)
        with stack:
            plugin_ui._dismiss_plugin_command_settlement(CTX, SESSION, "exit")
        self.assertEqual(fake_tmux.sends, [("Escape",)])
        self.assertIn("com.example.plugin-ui:exit", fake_tmux.screen_at_send[0])

    def test_missing_overlay_times_out_without_sending_keys(self) -> None:
        pane = FakePane([COMPOSER])
        fake_tmux, clock, stack = _harness(pane)
        with stack, self.assertRaises(RuntimeError) as raised:
            plugin_ui._dismiss_plugin_command_settlement(CTX, SESSION, "ctrlc")
        self.assertIn("ctrlc plugin command settlement", str(raised.exception))
        self.assertIn(f"last screen:\n{COMPOSER}", str(raised.exception))
        self.assertEqual(fake_tmux.sends, [])
        self.assertGreater(clock.now, 0)

    def test_overlay_that_never_closes_times_out_after_one_escape(self) -> None:
        pane = FakePane([CTRLC_OVERLAY])
        fake_tmux, clock, stack = _harness(pane)
        with stack, self.assertRaises(RuntimeError) as raised:
            plugin_ui._dismiss_plugin_command_settlement(CTX, SESSION, "ctrlc")
        self.assertIn("ctrlc plugin command output closed", str(raised.exception))
        self.assertIn(f"last screen:\n{CTRLC_OVERLAY}", str(raised.exception))
        self.assertEqual(fake_tmux.sends, [("Escape",)])
        self.assertGreater(clock.now, 0)


class PreparatoryUpSequencingTests(unittest.TestCase):
    def test_each_up_waits_until_the_previous_move_is_captured(self) -> None:
        for stale_frames in (0, 1, 3):
            with self.subTest(stale_frames=stale_frames):
                pane = UpCoupledPane([LIVE, AFTER_1, AFTER_2, AFTER_3, AFTER_4], stale_after_send=stale_frames)
                fake_tmux, _, stack = _harness(pane, on_send=pane.on_send)
                with stack:
                    numbers = streaming_scroll._wait_for_preparatory_up_steps(
                        CLIENT,
                        SESSION,
                        LIVE_NUMBERS,
                        DRAFT,
                        steps=4,
                        keyboard_scroll_rows=KEYBOARD_SCROLL_ROWS,
                    )
                self.assertEqual(fake_tmux.sends, [("Up",)] * 4)
                self.assertEqual(fake_tmux.screen_at_send, [LIVE, AFTER_1, AFTER_2, AFTER_3])
                self.assertEqual(numbers, list(range(21, 48)))
                self.assertEqual(pane.last, AFTER_4)

    def test_invalid_moved_windows_time_out_after_only_one_up(self) -> None:
        too_short = _numbered_screen(30, 38)
        noncontiguous_numbers = [number for number in range(30, 41) if number != 35]
        noncontiguous = "\n".join([*(f"stream line {number:03d}" for number in noncontiguous_numbers), DRAFT])
        cases = {
            "unchanged": LIVE,
            "too short": too_short,
            "noncontiguous": noncontiguous,
            "draft mutated": _numbered_screen(30, 56, draft="MUTATED-DRAFT"),
        }
        for name, screen in cases.items():
            with self.subTest(name=name):
                pane = FakePane([screen])
                fake_tmux, clock, stack = _harness(pane)
                with stack, self.assertRaises(RuntimeError) as raised:
                    streaming_scroll._wait_for_preparatory_up_steps(
                        CLIENT,
                        SESSION,
                        LIVE_NUMBERS,
                        DRAFT,
                        steps=1,
                        keyboard_scroll_rows=KEYBOARD_SCROLL_ROWS,
                    )
                self.assertIn("preparatory Up 1 of 1 moved numbered window upward", str(raised.exception))
                self.assertIn(f"last screen:\n{screen}", str(raised.exception))
                self.assertEqual(fake_tmux.sends, [("Up",)])
                self.assertGreater(clock.now, 0)

    def test_final_window_without_keyboard_step_room_fails_closed(self) -> None:
        windows = [LIVE, *(_numbered_screen(start, 59) for start in range(32, 28, -1))]
        pane = UpCoupledPane(windows, stale_after_send=0)
        fake_tmux, _, stack = _harness(pane, on_send=pane.on_send)
        with stack, self.assertRaises(RuntimeError) as raised:
            streaming_scroll._wait_for_preparatory_up_steps(
                CLIENT,
                SESSION,
                LIVE_NUMBERS,
                DRAFT,
                steps=4,
                keyboard_scroll_rows=KEYBOARD_SCROLL_ROWS,
            )
        self.assertIn("room for a keyboard-step Up/Down", str(raised.exception))
        self.assertIn(f"numbers: {list(range(29, 60))}", str(raised.exception))
        self.assertEqual(fake_tmux.sends, [("Up",)] * 4)


AUTH_MARKER = "slash tool commands still work offline."
NAME_DRAFT = "│  /name TUI smoke"
LIVE_START = 11
LIVE_END = 30
SESSION_LIVE_NUMBERS = list(range(LIVE_START, LIVE_END + 1))


def _session_seed_screen(
    start: int,
    end: int,
    *,
    receipt: bool = True,
    receipt_complete: bool = True,
    elapsed: str = "4ms",
    auth_path: str = "/tmp/ava/auth.json",
    composer: bool = True,
    draft: str | None = None,
    name_title: bool = False,
    name_set: bool = False,
    enter_close: bool = False,
    skip: tuple[int, ...] = (),
) -> str:
    lines = [f"    SESSION SCROLLBACK LINE {index:02d}" for index in range(start, end + 1) if index not in skip]
    if name_title:
        lines.append("Command /name")
    if name_set:
        lines.append('session name set: "TUI smoke"')
    if enter_close:
        lines.append("Enter/Esc close")
    if receipt:
        lines.append("  Auth is required for provider openai.")
        if receipt_complete:
            lines.extend(
                [
                    "  Connect with /connect or /login in this TUI, or run ava connect openai --headless-oauth.",
                    "  Environment setup also works with OPENAI_API_KEY.",
                    "  Auth file:",
                    f"  {auth_path}",
                    f"  {AUTH_MARKER}",
                    f"  │ * Build · GPT-5.5 · {elapsed}",
                ]
            )
    if composer:
        composer_text = draft if draft is not None else "Type a message..."
        lines.extend(["│", f"│  {composer_text}", "│  GPT-5.5 · ctx 518 (0.2%)"])
    return "\n".join(lines)


SESSION_LIVE = _session_seed_screen(LIVE_START, LIVE_END)
SESSION_UP_1 = _session_seed_screen(LIVE_START - 1, LIVE_END)
SESSION_UP_2 = _session_seed_screen(LIVE_START - 2, LIVE_END)
SESSION_UP_3 = _session_seed_screen(LIVE_START - KEYBOARD_SCROLL_ROWS, LIVE_END)
SESSION_DOWN_NOISY = _session_seed_screen(
    LIVE_START,
    LIVE_END,
    elapsed="9ms",
    auth_path="/tmp/ava/config/ava/auth.json",
)
TITLE_FREE_PARTIAL = _session_seed_screen(20, LIVE_END, receipt=False, enter_close=True)
DELAYED_SEED = _session_seed_screen(LIVE_START, LIVE_END, receipt=False)
PARTIAL_RECEIPT = _session_seed_screen(LIVE_START, LIVE_END, receipt_complete=False)
NAME_MODAL = _session_seed_screen(LIVE_START, LIVE_END, name_title=True, name_set=True, enter_close=True, receipt=False)


class IdleArrowRoundtripPane:
    """Serve partial Up frames, then a noisy Down tail, and reject a premature Down."""

    def __init__(self, up_frames: list[str], down: str) -> None:
        self.live = SESSION_LIVE
        self.up_queue = list(up_frames)
        self.full_up = up_frames[-1]
        self.down = down
        self.phase = "live"
        self.last = SESSION_LIVE
        self.saw_full_up = False

    def __call__(self, tmux_client: object, session: str) -> str:
        if self.phase == "up":
            if self.up_queue:
                self.last = self.up_queue.pop(0)
            else:
                self.last = self.full_up
            if self.last == self.full_up:
                self.saw_full_up = True
            return self.last
        if self.phase == "down":
            self.last = self.down
            return self.last
        self.last = self.live
        return self.last

    def on_send(self, payload: tuple[str, ...]) -> None:
        if payload == ("Up",):
            self.phase = "up"
            return
        if payload == ("Down",):
            if not self.saw_full_up:
                raise AssertionError("sent Down before the finished 3-row Up step was captured")
            self.phase = "down"
            return
        raise AssertionError(f"unexpected keys: {payload}")


def _old_up_any_row_difference(baseline_rows: tuple[str, ...]):
    return lambda screen: tuple(screen.splitlines()[:-3]) != baseline_rows and NAME_DRAFT not in screen


def _old_down_exact_rows(baseline_rows: tuple[str, ...]):
    return lambda screen: tuple(screen.splitlines()[:-3]) == baseline_rows and NAME_DRAFT not in screen


class MainSessionIdleArrowSequencingTests(unittest.TestCase):
    def test_old_line_30_wait_accepts_seed_before_auth_receipt(self) -> None:
        pane = FakePane([DELAYED_SEED])
        fake_tmux, _, stack = _harness(pane)
        with stack:
            screen = tui_smoke_helpers.wait_for(
                CLIENT, SESSION, r"SESSION SCROLLBACK LINE 30", "session-management ordinary scrollback seed"
            )
        self.assertEqual(screen, DELAYED_SEED)
        self.assertNotIn(AUTH_MARKER, screen)
        self.assertEqual(fake_tmux.sends, [])

    def test_old_name_absence_accepts_title_free_partial_frame(self) -> None:
        pane = FakePane([TITLE_FREE_PARTIAL])
        fake_tmux, _, stack = _harness(pane)
        with stack:
            screen = tui_smoke_helpers.wait_for_absent(
                CLIENT, SESSION, r"Command /name|session name set", "session name output closed"
            )
        self.assertEqual(screen, TITLE_FREE_PARTIAL)
        self.assertIn("Enter/Esc close", screen)
        self.assertNotIn(AUTH_MARKER, screen)
        self.assertEqual(fake_tmux.sends, [])

    def test_old_up_any_difference_accepts_partial_one_row_step(self) -> None:
        baseline = tuple(SESSION_LIVE.splitlines()[:-3])
        pane = FakePane([SESSION_UP_1])
        fake_tmux, _, stack = _harness(pane)
        with stack:
            screen = tui_smoke_helpers.wait_for_screen_state(
                CLIENT,
                SESSION,
                _old_up_any_row_difference(baseline),
                "idle Up arrow transcript movement without history recall",
            )
        self.assertEqual(screen, SESSION_UP_1)
        self.assertEqual(fake_tmux.sends, [])

    def test_old_down_exact_row_baseline_times_out_on_settled_receipt(self) -> None:
        # GitHub #72: an earlier incomplete baseline never matches the later settled
        # live tail (lines 11..30 plus the completed offline auth receipt).
        baseline = tuple(DELAYED_SEED.splitlines()[:-3])
        pane = FakePane([SESSION_LIVE])
        fake_tmux, clock, stack = _harness(pane)
        with stack, self.assertRaises(RuntimeError) as raised:
            tui_smoke_helpers.wait_for_screen_state(
                CLIENT,
                SESSION,
                _old_down_exact_rows(baseline),
                "idle Down arrow return to live tail",
            )
        message = str(raised.exception)
        self.assertIn("idle Down arrow return to live tail", message)
        self.assertIn(AUTH_MARKER, message)
        self.assertIn("SESSION SCROLLBACK LINE 11", message)
        self.assertEqual(fake_tmux.sends, [])
        self.assertGreater(clock.now, 0)

    def test_old_down_exact_row_baseline_times_out_on_elapsed_noise(self) -> None:
        baseline = tuple(SESSION_LIVE.splitlines()[:-3])
        pane = FakePane([SESSION_DOWN_NOISY])
        fake_tmux, clock, stack = _harness(pane)
        with stack, self.assertRaises(RuntimeError) as raised:
            tui_smoke_helpers.wait_for_screen_state(
                CLIENT,
                SESSION,
                _old_down_exact_rows(baseline),
                "idle Down arrow return to live tail",
            )
        self.assertIn("idle Down arrow return to live tail", str(raised.exception))
        self.assertEqual(fake_tmux.sends, [])
        self.assertGreater(clock.now, 0)

    def test_new_seed_wait_rejects_persistent_delayed_auth_receipt(self) -> None:
        pane = FakePane([DELAYED_SEED])
        fake_tmux, clock, stack = _harness(pane)
        with stack, self.assertRaises(RuntimeError) as raised:
            main_session_mgmt._wait_for_ordinary_scrollback_seed(CLIENT, SESSION)
        self.assertIn("session-management ordinary scrollback seed", str(raised.exception))
        self.assertIn(DELAYED_SEED, str(raised.exception))
        self.assertEqual(fake_tmux.sends, [])
        self.assertGreater(clock.now, 0)

    def test_new_name_close_rejects_persistent_title_free_partial_frame(self) -> None:
        pane = FakePane([TITLE_FREE_PARTIAL])
        fake_tmux, clock, stack = _harness(pane)
        with stack, self.assertRaises(RuntimeError) as raised:
            main_session_mgmt._wait_for_session_name_output_closed(CLIENT, SESSION)
        self.assertIn("session name output fully closed", str(raised.exception))
        self.assertIn(TITLE_FREE_PARTIAL, str(raised.exception))
        self.assertEqual(fake_tmux.sends, [])
        self.assertGreater(clock.now, 0)

    def test_ordinary_seed_waits_through_delayed_auth_receipt(self) -> None:
        pane = FakePane([DELAYED_SEED, PARTIAL_RECEIPT, SESSION_LIVE])
        fake_tmux, _, stack = _harness(pane)
        with stack:
            numbers = main_session_mgmt._wait_for_ordinary_scrollback_seed(CLIENT, SESSION)
        self.assertEqual(numbers, SESSION_LIVE_NUMBERS)
        self.assertEqual(pane.last, SESSION_LIVE)
        self.assertEqual(fake_tmux.sends, [])

    def test_name_close_waits_through_title_free_partial_frame(self) -> None:
        pane = FakePane([NAME_MODAL, TITLE_FREE_PARTIAL, SESSION_LIVE])
        fake_tmux, _, stack = _harness(pane)
        with stack:
            numbers = main_session_mgmt._wait_for_session_name_output_closed(CLIENT, SESSION)
        self.assertEqual(numbers, SESSION_LIVE_NUMBERS)
        self.assertEqual(pane.last, SESSION_LIVE)
        self.assertEqual(fake_tmux.sends, [])

    def test_idle_up_waits_through_partial_one_and_two_row_frames(self) -> None:
        pane = FakePane([SESSION_UP_1, SESSION_UP_2, SESSION_UP_3])
        fake_tmux, _, stack = _harness(pane)
        with stack:
            screen = main_session_mgmt._wait_for_idle_up_keyboard_step(CLIENT, SESSION, SESSION_LIVE_NUMBERS)
        self.assertEqual(screen, SESSION_UP_3)
        self.assertEqual(fake_tmux.sends, [("Up",)])
        self.assertEqual(fake_tmux.screen_at_send, [SESSION_UP_1])

    def test_idle_arrow_roundtrip_does_not_send_down_before_finished_up_step(self) -> None:
        pane = IdleArrowRoundtripPane([SESSION_UP_1, SESSION_UP_2, SESSION_UP_3], SESSION_DOWN_NOISY)
        fake_tmux, _, stack = _harness(pane, on_send=pane.on_send)
        with stack:
            up_screen = main_session_mgmt._wait_for_idle_up_keyboard_step(CLIENT, SESSION, SESSION_LIVE_NUMBERS)
            down_screen = main_session_mgmt._wait_for_idle_down_live_tail(CLIENT, SESSION, SESSION_LIVE_NUMBERS)
        self.assertEqual(up_screen, SESSION_UP_3)
        self.assertEqual(down_screen, SESSION_DOWN_NOISY)
        self.assertEqual(fake_tmux.sends, [("Up",), ("Down",)])
        self.assertEqual(fake_tmux.screen_at_send, [SESSION_LIVE, SESSION_UP_3])

    def test_idle_down_accepts_elapsed_status_and_path_noise(self) -> None:
        pane = FakePane([SESSION_DOWN_NOISY])
        fake_tmux, _, stack = _harness(pane)
        with stack:
            screen = main_session_mgmt._wait_for_idle_down_live_tail(CLIENT, SESSION, SESSION_LIVE_NUMBERS)
        self.assertEqual(screen, SESSION_DOWN_NOISY)
        self.assertEqual(fake_tmux.sends, [("Down",)])
        self.assertTrue(main_session_mgmt._idle_down_restored_live_tail(SESSION_DOWN_NOISY, SESSION_LIVE_NUMBERS))

    def test_wrong_scroll_and_draft_mutations_time_out_after_one_key(self) -> None:
        up_cases = {
            "unchanged": SESSION_LIVE,
            "one row": SESSION_UP_1,
            "two rows": SESSION_UP_2,
            "draft mutated": _session_seed_screen(LIVE_START - KEYBOARD_SCROLL_ROWS, LIVE_END, draft="/name TUI smoke"),
            "noncontiguous": _session_seed_screen(LIVE_START - KEYBOARD_SCROLL_ROWS, LIVE_END, skip=(15,)),
            "composer missing": _session_seed_screen(LIVE_START - KEYBOARD_SCROLL_ROWS, LIVE_END, composer=False),
        }
        for name, screen in up_cases.items():
            with self.subTest(gate="up", name=name):
                pane = FakePane([screen])
                fake_tmux, clock, stack = _harness(pane)
                with stack, self.assertRaises(RuntimeError) as raised:
                    main_session_mgmt._wait_for_idle_up_keyboard_step(CLIENT, SESSION, SESSION_LIVE_NUMBERS)
                self.assertIn("idle Up arrow transcript movement without history recall", str(raised.exception))
                self.assertIn(f"last screen:\n{screen}", str(raised.exception))
                self.assertEqual(fake_tmux.sends, [("Up",)])
                self.assertGreater(clock.now, 0)

        down_cases = {
            "still scrolled": SESSION_UP_3,
            "missing receipt": _session_seed_screen(LIVE_START, LIVE_END, receipt=False),
            "draft mutated": _session_seed_screen(LIVE_START, LIVE_END, draft="/name TUI smoke"),
            "composer missing": _session_seed_screen(LIVE_START, LIVE_END, composer=False),
            "wrong window": _session_seed_screen(LIVE_START + 1, LIVE_END),
        }
        for name, screen in down_cases.items():
            with self.subTest(gate="down", name=name):
                pane = FakePane([screen])
                fake_tmux, clock, stack = _harness(pane)
                with stack, self.assertRaises(RuntimeError) as raised:
                    main_session_mgmt._wait_for_idle_down_live_tail(CLIENT, SESSION, SESSION_LIVE_NUMBERS)
                self.assertIn("idle Down arrow return to live tail", str(raised.exception))
                self.assertIn(f"last screen:\n{screen}", str(raised.exception))
                self.assertEqual(fake_tmux.sends, [("Down",)])
                self.assertGreater(clock.now, 0)


if __name__ == "__main__":
    unittest.main()
