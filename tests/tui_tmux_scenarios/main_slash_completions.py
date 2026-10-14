"""The tmux TUI smoke scenario for main slash completions."""

from __future__ import annotations

import re
import time

from tui_smoke_helpers import (
    ACTIVE_CONTEXT_TOKEN_COUNT_PATTERN,
    SmokeContext,
    assert_screen_absent_for,
    assert_screen_present_for,
    capture,
    capture_styled,
    pane_cursor_position,
    save_evidence,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
    wait_for_cursor_change,
    wait_for_screen_change,
    wait_for_session_exit,
)
from .common import _finish_main, _main_session, _wait_for_normal_turn_request_count


def scenario_main_slash_completions(ctx: SmokeContext) -> None:
    tmux_exe, root, workspace, ava_config, env_prefix, session = _main_session(ctx)
    send_literal(tmux_exe, session, "/")
    palette = wait_for(tmux_exe, session, r"/help|Show commands", "slash palette")
    if "[200~" in palette or "[201~" in palette:
        raise RuntimeError(f"paste markers leaked before paste smoke\nscreen:\n{palette}")
    if "/help" not in palette or "Show commands" not in palette:
        raise RuntimeError(f"F3 ordinary palette did not retain command and description\nscreen:\n{palette}")
    def f3_main_width(width: int, height: int) -> int:
        if width >= 176 and height >= 16:
            return width - 39
        return min(width, 120)

    def f3_canvas_left(width: int, height: int) -> int:
        return 0 if width >= 176 and height >= 16 else (width - f3_main_width(width, height)) // 2

    def assert_f3_frame(
        screen: str, width: int, height: int, label: str, *, palette_visible: bool, cursor_column: str
    ) -> None:
        dimensions = tmux(tmux_exe, "display-message", "-p", "-t", session, "#{pane_width},#{pane_height}").stdout.strip()
        if dimensions != f"{width},{height}":
            raise RuntimeError(f"{label} dimensions were {dimensions}, expected {width},{height}")
        lines = screen.splitlines()
        if len(lines) != height or any(len(line) > width for line in lines):
            raise RuntimeError(f"{label} did not have an exact {height}-row, {width}-column-bounded capture\nscreen:\n{screen}")
        if "\x1b" in screen or any(ord(character) < 32 and character != "\n" for character in screen):
            raise RuntimeError(f"{label} contained ESC or unexpected C0 controls\nscreen:\n{screen}")
        main_width = f3_main_width(width, height)
        canvas_left = f3_canvas_left(width, height)
        rail_visible = width >= 176 and height >= 16
        if rail_visible and any(len(line) <= main_width or line[main_width] != "│" for line in lines):
            raise RuntimeError(f"{label} did not retain the automatic-rail divider at main width {main_width}\nscreen:\n{screen}")
        input_line = lines[height - 2][canvas_left : canvas_left + main_width]
        footer = lines[height - 1][canvas_left : canvas_left + main_width]
        if not input_line.startswith("│  /") or not footer.startswith("│  "):
            raise RuntimeError(f"{label} did not place the slash input/footer on rows {height - 1}/{height}\nscreen:\n{screen}")
        if not re.fullmatch(rf"GPT-5\.5 · ctx {ACTIVE_CONTEXT_TOKEN_COUNT_PATTERN}", footer[3:].strip()):
            raise RuntimeError(f"{label} footer exposed text beyond model/context\nscreen:\n{screen}")
        candidate_lines = [
            line[canvas_left : canvas_left + main_width]
            for line in lines[: height - 2]
            if re.match(r"│  [› ]+ /", line[canvas_left : canvas_left + main_width])
        ]
        if palette_visible:
            if not candidate_lines or not any("/help" in line or "Show commands" in line for line in candidate_lines):
                raise RuntimeError(f"{label} did not keep slash palette candidates within the composer bounds\nscreen:\n{screen}")
        elif candidate_lines:
            raise RuntimeError(f"{label} retained slash palette candidates after dismissal\nscreen:\n{screen}")
        expected_cursor_column = str(int(cursor_column) + canvas_left)
        if pane_cursor_position(tmux_exe, session).split(",", 1)[0] != expected_cursor_column:
            raise RuntimeError(f"{label} did not preserve the centered composer cursor column\nscreen:\n{screen}")

    def wait_for_f3_composer_reflow(width: int, height: int, label: str, *, palette_visible: bool) -> str:
        """Wait for AVA's compositor, not tmux's immediate stale resize reflow."""
        deadline = time.monotonic() + 8.0
        last = ""
        while time.monotonic() < deadline:
            dimensions = tmux(
                tmux_exe, "display-message", "-p", "-t", session, "#{pane_width},#{pane_height}"
            ).stdout.strip()
            last = capture(tmux_exe, session)
            lines = last.splitlines()
            main_width = f3_main_width(width, height)
            canvas_left = f3_canvas_left(width, height)
            input_ready = len(lines) == height and lines[height - 2][canvas_left : canvas_left + main_width].startswith("│  /")
            footer_ready = (
                len(lines) == height
                and lines[height - 1][canvas_left : canvas_left + main_width].startswith("│  ")
                and re.fullmatch(rf"GPT-5\.5 · ctx {ACTIVE_CONTEXT_TOKEN_COUNT_PATTERN}", lines[height - 1][canvas_left : canvas_left + main_width][3:].strip()) is not None
            )
            palette_ready = any(
                re.match(r"│  [› ]+ /", line[canvas_left : canvas_left + main_width])
                and ("/help" in line or "Show commands" in line)
                for line in lines[: height - 2]
            )
            rail_ready = width < 176 or height < 16 or (
                len(lines) == height and all(len(line) > main_width and line[main_width] == "│" for line in lines)
            )
            if dimensions == f"{width},{height}" and input_ready and footer_ready and rail_ready and palette_ready == palette_visible:
                return last
            time.sleep(0.05)
        raise RuntimeError(f"timed out waiting for {label} AVA composer reflow\nlast screen:\n{last}")

    cursor_before_resize = pane_cursor_position(tmux_exe, session)
    cursor_column_before_resize = cursor_before_resize.split(",", 1)[0]
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "36")
    palette = wait_for_f3_composer_reflow(120, 36, "F3 ordinary slash palette", palette_visible=True)
    assert_f3_frame(palette, 120, 36, "F3 ordinary slash palette", palette_visible=True, cursor_column=cursor_column_before_resize)
    save_evidence(root, "frontend-f3-slash-ordinary-120x36", palette)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "80", "-y", "24")
    narrow_palette = wait_for_f3_composer_reflow(80, 24, "F3 narrow slash palette", palette_visible=True)
    assert_f3_frame(narrow_palette, 80, 24, "F3 narrow slash palette", palette_visible=True, cursor_column=cursor_column_before_resize)
    save_evidence(root, "frontend-f3-slash-narrow-80x24", narrow_palette)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "36")
    palette = wait_for_f3_composer_reflow(120, 36, "F3 ordinary slash palette restore", palette_visible=True)
    assert_f3_frame(palette, 120, 36, "F3 ordinary slash palette restore", palette_visible=True, cursor_column=cursor_column_before_resize)
    send_keys(tmux_exe, session, "Escape")
    cancelled_palette = wait_for_f3_composer_reflow(120, 36, "F3 slash palette cancel/focus", palette_visible=False)
    assert_f3_frame(cancelled_palette, 120, 36, "F3 slash palette cancel/focus", palette_visible=False, cursor_column=cursor_column_before_resize)
    if "Esc stop" in cancelled_palette:
        raise RuntimeError(f"F3 Escape retained an idle hint row\nscreen:\n{cancelled_palette}")
    save_evidence(root, "frontend-f3-slash-cancel-focus", cancelled_palette)
    send_keys(tmux_exe, session, "BSpace")
    send_literal(tmux_exe, session, "/")
    palette = wait_for_f3_composer_reflow(120, 36, "F3 ordinary slash palette reopened", palette_visible=True)
    assert_f3_frame(palette, 120, 36, "F3 ordinary slash palette reopened", palette_visible=True, cursor_column=cursor_column_before_resize)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "160", "-y", "36")
    wide_palette = wait_for_f3_composer_reflow(160, 36, "F3 centered wide slash palette", palette_visible=True)
    assert_f3_frame(wide_palette, 160, 36, "F3 centered wide slash palette", palette_visible=True, cursor_column=cursor_column_before_resize)
    wide_lines = wide_palette.splitlines()
    if not wide_lines[34].startswith(" " * 20 + "│  /") or not wide_lines[35].startswith(" " * 20 + "│  "):
        raise RuntimeError(f"wide slash palette did not retain the exact 20-column canvas inset\nscreen:\n{wide_palette}")
    help_target = next(
        ((index + 1, line.index("/help") + 1) for index, line in enumerate(wide_lines) if "/help" in line),
        None,
    )
    if help_target is None:
        raise RuntimeError(f"wide slash palette did not expose a clickable help row\nscreen:\n{wide_palette}")
    help_row, help_column = help_target
    send_literal(tmux_exe, session, f"\x1b[<0;{help_column};{help_row}M")
    clicked_help = wait_for(tmux_exe, session, r"│  /help(?:\s|$)", "derived wide SGR slash palette mouse click")
    if "/help" not in clicked_help:
        raise RuntimeError(f"derived wide SGR mouse click did not select the slash palette row\nscreen:\n{clicked_help}")
    save_evidence(root, "frontend-f3-slash-centered-160x36", wide_palette)

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "176", "-y", "36")
    palette = wait_for_f3_composer_reflow(176, 36, "wide automatic-rail slash palette", palette_visible=True)
    assert_f3_frame(
        palette,
        176,
        36,
        "wide automatic-rail slash palette",
        palette_visible=True,
        cursor_column=cursor_column_before_resize,
    )
    sidebar_click_row = next((index + 1 for index, line in enumerate(palette.splitlines()) if "/help" in line or "Show commands" in line), None)
    if sidebar_click_row is None:
        raise RuntimeError(f"wide automatic-rail palette did not expose a candidate row\nscreen:\n{palette}")
    send_literal(tmux_exe, session, f"\x1b[<0;150;{sidebar_click_row}M")
    rejected_sidebar_click = capture(tmux_exe, session)
    if "│  /" not in rejected_sidebar_click or "│  /help" in rejected_sidebar_click:
        raise RuntimeError(f"automatic-rail sidebar click selected a main-pane slash candidate\nscreen:\n{rejected_sidebar_click}")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "36")
    wait_for_f3_composer_reflow(120, 36, "ordinary slash palette after rail click", palette_visible=True)

    # Seed a genuine ordinary provider turn in an isolated pane and wait for its
    # completion before submitting /images. This avoids accidentally exercising
    # the active-run command path while proving local output is temporary state.
    main_session = session
    provider = ctx.start_fake_provider("slash-command-output", delay_ms=0, scenario="text-three")
    session = ctx.session_name("slash-command-output")
    provider_command = ctx.fake_provider_command(
        provider,
        home=ctx.active_home,
        config=ctx.active_config,
        state=ctx.active_state,
        data=ctx.active_data,
    )
    ctx.launch_ava(session, workspace=ctx.active_workspace, command=provider_command, width=120, height=36)
    wait_for(tmux_exe, session, r"Type a message|live session", "slash command-output provider initial frame")
    ordinary_seed = "LOCAL-COMMAND-TRANSCRIPT-SEED"
    send_literal(tmux_exe, session, ordinary_seed)
    send_keys(tmux_exe, session, "Enter")
    _wait_for_normal_turn_request_count(provider, 1, "slash command-output ordinary provider request")
    wait_for(tmux_exe, session, r"headless active prompt complete", "slash command-output ordinary provider completion")
    wait_for_absent(tmux_exe, session, r"Esc stop|processing", "slash command-output provider turn settled")

    send_literal(tmux_exe, session, "/images")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"│  /images(?:\s|$)", "provider-pane images command selected")
    send_keys(tmux_exe, session, "Enter")
    images_output = wait_for(
        tmux_exe,
        session,
        r"(?s)Command /images.*TUI images:.*usage: /images \[on\|off\|reset\]",
        "images command-output modal",
    )
    if ordinary_seed not in images_output or "│  /images" in images_output:
        raise RuntimeError(f"/images command output lost the provider seed or rendered its invocation as chat\nscreen:\n{images_output}")
    save_evidence(root, "local-command-output-images", images_output)
    send_keys(tmux_exe, session, "Escape")
    images_closed = wait_for_absent(tmux_exe, session, r"TUI images:|Command /images", "images command output closed")
    if ordinary_seed not in images_closed or "│  /images" in images_closed:
        raise RuntimeError(f"/images invocation or output remained after closing the modal\nscreen:\n{images_closed}")

    send_literal(tmux_exe, session, "/help")
    send_keys(tmux_exe, session, "Enter")
    help_output = wait_for(tmux_exe, session, r"(?s)Command /help.*Commands:", "help command-output modal")
    if ordinary_seed not in help_output or "│  /help" in help_output:
        raise RuntimeError(f"/help command output lost the provider seed or rendered its invocation as chat\nscreen:\n{help_output}")
    send_keys(tmux_exe, session, "Escape")
    help_closed = wait_for_absent(tmux_exe, session, r"Command /help|Show commands and ef", "help command output closed")
    if ordinary_seed not in help_closed or "│  /help" in help_closed:
        raise RuntimeError(f"/help invocation or output remained after closing the modal\nscreen:\n{help_closed}")
    send_keys(tmux_exe, session, "C-d")
    wait_for_session_exit(tmux_exe, session)
    session = main_session

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/share")
    disabled_share_palette = wait_for(tmux_exe, session, r"│  /share", "idle disabled slash draft")
    send_keys(tmux_exe, session, "Tab")
    disabled_share_tab = assert_screen_present_for(tmux_exe, session, r"│  /share", "idle disabled slash Tab leaves the draft visible")
    if "│  /share" not in disabled_share_tab:
        raise RuntimeError(f"disabled slash Tab mutated the idle draft\nscreen:\n{disabled_share_tab}")
    send_keys(tmux_exe, session, "Enter")
    disabled_share_enter = capture(tmux_exe, session)
    if "│  /share" not in disabled_share_enter:
        raise RuntimeError(f"disabled slash Enter mutated or submitted the idle draft\nscreen:\n{disabled_share_enter}")
    disabled_share_row = next((index + 1 for index, line in enumerate(disabled_share_palette.splitlines()) if "/share" in line), None)
    if disabled_share_row is None:
        raise RuntimeError(f"idle disabled slash palette did not expose /share\nscreen:\n{disabled_share_palette}")
    send_literal(tmux_exe, session, f"\x1b[<0;4;{disabled_share_row}M")
    disabled_share_mouse = capture(tmux_exe, session)
    if "│  /share" not in disabled_share_mouse:
        raise RuntimeError(f"disabled slash mouse click mutated or submitted the idle draft\nscreen:\n{disabled_share_mouse}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/per")
    permissions_palette = wait_for(
        tmux_exe, session, r"/permissions|permission rules", "permission rule command palette"
    )
    if "/permissions" not in permissions_palette and "permission rules" not in permissions_palette:
        raise RuntimeError(f"permission command did not appear in the slash palette\nscreen:\n{permissions_palette}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/permissions list")
    send_keys(tmux_exe, session, "Enter")
    permissions_list = wait_for(tmux_exe, session, r"Permission rules:|No persistent permission rules", "permission rules command output")
    if "Permission rules:" not in permissions_list and "No persistent permission rules" not in permissions_list:
        raise RuntimeError(f"permission rules command did not render command output\nscreen:\n{permissions_list}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/rd")
    fuzzy_slash = wait_for(tmux_exe, session, r"/read", "fuzzy slash command palette")
    if "/read" not in fuzzy_slash:
        raise RuntimeError(f"fuzzy slash command palette did not show /read\nscreen:\n{fuzzy_slash}")
    send_keys(tmux_exe, session, "Tab")
    selected_fuzzy_slash = wait_for(
        tmux_exe,
        session,
        r"(?s)(?:\.ava/|src/).*│  /read(?:\s|$)",
        "fuzzy slash command selection",
    )
    selected_fuzzy_input = next((line for line in selected_fuzzy_slash.splitlines() if line.startswith("│  /read")), "")
    if not selected_fuzzy_input.startswith("│  /read") or "/rd" in selected_fuzzy_input:
        raise RuntimeError(f"fuzzy slash command selection did not update the draft\nscreen:\n{selected_fuzzy_slash}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/models 4sonnet")
    fuzzy_model_arg = wait_for(
        tmux_exe, session, r"anthropic/claude-sonnet-4-5|Claude Sonnet", "fuzzy model argument completion"
    )
    if "claude-sonnet-4-5" not in fuzzy_model_arg and "Claude Sonnet" not in fuzzy_model_arg:
        raise RuntimeError(
            f"fuzzy model argument completion did not show the Sonnet model\nscreen:\n{fuzzy_model_arg}"
        )
    send_keys(tmux_exe, session, "Tab")
    selected_fuzzy_model_arg = wait_for(
        tmux_exe,
        session,
        r"/models (?:anthropic/)?claude-sonnet-4-5",
        "fuzzy model argument completion selection",
    )
    if "/models claude-sonnet-4-5" not in selected_fuzzy_model_arg and "/models anthropic/claude-sonnet-4-5" not in selected_fuzzy_model_arg:
        raise RuntimeError(
            "fuzzy model argument completion did not update the draft\n"
            f"screen:\n{selected_fuzzy_model_arg}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/models open")
    wait_for(tmux_exe, session, r"openai/gpt-5\.5|GPT-5\.5", "slash argument completion before cursor movement")
    send_keys(tmux_exe, session, "Left", "Left", "Left", "Left", "Left")
    cursor_scoped_slash_palette = wait_for(
        tmux_exe,
        session,
        r"/models.*Open the model selector|Open the model selector.*models",
        "cursor-scoped slash command palette",
    )
    if "openai/gpt-5.5" in cursor_scoped_slash_palette:
        raise RuntimeError(
            "slash argument completion stayed visible after cursor moved back into the command name\n"
            f"screen:\n{cursor_scoped_slash_palette}"
        )

    send_keys(tmux_exe, session, "C-c")
    send_literal(tmux_exe, session, "/read sr")
    path_palette = wait_for(tmux_exe, session, r"src/main\.cpp|src/", "slash path completion palette")
    if "src/main.cpp" not in path_palette and "src/" not in path_palette:
        raise RuntimeError(f"slash path completion did not show workspace paths\nscreen:\n{path_palette}")
    if "[Files]" in path_palette or "directory" in path_palette or re.search(r"file [0-9]+ bytes", path_palette):
        raise RuntimeError(f"slash path completion retained duplicated file metadata\nscreen:\n{path_palette}")
    save_evidence(root, "frontend-f3-slash-path-quiet", path_palette)

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "review @sr")
    wait_for(tmux_exe, session, r"review @sr", "file reference completion draft")
    reference_palette = wait_for(tmux_exe, session, r"@src/main\.cpp|@src/", "file reference completion palette")
    if "@src/main.cpp" not in reference_palette and "@src/" not in reference_palette:
        raise RuntimeError(f"file reference completion did not show workspace paths\nscreen:\n{reference_palette}")
    if "[Files]" in reference_palette or "directory" in reference_palette or re.search(r"file [0-9]+ bytes", reference_palette):
        raise RuntimeError(f"file reference completion retained duplicated metadata\nscreen:\n{reference_palette}")
    save_evidence(root, "frontend-f3-file-reference-quiet", reference_palette)
    reference_row = next(
        (
            (index + 1, line)
            for index, line in enumerate(reference_palette.splitlines())
            if line.startswith("│") and "@src/main.cpp" in line and not line.startswith("│  review ")
        ),
        None,
    )
    if reference_row is None:
        raise RuntimeError(f"file reference completion did not expose a clickable file row\nscreen:\n{reference_palette}")
    reference_row_number, reference_row_text = reference_row
    reference_column = max(1, len(reference_row_text) - len(reference_row_text.lstrip()) + 4)
    send_literal(tmux_exe, session, f"\x1b[<0;{reference_column};{reference_row_number}M")
    clicked_reference = wait_for(
        tmux_exe, session, r"review @src/main\.cpp", "file reference completion mouse selection"
    )
    if "review @src/main.cpp" not in clicked_reference:
        raise RuntimeError(f"file reference mouse click did not update the draft\nscreen:\n{clicked_reference}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "include=@sr")
    wait_for(tmux_exe, session, r"include=@sr", "equals-delimited file reference draft")
    equals_reference_palette = wait_for(
        tmux_exe, session, r"@src/main\.cpp|@src/", "equals-delimited file reference completion palette"
    )
    if "@src/main.cpp" not in equals_reference_palette and "@src/" not in equals_reference_palette:
        raise RuntimeError(
            f"equals-delimited file reference completion did not show workspace paths\nscreen:\n{equals_reference_palette}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "include='@sr")
    wait_for(tmux_exe, session, r"include='@sr", "single-quote-delimited file reference draft")
    single_quote_reference_palette = wait_for(
        tmux_exe, session, r"@src/main\.cpp|@src/", "single-quote-delimited file reference completion palette"
    )
    if "@src/main.cpp" not in single_quote_reference_palette and "@src/" not in single_quote_reference_palette:
        raise RuntimeError(
            "single-quote-delimited file reference completion did not show workspace paths\n"
            f"screen:\n{single_quote_reference_palette}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "review @my")
    wait_for(tmux_exe, session, r"review @my", "quoted file reference draft")
    spaced_reference_palette = wait_for(
        tmux_exe, session, r'@"my folder/"', "quoted file reference completion palette"
    )
    if '@"my folder/"' not in spaced_reference_palette:
        raise RuntimeError(
            f"quoted file reference completion did not show a path with spaces\nscreen:\n{spaced_reference_palette}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "inspect src/")
    wait_for(tmux_exe, session, r"inspect src/", "normal prompt path completion draft")
    prompt_path_palette = wait_for(tmux_exe, session, r"src/main\.cpp", "normal prompt path completion palette")
    if "src/main.cpp" not in prompt_path_palette:
        raise RuntimeError(f"normal prompt path completion did not show workspace paths\nscreen:\n{prompt_path_palette}")
    if "[Files]" in prompt_path_palette or "directory" in prompt_path_palette or re.search(r"file [0-9]+ bytes", prompt_path_palette):
        raise RuntimeError(f"normal path completion retained duplicated metadata\nscreen:\n{prompt_path_palette}")
    save_evidence(root, "frontend-f3-natural-path-quiet", prompt_path_palette)
    # Prefer live palette chrome rows so earlier transcript receipts that mention the same path
    # (for example permission summaries) cannot steal the mouse hit target.
    path_row = next(
        (
            (index + 1, line)
            for index, line in enumerate(prompt_path_palette.splitlines())
            if line.startswith("│") and "src/main.cpp" in line and not line.startswith("│  inspect ")
        ),
        None,
    )
    if path_row is None:
        raise RuntimeError(f"normal prompt path completion did not expose a clickable file row\nscreen:\n{prompt_path_palette}")
    path_row_number, path_row_text = path_row
    path_column = max(1, len(path_row_text) - len(path_row_text.lstrip()) + 4)
    send_literal(tmux_exe, session, f"\x1b[<0;{path_column};{path_row_number}M")
    clicked_path = wait_for(tmux_exe, session, r"inspect src/main\.cpp", "normal path completion mouse selection")
    if "inspect src/main.cpp" not in clicked_path:
        raise RuntimeError(f"normal prompt path mouse click did not update the draft\nscreen:\n{clicked_path}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "ordinary")
    wait_for(tmux_exe, session, r"ordinary", "ordinary prompt word before trailing Space")
    cursor_before_space = pane_cursor_position(tmux_exe, session)
    send_keys(tmux_exe, session, "Space")
    cursor_after_space = wait_for_cursor_change(
        tmux_exe, session, cursor_before_space, "ordinary prompt cursor after trailing Space"
    )
    before_column, before_row = (int(value) for value in cursor_before_space.split(",", 1))
    after_column, after_row = (int(value) for value in cursor_after_space.split(",", 1))
    if after_row != before_row or after_column != before_column + 1:
        raise RuntimeError(
            "ordinary trailing Space did not move the cursor exactly one cell to the right "
            f"on the same row: before={cursor_before_space}, after={cursor_after_space}"
        )
    screen = assert_screen_absent_for(
        tmux_exe,
        session,
        r"@?src/main\.cpp|@?src/",
        "ordinary trailing Space opened a workspace file/path completion palette",
    )
    styled_screen = capture_styled(tmux_exe, session)
    if "ordinary" not in styled_screen:
        raise RuntimeError(
            "styled capture did not preserve the visible ordinary draft after trailing Space; "
            f"cursor before={cursor_before_space}, cursor after={cursor_after_space}\nscreen:\n{styled_screen}"
        )
    save_evidence(root, "composer-ordinary-space-no-completion", screen)

    send_keys(tmux_exe, session, "Tab")
    forced_whitespace_path_palette = wait_for(
        tmux_exe, session, r"src/main\.cpp|src/", "forced empty-token path completion after whitespace"
    )
    if "src/main.cpp" not in forced_whitespace_path_palette and "src/" not in forced_whitespace_path_palette:
        raise RuntimeError(
            "forced empty-token path completion after whitespace did not show workspace paths\n"
            f"screen:\n{forced_whitespace_path_palette}"
        )
    send_keys(tmux_exe, session, "C-c")
    wait_for_absent(tmux_exe, session, r"ordinary", "ordinary prompt cleared after forced completion")

    send_literal(tmux_exe, session, "inspect file=src/")
    equals_path_palette = wait_for(
        tmux_exe, session, r"src/main\.cpp", "equals-delimited normal prompt path completion palette"
    )
    if "src/main.cpp" not in equals_path_palette:
        raise RuntimeError(
            f"equals-delimited normal prompt path completion did not show workspace paths\nscreen:\n{equals_path_palette}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "inspect path='src/")
    single_quote_path_palette = wait_for(
        tmux_exe, session, r"src/main\.cpp", "single-quote-delimited normal prompt path completion palette"
    )
    if "src/main.cpp" not in single_quote_path_palette:
        raise RuntimeError(
            "single-quote-delimited normal prompt path completion did not show workspace paths\n"
            f"screen:\n{single_quote_path_palette}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "main")
    send_keys(tmux_exe, session, "Tab")
    forced_path_completion = wait_for(tmux_exe, session, r"src/main\.cpp", "forced bare-token path completion")
    if "src/main.cpp" not in forced_path_completion:
        raise RuntimeError(f"forced path completion did not insert workspace path\nscreen:\n{forced_path_completion}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/read main")
    wait_for(tmux_exe, session, r"/read main", "slash-command argument path fallback draft")
    send_keys(tmux_exe, session, "Tab")
    forced_slash_argument_path = wait_for(
        tmux_exe, session, r"/read src/main\.cpp", "forced slash-command argument path fallback"
    )
    if "/read src/main.cpp" not in forced_slash_argument_path:
        raise RuntimeError(
            f"forced slash-command argument path fallback did not update the draft\nscreen:\n{forced_slash_argument_path}"
        )

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/find src/*.cpp")
    wait_for(tmux_exe, session, r"/find src/\*\.cpp", "find alias command draft")
    send_keys(tmux_exe, session, "Enter")
    find_alias = wait_for(tmux_exe, session, r"(?s)find.*src/main\.cpp", "find alias command result")
    if "find" not in find_alias or "src/main.cpp" not in find_alias:
        raise RuntimeError(f"/find alias did not render glob output\nscreen:\n{find_alias}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/ls src")
    wait_for(tmux_exe, session, r"/ls src", "ls alias command draft")
    ls_palette_before_dismissal = capture(tmux_exe, session)
    send_keys(tmux_exe, session, "Escape")
    wait_for_screen_change(tmux_exe, session, ls_palette_before_dismissal, "ls palette dismissal")
    send_keys(tmux_exe, session, "Enter")
    ls_alias = wait_for(tmux_exe, session, r"(?s)ls.*main\.cpp", "ls alias command result")
    if "ls" not in ls_alias or "main.cpp" not in ls_alias:
        raise RuntimeError(f"/ls alias did not render directory output\nscreen:\n{ls_alias}")

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "!pwd")
    wait_for(tmux_exe, session, r"│  !pwd(?:\s|$)", "bang shell command draft")
    send_keys(tmux_exe, session, "Enter")
    bang_permission = wait_for(
        tmux_exe,
        session,
        r"(?s)! Permission required.*Shell command.*\$ pwd.*risk critical.*› Reject.*Allow once",
        "bang shell critical permission",
    )
    bang_dimensions = tmux(
        tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}"
    ).stdout.strip()
    bang_lines = bang_permission.splitlines()
    if (
        bang_dimensions != "120,36"
        or len(bang_lines) != 36
        or any(len(line) > 120 for line in bang_lines)
        or "reason " not in bang_permission
        or "permreq_" in bang_permission
        or "[" in bang_permission
        or "]" in bang_permission
        or "---" in bang_permission
        or "Always allow" in bang_permission
        or ("Always reject" not in bang_permission and "Never" not in bang_permission)
        or "\x1b" in bang_permission
        or any(ord(character) < 32 and character != "\n" for character in bang_permission)
    ):
        raise RuntimeError(
            f"raw ! shell helper did not render a clean one-shot shell permission dock at 120x36\nscreen:\n{bang_permission}"
        )
    send_keys(tmux_exe, session, "A", "Enter")
    bang_shell = wait_for(
        tmux_exe,
        session,
        r"(?s)Command !.*exit: 0",
        "allowed bang shell helper command output",
        timeout=30.0,
    )
    if "Permission required" in bang_shell or "PERMISSION REQUIRED" in bang_shell or "!pwd" in bang_shell:
        raise RuntimeError(f"allowed ! shell helper leaked its invocation or left its permission prompt open\nscreen:\n{bang_shell}")

    # After path/reference proofs, seed one durable rule and prove explain completions lead with the
    # human summary while selection still drafts the exact permrule id (no raw id / [complete] chrome).
    send_keys(tmux_exe, session, "C-u")
    send_literal(
        tmux_exe,
        session,
        '/permissions add action=allow operation=read path=src/main.cpp reason="tmux completion fixture"',
    )
    send_keys(tmux_exe, session, "Enter")
    permission_add = wait_for(
        tmux_exe,
        session,
        r"(?s)added permission rule.*Allow file reads · src/main\.cpp · Workspace.*Rule ID: permrule_",
        "permission rule add receipt for completion fixture",
    )
    rule_id_match = re.search(r"Rule ID: (permrule_\S+)", permission_add)
    if rule_id_match is None:
        raise RuntimeError(f"permission add receipt did not expose a Rule ID\nscreen:\n{permission_add}")
    permission_rule_id = rule_id_match.group(1)

    send_keys(tmux_exe, session, "C-u")
    send_literal(tmux_exe, session, "/permissions explain ")
    permission_rule_palette = wait_for(
        tmux_exe,
        session,
        r"Allow file reads · src/main\.cpp · Workspace",
        "permission explain human completion row",
    )
    # Bound checks to the live draft/completion chrome so earlier Rule ID receipts do not false-fail.
    completion_region_lines = []
    for line in permission_rule_palette.splitlines():
        if line.startswith("│") or "Allow file reads · src/main.cpp · Workspace" in line or "[complete]" in line:
            completion_region_lines.append(line)
    completion_region = "\n".join(completion_region_lines)
    if (
        "Allow file reads · src/main.cpp · Workspace" not in completion_region
        or "permrule_" in completion_region
        or "[complete]" in completion_region
    ):
        raise RuntimeError(
            "permission explain completion did not render a human summary without raw rule id or [complete]\n"
            f"completion region:\n{completion_region}\nscreen:\n{permission_rule_palette}"
        )
    send_keys(tmux_exe, session, "Tab")
    selected_permission_rule = wait_for(
        tmux_exe,
        session,
        rf"│  /permissions explain {re.escape(permission_rule_id)}",
        "permission explain completion selection drafts exact rule id",
    )
    selected_permission_input = next(
        (line for line in selected_permission_rule.splitlines() if line.startswith("│  /permissions explain ")),
        "",
    )
    if permission_rule_id not in selected_permission_input:
        raise RuntimeError(
            "permission explain completion selection did not draft the exact rule id\n"
            f"screen:\n{selected_permission_rule}"
        )
    send_keys(tmux_exe, session, "C-u")

    _finish_main(tmux_exe, session)
