#!/usr/bin/env python3
"""Test PTY cursor checks without launching another terminal lifecycle."""

import unittest
from unittest import mock

import tui_terminal_lifecycle_smoke as smoke


class CursorLifecycleTests(unittest.TestCase):
    """Distinguish actual visibility restoration from redundant output requirements."""

    def test_visible_cursor_needs_no_teardown_show(self) -> None:
        """Ncurses may suppress curs_set(1) when its cursor is already visible."""
        smoke.require_cursor_restored(smoke.CURSOR_HIDE + smoke.CURSOR_SHOW, b"", "visible")

    def test_hidden_cursor_requires_teardown_show(self) -> None:
        """An earlier startup show cannot excuse missing restoration after a hide."""
        before = smoke.CURSOR_SHOW + smoke.CURSOR_HIDE
        with self.assertRaisesRegex(RuntimeError, "did not leave the cursor visible"):
            smoke.require_cursor_restored(before, smoke.CURSOR_STYLE_RESET, "hidden")
        smoke.require_cursor_restored(before, smoke.CURSOR_SHOW, "restored")

    def test_show_followed_by_hide_is_not_restoration(self) -> None:
        """Check the final state rather than merely finding any show sequence."""
        with self.assertRaisesRegex(RuntimeError, "did not leave the cursor visible"):
            smoke.require_cursor_restored(smoke.CURSOR_HIDE, smoke.CURSOR_SHOW + smoke.CURSOR_HIDE, "hidden again")

    def test_missing_visibility_is_not_assumed_visible(self) -> None:
        """Style controls alone provide no evidence that the cursor is visible."""
        self.assertIsNone(smoke.cursor_visibility(smoke.CURSOR_STYLE_RESET))
        with self.assertRaisesRegex(RuntimeError, "did not leave the cursor visible"):
            smoke.require_cursor_restored(b"", b"", "unknown")

    def test_setup_requests_real_selector_and_waits_for_hidden_state(self) -> None:
        """Only Ctrl+L is injected, and both modal identity and final hide are required."""
        process = mock.Mock()
        ready = smoke.CURSOR_HIDE + b"Select model\nEsc close"

        def check_wait(fd, child, predicate, label):
            """Exercise partial and wrong-state captures before accepting the modal."""
            self.assertEqual(fd, 42)
            self.assertIs(child, process)
            self.assertIn("hidden cursor", label)
            self.assertFalse(predicate(b"Select model"))
            self.assertFalse(predicate(smoke.CURSOR_HIDE))
            self.assertFalse(predicate(smoke.CURSOR_HIDE + b"Select model"))
            self.assertFalse(predicate(ready + smoke.CURSOR_SHOW))
            self.assertTrue(predicate(ready))
            return ready

        with mock.patch.object(smoke.os, "write", return_value=1) as write, mock.patch.object(smoke, "read_until", side_effect=check_wait):
            self.assertEqual(smoke.hide_cursor_for_teardown(42, process, "signal"), ready)
        write.assert_called_once_with(42, b"\x0c")


if __name__ == "__main__":
    unittest.main()
