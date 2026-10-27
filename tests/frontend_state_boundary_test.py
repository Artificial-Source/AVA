#!/usr/bin/env python3
"""Enforce the reviewed ordinary-state boundary without banning retained protocols."""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
from tempfile import TemporaryDirectory


SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cxx"}
SEAM_PATH = "src/ava/tui/runtime.h"
SEAM_FIELDS = {
    "TuiRuntimeOptions": {
        "app_version", "initial_status", "initial_transcript", "slash_commands",
        "slash_catalog_generation", "file_references", "workspace_catalog_generation",
        "initial_display_settings", "mermaid_render", "key_bindings",
    },
    "TuiSubmitResult": {
        "quit", "ordinary_turn_committed", "output", "tool_timeline",
        "ordinary_turn_request_ids", "local_command_result", "conversation_output",
        "conversation_tool_timeline",
    },
}
RETAINED_CALLBACK_SNAPSHOTS = {
    "TuiBranchSummarySnapshot", "StartupOverviewSnapshot", "ProjectTrustSnapshot",
}
BANNED_IDENTIFIERS = {
    "TuiRuntimeStateSnapshot", "state_snapshot", "apply_runtime_state_snapshot",
    "TuiKeyBindingReloadResult", "refresh_token_status",
}
TUI_BACKEND_IDENTIFIERS = {
    "refresh_session_statistics", "compute_session_stats", "estimate_active_context_tokens",
    "read_authority", "observe_git_branch", "refresh_git_branch",
}
CANONICAL_WRAPPERS = {
    ("src/ava/app/runtime/AgentSelection.h", "AgentSelection_ts", "AgentSelection"),
    ("src/ava/app/runtime/Directories.h", "Directories_ts", "Directories"),
    ("src/ava/app/runtime/GitBranch.h", "GitBranch_ts", "GitBranch"),
    ("src/ava/app/runtime/ModelSelection.h", "ModelSelection_ts", "ModelSelection"),
    ("src/ava/app/runtime/ResolvedPromptState.h", "PromptModeAndContextSources_ts", "PromptModeAndContextSources"),
    ("src/ava/session/Stats.h", "Stats_ts", "Stats"),
    ("src/ava/session/session_store.h", "SessionStoreOptions_ts", "SessionStoreOptions"),
}
UI_ONLY_STEMS = {
    "src/ava/app/tui/display_reload", "src/ava/app/tui/tui_display_settings",
    "src/ava/tui/theme", "src/ava/tui/keybindings",
}
# Mermaid's mutex belongs to a retained cross-thread service, not UI-only settings.
# No blanket mutex restriction applies to permission/question/plugin/run protocols.
WRAPPER = re.compile(r"\bUnlockedData\s*<")
WRAPPER_ALIAS = re.compile(r"\busing\s+(\w+)\s*=\s*(?:ava::)?frontend::UnlockedData\s*<\s*([^;]+?)\s*>\s*;")
SESSION_ACCESS = re.compile(r"\b(?:session_ts|SCOPED_CRITICAL_AREA(?:_[A-Z]+)?)\b|\bapp\s*::\s*runtime\s*::\s*Session\b")
UI_LOCK = re.compile(r"\bstd\s*::\s*(?:mutex|shared_mutex)\b")


def source_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if path.is_file() and path.suffix in SOURCE_SUFFIXES)


def without_comments(text: str) -> str:
    # Preserve quoted literals and line numbers while removing C++ comments.
    pattern = r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/'
    return re.sub(pattern, lambda match: re.sub(r"[^\n]", " ", match[0]) if match[0].startswith(("//", "/*")) else match[0], text)


def struct_body(text: str, name: str) -> str:
    declarations = list(re.finditer(r"\bstruct\s+" + re.escape(name) + r"\s*\{", text))
    if len(declarations) != 1:
        raise AssertionError(f"seam: expected exactly one definition of {name}")
    start = declarations[0].end()
    depth = 1
    for index in range(start, len(text)):
        depth += (text[index] == "{") - (text[index] == "}")
        if depth == 0:
            return text[start:index]
    raise AssertionError(f"seam: unterminated {name}")


def check_seam(text: str) -> None:
    text = without_comments(text)
    for name, allowed_fields in SEAM_FIELDS.items():
        body = struct_body(text, name)
        body = re.sub(r"\bAVA_DEBUG_PRINT_MEMBERS_(?:ON|OPT_OUT)\b", "", body)
        for declaration in body.split(";"):
            declaration = declaration.strip()
            if not declaration:
                continue
            if name == "TuiRuntimeOptions" and re.fullmatch(r"void\s+print_on\s*\(std::ostream\s*&\s*os\)\s*const", declaration):
                continue
            member = re.fullmatch(r"(.+?)\s+(\w+)\s*(?:=[\s\S]*|\{[\s\S]*\})?", declaration, re.DOTALL)
            if not member:
                raise AssertionError(f"seam: unrecognized {name} declaration: {declaration}")
            member_type, member_name = member.groups()
            if re.match(r"std\s*::\s*function\s*<", member_type):
                snapshots = set(re.findall(r"\b\w*Snapshot\b", member_type))
                forbidden = snapshots - RETAINED_CALLBACK_SNAPSHOTS
                if forbidden:
                    raise AssertionError(f"seam: {name}::{member_name} transports forbidden snapshot types: {sorted(forbidden)}")
            elif member_name not in allowed_fields:
                raise AssertionError(f"seam: unreviewed data member {name}::{member_name}")


def check_tree(source: Path) -> None:
    check_seam((source / SEAM_PATH).read_text(encoding="utf-8"))
    wrappers: Counter[tuple[str, str, str]] = Counter()
    wrapper_count = 0
    for directory in ("src/ava/tui", "src/ava/app", "src/ava/frontend", "src/ava/session", "tests"):
        for path in source_files(source / directory):
            relative = path.relative_to(source).as_posix()
            text = without_comments(path.read_text(encoding="utf-8"))
            identifiers = set(re.findall(r"\b\w+\b", text))
            if directory != "src/ava/session":
                forbidden = identifiers & BANNED_IDENTIFIERS
                if forbidden:
                    raise AssertionError(f"identifiers: {relative}: forbidden {sorted(forbidden)}")
            if directory == "src/ava/tui":
                forbidden = identifiers & TUI_BACKEND_IDENTIFIERS
                if forbidden:
                    raise AssertionError(f"tui backend: {relative}: forbidden {sorted(forbidden)}")
                if SESSION_ACCESS.search(text):
                    raise AssertionError(f"session seam: {relative}: backend access belongs in frontend initialization, not TUI code")
            if relative.startswith(("src/ava/app/runtime/", "src/ava/session/")):
                wrapper_count += len(WRAPPER.findall(text))
                for alias, value in WRAPPER_ALIAS.findall(text):
                    wrappers[(relative, alias, re.sub(r"\s+", "", value))] += 1
            if path.relative_to(source).with_suffix("").as_posix() in UI_ONLY_STEMS and (WRAPPER.search(text) or UI_LOCK.search(text)):
                raise AssertionError(f"ui local: {relative}: appearance/interaction state must stay lock-free")
            if relative.startswith(("src/ava/tui/", "src/ava/app/tui/")) and WRAPPER.search(text):
                raise AssertionError(f"tui wrapper: {relative}: pending/protocol/UI state cannot use UnlockedData")
    expected = Counter(CANONICAL_WRAPPERS)
    if wrappers != expected or wrapper_count != len(CANONICAL_WRAPPERS):
        raise AssertionError(f"canonical wrappers: expected exactly seven reviewed aliases; missing={list((expected - wrappers).elements())}, "
                             f"unexpected={list((wrappers - expected).elements())}, instantiations={wrapper_count}")


def self_test() -> None:
    with TemporaryDirectory(prefix="ava-frontend-state-boundary-") as directory:
        root = Path(directory)

        def put(path: str, text: str) -> None:
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text, encoding="utf-8")

        seam = "struct TuiRuntimeOptions { std::string app_version; CALLBACKS };\nstruct TuiSubmitResult { bool quit = false; };\n"
        callbacks = "\n".join(f"std::function<{name}()> read_{index};" for index, name in enumerate(sorted(RETAINED_CALLBACK_SNAPSHOTS)))
        baseline = seam.replace("CALLBACKS", callbacks)
        put(SEAM_PATH, baseline)
        for path, alias, value in CANONICAL_WRAPPERS:
            put(path, f"using {alias} = frontend::UnlockedData<{value}>;\n")
        put("src/ava/frontend/FrontEnd.cxx", "app::runtime::session_ts::crat session_r; SCOPED_CRITICAL_AREA(); app::runtime::Session* session;")
        put("src/ava/app/tui/mermaid_tui_bridge.h", "std::mutex mutex; SubagentInspectorFrame frame;")
        put("src/ava/tui/composer.h", "struct ComposerSnapshot {}; struct SidebarSnapshot {};")
        put("src/ava/app/backend.cpp", "refresh_session_statistics(); read_authority();")
        put("docs/history/old.md", "TuiRuntimeStateSnapshot state_snapshot;")
        check_tree(root)

        cases = [
            ("seam", SEAM_PATH, baseline.replace("app_version;", "app_version; std::string backend_agent;")),
            ("seam", SEAM_PATH, baseline.replace("bool quit = false;", "bool quit = false; int replacement_state;")),
            ("seam", SEAM_PATH, baseline.replace("app_version;", "app_version; std::function<RenamedSnapshot()> leak;")),
            ("seam", SEAM_PATH, baseline.replace("app_version;", "app_version; std::function<void(SidebarSnapshot const&)> leak;")),
            ("seam", SEAM_PATH, baseline.replace("bool quit = false;", "std::function<ComposerSnapshot()> leak;")),
            ("canonical wrappers", "src/ava/app/runtime/Extra.h", "using Extra_ts = frontend::UnlockedData<int>;"),
            ("canonical wrappers", "src/ava/app/runtime/Extra.h", "frontend::UnlockedData<int> extra;"),
            ("canonical wrappers", "src/ava/app/runtime/GitBranch.h", "using GitBranch_ts = frontend::UnlockedData<int>;"),
            ("canonical wrappers", "src/ava/app/runtime/GitBranch.h", ""),
        ]
        for identifier in sorted(BANNED_IDENTIFIERS):
            for scope in ("src/ava/tui", "src/ava/app", "src/ava/frontend", "tests"):
                cases.append(("identifiers", f"{scope}/escape.cpp", f"void {identifier}();"))
        for identifier in sorted(TUI_BACKEND_IDENTIFIERS):
            cases.append(("tui backend", "src/ava/tui/escape.cpp", f"void {identifier}();"))
        for access in ("session_ts::crat session;", "SCOPED_CRITICAL_AREA();", "SCOPED_CRITICAL_AREA_W(session, owner);",
                       "SCOPED_CRITICAL_AREA_R(session, owner);", "ava::app::runtime::Session* session;"):
            cases.append(("session seam", "src/ava/tui/escape.h", access))
        for stem in sorted(UI_ONLY_STEMS):
            for declaration in ("std::mutex mutex;", "std::shared_mutex mutex;", "frontend::UnlockedData<int> value;"):
                cases.append(("ui local", stem + ".h", declaration))
        for scope in ("src/ava/tui", "src/ava/app/tui"):
            cases.append(("tui wrapper", scope + "/escape.cxx", "frontend::UnlockedData<int> value;"))
        for expected, path, text in cases:
            target = root / path
            original = target.read_text(encoding="utf-8") if target.exists() else None
            put(path, text)
            try:
                check_tree(root)
            except AssertionError as error:
                if not str(error).startswith(expected + ":"):
                    raise AssertionError(f"fixture expected {expected}, got {error}") from error
            else:
                raise AssertionError(f"negative fixture accepted: {expected}: {path}: {text}")
            finally:
                if original is None:
                    target.unlink()
                else:
                    put(path, original)
        check_tree(root)
        print(f"frontend state boundary self-test passed: {len(cases)} negative cases and retained/local positive fixtures")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif args.source is not None:
        check_tree(args.source.resolve())
        print("frontend state boundary passed: reviewed seam, scoped identifiers, Session access, seven canonical wrappers and lock-free UI state")
    else:
        parser.error("pass --source or --self-test")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
