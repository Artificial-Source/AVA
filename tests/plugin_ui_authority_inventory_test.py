#!/usr/bin/env python3
"""Fail closed when plugin UI authority escapes the direct-command seam."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


FACTORY = re.compile(r"\bmake_tui_plugin_ui_invocation_capability\s*\(")
CLAIM = re.compile(r"\bclaim_plugin_ui_invocation_capability\s*\(")
HANDLER = re.compile(r"\bPluginUiHandler\b")
CAPABILITY_FIELD = re.compile(r"\bplugin_ui_capability\b")

MODULE_FILES = {
    Path("app/plugin_ui_capability.h"),
    Path("app/plugin_ui_capability.cpp"),
}
HANDLER_ALLOWLIST = MODULE_FILES | {
    Path("plugin/runner.h"),
    Path("plugin/runner.cpp"),
    Path("app/command_plugins.cpp"),
}
CLAIM_ALLOWLIST = MODULE_FILES | {Path("app/command_plugins.cpp")}
FIELD_ALLOWLIST = MODULE_FILES | {
    Path("app/commands.h"),
    Path("app/command_plugins.cpp"),
    Path("app/interactive_tui.cpp"),
    Path("app/interactive.cpp"),
    Path("app/interactive_internal.h"),
}


def source_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if path.suffix in {".h", ".cpp"})


def matches(files: list[Path], root: Path, pattern: re.Pattern[str]) -> list[tuple[Path, int]]:
    found: list[tuple[Path, int]] = []
    for path in files:
        relative = path.relative_to(root)
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if pattern.search(line):
                found.append((relative, line_number))
    return found


def require_allowlist(name: str, found: list[tuple[Path, int]], allowed: set[Path]) -> None:
    escaped = [(path, line) for path, line in found if path not in allowed]
    if escaped:
        details = ", ".join(f"{path}:{line}" for path, line in escaped)
        raise AssertionError(f"{name} escaped its authority allowlist: {details}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()

    root = (args.source / "src" / "ava").resolve()
    files = source_files(root)

    factory = matches(files, root, FACTORY)
    production_factory_calls = [(path, line) for path, line in factory if path not in MODULE_FILES]
    expected_mint = Path("app/interactive_tui.cpp")
    if len(production_factory_calls) != 1 or production_factory_calls[0][0] != expected_mint:
        details = ", ".join(f"{path}:{line}" for path, line in production_factory_calls) or "none"
        raise AssertionError(f"Phase B must have exactly one production TUI factory call in app/interactive_tui.cpp: {details}")

    require_allowlist("plugin UI claim", matches(files, root, CLAIM), CLAIM_ALLOWLIST)
    require_allowlist("plugin UI handler", matches(files, root, HANDLER), HANDLER_ALLOWLIST)
    require_allowlist("CommandRequest plugin UI field", matches(files, root, CAPABILITY_FIELD), FIELD_ALLOWLIST)

    commands = (root / "app" / "commands.h").read_text(encoding="utf-8")
    if not re.search(r"shared_ptr<PluginUiInvocationCapability>\s+plugin_ui_capability\s*=\s*nullptr", commands):
        raise AssertionError("CommandRequest plugin UI authority must remain explicitly default-null")

    capability_header = (root / "app" / "plugin_ui_capability.h").read_text(encoding="utf-8")
    if re.search(r"\b(get|serialize|to_json|json)\w*\s*\([^;]*PluginUiInvocationCapability", capability_header, re.IGNORECASE):
        raise AssertionError("opaque plugin UI capability must not expose an internals getter or serialization API")

    interactive = (root / "app" / "interactive_tui.cpp").read_text(encoding="utf-8")
    if interactive.count("std::move(plugin_ui_capability)") != 1:
        raise AssertionError("the TUI capability must attach to exactly the first canonical handle_interactive_submission call")
    if not re.search(r"follow_up\.message[\s\S]{0,700}?context\.on_subagent_launch,\s*nullptr\)", interactive):
        raise AssertionError("every queued TUI follow-up must call handle_interactive_submission with explicit null UI authority")

    tui_files = source_files(root / "tui")
    tui_app_includes = matches(tui_files, root, re.compile(r'^\s*#\s*include\s*[<\"]ava/app/'))
    if tui_app_includes:
        details = ", ".join(f"{path}:{line}" for path, line in tui_app_includes)
        raise AssertionError(f"TUI-local plugin UI bridge must not depend on app headers: {details}")

    print("plugin UI authority inventory passed: one foreground TUI mint; direct command is the only claim/handler consumer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
