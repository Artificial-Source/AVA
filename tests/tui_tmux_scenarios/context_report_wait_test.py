#!/usr/bin/env python3
"""Replay incomplete trusted /context reports through the actual scenario waits.

Carlo #83's first capture ended midway through the prompt-command path. The
old identity-only wait accepted it before status/current_bytes and the modal
footer were painted. Keep that oracle replay and drive both real scenario
capture sites (including their retained assertions) with FakePane/FakeTime.
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
from tui_tmux_scenarios import main_startup_trust_keybinds

CLIENT = object()
SESSION = "unit-test-session"
PROMPT_SITE = "trusted_context"
APPEND_SITE = "trusted_prompt_context"
SITES = {
    PROMPT_SITE: (r"prompt_command\s+project\s+trust-smoke", "trusted project prompt command freshness"),
    APPEND_SITE: (r"append_system_prompt\s+project\s+APPEND_SYSTEM\.md", "trusted project append-system freshness"),
}
WORKSPACE = "/home/carlo/projects/github/ai-cli/AVA/build/Debug/tui-tmux-smoke/main_startup_trust_keybinds/workspace"
HEADER = [
    "Context freshness:",
    "  mode=build",
    "  model=openai/gpt-5.5",
    "  project_trust=trusted project_resources=enabled",
    "  context_sources=2",
    "  system_prompt_sources=1",
    "  prompt_commands=1",
    "  skills=0",
    "  plugin_sources=2",
    "  lsp_status=disabled lsp_configs=0 lsp_servers=0 lsp_errors=0 lsp_builtins_enabled=0 lsp_builtins_available=0",
]


def _report_frame(site: str, *, path_padding: int = 0) -> str:
    """Model the restored 120x32 pane: title row 2, body 3-28, footer 29.

    command_output_geometry gives 114x28; modal insets and
    wrap_transcript_text leave 102 columns per wrapped report row.
    """

    if site == PROMPT_SITE:
        identity, path, size = "prompt_command  project  trust-smoke", "commands/trust-smoke.md", 56
    else:
        identity, path, size = "append_system_prompt  project  APPEND_SYSTEM.md", "APPEND_SYSTEM.md", 27
    source = (
        f"  {identity}  {WORKSPACE}{'x' * path_padding}/.ava/{path}  "
        f"loaded_bytes={size}  status=current current_bytes={size}"
    )
    body = ["       " + line[start : start + 102] for line in [*HEADER, source] for start in range(0, len(line), 102)]
    return "\n".join(
        ["  ! OpenAI not connected · /connect", "", "       Command /context"]
        + body
        + [""] * (26 - len(body))
        + ["       Enter/Esc close", "│  Type a message...", "│  GPT-5.5 · ctx 577 (0.2%)"]
    )


PROMPT_FRAME = _report_frame(PROMPT_SITE)
APPEND_FRAME = _report_frame(APPEND_SITE)
# Exact reported partial frame, including the still-busy composer below it.
REPORTED_PARTIAL_FRAME = "\n".join(
    PROMPT_FRAME.splitlines()[:15]
    + [""] * 13
    + [
        "│  Esc stop · type a follow-up",
        "│",
        "│  Type a message...",
        "│  GPT-5.5 · ctx 577 (0.2%)" + " " * 87 + "▂▄▇▃",
    ]
) + "\n"


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
    return pane, clock, stack


def _scenario() -> ast.FunctionDef:
    tree = ast.parse(pathlib.Path(main_startup_trust_keybinds.__file__).read_text(encoding="utf-8"))
    return next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "scenario_main_startup_trust_keybinds"
    )


def _site_index(scenario: ast.FunctionDef, site: str) -> int:
    return next(
        index for index, node in enumerate(scenario.body)
        if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name) and target.id == site for target in node.targets)
    )


def _run_report_site(site: str) -> str:
    # Execute the actual wait assignment and following diagnostic assertions,
    # stopping before the next input. This also exercised the old acceptance
    # before the helper existed, rather than failing just on a missing helper.
    scenario = _scenario()
    start = _site_index(scenario, site)
    end = next(index for index in range(start + 1, len(scenario.body)) if isinstance(scenario.body[index], ast.Expr))
    code = compile(ast.Module(body=scenario.body[start:end], type_ignores=[]), main_startup_trust_keybinds.__file__, "exec")
    namespace = dict(vars(main_startup_trust_keybinds), tmux_exe=CLIENT, session=SESSION)
    exec(code, namespace)
    return namespace[site]


class ContextReportWaitTests(unittest.TestCase):
    def test_reported_partial_frame_waits_for_complete_prompt_report(self) -> None:
        pane, clock, stack = _harness([REPORTED_PARTIAL_FRAME, PROMPT_FRAME])
        with stack:
            frame = _run_report_site(PROMPT_SITE)
        self.assertEqual(frame, PROMPT_FRAME)
        self.assertEqual(pane.calls, 2)
        self.assertAlmostEqual(clock.now, POLL_INTERVAL)

    def test_both_sites_poll_past_source_identity_and_incomplete_footer(self) -> None:
        for site in SITES:
            complete = _report_frame(site)
            lines = complete.splitlines()
            partial_source = "\n".join(lines[:15] + [""] * 15 + lines[30:])
            stale_footer = "\n".join(lines[:15] + [""] * 14 + lines[29:])
            partial_footer = complete.replace("Enter/Esc close", "Enter/Esc clo")
            for partial in (partial_source, stale_footer, partial_footer):
                with self.subTest(site=site, partial=partial):
                    pane, _, stack = _harness([partial, complete])
                    with stack:
                        frame = _run_report_site(site)
                    self.assertEqual(frame, complete)
                    self.assertEqual(pane.calls, 2)

    def test_complete_reports_return_the_accepted_frame_without_recapture(self) -> None:
        for site in SITES:
            complete = _report_frame(site)
            pane, clock, stack = _harness([complete, REPORTED_PARTIAL_FRAME])
            with stack:
                frame = _run_report_site(site)
            self.assertEqual(frame, complete)
            self.assertEqual(pane.calls, 1)
            self.assertEqual(clock.now, 0.0)

    def test_path_lengths_wrap_freshness_fields_without_changing_their_values(self) -> None:
        for site in SITES:
            split_fields = set()
            # One full 102-column wrap cycle covers splits inside loaded_bytes,
            # status=current, current_bytes, and the spaces between fields.
            for padding in range(102):
                complete = _report_frame(site, path_padding=padding)
                split_fields.update(field for field in ("loaded_bytes=", "status=current", "current_bytes=") if field not in complete)
                with self.subTest(site=site, padding=padding):
                    pane, _, stack = _harness([complete])
                    with stack:
                        self.assertEqual(_run_report_site(site), complete)
                    self.assertEqual(pane.calls, 1)
            self.assertEqual(split_fields, {"loaded_bytes=", "status=current", "current_bytes="})

    def test_wrong_or_stale_reports_and_nonmodal_text_do_not_pass(self) -> None:
        for site, other in ((PROMPT_SITE, APPEND_FRAME), (APPEND_SITE, PROMPT_FRAME)):
            complete = _report_frame(site)
            extra_source = complete.splitlines()
            extra_source[18:21] = other.splitlines()[14:17]
            invalid = {
                "previous filtered report": other,
                "additional source report": "\n".join(extra_source),
                "wrong modal": complete.replace("Command /context", "Command /trust"),
                "title prefix only": complete.replace("Command /context", "Command /contextual"),
                "missing freshness header": complete.replace("Context freshness:", "Old context:"),
                "wrong scope": complete.replace("  project  ", "  global  "),
                "wrong source kind": complete.replace("         " + ("prompt_command" if site == PROMPT_SITE else "append_system_prompt"), "         other_source"),
                "name prefix only": complete.replace("trust-smoke  ", "trust-smoke-old  ") if site == PROMPT_SITE else complete.replace("APPEND_SYSTEM.md  ", "APPEND_SYSTEM.md.old  "),
                "untrusted report": complete.replace("project_trust=trusted", "project_trust=unknown"),
                "skipped resources": complete.replace("project_resources=enabled", "project_resources=skipped"),
                "footer outside modal": complete.replace("       Enter/Esc close", "").replace("│  Type a message...", "│  Enter/Esc close"),
                "incomplete viewport": complete.replace("Enter/Esc close", "1-26/30 · ↑↓/PgUp/PgDn scroll · Enter close"),
            }
            for name, partial in invalid.items():
                with self.subTest(site=site, frame=name):
                    pane, _, stack = _harness([partial, complete])
                    with stack:
                        frame = _run_report_site(site)
                    self.assertEqual(frame, complete)
                    self.assertEqual(pane.calls, 2)

    def test_persistent_incomplete_reports_fail_at_existing_deadline_with_last_screen(self) -> None:
        for site in SITES:
            for partial in (REPORTED_PARTIAL_FRAME, _report_frame(site).replace("Enter/Esc close", "")):
                with self.subTest(site=site, frame=partial):
                    pane, clock, stack = _harness([partial])
                    with stack:
                        with self.assertRaises(RuntimeError) as raised:
                            _run_report_site(site)
                    message = str(raised.exception)
                    self.assertIn(f"timed out waiting for {SITES[site][1]}", message)
                    self.assertIn("complete /context", message)
                    self.assertIn(f"last screen:\n{partial}", message)
                    self.assertGreaterEqual(clock.now, 8.0)
                    self.assertLess(clock.now, 8.0 + 3 * POLL_INTERVAL)
                    self.assertGreaterEqual(pane.calls, 399)

    def test_incorrect_or_missing_diagnostics_still_fail(self) -> None:
        for site in SITES:
            # Keep diagnostics unwrapped here so each replacement breaks only
            # its named condition; the wrap-cycle test covers split fields.
            complete = _report_frame(site, path_padding=50)
            self.assertIn("status=current", complete)
            self.assertIn("current_bytes=", complete)
            self.assertIn("loaded_bytes=", complete)
            invalid = {
                "wrong count": complete.replace("context_sources=2", "context_sources=1"),
                "count prefix only": complete.replace("context_sources=2", "context_sources=20"),
                "missing count": complete.replace("context_sources=2", ""),
                "changed source": complete.replace("status=current", "status=changed"),
                "status prefix only": complete.replace("status=current", "status=currentish"),
                "missing status": complete.replace("status=current", ""),
                "missing current bytes": re.sub(r"current_bytes=\d+", "", complete),
                "missing loaded bytes": re.sub(r"loaded_bytes=\d+", "", complete),
                "composer cannot supply status": complete.replace("status=current", "").replace("Type a message...", "status=current"),
            }
            for name, frame in invalid.items():
                with self.subTest(site=site, diagnostic=name):
                    _, clock, stack = _harness([frame])
                    with stack:
                        with self.assertRaises(RuntimeError) as raised:
                            _run_report_site(site)
                    self.assertIn(frame, str(raised.exception))
                    self.assertLess(clock.now, 8.0 + 3 * POLL_INTERVAL)

    def test_scenario_wires_both_waits_and_retains_inputs_geometry_and_assertions(self) -> None:
        scenario = _scenario()
        for site, (identity, label) in SITES.items():
            index = _site_index(scenario, site)
            call = scenario.body[index].value
            self.assertEqual(call.func.id, "_wait_for_context_report")
            self.assertEqual([ast.literal_eval(arg) for arg in call.args[2:]], [identity, label])
            self.assertEqual(call.keywords, [])  # Keep the existing default eight-second deadline.
            query = "/context trust-smoke " if site == PROMPT_SITE else "/context APPEND_SYSTEM "
            self.assertEqual(ast.literal_eval(scenario.body[index - 3].value.args[2]), query)
            self.assertEqual(scenario.body[index - 1].value.func.id, "send_keys")
            self.assertEqual(ast.literal_eval(scenario.body[index - 1].value.args[2]), "Enter")
        source = pathlib.Path(main_startup_trust_keybinds.__file__).read_text(encoding="utf-8")
        self.assertIn('"status=current" not in trusted_context_compact or "context_sources=2" not in trusted_context_compact', source)
        self.assertIn('"status=current" not in re.sub(r"\\s+", "", trusted_prompt_context)', source)
        self.assertIn('r"(?s)Context freshness:.*context_sources=1.*Enter/Esc close"', source)
        for forbidden in ("trust-smoke", "APPEND_SYSTEM"):
            self.assertIn(f'if "{forbidden}" in context_freshness_section:', source)
        resizes = [
            node for node in scenario.body[:_site_index(scenario, PROMPT_SITE)]
            if isinstance(node, ast.Expr) and isinstance(node.value, ast.Call)
            and isinstance(node.value.func, ast.Name) and node.value.func.id == "tmux"
            and any(isinstance(arg, ast.Constant) and arg.value == "resize-window" for arg in node.value.args)
        ]
        self.assertEqual([ast.literal_eval(arg) for arg in resizes[-1].value.args[-4:]], ["-x", "120", "-y", "32"])

    def test_old_identity_wait_accepts_reported_partial_before_diagnostics_and_footer(self) -> None:
        old_pattern = r"(?s)project_trust=trusted project_resources=enabled.*prompt_command\s+project\s+trust-smoke"
        pane, clock, stack = _harness([REPORTED_PARTIAL_FRAME, PROMPT_FRAME])
        with stack:
            frame = tui_smoke_helpers.wait_for(CLIENT, SESSION, old_pattern, SITES[PROMPT_SITE][1])
        self.assertEqual(frame, REPORTED_PARTIAL_FRAME)
        self.assertEqual(pane.calls, 1)
        self.assertEqual(clock.now, 0.0)
        self.assertIn("context_sources=2", frame)
        self.assertNotIn("status=current", re.sub(r"\s+", "", frame))
        self.assertNotRegex(frame, r"Enter(?:/Esc)? close")
        self.assertTrue(frame.splitlines()[14].endswith("/tui-tmux-sm"))


if __name__ == "__main__":
    unittest.main()
