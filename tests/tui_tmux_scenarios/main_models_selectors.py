"""The tmux TUI smoke scenario for main models selectors."""

from __future__ import annotations

import json
import re

from tui_smoke_helpers import (
    SmokeContext,
    capture,
    save_evidence,
    selected_modal_identity,
    selected_modal_row,
    send_keys,
    send_literal,
    tmux,
    wait_for,
    wait_for_absent,
    wait_for_json_file,
    wait_for_screen_change,
    wait_for_screen_state,
    wait_for_selected_modal_change,
    wait_for_session_exit,
)
from .common import _finish_main, _main_session, _wait_for_normal_turn_request_count


def _wait_for_selected_modal(
    tmux_exe: object,
    session: str,
    required_pattern: str,
    label: str,
    *,
    selected_row_pattern: str | None = None,
    geometry: tuple[int, int] | None = None,
) -> str:
    """Wait until the required modal text and its selected row share a frame."""

    def modal_is_ready(screen: str) -> bool:
        if re.search(required_pattern, screen) is None:
            return False
        selected_row = selected_modal_row(screen)
        if not selected_row:
            return False
        if (
            selected_row_pattern is not None
            and re.search(selected_row_pattern, selected_modal_identity(selected_row)) is None
        ):
            return False
        if geometry is None:
            return True
        width, height = geometry
        dimensions = tmux(
            tmux_exe,
            "display-message",
            "-p",
            "-t",
            session,
            "#{window_width},#{window_height},#{pane_width},#{pane_height}",
        ).stdout.strip()
        if dimensions != f"{width},{height},{width},{height}":
            return False
        lines = screen.split("\n")
        return len(lines) == height and all(len(line) <= width for line in lines)

    expectation = f"{label} with a selected modal row"
    if selected_row_pattern is not None:
        expectation += f" matching /{selected_row_pattern}/"
    if geometry is not None:
        expectation += f" at {geometry[0]}x{geometry[1]} window/pane geometry"
    return wait_for_screen_state(tmux_exe, session, modal_is_ready, expectation)


def scenario_main_models_selectors(ctx: SmokeContext) -> None:
    tmux_exe, root, workspace, ava_config, env_prefix, session = _main_session(ctx)
    scoped_persist_session = ctx.session_name("scoped-persist")
    send_keys(tmux_exe, session, "C-p")
    model_cycle = wait_for(
        tmux_exe,
        session,
        r"model cycled|GPT-5\.6 Sol|gpt-5\.6-sol|GPT-4\.1 mini|gpt-4\.1-mini",
        "ctrl-p model cycle",
    )
    if not any(value in model_cycle for value in ("model cycled", "GPT-5.6 Sol", "gpt-5.6-sol", "GPT-4.1 mini", "gpt-4.1-mini")):
        raise RuntimeError(f"Ctrl+P did not cycle the visible model state\nscreen:\n{model_cycle}")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "82", "-y", "10")
    wait_for(tmux_exe, session, r"Type a message|live session", "compact frame before selector navigation")
    send_literal(tmux_exe, session, "/models")
    wait_for(tmux_exe, session, r"/models", "exact models selector draft")
    send_keys(tmux_exe, session, "Enter")
    command_model_selector = _wait_for_selected_modal(
        tmux_exe, session, r"Select model|Search models", "exact models selector", geometry=(82, 10)
    )
    if "Select model" not in command_model_selector and "Search models" not in command_model_selector:
        raise RuntimeError(f"Exact /models did not bypass autocomplete and open the model selector\nscreen:\n{command_model_selector}")
    selected_row = selected_modal_row(command_model_selector)
    if not selected_row:
        raise RuntimeError(f"Model selector did not expose a selected row before navigation\nscreen:\n{command_model_selector}")
    initial_selected_identity = selected_modal_identity(selected_row)
    selected_rows = {initial_selected_identity}
    initial_model_selector = command_model_selector
    wheel_down = "\x1b[<65;4;6M"
    send_literal(tmux_exe, session, wheel_down * 12 + "/")
    wheel_burst_selector = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"filter\s+/",
        "model selector wheel burst queue synchronization",
        selected_row_pattern=rf"^(?!{re.escape(initial_selected_identity)}$)",
        geometry=(82, 10),
    )
    wheel_burst_row = selected_modal_row(wheel_burst_selector)
    if not wheel_burst_row or selected_modal_identity(wheel_burst_row) == initial_selected_identity:
        raise RuntimeError(
            "raw same-direction wheel burst did not advance the model selector\n"
            f"before:\n{command_model_selector}\nafter:\n{wheel_burst_selector}"
        )
    send_keys(tmux_exe, session, "BSpace")
    wheel_burst_cleared = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"filter\s+Search models",
        "model selector wheel burst filter cleared",
        selected_row_pattern=re.escape(selected_modal_identity(wheel_burst_row)),
        geometry=(82, 10),
    )
    cleared_burst_row = selected_modal_row(wheel_burst_cleared)
    if not cleared_burst_row or selected_modal_identity(cleared_burst_row) != selected_modal_identity(wheel_burst_row):
        raise RuntimeError(
            "clearing the model-selector synchronization query changed the wheel selection\n"
            f"before:\n{wheel_burst_selector}\nafter:\n{wheel_burst_cleared}"
        )
    send_keys(tmux_exe, session, "Up")
    selected_row, wheel_burst_restored = wait_for_selected_modal_change(
        tmux_exe, session, cleared_burst_row, "model selector wheel burst inverse step"
    )
    if selected_modal_identity(selected_row) != initial_selected_identity:
        raise RuntimeError(
            "a raw burst of twelve same-direction wheel events advanced the model selector by more than exactly one option\n"
            f"initial:\n{command_model_selector}\nburst:\n{wheel_burst_selector}\nrestored:\n{wheel_burst_restored}"
        )
    save_evidence(root, "model-selector-wheel-burst-governed", wheel_burst_selector)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "82", "-y", "11")
    resized_model_selector = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"Select model|Search models",
        "resized compact model selector",
        selected_row_pattern=re.escape(selected_modal_identity(selected_row)),
        geometry=(82, 11),
    )
    resized_selected_row = selected_modal_row(resized_model_selector)
    if not resized_selected_row or selected_modal_identity(resized_selected_row) != selected_modal_identity(selected_row):
        raise RuntimeError(
            "Model selector lost its selected row while resizing between compact terminal heights\n"
            f"before:\n{command_model_selector}\nafter:\n{resized_model_selector}"
        )
    selected_row = resized_selected_row
    ghostty_event_origin = selected_modal_identity(selected_row)
    send_literal(tmux_exe, session, "\x1b[1;1:1B")
    ghostty_press_row, ghostty_press_screen = wait_for_selected_modal_change(
        tmux_exe, session, selected_row, "Ghostty Kitty event-form Down press"
    )
    ghostty_press_identity = selected_modal_identity(ghostty_press_row)
    send_literal(tmux_exe, session, "\x1b[1;1:1A")
    ghostty_up_row, ghostty_up_screen = wait_for_selected_modal_change(
        tmux_exe, session, ghostty_press_row, "Ghostty Kitty event-form Up press"
    )
    if selected_modal_identity(ghostty_up_row) != ghostty_event_origin:
        raise RuntimeError(
            "raw Ghostty Kitty event-form Down/Up did not restore the original model selection\n"
            f"origin:\n{resized_model_selector}\ndown:\n{ghostty_press_screen}\nup:\n{ghostty_up_screen}"
        )
    send_literal(tmux_exe, session, "\x1b[1;1:2B")
    ghostty_repeat_row, ghostty_repeat_screen = wait_for_selected_modal_change(
        tmux_exe, session, ghostty_up_row, "Ghostty Kitty event-form Down repeat"
    )
    if selected_modal_identity(ghostty_repeat_row) != ghostty_press_identity:
        raise RuntimeError(
            "raw Ghostty Kitty event-form repeat did not decode as the underlying Down key\n"
            f"press:\n{ghostty_press_screen}\nrepeat:\n{ghostty_repeat_screen}"
        )
    send_literal(tmux_exe, session, "\x1b[1;1:3B/")
    wait_for(tmux_exe, session, r"filter\s+/", "Ghostty Kitty release queue synchronization")
    send_keys(tmux_exe, session, "BSpace")
    ghostty_release_screen = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"filter\s+Search models",
        "Ghostty Kitty release synchronization cleared",
        selected_row_pattern=re.escape(ghostty_press_identity),
        geometry=(82, 11),
    )
    ghostty_release_row = selected_modal_row(ghostty_release_screen)
    if not ghostty_release_row or selected_modal_identity(ghostty_release_row) != ghostty_press_identity:
        raise RuntimeError(
            "raw Ghostty Kitty event-form release caused duplicate model navigation\n"
            f"before release:\n{ghostty_repeat_screen}\nafter release:\n{ghostty_release_screen}"
        )
    selected_row = ghostty_release_row
    selected_rows.add(selected_modal_identity(selected_row))
    save_evidence(root, "model-selector-ghostty-event-arrows", ghostty_release_screen)
    send_keys(tmux_exe, session, "Down")
    selected_row, _ = wait_for_selected_modal_change(
        tmux_exe, session, selected_row, "tmux Down arrow model navigation"
    )
    selected_rows.add(selected_modal_identity(selected_row))
    send_literal(tmux_exe, session, "\x1b[1;129B")
    selected_row, _ = wait_for_selected_modal_change(
        tmux_exe, session, selected_row, "physical Ghostty CSI arrow model navigation with Num Lock"
    )
    selected_rows.add(selected_modal_identity(selected_row))
    for step in range(7):
        send_keys(tmux_exe, session, "Down")
        selected_row, _ = wait_for_selected_modal_change(
            tmux_exe, session, selected_row, f"model selector navigation step {step + 3}"
        )
        selected_rows.add(selected_modal_identity(selected_row))
    if len(selected_rows) < 9:
        raise RuntimeError(
            "Arrow navigation did not visit nine distinct model rows\n"
            f"visited: {sorted(selected_rows)}\nscreen:\n{capture(tmux_exe, session)}"
        )
    if not any(identity and identity not in initial_model_selector for identity in selected_rows):
        raise RuntimeError(
            "Model selector selection never advanced beyond the initial compact viewport\n"
            f"visited: {sorted(selected_rows)}\ninitial screen:\n{initial_model_selector}\n"
            f"final screen:\n{capture(tmux_exe, session)}"
        )
    save_evidence(root, "model-selector-arrow-scroll", capture(tmux_exe, session))
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Select model|Search models", "exact models selector canceled")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "82", "-y", "10")
    wait_for(tmux_exe, session, r"Type a message|live session", "compact frame before provider modal navigation")
    send_literal(tmux_exe, session, "/connect")
    wait_for(tmux_exe, session, r"/connect", "provider modal command draft")
    send_keys(tmux_exe, session, "Enter")
    provider_modal = _wait_for_selected_modal(
        tmux_exe, session, r"Connect a provider|Select provider", "provider question modal", geometry=(82, 10)
    )
    provider_selected_row = selected_modal_row(provider_modal)
    if not provider_selected_row:
        raise RuntimeError(f"Provider question modal did not expose a selected row\nscreen:\n{provider_modal}")
    send_literal(tmux_exe, session, "\x1b[1;129B")
    _, provider_modal_after_arrow = wait_for_selected_modal_change(
        tmux_exe, session, provider_selected_row, "provider modal physical Ghostty arrow navigation with Num Lock"
    )
    save_evidence(root, "provider-modal-arrow-navigation", provider_modal_after_arrow)
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Connect a provider|Select provider", "provider question modal canceled")
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "32")
    wait_for(tmux_exe, session, r"Type a message|live session", "restored frame after compact modal navigation")
    send_keys(tmux_exe, session, "C-l")
    model_selector = wait_for(tmux_exe, session, r"Select model|Search models", "ctrl-l model selector")
    if "Select model" not in model_selector and "Search models" not in model_selector:
        raise RuntimeError(f"Ctrl+L did not open the model selector\nscreen:\n{model_selector}")
    send_literal(tmux_exe, session, "5.5")
    configurable_model = wait_for(
        tmux_exe, session, r"(?s)filter\s+5\.5█.*›\s+GPT-5\.5", "configurable model selector row"
    )
    send_keys(tmux_exe, session, "Enter")
    chained_thinking = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"(?s)Select thinking mode.*Esc keep default",
        "staged thinking-mode selector",
        selected_row_pattern=r"Default",
        geometry=(120, 32),
    )
    chained_row = selected_modal_row(chained_thinking)
    if not chained_row or "Default" not in chained_row or "Esc keep default" not in chained_thinking:
        raise RuntimeError(
            "Selecting a reasoning-capable model did not stage a Default thinking-mode selector\n"
            f"model selector:\n{configurable_model}\nthinking selector:\n{chained_thinking}"
        )
    if any(secret in chained_thinking for secret in ("budget_tokens", "provider_level", "reasoning.effort")):
        raise RuntimeError(f"Thinking-mode selector exposed provider controls\nscreen:\n{chained_thinking}")
    send_keys(tmux_exe, session, "Escape")
    escaped_thinking = wait_for_absent(tmux_exe, session, r"Select thinking mode", "staged thinking selector escaped")
    if "GPT-5.5" not in escaped_thinking:
        raise RuntimeError(f"Esc from staged thinking selector rolled back the selected model\nscreen:\n{escaped_thinking}")
    send_keys(tmux_exe, session, "C-t")
    direct_thinking = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"(?s)Select thinking mode.*Esc cancel",
        "direct thinking-mode selector",
        selected_row_pattern=r"Default",
        geometry=(120, 32),
    )
    direct_row = selected_modal_row(direct_thinking)
    if not direct_row or "Default" not in direct_row or "Esc cancel" not in direct_thinking:
        raise RuntimeError(f"Ctrl+T did not reopen the same selector with Default current\nscreen:\n{direct_thinking}")
    send_keys(tmux_exe, session, "Down")
    low_row, low_selection = wait_for_selected_modal_change(
        tmux_exe, session, direct_row, "thinking-mode concrete selection"
    )
    if "Low" not in low_row:
        raise RuntimeError(f"First configurable thinking mode was not the concise Low row\nscreen:\n{low_selection}")
    send_keys(tmux_exe, session, "Enter")
    wait_for_absent(tmux_exe, session, r"Select thinking mode", "thinking-mode Low selection closed")
    send_keys(tmux_exe, session, "C-t")
    reopened_low = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"Select thinking mode",
        "thinking-mode explicit current reopen",
        selected_row_pattern=r"Low",
        geometry=(120, 32),
    )
    reopened_low_row = selected_modal_row(reopened_low)
    if not reopened_low_row or "Low" not in reopened_low_row:
        raise RuntimeError(f"Concrete thinking-mode selection was not authoritative on reopen\nscreen:\n{reopened_low}")
    send_keys(tmux_exe, session, "Up", "Enter")
    wait_for_absent(tmux_exe, session, r"Select thinking mode", "thinking-mode Default selection closed")
    send_keys(tmux_exe, session, "C-t")
    reopened_default = _wait_for_selected_modal(
        tmux_exe,
        session,
        r"Select thinking mode",
        "thinking-mode Default current reopen",
        selected_row_pattern=r"Default",
        geometry=(120, 32),
    )
    reopened_default_row = selected_modal_row(reopened_default)
    if not reopened_default_row or "Default" not in reopened_default_row:
        raise RuntimeError(f"Default thinking-mode selection was not authoritative on reopen\nscreen:\n{reopened_default}")
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Select thinking mode", "Default thinking-mode verification closed")

    send_keys(tmux_exe, session, "C-l")
    wait_for(tmux_exe, session, r"Select model|Search models", "model selector before unsupported model")
    send_literal(tmux_exe, session, "Diagnostic")
    diagnostic_model_selector = wait_for(
        tmux_exe, session, r"(?s)filter\s+Diagnostic█.*›\s+Diagnostic Local", "quiet filtered model selector"
    )
    if (
        "Diagnostic Local" not in diagnostic_model_selector
        or "diagnostics" in diagnostic_model_selector
        or "reasoning" in diagnostic_model_selector
        or "tools yes" in diagnostic_model_selector
        or "openai/diagnostic-local" in diagnostic_model_selector
    ):
        raise RuntimeError(
            f"Model selector did not keep the custom model row quiet and human-readable\nscreen:\n{diagnostic_model_selector}"
        )
    save_evidence(root, "model-selector-quiet-filtered", diagnostic_model_selector)
    model_before_short_resize = capture(tmux_exe, session)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "100", "-y", "12")
    wait_for_screen_change(tmux_exe, session, model_before_short_resize, "100x12 model selector resize")
    diagnostic_model_short = wait_for(
        tmux_exe, session, r"(?s)filter\s+Diagnostic█.*›\s+Diagnostic Local", "100x12 quiet model selector"
    )
    if tmux(tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}").stdout.strip() != "100,12":
        raise RuntimeError("short model selector did not settle at 100x12")
    if "diagnostics" in diagnostic_model_short or "openai/diagnostic-local" in diagnostic_model_short:
        raise RuntimeError(f"100x12 model selector exposed backend metadata\nscreen:\n{diagnostic_model_short}")
    save_evidence(root, "model-selector-quiet-100x12", diagnostic_model_short)
    model_before_restore = capture(tmux_exe, session)
    tmux(tmux_exe, "resize-window", "-t", session, "-x", "120", "-y", "32")
    wait_for_screen_state(
        tmux_exe,
        session,
        lambda screen: screen != model_before_restore
        and tmux(
            tmux_exe, "display-message", "-p", "-t", session, "#{window_width},#{window_height}"
        ).stdout.strip()
        == "120,32"
        and len(screen.splitlines()) == 32
        and any(line.strip() for line in screen.splitlines()[12:])
        and "Diagnostic Local" in screen
        and "filter" in screen,
        "restored and settled 120x32 quiet model selector",
    )
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Select model|Search models", "diagnostic model selector canceled")
    send_keys(tmux_exe, session, "C-l")
    wait_for(tmux_exe, session, r"Select model|Search models", "model selector before non-reasoning model")
    send_literal(tmux_exe, session, "4.1 mini")
    wait_for(
        tmux_exe,
        session,
        r"(?s)filter\s+4\.1 mini█.*›\s+GPT-4\.1 mini",
        "non-reasoning model selector row",
    )
    send_keys(tmux_exe, session, "Enter")
    wait_for_absent(tmux_exe, session, r"Select model|Search models", "non-configurable model selection")
    unsupported_model_selected = wait_for(tmux_exe, session, r"GPT-4\.1 mini", "non-configurable model applied snapshot")
    if "Select thinking mode" in unsupported_model_selected:
        raise RuntimeError(
            "Model without configurable policy-resolved levels opened a thinking selector\n"
            f"screen:\n{unsupported_model_selected}"
        )
    send_keys(tmux_exe, session, "C-t")
    unavailable_thinking = wait_for(
        tmux_exe,
        session,
        r"thinking mode unavailable for current model",
        "non-configurable thinking-mode direct status",
    )
    if "Select thinking mode" in unavailable_thinking:
        raise RuntimeError(f"Ctrl+T opened an empty thinking selector\nscreen:\n{unavailable_thinking}")
    send_literal(tmux_exe, session, "/scoped-models")
    wait_for(tmux_exe, session, r"/scoped-models", "scoped model selector draft")
    send_keys(tmux_exe, session, "Enter")
    scoped_model_selector = wait_for(
        tmux_exe, session, r"Scoped model cycle|Search models", "scoped model selector"
    )
    if "Scoped model cycle" not in scoped_model_selector:
        raise RuntimeError(f"/scoped-models did not open the scoped cycle selector\nscreen:\n{scoped_model_selector}")
    send_literal(tmux_exe, session, "Diagnostic")
    wait_for(tmux_exe, session, r"Diagnostic Local", "scoped model reorder filtered row")
    send_keys(tmux_exe, session, "M-Up")
    send_keys(tmux_exe, session, *("BSpace" for _ in "Diagnostic"))
    wait_for(tmux_exe, session, r"filter\s+Search models", "scoped model reorder filter clear acknowledgement")
    send_keys(tmux_exe, session, "C-s")
    saved_reordered_models = wait_for_json_file(
        ava_config / "models.json",
        lambda value: isinstance(value, dict)
        and isinstance(value.get("scoped_model_cycle"), list)
        and len(value["scoped_model_cycle"]) >= 2
        and "openai/diagnostic-local" in value["scoped_model_cycle"],
        "persisted reordered scoped model cycle",
    )
    saved_reordered_cycle = json.loads(saved_reordered_models).get("scoped_model_cycle")
    if (
        not isinstance(saved_reordered_cycle, list)
        or len(saved_reordered_cycle) < 2
        or "openai/diagnostic-local" not in saved_reordered_cycle
    ):
        raise RuntimeError(
            "Alt+Up did not make the scoped model cycle explicit before saving\n"
            f"content:\n{saved_reordered_models}"
        )
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Scoped model cycle|Search models", "scoped model reorder selector canceled")
    send_literal(tmux_exe, session, "/scoped-models")
    wait_for(tmux_exe, session, r"/scoped-models", "scoped model selector draft after reorder")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Scoped model cycle|Search models", "scoped model selector after reorder")
    send_keys(tmux_exe, session, "C-x")
    scoped_model_cleared = wait_for(
        tmux_exe, session, r"0 of [0-9]+ enabled|disabled", "scoped model selector clear visible"
    )
    if "0 of " not in scoped_model_cleared or "disabled" not in scoped_model_cleared:
        raise RuntimeError(
            f"Ctrl+X did not clear the visible scoped model cycle\nscreen:\n{scoped_model_cleared}"
        )
    send_keys(tmux_exe, session, "C-s")
    saved_empty_models = wait_for_json_file(
        ava_config / "models.json",
        lambda value: isinstance(value, dict)
        and value.get("scoped_model_cycle") == []
        and any(model.get("name") == "Diagnostic Local" for model in value.get("models", []) if isinstance(model, dict)),
        "persisted empty scoped model cycle",
    )
    if '"scoped_model_cycle": []' not in saved_empty_models or "Diagnostic Local" not in saved_empty_models:
        raise RuntimeError(
            f"Ctrl+S did not persist the empty scoped model cycle while preserving custom models\ncontent:\n{saved_empty_models}"
        )
    tmux(
        tmux_exe,
        "new-session",
        "-d",
        "-s",
        scoped_persist_session,
        "-x",
        "100",
        "-y",
        "24",
        "-c",
        str(workspace),
        env_prefix,
    )
    wait_for(tmux_exe, scoped_persist_session, r"Type a message|live session", "scoped model restart frame")
    send_keys(tmux_exe, scoped_persist_session, "C-p")
    scoped_model_cycle_restart_empty = wait_for(
        tmux_exe,
        scoped_persist_session,
        r"enabled for cycling|no registered provider models",
        "persisted empty scoped model cycle status",
    )
    if (
        "enabled for cycling" not in scoped_model_cycle_restart_empty
        and "no registered provider models" not in scoped_model_cycle_restart_empty
    ):
        raise RuntimeError(
            "A fresh TUI did not load the persisted empty scoped model cycle\n"
            f"screen:\n{scoped_model_cycle_restart_empty}"
        )
    send_keys(tmux_exe, scoped_persist_session, "C-d")
    wait_for_session_exit(tmux_exe, scoped_persist_session)
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Scoped model cycle|Search models", "scoped model selector canceled")
    send_keys(tmux_exe, session, "C-p")
    scoped_model_cycle_empty = wait_for(
        tmux_exe, session, r"enabled for cycling|no registered provider models", "empty scoped model cycle status"
    )
    if "enabled for cycling" not in scoped_model_cycle_empty and "no registered provider models" not in scoped_model_cycle_empty:
        raise RuntimeError(
            f"Ctrl+P did not report the empty scoped model cycle\nscreen:\n{scoped_model_cycle_empty}"
        )
    send_literal(tmux_exe, session, "/scoped-models")
    wait_for(tmux_exe, session, r"/scoped-models", "scoped model selector restore draft")
    send_keys(tmux_exe, session, "Enter")
    wait_for(tmux_exe, session, r"Scoped model cycle|Search models", "scoped model selector restore")
    send_keys(tmux_exe, session, "C-a")
    scoped_model_enabled = wait_for(
        tmux_exe, session, r"All registered models enabled", "scoped model selector enable visible"
    )
    if "All registered models enabled" not in scoped_model_enabled:
        raise RuntimeError(
            f"Ctrl+A did not restore the visible scoped model cycle\nscreen:\n{scoped_model_enabled}"
    )
    send_keys(tmux_exe, session, "C-s")
    saved_all_models = wait_for_json_file(
        ava_config / "models.json",
        lambda value: isinstance(value, dict)
        and "scoped_model_cycle" not in value
        and any(model.get("name") == "Diagnostic Local" for model in value.get("models", []) if isinstance(model, dict)),
        "persisted all-model scoped cycle",
    )
    if "scoped_model_cycle" in saved_all_models or "Diagnostic Local" not in saved_all_models:
        raise RuntimeError(
            f"Ctrl+S did not remove the scoped model cycle field while preserving custom models\ncontent:\n{saved_all_models}"
        )
    send_keys(tmux_exe, session, "Escape")
    wait_for_absent(tmux_exe, session, r"Scoped model cycle|Search models", "scoped model selector restore canceled")
    send_keys(tmux_exe, session, "C-p")
    restored_model_cycle = wait_for(
        tmux_exe,
        session,
        r"model cycled|GPT-5\.6 (?:Sol|Terra|Luna)|gpt-5\.6-(?:sol|terra|luna)|GPT-4\.1 mini|gpt-4\.1-mini|Claude Sonnet 4\.5|claude-sonnet-4-5",
        "restored scoped model cycle",
    )
    restored_cycle_markers = (
        "model cycled",
        "GPT-5.6 Sol",
        "GPT-5.6 Terra",
        "GPT-5.6 Luna",
        "gpt-5.6-sol",
        "gpt-5.6-terra",
        "gpt-5.6-luna",
        "GPT-4.1 mini",
        "gpt-4.1-mini",
        "Claude Sonnet 4.5",
        "claude-sonnet-4-5",
    )
    if not any(value in restored_model_cycle for value in restored_cycle_markers):
        raise RuntimeError(f"Ctrl+P did not cycle after restoring scoped models\nscreen:\n{restored_model_cycle}")

    active_provider = ctx.start_fake_provider("models-thinking-active", delay_ms=0, scenario="text-three-delayed-first")
    active_session = ctx.session_name("models-thinking-active")
    active_env_prefix = ctx.fake_provider_command(
        active_provider,
        home=ctx.active_home,
        config=ctx.active_config,
        state=ctx.active_state,
        data=ctx.active_data,
    )
    tmux(
        tmux_exe,
        "new-session",
        "-d",
        "-s",
        active_session,
        "-x",
        "100",
        "-y",
        "24",
        "-c",
        str(ctx.active_workspace),
        active_env_prefix,
    )
    wait_for(tmux_exe, active_session, r"Type a message|live session", "thinking active-run initial frame")
    send_literal(tmux_exe, active_session, "thinking selector active-run guard")
    send_keys(tmux_exe, active_session, "Enter")
    active_provider.wait_for_request(0, "thinking active-run provider request")
    wait_for(tmux_exe, active_session, r"Esc stop", "thinking active-run streaming state")
    send_keys(tmux_exe, active_session, "C-t")
    active_rejection = wait_for(
        tmux_exe,
        active_session,
        r"thinking mode can be changed between turns",
        "thinking-mode active-run rejection",
    )
    if "Select thinking mode" in active_rejection:
        raise RuntimeError(f"Ctrl+T opened or persisted a thinking selector during an active run\nscreen:\n{active_rejection}")
    send_keys(tmux_exe, active_session, "Escape")
    wait_for(tmux_exe, active_session, r"stop requested|stopped|submit a new prompt", "thinking active-run stop")
    active_provider.release_request(0)
    send_keys(tmux_exe, active_session, "C-d")
    wait_for_session_exit(tmux_exe, active_session)
    tmux(tmux_exe, "kill-session", "-t", active_session, check=False)

    _finish_main(tmux_exe, session)
