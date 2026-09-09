#!/usr/bin/env python3
"""Deterministic regression tests for model-selector synchronization waits."""

from __future__ import annotations

import ast
import pathlib
import sys
import types
import unittest
from contextlib import ExitStack
from unittest import mock

# Scenario modules import the harness helpers by top-level name.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import tui_smoke_helpers
from tui_smoke_helpers import POLL_INTERVAL
from tui_tmux_scenarios import main_models_selectors

WIDTH = 40
HEIGHT = 4
CLIENT = object()
SESSION = "unit-test-session"
LABEL = "unit model selector"
TITLE_ONLY = "Select model\nfilter Search models\nmodel row\n"
VALID_FRAME = "Select model\nfilter Search models\n› GPT-5.5\n"
IRRELEVANT_MODAL = "Select provider\nfilter Search providers\n› Moonshot\n"
WRONG_HEIGHT = "Select model\nfilter Search models\n› GPT-5.5"


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


class FakeTmux:
    """Serve session checks and scripted authoritative geometry queries."""

    def __init__(self, dimensions: list[str] | None = None) -> None:
        self._dimensions = list(dimensions or [f"{WIDTH},{HEIGHT},{WIDTH},{HEIGHT}"])
        self.dimension_queries = 0

    def __call__(self, tmux_client: object, *args: str, check: bool = True) -> types.SimpleNamespace:
        if args and args[0] == "has-session":
            return types.SimpleNamespace(returncode=0, stdout="")
        if args and args[0] == "display-message":
            self.dimension_queries += 1
            if len(self._dimensions) > 1:
                dimensions = self._dimensions.pop(0)
            else:
                dimensions = self._dimensions[0]
            return types.SimpleNamespace(returncode=0, stdout=f"{dimensions}\n")
        raise AssertionError(f"unexpected tmux call: {args}")


class FakeTime:
    """Advance the monotonic clock through polling sleeps only."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


def _harness(
    frames: list[str], dimensions: list[str] | None = None
) -> tuple[FakePane, FakeTmux, FakeTime, ExitStack]:
    pane = FakePane(frames)
    fake_tmux = FakeTmux(dimensions)
    clock = FakeTime()
    stack = ExitStack()
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "capture", side_effect=pane))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "tmux", side_effect=fake_tmux))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "time", clock))
    stack.enter_context(mock.patch.object(main_models_selectors, "tmux", side_effect=fake_tmux))
    return pane, fake_tmux, clock, stack


def _wait() -> str:
    return main_models_selectors._wait_for_selected_modal(
        CLIENT,
        SESSION,
        r"Select model|filter\s+Search models",
        LABEL,
        selected_row_pattern=r"GPT-5\.5",
        geometry=(WIDTH, HEIGHT),
    )


class ModelSelectorWaitTests(unittest.TestCase):
    def test_title_only_frame_waits_for_selected_row_in_complete_frame(self) -> None:
        pane, _, clock, stack = _harness([TITLE_ONLY, VALID_FRAME])
        with stack:
            frame = _wait()
        self.assertEqual(frame, VALID_FRAME)
        self.assertEqual(pane.calls, 2)
        self.assertLess(clock.now, 1.0)

    def test_irrelevant_modal_selection_does_not_pass(self) -> None:
        pane, _, _, stack = _harness([IRRELEVANT_MODAL, VALID_FRAME])
        with stack:
            frame = _wait()
        self.assertEqual(frame, VALID_FRAME)
        self.assertEqual(pane.calls, 2)

    def test_stale_capture_height_does_not_pass_at_target_geometry(self) -> None:
        pane, _, _, stack = _harness([WRONG_HEIGHT, VALID_FRAME])
        with stack:
            frame = _wait()
        self.assertEqual(frame, VALID_FRAME)
        self.assertEqual(pane.calls, 2)

    def test_wrong_geometry_waits_and_identical_valid_text_then_passes(self) -> None:
        dimensions = [f"{WIDTH},{HEIGHT - 1},{WIDTH},{HEIGHT - 1}", f"{WIDTH},{HEIGHT},{WIDTH},{HEIGHT}"]
        pane, fake_tmux, _, stack = _harness([VALID_FRAME], dimensions)
        with stack:
            frame = _wait()
        self.assertEqual(frame, VALID_FRAME)
        self.assertEqual(pane.calls, 2)
        self.assertEqual(fake_tmux.dimension_queries, 2)

    def test_persistent_invalid_frame_times_out_with_actionable_bounded_error(self) -> None:
        pane, _, clock, stack = _harness([TITLE_ONLY])
        with stack:
            with self.assertRaises(RuntimeError) as raised:
                _wait()
        message = str(raised.exception)
        self.assertIn(f"timed out waiting for {LABEL} with a selected modal row", message)
        self.assertIn(f"at {WIDTH}x{HEIGHT} window/pane geometry", message)
        self.assertIn("Select model", message)
        self.assertGreaterEqual(clock.now, 8.0)
        self.assertLess(clock.now, 8.0 + 3 * POLL_INTERVAL)
        self.assertGreaterEqual(pane.calls, 399)

    def test_scenario_selected_row_sites_use_helper_and_keep_assertions(self) -> None:
        source = pathlib.Path(main_models_selectors.__file__).read_text(encoding="utf-8")
        tree = ast.parse(source)
        scenario = next(
            node
            for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name == "scenario_main_models_selectors"
        )
        helper_assignments = {
            target.id
            for node in ast.walk(scenario)
            if isinstance(node, ast.Assign)
            and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Name)
            and node.value.func.id == "_wait_for_selected_modal"
            for target in node.targets
            if isinstance(target, ast.Name)
        }
        expected = {
            "command_model_selector",
            "wheel_burst_selector",
            "wheel_burst_cleared",
            "resized_model_selector",
            "ghostty_release_screen",
            "provider_modal",
            "chained_thinking",
            "direct_thinking",
            "reopened_low",
            "reopened_default",
        }
        self.assertEqual(helper_assignments, expected)

        # Existing scenario-level identity/current-value checks remain explicit;
        # the wait only synchronizes the frame on which they operate.
        selected_row_calls = [
            node
            for node in ast.walk(scenario)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "selected_modal_row"
        ]
        self.assertEqual(len(selected_row_calls), len(expected))
        for assertion_text in (
            "Model selector did not expose a selected row before navigation",
            "raw same-direction wheel burst did not advance the model selector",
            "Model selector lost its selected row while resizing",
            "Provider question modal did not expose a selected row",
            "did not stage a Default thinking-mode selector",
            "Ctrl+T did not reopen the same selector with Default current",
            "Concrete thinking-mode selection was not authoritative on reopen",
            "Default thinking-mode selection was not authoritative on reopen",
        ):
            self.assertIn(assertion_text, source)


if __name__ == "__main__":
    unittest.main()
