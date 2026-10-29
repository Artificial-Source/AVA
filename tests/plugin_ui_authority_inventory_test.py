#!/usr/bin/env python3
"""Fail closed when plugin UI authority escapes the direct-command seam."""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from tempfile import TemporaryDirectory


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
    Path("app/tui/TuiFrontEnd.cpp"),
    Path("app/tui/TuiFrontEnd.h"),
    Path("app/interactive.cpp"),
    Path("app/interactive_internal.h"),
}


def source_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if path.suffix in {".h", ".cpp"})


def matches(files: list[Path], root: Path, pattern: re.Pattern[str]) -> list[tuple[Path, int]]:
    found: list[tuple[Path, int]] = []
    pattern = re.compile(pattern.pattern, pattern.flags | re.MULTILINE)
    for path in files:
        relative = path.relative_to(root)
        text = path.read_text(encoding="utf-8")
        for match in pattern.finditer(text):
            found.append((relative, text.count("\n", 0, match.start()) + 1))
    return found


def require_allowlist(name: str, found: list[tuple[Path, int]], allowed: set[Path]) -> None:
    escaped = [(path, line) for path, line in found if path not in allowed]
    if escaped:
        details = ", ".join(f"{path}:{line}" for path, line in escaped)
        raise AssertionError(f"{name} escaped its authority allowlist: {details}")


def self_test_inventory() -> None:
    """Prove both violation detection and identifier-boundary acceptance."""
    cases = (
        ("factory", FACTORY, "make_tui_plugin_ui_invocation_capability\n(", "prefix_make_tui_plugin_ui_invocation_capability()"),
        ("claim", CLAIM, "claim_plugin_ui_invocation_capability (", "claim_plugin_ui_invocation_capability_extra()"),
        ("handler", HANDLER, "PluginUiHandler handler", "MyPluginUiHandler handler"),
        ("field", CAPABILITY_FIELD, "plugin_ui_capability = nullptr", "plugin_ui_capability_extra = nullptr"),
        ("include", re.compile(r'^\s*#\s*include\s*[<"]ava/app/private\.h[>"]'), '#include "ava/app/private.h"', '#include "ava/core/error.h"'),
    )
    with TemporaryDirectory(prefix="ava-plugin-ui-inventory-") as directory:
        root = Path(directory)
        source = root / "escape.cpp"
        for name, pattern, forbidden, allowed in cases:
            source.write_text("// fixture\n" + forbidden + "\n", encoding="utf-8")
            found = matches([source], root, pattern)
            if found != [(Path("escape.cpp"), 2)]:
                raise AssertionError(f"{name} scanner missed or mislocated its negative fixture: {found}")
            try:
                require_allowlist(name, found, set())
            except AssertionError:
                pass
            else:
                raise AssertionError(f"{name} scanner accepted authority outside the allowlist")
            require_allowlist(name, found, {Path("escape.cpp")})
            source.write_text(allowed + "\n", encoding="utf-8")
            if matches([source], root, pattern):
                raise AssertionError(f"{name} scanner rejected an allowed identifier boundary")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    self_test_inventory()

    root = (args.source / "src" / "ava").resolve()
    files = source_files(root)

    factory = matches(files, root, FACTORY)
    production_factory_calls = [(path, line) for path, line in factory if path not in MODULE_FILES]
    expected_mint = Path("app/tui/TuiFrontEnd.cpp")
    if len(production_factory_calls) != 1 or production_factory_calls[0][0] != expected_mint:
        details = ", ".join(f"{path}:{line}" for path, line in production_factory_calls) or "none"
        raise AssertionError(f"Phase B must have exactly one production TUI factory call in app/tui/TuiFrontEnd.cpp: {details}")

    require_allowlist("plugin UI claim", matches(files, root, CLAIM), CLAIM_ALLOWLIST)
    require_allowlist("plugin UI handler", matches(files, root, HANDLER), HANDLER_ALLOWLIST)
    require_allowlist("CommandRequest plugin UI field", matches(files, root, CAPABILITY_FIELD), FIELD_ALLOWLIST)

    commands = (root / "app" / "commands.h").read_text(encoding="utf-8")
    if not re.search(r"shared_ptr<PluginUiInvocationCapability>\s+plugin_ui_capability\s*=\s*nullptr", commands):
        raise AssertionError("CommandRequest plugin UI authority must remain explicitly default-null")

    capability_header = (root / "app" / "plugin_ui_capability.h").read_text(encoding="utf-8")
    if re.search(r"\b(get|serialize|to_json|json)\w*\s*\([^;]*PluginUiInvocationCapability", capability_header, re.IGNORECASE):
        raise AssertionError("opaque plugin UI capability must not expose an internals getter or serialization API")

    frontend = (root / "app" / "tui" / "TuiFrontEnd.cpp").read_text(encoding="utf-8")
    if frontend.count("std::move(plugin_ui_capability)") != 1:
        raise AssertionError("the TUI capability must attach to exactly the first canonical handle_interactive_submission call")
    if not re.search(r"follow_up\.message[\s\S]{0,700}?context\.on_subagent_launch,\s*nullptr\)", frontend):
        raise AssertionError("every queued TUI follow-up must call handle_interactive_submission with explicit null UI authority")

    tui_files = source_files(root / "tui")
    # Shared frontend contracts and the app-common terminal text sanitizer are intentionally consumed by concrete UIs.
    # Other app headers remain forbidden here so the TUI cannot acquire app-owned plugin UI authority.
    tui_app_includes = matches(tui_files, root, re.compile(r'^\s*#\s*include\s*[<\"]ava/app/(?!frontend/|terminal_text\.h[>\"])'))
    if tui_app_includes:
        details = ", ".join(f"{path}:{line}" for path, line in tui_app_includes)
        raise AssertionError(f"TUI must not depend on app headers outside the shared frontend and terminal-text contracts: {details}")

    print("plugin UI authority inventory passed: one foreground TUI mint; direct command is the only claim/handler consumer")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
