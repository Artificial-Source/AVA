"""The tmux TUI smoke scenario for active run."""

from __future__ import annotations

import re
import shlex
import time

from tui_smoke_helpers import (
    SmokeContext,
    capture,
    save_evidence,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
    wait_for_count,
    wait_for_session_exit,
    wait_for_screen_state,
)
from .common import (
    _assert_normal_turn_request_count_stays,
    _wait_for_normal_turn_request_count,
)


def scenario_active_run(ctx: SmokeContext) -> None:
    tmux_exe = ctx.tmux
    root = ctx.root
    active_workspace = ctx.active_workspace

    def wait_for_command_output_closed(
        session: str,
        title_pattern: str,
        forbidden: tuple[str, ...],
        required: tuple[str, ...],
        label: str,
    ) -> str:
        """Wait for one fully repainted composer frame after closing command output."""

        def command_output_is_closed(screen: str) -> bool:
            return (
                re.search(title_pattern, screen) is None
                and re.search(r"Enter(?:/Esc)? close", screen) is None
                and all(text not in screen for text in forbidden)
                and all(text in screen for text in required)
                and re.search(r"(?m)^[ \t]*│[ \t]+Type a message\.\.\.", screen) is not None
            )

        # Tmux can observe doupdate midway through clearing the modal. A title
        # disappearing does not prove that lower body and footer rows are gone.
        return wait_for_screen_state(tmux_exe, session, command_output_is_closed, f"{label} fully closed")

    # A delayed provider-backed /compact remains an initial local command even
    # when Alt+Enter queues a genuine ordinary follow-up under the same active run.
    compact_session = ctx.session_name("compact-follow-up")
    compact_provider = ctx.start_fake_provider(
        "compact-follow-up",
        delay_ms=0,
        scenario="compact-follow-up",
        target=ctx.restore_workspace / "AGENTS.md",
    )
    compact_request_log = compact_provider.request_log
    compact_command = ctx.pane_command(
        home=ctx.restore_home,
        config=ctx.restore_config,
        state=ctx.restore_state,
        data=ctx.restore_data,
        extra={
            "COLORFGBG": "",
            "NO_COLOR": "1",
            "MOONSHOT_API_KEY": "test-key",
            "MOONSHOT_BASE_URL": f"http://127.0.0.1:{compact_provider.port}",
            "AVA_SESSION_TITLES": "off",
        },
    )
    ctx.launch_ava(compact_session, workspace=ctx.restore_workspace, command=compact_command, width=82, height=24)
    wait_for(tmux_exe, compact_session, r"Type a message|live session", "compact follow-up initial frame")
    send_literal(tmux_exe, compact_session, "source before compact")
    send_keys(tmux_exe, compact_session, "Enter")
    _wait_for_normal_turn_request_count(compact_provider, 1, "compact source provider request")
    wait_for(tmux_exe, compact_session, r"before compact", "compact source completion", timeout=12.0)
    send_literal(tmux_exe, compact_session, "/compact")
    wait_for(tmux_exe, compact_session, r"│  /compact(?:\s|$)", "compact command draft")
    send_keys(tmux_exe, compact_session, "Enter")
    time.sleep(0.25)
    if re.search(r"│  /compact(?:\s|$)", capture(tmux_exe, compact_session)):
        send_keys(tmux_exe, compact_session, "Enter")
    compact_provider.wait_for_request(1, "delayed compact summary request")
    send_literal(tmux_exe, compact_session, "queued after compact")
    send_literal(tmux_exe, compact_session, "\x1b\r")
    wait_for(tmux_exe, compact_session, r"follow-up queued", "compact ordinary follow-up queued")
    compact_provider.release_request(1)
    compact_provider.wait_for_request(2, "compact queued follow-up provider request", timeout=14.0)
    compact_log = compact_request_log.read_text(encoding="utf-8")
    if "queued after compact" not in compact_log:
        raise RuntimeError(f"queued compact follow-up did not reach the fake provider\nrequest log:\n{compact_log}")
    compact_output = wait_for(
        tmux_exe,
        compact_session,
        r"(?s)Command /compact.*compaction summary recorded.*Enter/Esc close",
        "compact local output retained beside queued conversation",
        timeout=14.0,
    )
    save_evidence(root, "active-run-compact-queued-output", compact_output)
    send_keys(tmux_exe, compact_session, "Escape")
    compact_forbidden = ("/compact", "compaction summary recorded", "compaction completed", "LOCAL-TOOL-MUST-NOT-LEAK")
    compact_closed = wait_for_command_output_closed(
        compact_session,
        r"Command /compact",
        compact_forbidden,
        ("queued after compact", "after compact queued answer"),
        "compact queued conversation after local output close",
    )
    for forbidden in compact_forbidden:
        if forbidden in compact_closed:
            raise RuntimeError(f"compact local activity remained in chat after output close ({forbidden!r})\nscreen:\n{compact_closed}")
    save_evidence(root, "active-run-compact-queued-transcript", compact_closed)

    # Repeat in the same production path with a normal follow-up that fails
    # before commit. Its actionable error remains local to the initial command
    # modal and its request receives no conversation projection authority.
    send_literal(tmux_exe, compact_session, "/compact")
    wait_for(tmux_exe, compact_session, r"│  /compact(?:\s|$)", "failing compact command draft")
    send_keys(tmux_exe, compact_session, "Enter")
    time.sleep(0.25)
    if re.search(r"│  /compact(?:\s|$)", capture(tmux_exe, compact_session)):
        send_keys(tmux_exe, compact_session, "Enter")
    compact_provider.wait_for_request(3, "second delayed compact summary request")
    send_literal(tmux_exe, compact_session, "queued compact failure")
    wait_for(tmux_exe, compact_session, r"queued compact failure", "failing compact follow-up draft")
    send_literal(tmux_exe, compact_session, "\x1b\r")
    wait_for_count(tmux_exe, compact_session, r"follow-up queued", 2, "failing compact follow-up queued")
    compact_provider.release_request(3)
    compact_provider.wait_for_request(4, "failing compact follow-up tool-call request", timeout=14.0)
    failed_compact_log = compact_request_log.read_text(encoding="utf-8")
    if "queued compact failure" not in failed_compact_log:
        raise RuntimeError(f"failing compact follow-up did not reach the fake provider\nrequest log:\n{failed_compact_log}")
    permission_deadline = time.monotonic() + 10.0
    while time.monotonic() < permission_deadline:
        current_log = compact_request_log.read_text(encoding="utf-8")
        if current_log.count("--- request ") >= 6:
            break
        current_screen = capture(tmux_exe, compact_session)
        if "Permission required" in current_screen:
            send_keys(tmux_exe, compact_session, "A")
        time.sleep(0.1)
    compact_provider.wait_for_request(5, "failing compact follow-up provider continuation", timeout=14.0)
    failed_continuation_log = compact_request_log.read_text(encoding="utf-8")
    if "restore tmux smoke context" not in failed_continuation_log:
        raise RuntimeError(
            "failing compact follow-up continuation did not contain completed tool output"
            f"\nrequest log:\n{failed_continuation_log}"
        )
    failed_compact_output = wait_for(
        tmux_exe,
        compact_session,
        r"(?s)Command /compact.*compaction summary recorded.*Moonshot HTTP request failed with status 400.*read_file.*restore tmux smoke context.*Enter/Esc close",
        "compact output retained beside actionable queued failure and completed tool",
        timeout=14.0,
    )
    save_evidence(root, "active-run-compact-queued-failure-output", failed_compact_output)
    send_keys(tmux_exe, compact_session, "Escape")
    failed_compact_forbidden = (
        "/compact",
        "compaction summary recorded",
        "compaction completed",
        "LOCAL-TOOL-MUST-NOT-LEAK",
        "queued compact failure",
        "HTTP request failed with status 400",
        "read_file",
        "restore tmux smoke context",
    )
    failed_compact_closed = wait_for_command_output_closed(
        compact_session,
        r"Command /compact|Moonshot HTTP request failed with status 400",
        failed_compact_forbidden,
        ("queued after compact", "after compact queued answer"),
        "failing compact output close",
    )
    for forbidden in failed_compact_forbidden:
        if forbidden in failed_compact_closed:
            raise RuntimeError(
                f"failed compact request gained chat projection authority ({forbidden!r})\nscreen:\n{failed_compact_closed}"
            )
    save_evidence(root, "active-run-compact-queued-failure-transcript", failed_compact_closed)
    send_keys(tmux_exe, compact_session, "C-d")
    wait_for_session_exit(tmux_exe, compact_session)
    tmux(tmux_exe, "kill-session", "-t", compact_session, check=False)

    active_workspace.joinpath("active-card.txt").write_text(
        "".join(f"ACTIVE-OLD-LINE-{line:02d}\n" for line in range(1, 31)), encoding="utf-8"
    )
    active_session = ctx.session_name("active")
    active_provider = ctx.start_fake_provider("active", delay_ms=0, scenario="text-three-delayed-third")
    active_request_log = active_provider.request_log
    active_env_prefix = ctx.fake_provider_command(
        active_provider,
        home=ctx.active_home,
        config=ctx.active_config,
        state=ctx.active_state,
        data=ctx.active_data,
    )

    def wait_for_tmux_clipboard_transport(path, label: str) -> bytes:
        deadline = time.monotonic() + 8.0
        last = b""
        while time.monotonic() < deadline:
            if path.exists():
                last = path.read_bytes()
                if b"\x1b]52;c;" in last and b"\x1bPtmux;" in last:
                    return last
            time.sleep(0.05)
        raise RuntimeError(f"timed out waiting for {label}; pane output did not contain bounded tmux OSC 52 transport: {last!r}")

    tmux(
        tmux_exe,
        "new-session",
        "-d",
        "-s",
        active_session,
        "-x",
        "110",
        "-y",
        "28",
        "-c",
        str(active_workspace),
        active_env_prefix,
    )
    wait_for(tmux_exe, active_session, r"Type a message|live session", "active-run fake-provider initial frame")
    send_literal(tmux_exe, active_session, "/help")
    wait_for(tmux_exe, active_session, r"/help", "active-run scrollback seed draft")
    send_keys(tmux_exe, active_session, "Enter")
    help_output = wait_for(
        tmux_exe,
        active_session,
        r"(?s)Command /help.*Commands:",
        "active-run help command-output seed",
    )
    if "│  /help" in help_output:
        raise RuntimeError(f"active-run /help rendered its invocation as chat\nscreen:\n{help_output}")
    send_keys(tmux_exe, active_session, "Escape")
    wait_for_absent(tmux_exe, active_session, r"Command /help", "active-run help output closed")
    send_literal(tmux_exe, active_session, "/write active-card.txt active card detail")
    send_keys(tmux_exe, active_session, "Enter")
    active_card_seed = wait_for(
        tmux_exe,
        active_session,
        r"wrote 18 bytes|Permission required",
        "active-run tool-card seed",
    )
    if "Permission required" in active_card_seed:
        send_keys(tmux_exe, active_session, "Tab", "Enter")
        active_card_seed = wait_for(tmux_exe, active_session, r"wrote 18 bytes", "allowed active-run tool-card seed")
    if "wrote 18 bytes" not in active_card_seed:
        raise RuntimeError(f"active-run tool-card seed did not complete\nscreen:\n{active_card_seed}")
    active_write_output = wait_for(tmux_exe, active_session, r"changed:.*active-card\.txt", "active-run Rich local tool output details")
    if "/write active-card.txt active card detail" in active_write_output:
        raise RuntimeError(f"active-run local write arguments leaked into modal chrome\nscreen:\n{active_write_output}")
    send_keys(tmux_exe, active_session, "Escape")
    local_write_closed = wait_for_absent(tmux_exe, active_session, r"Command /write|changed:.*active-card\.txt", "active-run local write output closed")
    if "active card detail" in local_write_closed:
        raise RuntimeError(f"active-run local write tool card remained in transcript after closing output\nscreen:\n{local_write_closed}")

    idle_seed = "tmux idle F5 assistant seed\n" + "\n".join(
        f"ACTIVE TRANSCRIPT SEED {index:02d}" for index in range(1, 19)
    )
    send_literal(tmux_exe, active_session, f"\x1b[200~{idle_seed}\x1b[201~")
    send_keys(tmux_exe, active_session, "Enter")
    _wait_for_normal_turn_request_count(active_provider, 1, "idle F5 assistant seed provider request")
    wait_for(tmux_exe, active_session, r"headless active prompt complete", "idle F5 assistant seed completion", timeout=14.0)

    send_literal(tmux_exe, active_session, "F5-IDLE-DRAFT-KEEP")
    idle_f5_output = root / "idle-f5-pane-output.bin"
    tmux(tmux_exe, "pipe-pane", "-t", active_session, f"cat > {shlex.quote(str(idle_f5_output))}")
    send_literal(tmux_exe, active_session, "\x1b[15~")
    wait_for_tmux_clipboard_transport(idle_f5_output, "idle F5 latest assistant copy")
    tmux(tmux_exe, "pipe-pane", "-t", active_session)
    idle_f5_copy = wait_for(tmux_exe, active_session, r"F5-IDLE-DRAFT-KEEP", "idle F5 preserved draft")
    if "Type a message" in idle_f5_copy:
        raise RuntimeError(f"idle F5 copy cleared or submitted the composer draft\nscreen:\n{idle_f5_copy}")
    send_keys(tmux_exe, active_session, "C-c")

    send_literal(tmux_exe, active_session, "F6-IDLE-STASH-RESTORE")
    send_literal(tmux_exe, active_session, "\x1b[17~")
    idle_stashed = wait_for_screen_state(
        tmux_exe,
        active_session,
        lambda screen: "F6-IDLE-STASH-RESTORE" not in screen and "Type a message" in screen,
        "idle F6 prompt stash",
    )
    if "F6-IDLE-STASH-RESTORE" in idle_stashed:
        raise RuntimeError(f"idle F6 stash did not clear the composer draft\nscreen:\n{idle_stashed}")
    send_literal(tmux_exe, active_session, "\x1b[17~")
    idle_selector = wait_for(tmux_exe, active_session, r"F6-IDLE-STASH-RESTORE", "idle F6 stash selector")
    if "Prompt stash" not in idle_selector:
        raise RuntimeError(f"idle F6 did not open the stash selector\nscreen:\n{idle_selector}")
    send_keys(tmux_exe, active_session, "Enter")
    wait_for(tmux_exe, active_session, r"F6-IDLE-STASH-RESTORE", "idle stash selector restore")
    send_keys(tmux_exe, active_session, "C-c")

    send_literal(tmux_exe, active_session, "tmux active first prompt")
    wait_for(tmux_exe, active_session, r"tmux active first prompt", "active-run first prompt draft")
    send_keys(tmux_exe, active_session, "Enter")
    active_provider.wait_for_request(2, "active-run first provider request")
    active_empty_hint = wait_for(tmux_exe, active_session, r"Esc stop.*type a follow-up", "F3 active empty contextual hint")
    active_dimensions = tmux(tmux_exe, "display-message", "-p", "-t", active_session, "#{pane_width},#{pane_height}").stdout.strip()
    if active_dimensions != "110,28":
        raise RuntimeError(f"F3 active palette dimensions were {active_dimensions}, expected 110,28")
    active_hint_lines = active_empty_hint.splitlines()
    if len(active_hint_lines) != 28 or any(len(line) > 110 for line in active_hint_lines) or not any(line.startswith("│  Esc stop") for line in active_hint_lines):
        raise RuntimeError(f"F3 active contextual hint did not retain its bounded shared composer gutter\nscreen:\n{active_empty_hint}")
    if "\x1b" in active_empty_hint or any(ord(character) < 32 and character != "\n" for character in active_empty_hint):
        raise RuntimeError(f"F3 active contextual hint contained ESC or unexpected C0 controls\nscreen:\n{active_empty_hint}")
    save_evidence(root, "frontend-f3-active-empty-hint", active_empty_hint)

    send_literal(tmux_exe, active_session, "F5-ACTIVE-DRAFT-KEEP")
    active_f5_output = root / "active-f5-pane-output.bin"
    tmux(tmux_exe, "pipe-pane", "-t", active_session, f"cat > {shlex.quote(str(active_f5_output))}")
    send_literal(tmux_exe, active_session, "\x1b[15~")
    wait_for_tmux_clipboard_transport(active_f5_output, "active-run preemptive F5 copy")
    tmux(tmux_exe, "pipe-pane", "-t", active_session)
    active_f5_copy = wait_for(tmux_exe, active_session, r"F5-ACTIVE-DRAFT-KEEP", "active-run F5 preserved draft")
    if "Type a message" in active_f5_copy:
        raise RuntimeError(f"active-run F5 copy cleared or queued the composer draft\nscreen:\n{active_f5_copy}")
    send_literal(tmux_exe, active_session, "\x1b[17~")
    active_stashed = wait_for_screen_state(
        tmux_exe,
        active_session,
        lambda screen: "F5-ACTIVE-DRAFT-KEEP" not in screen and "type a follow-up" in screen,
        "active-run preemptive F6 stash",
    )
    if "F5-ACTIVE-DRAFT-KEEP" in active_stashed:
        raise RuntimeError(f"active-run F6 stash did not clear the composer draft\nscreen:\n{active_stashed}")
    send_literal(tmux_exe, active_session, "\x1b[17~")
    active_selector = wait_for(tmux_exe, active_session, r"F5-ACTIVE-DRAFT-KEEP", "active-run stash selector")
    if "Prompt stash" not in active_selector:
        raise RuntimeError(f"active-run F6 did not open the stash selector\nscreen:\n{active_selector}")
    send_keys(tmux_exe, active_session, "Enter")
    wait_for(tmux_exe, active_session, r"F5-ACTIVE-DRAFT-KEEP", "active-run stash selector restore")
    send_keys(tmux_exe, active_session, "C-c")

    # Active-run nonblocking local output must use the same modal and must not
    # project its invocation or report into the still-streaming conversation.
    send_literal(tmux_exe, active_session, "/jobs show missing-job")
    send_keys(tmux_exe, active_session, "Enter")
    active_jobs_output = wait_for(
        tmux_exe,
        active_session,
        r"(?s)Command /jobs.*(?:job|Job|No background)",
        "active /jobs command-output modal",
    )
    if "/jobs show missing-job" in active_jobs_output:
        raise RuntimeError(f"active /jobs leaked its arguments into command output chrome\nscreen:\n{active_jobs_output}")
    save_evidence(root, "active-run-local-command-output", active_jobs_output)
    send_keys(tmux_exe, active_session, "Escape")
    active_jobs_closed = wait_for_absent(tmux_exe, active_session, r"Command /jobs", "active /jobs output closed")
    if "/jobs show missing-job" in active_jobs_closed:
        raise RuntimeError(f"active /jobs invocation remained in transcript after closing output\nscreen:\n{active_jobs_closed}")

    def set_active_details(mode: str) -> None:
        command = f"/details {mode}"
        send_literal(tmux_exe, active_session, command)
        wait_for(tmux_exe, active_session, rf"│  {re.escape(command)}(?:\s|$)", f"active {command} draft before submit")
        send_keys(tmux_exe, active_session, "Enter")
        settled = wait_for_absent(tmux_exe, active_session, re.escape(command), f"active {command} transient status")
        if command in settled:
            raise RuntimeError(f"{command} remained in the active transcript\nscreen:\n{settled}")

    def open_active_local_tool(command: str, pattern: str, label: str) -> str:
        send_literal(tmux_exe, active_session, command)
        send_keys(tmux_exe, active_session, "Enter")
        screen = wait_for(tmux_exe, active_session, rf"(?s)Command /tool.*{pattern}", label)
        if command in screen:
            raise RuntimeError(f"{command} arguments leaked into active command-output chrome\nscreen:\n{screen}")
        return screen

    set_active_details("compact")
    compact_active_card = open_active_local_tool("/tool write", r"write.*wrote 18 bytes", "active local tool history compact modal")
    if "changed:" in compact_active_card or "ACTIVE-OLD-LINE-25" in compact_active_card:
        raise RuntimeError(f"active compact local history unexpectedly rendered expanded details\nscreen:\n{compact_active_card}")
    send_keys(tmux_exe, active_session, "Escape")
    wait_for_absent(tmux_exe, active_session, r"Command /tool", "active compact local tool history closed")

    set_active_details("rich")
    rich_active_card = open_active_local_tool("/tools write", r"write.*changed:.*active-card\.txt", "active local tool history Rich alias modal")
    if "ACTIVE-OLD-LINE-25" in rich_active_card:
        raise RuntimeError(f"active Rich local history rendered expanded diff body\nscreen:\n{rich_active_card}")
    send_keys(tmux_exe, active_session, "Escape")
    wait_for_absent(tmux_exe, active_session, r"Command /tool", "active Rich local tool history closed")

    set_active_details("expanded")
    open_active_local_tool("/tool write", r"write.*changed:.*active-card\.txt", "active local tool history expanded modal")
    send_keys(tmux_exe, active_session, "End")
    expanded_active_card = wait_for(
        tmux_exe,
        active_session,
        r"ACTIVE-OLD-LINE-25",
        "active expanded local tool history scrolled diff body",
    )
    save_evidence(root, "active-run-local-tool-card-controls", expanded_active_card)
    send_keys(tmux_exe, active_session, "Escape")
    active_tool_closed = wait_for_absent(tmux_exe, active_session, r"Command /tool|ACTIVE-OLD-LINE-25", "active expanded local history closed")
    if any(command in active_tool_closed for command in ("/details compact", "/details rich", "/details expanded", "/tool write", "/tools write")):
        raise RuntimeError(f"active local detail commands or tool cards remained in transcript\nscreen:\n{active_tool_closed}")
    set_active_details("rich")

    send_literal(tmux_exe, active_session, "AGENTS")
    active_draft_hint = wait_for(tmux_exe, active_session, r"Esc stop|queue", "F3 active draft contextual hint")
    send_keys(tmux_exe, active_session, "Tab")
    active_forced_path = wait_for(tmux_exe, active_session, r"AGENTS\.md", "F3 active forced path completion")
    if "AGENTS.md" not in active_forced_path:
        raise RuntimeError(f"F3 active forced Tab did not insert the canonical workspace path\nscreen:\n{active_forced_path}")
    save_evidence(root, "frontend-f3-active-forced-path", active_forced_path)
    send_keys(tmux_exe, active_session, "C-u")

    def click_active_candidate(screen: str, needle: str, label: str) -> None:
        candidate = next(((index + 1, line) for index, line in enumerate(screen.splitlines()) if needle in line), None)
        if candidate is None:
            raise RuntimeError(f"{label} did not expose the visible candidate {needle!r}\nscreen:\n{screen}")
        row, line = candidate
        column = line.index(needle) + 1
        send_literal(tmux_exe, active_session, f"\x1b[<0;{column};{row}M")

    send_literal(tmux_exe, active_session, "/")
    active_slash_palette = wait_for(tmux_exe, active_session, r"│\s+› /help", "active-run slash palette")
    click_active_candidate(active_slash_palette, "/help", "active-run slash mouse palette")
    active_slash_selected = wait_for(tmux_exe, active_session, r"│  /help(?:\s|$)", "active-run slash mouse selection")
    if "│  /help" not in active_slash_selected:
        raise RuntimeError(f"active slash mouse selection did not insert the canonical command\nscreen:\n{active_slash_selected}")
    send_keys(tmux_exe, active_session, "C-u")

    send_literal(tmux_exe, active_session, "review @AG")
    active_reference_palette = wait_for(tmux_exe, active_session, r"│\s+› @AGENTS\.md", "active-run @ reference palette")
    click_active_candidate(active_reference_palette, "@AGENTS.md", "active-run @ reference mouse palette")
    active_reference_selected = wait_for(tmux_exe, active_session, r"review @AGENTS\.md", "active-run @ reference mouse selection")
    if "review @AGENTS.md" not in active_reference_selected:
        raise RuntimeError(f"active @ mouse selection did not insert the canonical reference\nscreen:\n{active_reference_selected}")
    send_keys(tmux_exe, active_session, "C-u")

    send_literal(tmux_exe, active_session, "inspect ./AG")
    active_path_palette = wait_for(tmux_exe, active_session, r"│\s+› \./AGENTS\.md", "active-run normal path palette")
    click_active_candidate(active_path_palette, "AGENTS.md", "active-run normal path mouse palette")
    active_path_selected = wait_for(tmux_exe, active_session, r"inspect \./AGENTS\.md", "active-run normal path mouse selection")
    if "inspect ./AGENTS.md" not in active_path_selected:
        raise RuntimeError(f"active path mouse selection did not insert the canonical path\nscreen:\n{active_path_selected}")
    send_keys(tmux_exe, active_session, "C-u")
    send_literal(tmux_exe, active_session, "/share")
    active_disabled_share = wait_for(tmux_exe, active_session, r"│  /share", "active disabled slash draft")
    disabled_share_status = r"command disabled: cloud sharing is deferred"
    send_keys(tmux_exe, active_session, "Tab")
    active_disabled_tab = wait_for(
        tmux_exe, active_session, disabled_share_status, "active disabled slash Tab rejection status"
    )
    if "/share" not in active_disabled_tab:
        raise RuntimeError(f"disabled slash Tab mutated the active draft\nscreen:\n{active_disabled_tab}")
    send_keys(tmux_exe, active_session, "Enter")
    active_disabled_enter = wait_for(
        tmux_exe, active_session, disabled_share_status, "active disabled slash Enter rejection status"
    )
    if "/share" not in active_disabled_enter or any(
        status in active_disabled_enter for status in ("job command complete", "follow-up queued", "steering queued", "commands run between turns")
    ):
        raise RuntimeError(f"disabled slash Enter dispatched command/queue output or mutated the active draft\nscreen:\n{active_disabled_enter}")
    click_active_candidate(active_disabled_enter, "/share", "active disabled slash mouse palette")
    active_disabled_mouse = wait_for(
        tmux_exe, active_session, disabled_share_status, "active disabled slash mouse rejection status"
    )
    if "/share" not in active_disabled_mouse or "commands run between turns" in active_disabled_mouse:
        raise RuntimeError(f"disabled slash mouse click mutated or queued the active draft\nscreen:\n{active_disabled_mouse}")
    # The request log is append-only and the active request remains gated, so
    # one final observation covers every non-submitting action above without
    # paying four separate negative-observation windows.
    _assert_normal_turn_request_count_stays(
        active_request_log,
        2,
        "active F5/F6, local commands, palettes, and disabled commands must not reach the provider",
    )
    send_keys(tmux_exe, active_session, "C-u")

    send_literal(tmux_exe, active_session, "/")
    wait_for(tmux_exe, active_session, r"/help|Show commands", "active-run slash palette")
    send_literal(tmux_exe, active_session, "\x1b[1;129B")
    wait_for(tmux_exe, active_session, r"› /hotkeys|> /hotkeys", "active-run physical Ghostty arrow palette navigation")
    send_keys(tmux_exe, active_session, "C-u")
    wait_for_absent(tmux_exe, active_session, r"› /hotkeys|> /hotkeys", "active-run slash palette cleared")
    send_literal(tmux_exe, active_session, "\x1b[200~tmux active follow-up\nsecond line\x1b[201~")
    active_live_tail = wait_for(
        tmux_exe, active_session, r"tmux active follow-up.*second line|second line", "active-run multiline follow-up draft"
    )
    transcript_rows = lambda screen: tuple(screen.splitlines()[:-4])
    active_live_rows = transcript_rows(active_live_tail)
    send_literal(tmux_exe, active_session, "\x1b[1;129A")
    active_arrow_scrolled = wait_for_screen_state(
        tmux_exe,
        active_session,
        lambda screen: transcript_rows(screen) != active_live_rows and "tmux active follow-up" in screen and "second line" in screen,
        "active-run physical Ghostty arrow transcript movement",
    )
    # /help intentionally lists the jump_to_bottom action name; only the
    # removed detached-banner phrases indicate stale product chrome.
    if any(text in active_arrow_scrolled for text in ("scrollback detached", "updates below")):
        raise RuntimeError(f"active-run arrow scroll surfaced deleted detached chrome\nscreen:\n{active_arrow_scrolled}")
    send_literal(tmux_exe, active_session, "X")
    active_multiline_cursor = wait_for(
        tmux_exe, active_session, r"second lineX", "active-run multiline cursor preserved by arrow scroll"
    )
    if "follow-upX" in active_multiline_cursor:
        raise RuntimeError(
            "active-run arrow moved the multiline composer cursor instead of scrolling only the transcript\n"
            f"screen:\n{active_multiline_cursor}"
        )
    send_literal(tmux_exe, active_session, "\x1b[1;129B")
    active_arrow_tail = wait_for_screen_state(
        tmux_exe,
        active_session,
        lambda screen: transcript_rows(screen) == active_live_rows and "tmux active follow-up" in screen and "second lineX" in screen,
        "active-run physical Ghostty arrow return to live tail",
    )
    active_wheel_live_rows = transcript_rows(active_arrow_tail)
    send_literal(tmux_exe, active_session, "\x1b[<64;4;6M")
    active_wheel_scrolled = wait_for_screen_state(
        tmux_exe,
        active_session,
        lambda screen: transcript_rows(screen) != active_wheel_live_rows and "tmux active follow-up" in screen and "second lineX" in screen,
        "active-run mouse wheel transcript movement",
    )
    if any(text in active_wheel_scrolled for text in ("scrollback detached", "updates below")):
        raise RuntimeError(f"active-run wheel scroll surfaced deleted detached chrome\nscreen:\n{active_wheel_scrolled}")
    # A harmless non-wheel ordering boundary at the end of the draft resets the
    # physical-wheel burst governor without changing the draft or cursor.
    send_literal(tmux_exe, active_session, "\x1b[1;129C\x1b[<65;4;6M")
    active_wheel_tail = wait_for_screen_state(
        tmux_exe,
        active_session,
        lambda screen: transcript_rows(screen) == active_wheel_live_rows and "tmux active follow-up" in screen and "second lineX" in screen,
        "active-run mouse wheel return to live tail",
    )
    send_literal(tmux_exe, active_session, "\x1b\r")
    queued_follow_up = wait_for(tmux_exe, active_session, r"follow-up queued", "active-run Alt+Enter follow-up queued")
    if "tmux active follow-up" not in queued_follow_up:
        raise RuntimeError(f"active-run Alt+Enter did not render the queued follow-up text\nscreen:\n{queued_follow_up}")
    save_evidence(root, "active-run-follow-up-queued", queued_follow_up)
    active_provider.release_request(2)
    active_provider.wait_for_request(3, "active-run queued follow-up provider request", timeout=14.0)
    active_log = active_request_log.read_text(encoding="utf-8")
    if (
        "tmux idle F5 assistant seed" not in active_log
        or "tmux active first prompt" not in active_log
        or "tmux active follow-up" not in active_log
        or "second lineX" not in active_log
    ):
        raise RuntimeError(f"active-run follow-up did not reach the fake provider intact\nrequest log:\n{active_log}")
    wait_for(tmux_exe, active_session, r"follow-up started|headless active prompt complete", "active-run follow-up delivery")
    send_keys(tmux_exe, active_session, "C-d")
    wait_for_session_exit(tmux_exe, active_session)
    tmux(tmux_exe, "kill-session", "-t", active_session, check=False)
