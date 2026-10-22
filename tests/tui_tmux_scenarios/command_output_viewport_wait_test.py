#!/usr/bin/env python3
"""Deterministic regression tests for the bounded bash modal viewport wait.

A composer repaint can satisfy the old text-change wait while the modal still
shows 1-26/409. Replay that acceptance and exercise the scenario's real state
wait with scripted captures and a fake clock, including bounded wheel failure.
"""

from __future__ import annotations

import ast
import pathlib
import re
import sys
import types
import unittest
from contextlib import ExitStack
from unittest import mock

# Scenario modules import the harness helpers by top-level name.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import tui_smoke_helpers
from tui_smoke_helpers import POLL_INTERVAL
from tui_tmux_scenarios import main_permission_flow

CLIENT = object()
SESSION = "unit-test-session"
INITIAL_LABEL = "allowed bounded local bash spill modal"
WHEEL_LABEL = "bounded local bash modal wheel scroll redraw"
INITIAL_VIEWPORT = (1, 26, 409)


def _spill_frame(first: int = 1) -> str:
    """Model the reported 120x32 capture and its truthful modal footer."""

    output = ["exit: 0 (output truncated to last 200/20000 lines)"]
    output.extend(str(value) for value in range(19801, 20001))
    return "\n".join(
        ["  ! OpenAI not connected · /connect", "", "       Command /bash"]
        + [f"       {line}" for line in output[first - 1 : first + 25]]
        + [
            f"       {first}-{first + 25}/409 · ↑↓/PgUp/PgDn scroll · Enter close",
            "│  Type a message...",
            "│  GPT-5.5 · ctx 512 (0.2%)",
        ]
    )


INITIAL_FRAME = _spill_frame()
UNRELATED_REPAINT = INITIAL_FRAME.replace("ctx 512", "ctx 513")
MOVED_FRAME = _spill_frame(9)
PARTIAL_FRAME = INITIAL_FRAME.replace("       1-26/409 · ↑↓/PgUp/PgDn scroll · Enter close", "")


class FakePane:
    """Replay scripted captures, then keep returning the final capture."""

    def __init__(self, frames: list[str]) -> None:
        if not frames:
            raise ValueError("FakePane needs at least one frame")
        self._frames = list(frames)
        self.calls = 0

    def __call__(self, tmux_client: object, session: str) -> str:
        self.calls += 1
        if len(self._frames) > 1:
            return self._frames.pop(0)
        return self._frames[0]


class FakeTime:
    """Advance the monotonic clock through polling sleeps only."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


def _harness(frames: list[str]) -> tuple[FakePane, FakeTime, ExitStack]:
    pane = FakePane(frames)
    clock = FakeTime()
    stack = ExitStack()
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "capture", side_effect=pane))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "tmux", return_value=types.SimpleNamespace(returncode=0)))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "time", clock))
    stack.enter_context(mock.patch.object(main_permission_flow, "capture", side_effect=pane))
    return pane, clock, stack


def _wait_for_initial() -> str:
    return main_permission_flow._wait_for_bash_modal_viewport(CLIENT, SESSION, INITIAL_LABEL, timeout=30.0)


def _wait_for_movement() -> str:
    return main_permission_flow._wait_for_bash_modal_viewport(CLIENT, SESSION, WHEEL_LABEL, after=INITIAL_VIEWPORT)


class CommandOutputViewportWaitTests(unittest.TestCase):
    def test_new_wait_polls_past_the_same_unrelated_repaints(self) -> None:
        pane, clock, stack = _harness([UNRELATED_REPAINT, UNRELATED_REPAINT, MOVED_FRAME])
        with stack:
            frame = _wait_for_movement()
        self.assertEqual(frame, MOVED_FRAME)
        self.assertEqual(pane.calls, 3)
        self.assertAlmostEqual(clock.now, 2 * POLL_INTERVAL)

    def test_initial_wait_requires_a_valid_viewport_before_wheel_input(self) -> None:
        pane, clock, stack = _harness([PARTIAL_FRAME, INITIAL_FRAME])
        with stack:
            frame = _wait_for_initial()
        self.assertEqual(frame, INITIAL_FRAME)
        self.assertEqual(pane.calls, 2)
        self.assertAlmostEqual(clock.now, POLL_INTERVAL)

    def test_initial_wait_still_requires_successful_bash_output(self) -> None:
        incomplete = INITIAL_FRAME.replace("exit: 0", "running")
        pane, _, stack = _harness([incomplete, INITIAL_FRAME])
        with stack:
            frame = _wait_for_initial()
        self.assertEqual(frame, INITIAL_FRAME)
        self.assertEqual(pane.calls, 2)

    def test_genuine_movement_returns_the_accepted_frame_without_another_capture(self) -> None:
        pane, clock, stack = _harness([MOVED_FRAME, PARTIAL_FRAME])
        with stack:
            frame = _wait_for_movement()
        self.assertEqual(frame, MOVED_FRAME)
        self.assertNotIn("exit: 0", frame)  # The success summary has scrolled out of view.
        self.assertEqual(pane.calls, 1)
        self.assertEqual(clock.now, 0.0)

    def test_partial_repaint_with_old_footer_does_not_count_as_movement(self) -> None:
        partial_scroll = MOVED_FRAME.replace("9-34/409", "1-26/409")
        pane, _, stack = _harness([partial_scroll, PARTIAL_FRAME, MOVED_FRAME])
        with stack:
            frame = _wait_for_movement()
        self.assertEqual(frame, MOVED_FRAME)
        self.assertEqual(pane.calls, 3)

    def test_wrong_modal_unrelated_ranges_and_invalid_footers_are_polled_past(self) -> None:
        invalid_frames = {
            "different modal": MOVED_FRAME.replace("Command /bash", "Command /permissions"),
            "title prefix only": MOVED_FRAME.replace("Command /bash", "Command /bashful"),
            "body range only": UNRELATED_REPAINT.replace("19801", "99-124/409"),
            "range outside modal": PARTIAL_FRAME.replace("ctx 512", "99-124/409 · ↑↓/PgUp/PgDn scroll · Enter close"),
            "footer in body": (
                MOVED_FRAME.replace("       19808", "       9-34/409 · ↑↓/PgUp/PgDn scroll · Enter close")
                .replace("       9-34/409 · ↑↓/PgUp/PgDn scroll · Enter close\n│", "\n│")
            ),
            "missing footer chrome": MOVED_FRAME.replace(" · ↑↓/PgUp/PgDn scroll · Enter close", ""),
            "partial footer chrome": MOVED_FRAME.replace("Enter close", "Enter"),
            "different total": MOVED_FRAME.replace("9-34/409", "9-34/410"),
            "zero first row": MOVED_FRAME.replace("9-34/409", "0-25/409"),
            "reversed range": MOVED_FRAME.replace("9-34/409", "34-9/409"),
            "past total": MOVED_FRAME.replace("9-34/409", "400-425/409"),
            "wrong visible row count": MOVED_FRAME.replace("9-34/409", "9-35/409"),
        }
        for name, invalid in invalid_frames.items():
            with self.subTest(frame=name):
                pane, _, stack = _harness([invalid, MOVED_FRAME])
                with stack:
                    frame = _wait_for_movement()
                self.assertEqual(frame, MOVED_FRAME)
                self.assertEqual(pane.calls, 2)

    def test_backward_movement_does_not_pass(self) -> None:
        pane, _, stack = _harness([INITIAL_FRAME, MOVED_FRAME])
        with stack:
            frame = main_permission_flow._wait_for_bash_modal_viewport(
                CLIENT, SESSION, WHEEL_LABEL, after=(5, 30, 409)
            )
        self.assertEqual(frame, MOVED_FRAME)
        self.assertEqual(pane.calls, 2)

    def test_persistent_no_movement_times_out_with_last_frame(self) -> None:
        for stalled in (INITIAL_FRAME, UNRELATED_REPAINT, PARTIAL_FRAME):
            with self.subTest(frame=stalled):
                pane, clock, stack = _harness([stalled])
                with stack:
                    with self.assertRaises(RuntimeError) as raised:
                        _wait_for_movement()
                message = str(raised.exception)
                self.assertIn(f"timed out waiting for {WHEEL_LABEL}", message)
                self.assertIn("past 1-26/409", message)
                self.assertIn(f"last screen:\n{stalled}", message)
                self.assertGreaterEqual(clock.now, 8.0)
                self.assertLess(clock.now, 8.0 + 3 * POLL_INTERVAL)
                self.assertGreaterEqual(pane.calls, 399)

    def test_persistent_partial_initial_frame_times_out_at_existing_deadline(self) -> None:
        _, clock, stack = _harness([PARTIAL_FRAME])
        with stack:
            with self.assertRaises(RuntimeError) as raised:
                _wait_for_initial()
        message = str(raised.exception)
        self.assertIn(f"timed out waiting for {INITIAL_LABEL}", message)
        self.assertIn(f"last screen:\n{PARTIAL_FRAME}", message)
        self.assertGreaterEqual(clock.now, 30.0)
        self.assertLess(clock.now, 30.0 + 3 * POLL_INTERVAL)

    def test_scenario_wires_both_waits_around_the_unchanged_wheel_input(self) -> None:
        source = pathlib.Path(main_permission_flow.__file__).read_text(encoding="utf-8")
        tree = ast.parse(source)
        scenario = next(
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == "scenario_main_permission_flow"
        )
        assignments = {
            target.id: node
            for node in scenario.body
            if isinstance(node, ast.Assign)
            for target in node.targets
            if isinstance(target, ast.Name)
        }
        for name in ("seq_result", "wheel_scrolled"):
            self.assertEqual(assignments[name].value.func.id, "_wait_for_bash_modal_viewport")
        self.assertEqual(ast.literal_eval(assignments["seq_result"].value.keywords[0].value), 30.0)
        wheel_wait = assignments["wheel_scrolled"].value
        self.assertEqual([(kw.arg, kw.value.id) for kw in wheel_wait.keywords], [("after", "initial_footer")])
        for name, frame in (("initial_footer", "seq_result"), ("wheel_footer", "wheel_scrolled")):
            self.assertEqual(assignments[name].value.func.id, "_bash_modal_viewport")
            self.assertEqual(assignments[name].value.args[0].id, frame)
        # Exactly the same eight SGR wheel events remain between the two waits.
        wheel_input = scenario.body[scenario.body.index(assignments["initial_footer"]) + 1]
        self.assertEqual(wheel_input.value.func.id, "send_literal")
        self.assertEqual(ast.literal_eval(wheel_input.value.args[2].left), "\x1b[<65;4;6M")
        self.assertIsInstance(wheel_input.value.args[2].op, ast.Mult)
        self.assertEqual(ast.literal_eval(wheel_input.value.args[2].right), 8)
        self.assertLess(assignments["seq_result"].lineno, wheel_input.lineno)
        self.assertLess(wheel_input.lineno, assignments["wheel_scrolled"].lineno)
        self.assertIn("wheel_footer[0] <= initial_footer[0]", source)
        for assertion_text in (
            "command-output mouse wheel did not advance the bounded viewport",
            "bounded local spill modal did not expose only its retained tail with truthful summary",
            "expanded local spill modal lacked bounded truthful metadata",
        ):
            self.assertIn(assertion_text, source)

    def test_old_change_wait_and_recapture_accept_unrelated_repaint_before_movement(self) -> None:
        pane, clock, stack = _harness([UNRELATED_REPAINT, UNRELATED_REPAINT, MOVED_FRAME])
        with stack:
            changed = main_permission_flow.wait_for_screen_change(
                CLIENT, SESSION, INITIAL_FRAME, "bounded local bash modal wheel scroll redraw"
            )
            recaptured = main_permission_flow.capture(CLIENT, SESSION)
        self.assertEqual(changed, UNRELATED_REPAINT)
        self.assertEqual(recaptured, UNRELATED_REPAINT)
        self.assertEqual(pane.calls, 2)
        self.assertEqual(clock.now, 0.0)
        # The strict movement assertion fails even though the next capture
        # would show movement: neither of the old acceptance points waited for it.
        initial_footer = re.search(r"(\d+)-(\d+)/(\d+)", INITIAL_FRAME)
        wheel_footer = re.search(r"(\d+)-(\d+)/(\d+)", recaptured)
        self.assertEqual(int(wheel_footer.group(1)), int(initial_footer.group(1)))
        self.assertEqual(pane(CLIENT, SESSION), MOVED_FRAME)

    def test_old_initial_wait_accepts_partial_frame_without_viewport(self) -> None:
        pane, clock, stack = _harness([PARTIAL_FRAME, INITIAL_FRAME])
        with stack:
            frame = main_permission_flow.wait_for(
                CLIENT, SESSION, r"(?s)Command /bash.*exit: 0", "allowed bounded local bash spill modal", timeout=30.0
            )
        self.assertEqual(frame, PARTIAL_FRAME)
        self.assertIsNone(re.search(r"(\d+)-(\d+)/(\d+)", frame))
        self.assertEqual(pane.calls, 1)
        self.assertEqual(clock.now, 0.0)


if __name__ == "__main__":
    unittest.main()
