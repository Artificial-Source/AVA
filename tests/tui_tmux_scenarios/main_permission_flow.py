"""The tmux TUI smoke scenario for permissioned local command output."""

from __future__ import annotations

import re

from tui_smoke_helpers import (
    SmokeContext,
    capture,
    capture_styled,
    save_evidence,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
    wait_for_screen_change,
    wait_for_screen_state,
)
from .common import _finish_main, _main_session


def scenario_main_permission_flow(ctx: SmokeContext) -> None:
    tmux_exe, root, workspace, ava_config, env_prefix, session = _main_session(ctx)

    def assert_exact_frame(screen: str, width: int, height: int, label: str, *, allow_trimmed_blank_tail: bool = False) -> None:
        dimensions = tmux(
            tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}"
        ).stdout.strip()
        lines = screen.splitlines()
        height_mismatch = len(lines) > height or (not allow_trimmed_blank_tail and len(lines) != height)
        if dimensions != f"{width},{height}" or height_mismatch or any(len(line) > width for line in lines):
            raise RuntimeError(f"{label} did not retain a bounded {width}x{height} terminal frame\nscreen:\n{screen}")
        if "\x1b" in screen or any(ord(character) < 32 and character != "\n" for character in screen):
            raise RuntimeError(f"{label} contained terminal control bytes\nscreen:\n{screen}")

    def assert_token_only_title(screen: str, token: str, submitted: str, label: str) -> None:
        title_line = next((line for line in screen.splitlines() if f"Command {token}" in line), "")
        if not title_line:
            raise RuntimeError(f"{label} did not render the sanitized command token title\nscreen:\n{screen}")
        if submitted in title_line or f"│  {submitted}" in screen:
            raise RuntimeError(f"{label} leaked the full command invocation into modal chrome or transcript\nscreen:\n{screen}")

    def close_command_output(title_pattern: str, forbidden: tuple[str, ...], label: str) -> str:
        send_keys(tmux_exe, session, "Escape")

        def command_output_is_closed(screen: str) -> bool:
            return (
                re.search(title_pattern, screen) is None
                and re.search(r"Enter(?:/Esc)? close", screen) is None
                and all(not text or text not in screen for text in forbidden)
                and re.search(r"(?m)^[ \t]*│[ \t]+Type a message\.\.\.", screen) is not None
            )

        # Tmux can capture ncurses' repaint after the title row is cleared but
        # before lower modal rows disappear. Require one fully restored composer
        # frame rather than treating title absence alone as a close boundary.
        closed = wait_for_screen_state(tmux_exe, session, command_output_is_closed, f"{label} fully closed")
        leaked = [text for text in forbidden if text and text in closed]
        if leaked:
            raise RuntimeError(f"{label} remained in transcript after close: {leaked}\nscreen:\n{closed}")
        return closed

    def submit_report(command: str, output_pattern: str, label: str) -> str:
        send_keys(tmux_exe, session, "C-u")
        send_literal(tmux_exe, session, command)
        send_keys(tmux_exe, session, "Enter")
        report = wait_for(
            tmux_exe,
            session,
            rf"(?s)Command /permissions.*{output_pattern}",
            label,
        )
        assert_token_only_title(report, "/permissions", command, label)
        return report

    # Preserve the release-gate prompt geometry, truthful risk metadata, exact
    # choices, and wheel burst governor while command events are buffered.
    send_keys(tmux_exe, session, "C-u")
    denied_command = "/bash git push origin main"
    send_literal(tmux_exe, session, denied_command)
    send_keys(tmux_exe, session, "Enter")
    permission = wait_for(
        tmux_exe,
        session,
        r"(?s)! Permission required.*Shell command.*\$ git push origin main.*risk critical.*reason sealed.*› Reject.*Allow once",
        "permission prompt risk metadata",
    )
    assert_exact_frame(permission, 120, 32, "roomy permission prompt")
    if (
        "Command /bash" in permission
        or "permreq_" in permission
        or "Always allow" in permission
        or ("Always reject" not in permission and "Never" not in permission)
    ):
        raise RuntimeError(f"critical raw-shell prompt did not expose only truthful one-shot/deny choices\nscreen:\n{permission}")
    save_evidence(root, "frontend-f5-permission-prompt-roomy", permission)
    save_evidence(root, "permission-prompt-risk-reason", permission)

    wheel_down = "\x1b[<65;4;6M"
    wheel_up = "\x1b[<64;4;6M"
    send_literal(tmux_exe, session, wheel_down * 12)
    permission_burst = wait_for(
        tmux_exe,
        session,
        r"(?s)! Permission required.*Reject.*› Allow once",
        "permission same-direction wheel burst governed to one choice",
    )
    if "› Allow once" not in permission_burst:
        raise RuntimeError(f"permission wheel burst did not settle on Allow once\nscreen:\n{permission_burst}")
    send_literal(tmux_exe, session, "x")
    send_literal(tmux_exe, session, wheel_up)
    deliberate_wheel = wait_for(
        tmux_exe,
        session,
        r"(?s)! Permission required.*› Reject.*Allow once",
        "permission wheel accepted after the non-wheel reset boundary",
    )
    save_evidence(root, "permission-wheel-burst-governed", deliberate_wheel)
    send_keys(tmux_exe, session, "R", "Enter")
    wait_for_absent(tmux_exe, session, r"Permission required", "permission prompt denied and remembered")

    denied = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /bash.*command permission denied.*x bash.*\$ <redacted one-shot command>",
        "permission denial command-output modal",
    )
    assert_token_only_title(denied, "/bash", denied_command, "permission denial command output")
    denied_primary_rows = [line for line in denied.splitlines() if re.search(r"[~+x] bash ·", line)]
    if (
        len(denied_primary_rows) != 1
        or "x bash · command permission denied" not in denied_primary_rows[0]
        or "permission: deny" in denied
        or "resolver:" in denied
    ):
        raise RuntimeError(f"permission denial did not render one safe Rich modal tool card\nscreen:\n{denied}")
    save_evidence(root, "frontend-f5-denied-tool-card", denied)
    close_command_output(
        r"Command /bash|command permission denied|<redacted one-shot command>",
        (denied_command, "command permission denied", "<redacted one-shot command>"),
        "denied local command output",
    )

    # The remembered deny remains backend/session authority. Its listing is a
    # local report modal and the raw command body never enters transcript.
    remembered_rule = submit_report(
        "/permissions list",
        r"Permission rules:.*1\. Block Exact command · \. · Workspace · Build",
        "remembered permission rule listing",
    )
    list_match = re.search(
        r"(?s)Permission rules:.*?Use /permissions explain or /permissions remove, then choose or complete a rule\.",
        remembered_rule,
    )
    list_section = list_match.group(0) if list_match else ""
    if (
        not list_section
        or "1. Block Exact command · . · Workspace · Build" not in list_section
        or "git push origin main" in list_section
        or "permrule_" in list_section
        or 'command="' in list_section
    ):
        raise RuntimeError(
            "remembered deny rule was not a path-qualified human summary without raw command body\n"
            f"screen:\n{remembered_rule}"
        )
    close_command_output(
        r"Command /permissions|Permission rules:",
        ("/permissions list", "Permission rules:", denied_command),
        "permission rule report",
    )

    send_literal(tmux_exe, session, denied_command)
    send_keys(tmux_exe, session, "Enter")
    repeated_denial = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /bash.*x bash\s+·\s+command permission denied.*\$ <redacted one-shot command>",
        "remembered denial result modal",
    )
    if "Permission required" in repeated_denial or "PERMISSION REQUIRED" in repeated_denial:
        raise RuntimeError(f"remembered deny rule did not suppress a repeated prompt\nscreen:\n{repeated_denial}")
    assert_token_only_title(repeated_denial, "/bash", denied_command, "remembered denial command output")
    close_command_output(
        r"Command /bash|command permission denied|<redacted one-shot command>",
        (denied_command, "command permission denied", "<redacted one-shot command>"),
        "remembered denial output",
    )

    # Explicit audit is TUI-only rich output. Reuse its request id for copy and
    # every release report surface, closing each modal before the next command.
    permission_audit_listing = submit_report(
        "/permissions audit bash",
        r"Permission audit.*permreq_",
        "explicit permission audit listing",
    )
    permission_request_match = re.search(r"permreq_[A-Za-z0-9_]+", permission_audit_listing)
    if not permission_request_match:
        raise RuntimeError(f"explicit audit surface did not expose a reusable permission request id\nscreen:\n{permission_audit_listing}")
    permission_request_prefix = permission_request_match.group(0)
    close_command_output(
        r"Command /permissions|Permission audit",
        ("/permissions audit bash", "Permission audit", permission_request_prefix),
        "permission audit listing",
    )

    copy_permission_command = f"/copy permission {permission_request_prefix}"
    send_literal(tmux_exe, session, copy_permission_command)
    send_keys(tmux_exe, session, "Enter")
    copied_permission = wait_for(
        tmux_exe,
        session,
        r"matching permission details copy request sent",
        "copy matching permission details",
    )
    if "Command /copy" in copied_permission or copy_permission_command in copied_permission:
        raise RuntimeError(f"permission copy confirmation used chat/modal history\nscreen:\n{copied_permission}")

    # Local write output lives in the modal. The tool remains available only in
    # bounded TUI history for /tool, /diff, and /copy after the modal closes.
    write_command = "/write src/main.cpp int changed() { return 1; }"
    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, write_command)
    send_keys(tmux_exe, session, "Enter")
    write_result = wait_for(
        tmux_exe,
        session,
        r"wrote 27 bytes|Permission required",
        "write command result for local history",
    )
    if "Permission required" in write_result or "PERMISSION REQUIRED" in write_result:
        send_keys(tmux_exe, session, "Tab", "Enter")
        write_result = wait_for(
            tmux_exe,
            session,
            r"(?s)Command /write.*wrote 27 bytes",
            "allowed write command-output modal",
        )
    assert_token_only_title(write_result, "/write", write_command, "write command output")
    write_changed_details = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /write.*changed:.*src/main\.cpp",
        "write changed-file modal detail row",
    )
    write_primary = next((line for line in write_changed_details.splitlines() if "+ write" in line), "")
    if "src/main.cpp · wrote 27 bytes" not in write_primary or "/src/main.cpp" in write_primary:
        raise RuntimeError(f"write modal tool-card header lost its workspace-relative target\nscreen:\n{write_changed_details}")
    save_evidence(root, "permission-local-write-command-output", write_changed_details)

    def resize_tool_modal(width: int, height: int, name: str, *, title: str, expected: str) -> str:
        previous = capture(tmux_exe, session)
        tmux(tmux_exe, "resize-window", "-t", session, "-x", str(width), "-y", str(height))
        if capture(tmux_exe, session) == previous:
            wait_for_screen_change(tmux_exe, session, previous, f"{name} resize redraw")
        screen = wait_for(
            tmux_exe,
            session,
            rf"(?s){re.escape(title)}.*{expected}.*Enter(?:/Esc)? close",
            f"{name} settled local tool modal",
        )
        assert_exact_frame(screen, width, height, name, allow_trimmed_blank_tail=True)
        if write_command in screen or "│  /write" in screen:
            raise RuntimeError(f"{name} leaked command arguments into modal chrome or transcript\nscreen:\n{screen}")
        save_evidence(root, name, screen)
        return screen

    resize_tool_modal(120, 36, "frontend-f2-local-tool-modal-ordinary", title="Command /write", expected=r"write.*wrote 27 bytes")
    resize_tool_modal(160, 48, "frontend-f2-local-tool-modal-wide", title="Command /write", expected=r"write.*wrote 27 bytes")
    resize_tool_modal(80, 24, "frontend-f2-local-tool-modal-narrow", title="Command /write", expected=r"write.*wrote 27 bytes")
    resize_tool_modal(100, 12, "frontend-f2-local-tool-modal-short", title="Command /write", expected=r"write.*wrote 27 bytes")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "32")
    wait_for(tmux_exe, session, r"Command /write", "write modal baseline resize restore")
    close_command_output(
        r"Command /write|wrote 27 bytes|changed:.*src/main\.cpp",
        (write_command, "wrote 27 bytes", "+int changed()"),
        "write command output",
    )

    def set_details(mode: str) -> None:
        command = f"/details {mode}"
        send_literal(tmux_exe, session, command)
        send_keys(tmux_exe, session, "Enter")
        settled = wait_for_absent(tmux_exe, session, re.escape(command), f"{command} transient status")
        if command in settled:
            raise RuntimeError(f"{command} remained in transcript\nscreen:\n{settled}")

    def open_local_tool(command: str, pattern: str, label: str) -> str:
        send_literal(tmux_exe, session, command)
        send_keys(tmux_exe, session, "Enter")
        modal = wait_for(tmux_exe, session, rf"(?s)Command /tool.*{pattern}", label)
        if command in modal:
            raise RuntimeError(f"{command} arguments leaked into tool-history modal chrome\nscreen:\n{modal}")
        return modal

    set_details("compact")
    compact_tool = open_local_tool("/tool write", r"write.*wrote 27 bytes", "compact local tool history modal")
    if "changed:" in compact_tool or "+int changed()" in compact_tool:
        raise RuntimeError(f"compact local tool history retained expanded details\nscreen:\n{compact_tool}")
    close_command_output(r"Command /tool", ("/tool write", "wrote 27 bytes"), "compact local tool history")

    set_details("rich")
    rich_tool = open_local_tool("/tools write", r"write.*changed:.*src/main\.cpp", "Rich local tool history alias modal")
    if "+int changed()" not in rich_tool:
        raise RuntimeError(f"Rich local tool history lost its bounded diff evidence\nscreen:\n{rich_tool}")
    close_command_output(r"Command /tools", ("/tools write", "wrote 27 bytes", "changed:"), "Rich local tool history")

    set_details("expanded")
    expanded_tool = open_local_tool("/tool write", r"write.*changed:.*src/main\.cpp", "expanded local tool history modal")
    send_keys(tmux_exe, session, "End")
    expanded_tool = wait_for(
        tmux_exe,
        session,
        r"\+int changed\(\)",
        "expanded local tool history diff body",
    )
    save_evidence(root, "visible-tool-details", expanded_tool)
    resize_tool_modal(160, 48, "frontend-f2-local-history-wide", title="Command /tool", expected=r"write.*wrote 27 bytes")
    resize_tool_modal(120, 36, "frontend-f2-local-history-ordinary", title="Command /tool", expected=r"write.*wrote 27 bytes")
    resize_tool_modal(80, 24, "frontend-f2-local-history-narrow", title="Command /tool", expected=r"write.*wrote 27 bytes")
    resize_tool_modal(100, 12, "frontend-f2-local-history-short", title="Command /tool", expected=r"write.*wrote 27 bytes")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "32")
    wait_for(tmux_exe, session, r"Command /tool", "local history baseline resize restore")
    close_command_output(
        r"Command /tool|wrote 27 bytes|changed:|\+int changed\(\)",
        ("/tool write", "wrote 27 bytes", "changed:", "+int changed()"),
        "expanded local tool history",
    )
    set_details("rich")

    # Copy confirmations stay in the footer, while visible diff uses another
    # temporary modal sourced from local TUI-only history.
    for command, status, label in (
        ("/copy tool", "latest tool details copy request sent", "copy latest tool details"),
        ("/copy tool write", "matching tool details copy request sent", "copy matching tool details"),
        ("/copy diff", "latest tool diff copy request sent", "copy latest tool diff"),
        ("/copy diff main.cpp", "matching tool diff copy request sent", "copy matching tool diff"),
    ):
        send_literal(tmux_exe, session, command)
        send_keys(tmux_exe, session, "Enter")
        copied = wait_for(tmux_exe, session, re.escape(status), label)
        if "Command /copy" in copied or command in copied:
            raise RuntimeError(f"{label} did not remain a transient footer confirmation\nscreen:\n{copied}")

    diff_command = "/diff main.cpp"
    send_literal(tmux_exe, session, diff_command)
    send_keys(tmux_exe, session, "Enter")
    visible_matching_diff = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /diff.*Matching tool diff:.*\+int changed\(\)",
        "visible matching local tool diff modal",
    )
    assert_token_only_title(visible_matching_diff, "/diff", diff_command, "visible matching diff")
    save_evidence(root, "visible-diff-card", visible_matching_diff)
    close_command_output(
        r"Command /diff|Matching tool diff:|\+int changed\(\)",
        (diff_command, "Matching tool diff:", "+int changed()"),
        "visible matching diff",
    )

    # Restore all explicit permission release-report surfaces. Each report is
    # independently useful in the modal, then demonstrably absent from chat.
    report_cases = (
        (
            f"/permissions audit {permission_request_prefix}",
            r"request=permreq_.*<redacted one-shot command>",
            ("request=permreq_", "<redacted one-shot command>"),
            "permission audit command output",
        ),
        (
            f"/permissions audit summary {permission_request_prefix}",
            r"Permission audit summary:.*denials: [1-9].*by resolution: deny=",
            ("Permission audit summary:", "by resolution: deny="),
            "permission audit summary command output",
        ),
        (
            f"/permissions audit export {permission_request_prefix}",
            r"<redacted one-shot command>.*```",
            ("<redacted one-shot command>", "```"),
            "permission audit export command output",
        ),
        (
            f"/permissions diagnose {permission_request_prefix}",
            r"Recent permission denials:.*<redacted one-shot command>",
            ("Recent permission denials:", "<redacted one-shot command>"),
            "permission denial diagnostics command output",
        ),
        (
            f"/permissions audit show {permission_request_prefix}",
            r"command: <redacted one-shot command>",
            ("command: <redacted one-shot command>", "Related commands"),
            "permission audit detail command output",
        ),
    )
    for command, pattern, forbidden_output, label in report_cases:
        report = submit_report(command, pattern, label)
        observed_report = report
        if not all(text in observed_report for text in forbidden_output):
            send_keys(tmux_exe, session, "End")
            report_bottom = wait_for(
                tmux_exe,
                session,
                re.escape(forbidden_output[-1]),
                f"{label} scrolled report tail",
            )
            observed_report += "\n" + report_bottom
        if not all(text in observed_report for text in forbidden_output):
            raise RuntimeError(f"{label} lost required redacted output\nscreen:\n{observed_report}")
        save_evidence(root, label.replace(" ", "-"), observed_report)
        close_command_output(
            r"Command /permissions",
            (command, *forbidden_output),
            label,
        )

    # Exercise bounded local shell spill output in the command modal. Expanded
    # tool metadata must remain truthful, scrollable, and dimension-safe at all
    # release terminal sizes without ever becoming transcript history.
    set_details("expanded")
    seq_command = "/bash seq 1 20000"
    send_literal(tmux_exe, session, seq_command)
    send_keys(tmux_exe, session, "Enter")
    wait_for(
        tmux_exe,
        session,
        r"Permission required",
        "bounded local bash spill permission",
        timeout=12.0,
    )
    send_literal(tmux_exe, session, "a")
    seq_result = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /bash.*exit: 0",
        "allowed bounded local bash spill modal",
        timeout=30.0,
    )
    assert_token_only_title(seq_result, "/bash", seq_command, "bounded bash command output")
    if re.search(r"(?m)^\s*(?:1|10000)\s*$", seq_result) or "output truncated to last 200/20000 lines" not in seq_result:
        raise RuntimeError(f"bounded local spill modal did not expose only its retained tail with truthful summary\nscreen:\n{seq_result}")

    initial_footer = re.search(r"(\d+)-(\d+)/(\d+)", seq_result)
    send_literal(tmux_exe, session, "\x1b[<65;4;6M" * 8)
    wait_for_screen_change(tmux_exe, session, seq_result, "bounded local bash modal wheel scroll redraw")
    wheel_scrolled = capture(tmux_exe, session)
    wheel_footer = re.search(r"(\d+)-(\d+)/(\d+)", wheel_scrolled)
    if not initial_footer or not wheel_footer or int(wheel_footer.group(1)) <= int(initial_footer.group(1)):
        raise RuntimeError(f"command-output mouse wheel did not advance the bounded viewport\nscreen:\n{wheel_scrolled}")
    send_keys(tmux_exe, session, "End")
    seq_expanded = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /bash.*truncation:.*full output:",
        "expanded bounded local bash spill metadata",
        timeout=10.0,
    )
    if (
        "… 19800 lines hidden" not in seq_expanded
        or "20001 lines" in seq_expanded
        or "19801 lines hidden" in seq_expanded
        or "truncation:" not in seq_expanded
        or "bytes" not in seq_expanded
        or "full output:" not in seq_expanded
    ):
        raise RuntimeError(f"expanded local spill modal lacked bounded truthful metadata\nscreen:\n{seq_expanded}")
    save_evidence(root, "bounded-local-bash-spill-metadata", seq_expanded)

    def resize_spill_modal(width: int, height: int, name: str) -> None:
        previous = capture(tmux_exe, session)
        tmux(tmux_exe, "resize-window", "-t", session, "-x", str(width), "-y", str(height))
        if capture(tmux_exe, session) == previous:
            wait_for_screen_change(tmux_exe, session, previous, f"{name} resize redraw")
        wait_for(tmux_exe, session, r"Command /bash", f"{name} modal title after resize")
        send_keys(tmux_exe, session, "End")
        lifecycle = wait_for(
            tmux_exe,
            session,
            r"(?s)Command /bash.*truncation:.*full output:.*Enter(?:/Esc)? close",
            f"{name} lifecycle frame",
        )
        assert_exact_frame(lifecycle, width, height, name, allow_trimmed_blank_tail=True)
        if (
            seq_command in lifecycle
            or "20001 lines" in lifecycle
            or "19801 lines hidden" in lifecycle
            or "… 19800 lines hidden" not in lifecycle
            or "bytes" not in lifecycle
        ):
            raise RuntimeError(f"{name} lost truthful bounded spill metadata or leaked invocation arguments\nscreen:\n{lifecycle}")
        styled = capture_styled(tmux_exe, session)
        if "\x1b[" in styled:
            raise RuntimeError(f"{name} violated the scenario NO_COLOR contract\nstyled screen:\n{styled!r}")
        save_evidence(root, name, lifecycle)

    resize_spill_modal(160, 48, "frontend-f5-lifecycle-wide")
    resize_spill_modal(120, 36, "frontend-f5-lifecycle-ordinary")
    resize_spill_modal(80, 24, "frontend-f5-lifecycle-narrow")
    resize_spill_modal(100, 12, "frontend-f5-lifecycle-short")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "32")
    wait_for(tmux_exe, session, r"Command /bash", "bounded spill baseline resize restore")
    seq_closed = close_command_output(
        r"Command /bash|truncation:|full output:|19800 lines hidden",
        (seq_command, "truncation:", "full output:", "19800 lines hidden", "19999", "20000"),
        "bounded local bash spill output",
    )
    if any(text in seq_closed for text in (write_command, "/tool write", diff_command, denied_command)):
        raise RuntimeError(f"prior local command invocations remained after the final modal closed\nscreen:\n{seq_closed}")

    _finish_main(tmux_exe, session)
