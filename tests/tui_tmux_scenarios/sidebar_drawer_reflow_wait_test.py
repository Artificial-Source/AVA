#!/usr/bin/env python3
"""Deterministic regression tests for the open-drawer 160x12 reflow wait.

The extra full-terminal run caught ``main_startup_trust_keybinds`` failing at
the open sidebar drawer 100x12 -> 160x12 resize: the scenario waited for a
trimmed-capture text change, but ``capture()`` strips trailing empty columns,
so the already-reflowed 160x12 drawer frame was byte-identical to the 100x12
frame and the change wait timed out after eight seconds even though every
``assert_drawer_frame`` condition already passed at the target geometry. This
was a test-design defect, not an app resize failure: a width-only resize must
synchronize on tmux's authoritative dimensions and one fully valid drawer
frame, never on a text difference (the same file's ``capture_idle_shell``
already documents width-only text identity).

These tests exercise the real scenario helper (``_wait_for_drawer_reflow``,
which reuses ``_assert_drawer_frame`` inside its predicate) against scripted
pane captures, a fake tmux dimension query, and a fake monotonic clock, so the
persistent-invalid cases prove the bounded eight-second poll without any real
sleeps.

This file additionally covers the second demonstrated short-path-length
regression in the same scenario (TUI-003): at 84 captured columns the /context
modal wraps rows mid-field, splitting required freshness tokens such as
loaded_bytes=19 ("loade" + indented "d_bytes=19") and status=current
("sta"/"tus=current"). The real ``_normalize_context_section`` normalizer is
exercised directly so qualifying a valid short path never depends on where the
wrap lands.
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

# The scenario package imports its sibling harness modules (tui_smoke_helpers,
# fake_provider, test_timing_trace) by top-level name, so the tests/ directory
# that owns the package must precede the script directory on sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import tui_smoke_helpers
from tui_smoke_helpers import POLL_INTERVAL, wait_for_screen_change
from tui_tmux_scenarios import main_startup_trust_keybinds

WIDTH = 160
HEIGHT = 12
LABEL = "open sidebar drawer 160x12 reflow"
WAIT_LABEL = f"{LABEL} valid {WIDTH}x{HEIGHT} drawer frame"
CLIENT = object()
SESSION = "unit-test-session"
BLANK_FRAME = ""


def _drawer_frame(
    *,
    width: int = WIDTH,
    height: int = HEIGHT,
    title: str | None = "Session overview",
    composer: str = "│  Type a message...",
    footer: str = "│  GPT-5.5 · ctx 12k",
    activity_rows: int = 1,
    live_session: bool = False,
    overflow: bool = False,
    control: bool = False,
) -> str:
    """Model a trimmed drawer capture; only the specified condition is broken."""

    lines = [f"drawer row {index}" for index in range(height)]
    if title is not None:
        lines[1] = title
    for index in range(activity_rows):
        lines[3 + index] = f"Activity section {index}"
    if live_session:
        lines[6] = "live session"
    lines[height - 2] = composer
    lines[height - 1] = footer
    if overflow:
        lines[4] = "x" * (width + 1)
    text = "\n".join(lines)
    if control:
        text += "\x07"
    return text


VALID_FRAME = _drawer_frame()


class FakePane:
    """Scripted pane captures: replay queued frames, then repeat the last one."""

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
    """Answer only the tmux queries the wait really issues."""

    def __init__(self, dimensions: str) -> None:
        self.dimensions = dimensions
        self.dimension_queries = 0

    def __call__(self, tmux_client: object, *args: str, check: bool = True) -> types.SimpleNamespace:
        if args and args[0] == "display-message":
            self.dimension_queries += 1
            return types.SimpleNamespace(returncode=0, stdout=f"{self.dimensions}\n")
        if args and args[0] == "has-session":
            return types.SimpleNamespace(returncode=0, stdout="")
        raise AssertionError(f"unexpected tmux call: {args}")


class FakeTime:
    """Monotonic clock advanced only by ``sleep``, making timeouts instant."""

    def __init__(self) -> None:
        self.now = 0.0

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


def _harness(frames: list[str], dimensions: str = f"{WIDTH},{HEIGHT}") -> tuple[FakePane, FakeTmux, FakeTime, ExitStack]:
    """Patch the wait primitives the scenario helper really calls.

    ``tui_smoke_helpers.wait_for_screen_state`` resolves ``capture``, ``tmux``,
    and ``time`` through its own module globals, while ``_wait_for_drawer_reflow``
    resolves ``tmux`` through the scenario module globals for the authoritative
    dimension query; patching both namespaces drives the real wait machinery
    deterministically.
    """

    pane = FakePane(frames)
    fake_tmux = FakeTmux(dimensions)
    clock = FakeTime()
    stack = ExitStack()
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "capture", side_effect=pane))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "tmux", side_effect=fake_tmux))
    stack.enter_context(mock.patch.object(tui_smoke_helpers, "time", clock))
    stack.enter_context(mock.patch.object(main_startup_trust_keybinds, "tmux", fake_tmux))
    return pane, fake_tmux, clock, stack


class SidebarDrawerReflowWaitTests(unittest.TestCase):
    """Exercise the real ``_wait_for_drawer_reflow`` against scripted frames."""

    def test_identical_valid_frame_at_target_geometry_is_accepted(self) -> None:
        # The exact failure mode from the extra full-terminal run: the trimmed
        # reflowed frame is byte-identical to the pre-resize frame, and it must
        # still complete the wait because the geometry is authoritative.
        pane, fake_tmux, clock, stack = _harness([VALID_FRAME])
        with stack:
            frame = main_startup_trust_keybinds._wait_for_drawer_reflow(CLIENT, SESSION, WIDTH, HEIGHT, LABEL)
        self.assertEqual(frame, VALID_FRAME)
        self.assertEqual(pane.calls, 1)
        self.assertEqual(fake_tmux.dimension_queries, 1)
        self.assertLess(clock.now, 1.0)

    def test_transient_blank_and_wrong_row_frames_are_polled_past(self) -> None:
        wrong_rows = _drawer_frame(height=HEIGHT - 1)
        pane, _, clock, stack = _harness([BLANK_FRAME, wrong_rows, VALID_FRAME])
        with stack:
            frame = main_startup_trust_keybinds._wait_for_drawer_reflow(CLIENT, SESSION, WIDTH, HEIGHT, LABEL)
        # Blank repaints and mid-reflow frames with the old row count never
        # satisfy the wait and never fail it immediately; the first fully valid
        # frame at the target geometry does.
        self.assertEqual(frame, VALID_FRAME)
        self.assertEqual(pane.calls, 3)
        self.assertLess(clock.now, 1.0)

    def test_persistent_invalid_frames_time_out_within_the_bounded_wait(self) -> None:
        invalid_frames = {
            "missing drawer title": _drawer_frame(title=None),
            "missing composer row": _drawer_frame(composer="│  drawer content"),
            "missing footer row": _drawer_frame(footer="│  drawer content"),
            "column overflow": _drawer_frame(overflow=True),
            "control bytes": _drawer_frame(control=True),
            "duplicate rail live session": _drawer_frame(live_session=True),
            "duplicate rail activity": _drawer_frame(activity_rows=2),
        }
        for name, frame in invalid_frames.items():
            with self.subTest(invalid_frame=name):
                # Prove each scripted frame really is rejected by the reused
                # _assert_drawer_frame conditions before driving the wait.
                with self.assertRaises(RuntimeError):
                    main_startup_trust_keybinds._assert_drawer_frame(frame, WIDTH, HEIGHT, LABEL)
                pane, _, clock, stack = _harness([frame])
                with stack:
                    with self.assertRaises(RuntimeError) as raised:
                        main_startup_trust_keybinds._wait_for_drawer_reflow(CLIENT, SESSION, WIDTH, HEIGHT, LABEL)
                message = str(raised.exception)
                self.assertIn(f"timed out waiting for {WAIT_LABEL}", message)
                # The default eight-second bound was fully consumed by the fake
                # clock; the real wall-clock cost stays tiny because sleep is faked.
                self.assertGreaterEqual(clock.now, 8.0)
                self.assertLess(clock.now, 8.0 + 3 * POLL_INTERVAL)
                self.assertGreaterEqual(pane.calls, 399)

    def test_wrong_dimensions_are_rejected_before_any_polling(self) -> None:
        pane, fake_tmux, _, stack = _harness([VALID_FRAME], dimensions="100,12")
        with stack:
            with self.assertRaises(RuntimeError) as raised:
                main_startup_trust_keybinds._wait_for_drawer_reflow(CLIENT, SESSION, WIDTH, HEIGHT, LABEL)
        message = str(raised.exception)
        self.assertIn(f"{LABEL} dimensions were 100,12, expected 160,12", message)
        # The authoritative geometry check fails fast: no frame was ever
        # captured or accepted at the wrong dimensions.
        self.assertEqual(pane.calls, 0)
        self.assertEqual(fake_tmux.dimension_queries, 1)

    def test_old_text_change_wait_times_out_on_the_identical_valid_frame(self) -> None:
        # Regression documentation: the previous wait_for_screen_change cannot
        # complete when the trimmed reflowed frame is byte-identical, which is
        # precisely the valid 160x12 drawer frame the new wait accepts.
        pane, _, clock, stack = _harness([VALID_FRAME])
        with stack:
            with self.assertRaises(RuntimeError) as raised:
                wait_for_screen_change(CLIENT, SESSION, VALID_FRAME, "old text-change reflow wait")
        self.assertIn("screen did not change", str(raised.exception))
        self.assertGreaterEqual(clock.now, 8.0)

    def test_scenario_reflow_site_uses_the_dimension_anchored_wait(self) -> None:
        source = pathlib.Path(main_startup_trust_keybinds.__file__).read_text(encoding="utf-8")
        # The 160x12 resize is followed directly by the helper wait and the
        # retained explicit frame assertion (interleaved comments allowed).
        site = re.search(
            r'^    tmux\(tmux_exe, "resize-window", "-t", session, "-x", "160", "-y", "12"\)\n'
            r"(?:    #[^\n]*\n)*"
            r'    reflowed_drawer = _wait_for_drawer_reflow\(tmux_exe, session, 160, 12, "open sidebar drawer 160x12 reflow"\)\n'
            r'    _assert_drawer_frame\(reflowed_drawer, 160, 12, "reflowed short-wide sidebar drawer"\)$',
            source,
            re.MULTILINE,
        )
        self.assertIsNotNone(site, "160x12 drawer reflow site does not use _wait_for_drawer_reflow")
        # The invalid text-change wait is gone from this site; other
        # wait_for_screen_change sites stay out of scope.
        self.assertIsNone(
            re.search(r"wait_for_screen_change\([^)]*open sidebar drawer 160x12 reflow", source),
            "160x12 drawer reflow still waits for a trimmed-text change",
        )
        # AST cross-check: exactly one helper call at the target geometry, and
        # the helper predicate reuses the shared frame conditions.
        tree = ast.parse(source)
        helper_calls = [
            node
            for node in ast.walk(tree)
            if isinstance(node, ast.Call)
            and isinstance(node.func, ast.Name)
            and node.func.id == "_wait_for_drawer_reflow"
        ]
        self.assertEqual(len(helper_calls), 1)
        constants = [arg.value for arg in helper_calls[0].args if isinstance(arg, ast.Constant)]
        self.assertIn(160, constants)
        self.assertIn(12, constants)
        helper_def = next(
            node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "_wait_for_drawer_reflow"
        )
        predicate_reuses_conditions = any(
            isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "_assert_drawer_frame"
            for node in ast.walk(helper_def)
        )
        self.assertTrue(predicate_reuses_conditions, "_wait_for_drawer_reflow must reuse _assert_drawer_frame")


class ContextFreshnessSectionNormalizerTests(unittest.TestCase):
    """Exercise the real ``_normalize_context_section`` against wrap-split rows."""

    def test_unwrapped_fields_normalize_unchanged(self) -> None:
        screen = (
            "chat row before the modal\n"
            "Context freshness: prompt=builtin  context_sources=1  loaded_bytes=19  "
            "status=current  project_resources=skipped  system_prompt_sources=0\n"
            "Enter/Esc close\n"
        )
        section = main_startup_trust_keybinds._normalize_context_section(screen)
        for token in (
            "prompt=builtin",
            "context_sources=1",
            "loaded_bytes=19",
            "status=current",
            "project_resources=skipped",
            "system_prompt_sources=0",
        ):
            self.assertIn(token, section)

    def test_wrapped_loaded_bytes_and_status_tokens_are_rejoined(self) -> None:
        # Modeled on the reproduced 84-column short-root capture: the workspace
        # row trails "loade" and the indented continuation carries the rest,
        # while a shorter baseline split status=current as "sta"/"tus=current".
        screen = (
            "Context freshness: prompt=builtin  context_sources=1  project /workspace/AGENTS.md  loade\n"
            "    d_bytes=19  status=current current_bytes=19  project_resources=skipped\n"
            "    system_prompt_sources=0  sta\n"
            "    tus=current Enter/Esc close\n"
        )
        section = main_startup_trust_keybinds._normalize_context_section(screen)
        self.assertIn("loaded_bytes=19", section)
        self.assertIn("status=current", section)
        self.assertIn("project_resources=skipped", section)
        self.assertIn("system_prompt_sources=0", section)

    def test_missing_or_incorrect_values_are_not_manufactured(self) -> None:
        screen = (
            "Context freshness: prompt=builtin  context_sources=1  loaded_bytes=20  "
            "project_resources=skipped  system_prompt_sources=0\n"
        )
        section = main_startup_trust_keybinds._normalize_context_section(screen)
        self.assertNotIn("loaded_bytes=19", section)
        self.assertNotIn("status=current", section)
        # A split that only resembles the token must not reassemble into it.
        split_lookalike = "Context freshness: loaded_bytes=1\n    8  status=current\n"
        self.assertNotIn(
            "loaded_bytes=19", main_startup_trust_keybinds._normalize_context_section(split_lookalike)
        )

    def test_split_forbidden_tokens_remain_detectable(self) -> None:
        # The negative trust-smoke/APPEND_SYSTEM assertions run on the
        # normalized section, so a wrap-split leak is still caught.
        screen = (
            "Context freshness: prompt=builtin  trust-\n"
            "    smoke  APPEND_\n"
            "    SYSTEM.md  status=current\n"
        )
        section = main_startup_trust_keybinds._normalize_context_section(screen)
        self.assertIn("trust-smoke", section)
        self.assertIn("APPEND_SYSTEM", section)

    def test_within_row_spaces_are_preserved(self) -> None:
        # Only row padding and newlines are removed; spaces inside a row stay,
        # so multi-token continuation rows keep their original spacing.
        screen = (
            "Context freshness: project /workspace/AGENTS.md  loade\n"
            "    d_bytes=19  status=current current_bytes=19  \n"
        )
        section = main_startup_trust_keybinds._normalize_context_section(screen)
        self.assertIn("loaded_bytes=19  status=current current_bytes=19", section)

    def test_scenario_call_site_uses_the_normalizer(self) -> None:
        source = pathlib.Path(main_startup_trust_keybinds.__file__).read_text(encoding="utf-8")
        assignment = re.search(
            r"^    context_freshness_section = _normalize_context_section\(context_freshness\)\s*$",
            source,
            re.MULTILINE,
        )
        self.assertIsNotNone(assignment, "/context freshness section does not use _normalize_context_section")
        self.assertNotIn(
            'context_freshness_section = context_freshness.rsplit("Context freshness:", 1)[-1]',
            source,
            "raw rsplit section survived at the /context call site",
        )
        # All six required-field tokens and both forbidden tokens are still
        # asserted against the normalized section.
        for token in (
            "prompt=builtin",
            "context_sources=1",
            "loaded_bytes=19",
            "status=current",
            "project_resources=skipped",
            "system_prompt_sources=0",
            "trust-smoke",
            "APPEND_SYSTEM",
        ):
            self.assertRegex(source, rf'"{re.escape(token)}" (?:not )?in context_freshness_section')


if __name__ == "__main__":
    unittest.main()
