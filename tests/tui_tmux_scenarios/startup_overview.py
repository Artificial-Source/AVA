"""Credential-free real-terminal coverage for the on-demand path-free startup overview."""

from __future__ import annotations

import re

from tui_smoke_helpers import (
    ACTIVE_CONTEXT_TOKEN_COUNT_PATTERN,
    SmokeContext,
    capture,
    save_evidence,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
    wait_for_screen_state,
)
from .common import _finish_main, _main_session


def _assert_zero_overview_chrome(screen: str, label: str) -> None:
    # Fresh startup reserves no persistent collapsed overview rows at any height; the
    # overview surface is reachable only through the exact on-demand /overview command.
    if "/overview" in screen:
        raise RuntimeError(f"{label}: fresh startup reserved persistent overview chrome rows\nscreen:\n{screen}")


def _wait_for_closed_overview(tmux_exe: object, session: str, label: str) -> str:
    """Wait for one frame that both dropped the overview and restored the composer.

    Absence of "Startup overview" alone can be satisfied by a transient blank
    repaint between overview teardown and the composer redraw. Requiring the
    composer placeholder in that same captured frame proves the close finished,
    so a blank or partially drawn frame never satisfies this wait.
    """

    def overview_closed(screen: str) -> bool:
        return "Startup overview" not in screen and "Type a message" in screen

    return wait_for_screen_state(tmux_exe, session, overview_closed, label)


def scenario_startup_overview(ctx: SmokeContext) -> None:
    tmux_exe, root, workspace, ava_config, env_prefix, session = _main_session(ctx)
    # Main startup does not start a fake provider; prove no request log is created either.
    request_logs = sorted(ctx.root.glob("*-provider-requests.log"))
    session_dir = ctx.state / "ava" / "sessions"

    def session_jsonl_snapshot() -> dict[str, str]:
        files: dict[str, str] = {}
        if session_dir.exists():
            for path in sorted(session_dir.rglob("*.jsonl")):
                files[str(path.relative_to(session_dir))] = path.read_text(encoding="utf-8", errors="replace")
        return files

    before_jsonl = session_jsonl_snapshot()
    before_request_bytes = sum(path.stat().st_size for path in request_logs if path.exists())

    initial = wait_for(tmux_exe, session, r"Type a message", "startup initial frame")
    _assert_zero_overview_chrome(initial, "roomy startup frame")
    if re.search(r"/home/|AGENTS\.md|api[_-]?key|OPENAI_API_KEY|auth\.json", initial, re.IGNORECASE):
        raise RuntimeError(f"startup frame leaked private path/secret text\nscreen:\n{initial}")
    footer_lines = [line for line in initial.splitlines() if "GPT-5.5" in line and "ctx " in line]
    if not footer_lines:
        raise RuntimeError(f"composer footer missing from startup frame\nscreen:\n{initial}")
    footer_text = footer_lines[-1].removeprefix("│  ").strip()
    if not re.fullmatch(rf"GPT-5\.5 · ctx {ACTIVE_CONTEXT_TOKEN_COUNT_PATTERN}", footer_text):
        raise RuntimeError(f"startup chrome changed the quiet composer footer\nscreen:\n{initial}")
    save_evidence(root, "startup-no-overview-chrome", initial)

    # Zero overview chrome at short and tiny heights as well.
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "48", "-y", "10")
    short = wait_for(tmux_exe, session, r"Type a message", "startup frame at 48x10")
    _assert_zero_overview_chrome(short, "48x10 startup frame")
    save_evidence(root, "startup-no-overview-chrome-short", short)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "100", "-y", "7")
    tiny = wait_for(tmux_exe, session, r"Type a message", "startup frame at 100x7")
    _assert_zero_overview_chrome(tiny, "100x7 startup frame")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "100", "-y", "24")
    wait_for(tmux_exe, session, r"Type a message", "startup frame restored to 100x24")

    # Expand via exact /overview (selector bypass).
    send_literal(tmux_exe, session, "/overview")
    send_keys(tmux_exe, session, "Enter")
    expanded = wait_for(tmux_exe, session, r"Startup overview|Mode|Provider", "overview expanded select-list")
    if "Startup overview" not in expanded:
        raise RuntimeError(f"/overview did not open the read-only select-list\nscreen:\n{expanded}")
    if re.search(r"MCP\s*/\s*LSP|enabled MCP|LSP enabled", expanded, re.IGNORECASE):
        raise RuntimeError(f"expanded overview made MCP/LSP enablement claims\nscreen:\n{expanded}")
    save_evidence(root, "startup-overview-expanded", expanded)

    # Filter + scroll remain host-owned list behavior.
    # Ctrl+U is composer-only; select-list filters clear with Backspace.
    send_literal(tmux_exe, session, "mode")
    filtered = wait_for(tmux_exe, session, r"filter\s+mode", "overview filter mode")
    if "Mode" not in filtered:
        raise RuntimeError(f"overview filter did not keep the Mode row\nscreen:\n{filtered}")
    for _ in range(4):
        send_keys(tmux_exe, session, "BSpace")
    wait_for_absent(tmux_exe, session, r"filter\s+mode", "overview filter cleared")
    send_keys(tmux_exe, session, "PageDown")
    paged = capture(tmux_exe, session)
    save_evidence(root, "startup-overview-paged", paged)

    # Esc closes without backend mutation and without resurrecting collapsed chrome.
    send_keys(tmux_exe, session, "Escape")
    closed = _wait_for_closed_overview(tmux_exe, session, "overview closed via Esc")
    _assert_zero_overview_chrome(closed, "frame after closing overview")
    save_evidence(root, "startup-overview-closed", closed)

    # Short + narrow resize keeps expanded view usable.
    send_literal(tmux_exe, session, "/overview")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Startup overview", "overview reopened before resize")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "48", "-y", "10")
    dimensions = tmux(tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}").stdout.strip()
    if dimensions != "48,10":
        raise RuntimeError(f"short/narrow resize dimensions were {dimensions}, expected 48,10")
    short_open = wait_for(tmux_exe, session, r"Startup overview|Mode|filter", "overview usable at 48x10")
    if "Startup overview" not in short_open:
        raise RuntimeError(f"overview select-list did not remain usable at short height\nscreen:\n{short_open}")
    save_evidence(root, "startup-overview-short-narrow", short_open)

    # Restore ordinary geometry and close.
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "100", "-y", "24")
    wait_for(tmux_exe, session, r"Startup overview", "overview restored after resize")
    send_keys(tmux_exe, session, "Escape")
    restored = _wait_for_closed_overview(tmux_exe, session, "overview closed after resize")

    after_jsonl = session_jsonl_snapshot()
    if after_jsonl != before_jsonl:
        raise RuntimeError(
            "startup overview mutated session JSONL\n"
            f"before keys={sorted(before_jsonl)}\nafter keys={sorted(after_jsonl)}"
        )
    after_logs = sorted(ctx.root.glob("*-provider-requests.log"))
    after_request_bytes = sum(path.stat().st_size for path in after_logs if path.exists())
    if after_logs or after_request_bytes != before_request_bytes:
        raise RuntimeError(
            "startup overview issued provider requests\n"
            f"before bytes={before_request_bytes} after bytes={after_request_bytes} logs={after_logs}"
        )

    # Keep restored capture referenced so short-height cleanup stays intentional.
    if "Type a message" not in restored:
        raise RuntimeError(f"final restored frame lost composer chrome\nscreen:\n{restored}")
    _assert_zero_overview_chrome(restored, "final restored frame")

    _finish_main(tmux_exe, session)
