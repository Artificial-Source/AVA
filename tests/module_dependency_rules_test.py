#!/usr/bin/env python3
"""Check internal-module dependency direction and temporary exceptions.

Production paths are classified into logical modules before their include edges
are checked. This lets the app-owned common target remain in src/ava/app while
still enforcing its narrower dependency rules.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import sys
import tempfile
import unittest

SCANNED_MODULES = (
    "process",
    "config",
    "http",
    "provider",
    "session",
    "agent",
    "tools",
    "permissions",
    "mcp",
    "plugin",
    "lsp",
    "event",
    "app",
    "app_common",
    "tui",
)
SCANNED_MODULE_SET = frozenset(SCANNED_MODULES)
SOURCE_ROOT_MODULES = tuple(module for module in SCANNED_MODULES if module != "app_common")
RECOGNIZED_FIRST_PARTY_ROOTS = frozenset(
    {
        "agent",
        "app",
        "app_common",
        "command",
        "config",
        "containment",
        "context",
        "core",
        "debug",
        "desktop",
        "diagnostics",
        "event",
        "http",
        "lsp",
        "mcp",
        "observability",
        "permissions",
        "plugin",
        "process",
        "provider",
        "session",
        "tools",
        "tui",
    }
)
ALLOWED = {
    "app": (SCANNED_MODULE_SET | {"app_common"}) - {"app"},
    "agent": frozenset({"config", "http", "provider", "session", "tools", "permissions", "mcp", "plugin", "process"}),
    "provider": frozenset({"config", "http"}),
    "session": frozenset({"config"}),
    "tools": frozenset({"http", "lsp", "permissions", "process"}),
    "config": frozenset({"http"}),
    "http": frozenset({"process"}),
    "permissions": frozenset(),
    "mcp": frozenset({"config", "tools", "permissions", "process"}),
    "plugin": frozenset({"config", "tools", "permissions", "process"}),
    "lsp": frozenset({"permissions", "process"}),
    "event": frozenset({"core", "debug"}),
    "process": frozenset({"core", "debug"}),
    "app_common": frozenset({"core", "debug"}),
    "tui": frozenset({"agent", "app_common", "config", "core", "debug", "event", "permissions", "session"}),
}
APP_COMMON_PATHS = frozenset({"app/terminal_text.cpp", "app/terminal_text.h"})
# The historical frontend base remains app-owned; do not classify its relative
# Data.h dependency as part of AVA::app_common.
APP_FRONTEND_CONTRACT_INCLUDES = frozenset({"ava/app/frontend/FrontEnd.h"})
SOURCE_SUFFIXES = frozenset({".h", ".hpp", ".cpp"})
IGNORED_TREE_NAMES = frozenset({"build", "generated", "reference", "tests", "vendor"})
INCLUDE = re.compile(r'^\s*#\s*include\s*(?:<([^>]+)>|"([^"]+)")')


class PolicyError(ValueError):
    pass


def source_kind(path: Path) -> str:
    return "implementation" if path.suffix == ".cpp" else "public"


def logical_module(relative_to_ava: PurePosixPath) -> str:
    if relative_to_ava.as_posix() in APP_COMMON_PATHS:
        return "app_common"
    return relative_to_ava.parts[0] if len(relative_to_ava.parts) >= 2 else "<noncanonical>"


def include_target(source: Path, including_path: Path, literal: str) -> str | None:
    if literal.startswith("ava/"):
        parts = PurePosixPath(literal).parts
        if PurePosixPath(literal).as_posix() != literal or "." in parts or ".." in parts or len(parts) < 3:
            return "<noncanonical>"
        return logical_module(PurePosixPath(*parts[1:]))

    # Bare generated/project-root/system headers do not express a relative
    # module edge. Relative module includes necessarily contain a path separator.
    if "/" not in literal:
        return None

    # A relative include can otherwise bypass the canonical ava/<module>/ form.
    # Resolve it lexically against the including file and classify it whenever it
    # still names production code below src/ava.
    candidate = Path(*including_path.parent.parts, *PurePosixPath(literal).parts)
    normalized = Path(candidate.parent, candidate.name).resolve(strict=False)
    try:
        relative = normalized.relative_to((source / "src" / "ava").resolve())
    except ValueError:
        return None
    return logical_module(PurePosixPath(relative.as_posix()))


def is_policy_target(module: str, target: str) -> bool:
    if target == "<noncanonical>":
        return True
    if module in {"app_common", "event", "process"}:
        return target in RECOGNIZED_FIRST_PARTY_ROOTS
    return target in SCANNED_MODULE_SET


def collect_edges(source: Path) -> list[tuple[str, int, str, str, str, str]]:
    edges = []
    for source_root_module in SOURCE_ROOT_MODULES:
        directory = source / "src" / "ava" / source_root_module
        if not directory.is_dir():
            continue
        for path in sorted(
            candidate
            for candidate in directory.rglob("*")
            if candidate.is_file() and candidate.suffix in SOURCE_SUFFIXES and not any(part in IGNORED_TREE_NAMES for part in candidate.relative_to(directory).parts)
        ):
            relative = path.relative_to(source).as_posix()
            module = logical_module(PurePosixPath(path.relative_to(source / "src" / "ava").as_posix()))
            for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                matched = INCLUDE.match(line)
                if not matched:
                    continue
                literal = matched.group(1) or matched.group(2)
                target = include_target(source, path, literal)
                if target is None or target == module or not is_policy_target(module, target):
                    continue
                app_tui_seam = module == "app" and target == "tui" and relative.startswith("src/ava/app/tui/")
                tui_frontend_contract = module == "tui" and target == "app" and literal in APP_FRONTEND_CONTRACT_INCLUDES
                forbidden_app_tui = module == "app" and target == "tui" and not app_tui_seam
                if target == "<noncanonical>" or forbidden_app_tui or (target not in ALLOWED[module] and not app_tui_seam and not tui_frontend_contract):
                    edges.append((relative, line_number, source_kind(path), module, target, literal))
    return sorted(edges)


def relative_source(value: object) -> str:
    if not isinstance(value, str) or not value:
        raise PolicyError("exception source must be a nonempty relative path")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts or path.as_posix() != value or not value.startswith("src/ava/"):
        raise PolicyError(f"exception source is not a normalized relative production path: {value!r}")
    parts = path.parts
    if len(parts) < 4 or parts[2] not in SCANNED_MODULE_SET or path.suffix not in SOURCE_SUFFIXES:
        raise PolicyError(f"exception source has an unknown module or unsupported extension: {value!r}")
    return value


def exception_key(entry: object) -> tuple[str, str, str]:
    if not isinstance(entry, dict) or set(entry) != {"source", "include", "kind", "reason", "tracking"}:
        raise PolicyError("each exception must contain exactly source, include, kind, reason, and tracking")
    source = relative_source(entry["source"])
    include = entry["include"]
    kind = entry["kind"]
    reason = entry["reason"]
    tracking = entry["tracking"]
    include_parts = PurePosixPath(include).parts if isinstance(include, str) else ()
    if (
        not isinstance(include, str)
        or PurePosixPath(include).as_posix() != include
        or "." in include_parts
        or ".." in include_parts
        or len(include_parts) < 3
        or include_parts[0] != "ava"
        or include_parts[1] not in RECOGNIZED_FIRST_PARTY_ROOTS
    ):
        raise PolicyError(f"exception include is not a canonical ava/<module>/... path with a recognized first-party root: {include!r}")
    source_module = PurePosixPath(source).parts[2]
    if not is_policy_target(source_module, include_parts[1]):
        raise PolicyError(f"exception include root is not governed for source module {source_module}: {include!r}")
    if kind not in {"public", "implementation"}:
        raise PolicyError(f"exception kind must be public or implementation: {kind!r}")
    if not isinstance(reason, str) or not reason.strip():
        raise PolicyError("exception reason must be nonempty")
    if not isinstance(tracking, str) or not tracking.strip():
        raise PolicyError("exception tracking must be nonempty")
    return source, include, kind


def load_policy(path: Path) -> set[tuple[str, str, str]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise PolicyError(f"cannot read policy {path}: {error}") from error
    if not isinstance(document, dict) or set(document) != {"exceptions"} or not isinstance(document["exceptions"], list):
        raise PolicyError("policy must be an object containing only an exceptions array")
    keys = set()
    duplicates = []
    for entry in document["exceptions"]:
        key = exception_key(entry)
        if key in keys:
            duplicates.append(key)
        keys.add(key)
    if duplicates:
        rendered = "\n".join(f"duplicate exception: {source} {kind} {include}" for source, include, kind in sorted(set(duplicates)))
        raise PolicyError(rendered)
    return keys


def check(source: Path, policy: Path) -> list[str]:
    exceptions = load_policy(policy)
    forbidden = collect_edges(source)
    scanned = {(path, include, kind) for path, _, kind, _, _, include in forbidden}
    diagnostics = [
        f"{path}:{line}: {kind} {module} -> {target} ({include})"
        for path, line, kind, module, target, include in forbidden
        if (path, include, kind) not in exceptions
    ]
    diagnostics.extend(
        f"stale exception (not a currently forbidden include): {path} {kind} {include}"
        for path, include, kind in sorted(exceptions - scanned)
    )
    return sorted(diagnostics)


class ModuleDependencyRulesSelfTest(unittest.TestCase):
    def run_check(self, source_text: str, exceptions: list[dict], module: str = "config", source_name: str = "fixture.cpp") -> list[str]:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "src" / "ava" / module / source_name
            source.parent.mkdir(parents=True)
            source.write_text(source_text, encoding="utf-8")
            policy = root / "policy.json"
            policy.write_text(json.dumps({"exceptions": exceptions}), encoding="utf-8")
            return check(root, policy)

    def test_allowed_edge(self) -> None:
        self.assertEqual(self.run_check('#include "ava/config/model_config.h"\n', [], module="provider"), [])

    def test_config_and_tools_may_depend_on_http(self) -> None:
        self.assertEqual(self.run_check('#include "ava/http/transport.h"\n', [], module="config"), [])
        self.assertEqual(self.run_check('#include "ava/http/transport.h"\n', [], module="tools"), [])

    def test_agent_may_depend_on_external_tool_brokers(self) -> None:
        self.assertEqual(self.run_check('#include "ava/mcp/tool_broker.h"\n', [], module="agent"), [])
        self.assertEqual(self.run_check('#include "ava/plugin/tool_broker.h"\n', [], module="agent"), [])

    def test_agent_may_propagate_process_authority(self) -> None:
        self.assertEqual(self.run_check('#include "ava/process/scope.h"\n', [], module="agent"), [])

    def test_external_tool_brokers_may_depend_only_on_lower_tool_layers(self) -> None:
        for module in ("mcp", "plugin"):
            for target in ("config", "tools", "permissions"):
                with self.subTest(module=module, target=target):
                    self.assertEqual(self.run_check(f'#include "ava/{target}/fixture.h"\n', [], module=module), [])

    def test_external_tool_brokers_cannot_depend_on_agent(self) -> None:
        for module in ("mcp", "plugin"):
            with self.subTest(module=module):
                self.assertEqual(
                    self.run_check('#include "ava/agent/tool_registry.h"\n', [], module=module),
                    [f"src/ava/{module}/fixture.cpp:1: implementation {module} -> agent (ava/agent/tool_registry.h)"],
                )

    def test_app_may_depend_on_event(self) -> None:
        self.assertEqual(self.run_check('#include "ava/event/events.h"\n', [], module="app"), [])

    def test_tui_app_contracts_are_exactly_governed(self) -> None:
        self.assertEqual(self.run_check('#include "ava/app/terminal_text.h"\n', [], module="tui"), [])
        self.assertEqual(self.run_check('#include "ava/app/frontend/FrontEnd.h"\n', [], module="tui"), [])
        self.assertEqual(
            self.run_check('#include "ava/app/command_palette.h"\n', [], module="tui"),
            ["src/ava/tui/fixture.cpp:1: implementation tui -> app (ava/app/command_palette.h)"],
        )

    def test_backend_cannot_depend_on_app_common(self) -> None:
        self.assertEqual(
            self.run_check('#include "ava/app/terminal_text.h"\n', [], module="agent"),
            ["src/ava/agent/fixture.cpp:1: implementation agent -> app_common (ava/app/terminal_text.h)"],
        )

    def test_app_common_implementation_may_only_depend_on_neutral_layers(self) -> None:
        self.assertEqual(self.run_check('#include "ava/core/utf8.h"\n', [], module="app", source_name="terminal_text.cpp"), [])
        self.assertEqual(
            self.run_check('#include "ava/tui/composer.h"\n', [], module="app", source_name="terminal_text.cpp"),
            ["src/ava/app/terminal_text.cpp:1: implementation app_common -> tui (ava/tui/composer.h)"],
        )
        self.assertEqual(
            self.run_check('#include "ava/diagnostics/runtime_diagnostics.h"\n', [], module="app", source_name="terminal_text.cpp"),
            ["src/ava/app/terminal_text.cpp:1: implementation app_common -> diagnostics (ava/diagnostics/runtime_diagnostics.h)"],
        )

    def test_relative_includes_cannot_escape_app_common_rules(self) -> None:
        self.assertEqual(
            self.run_check('#include "../app/terminal_text.h"\n', [], module="agent"),
            ["src/ava/agent/fixture.cpp:1: implementation agent -> app_common (../app/terminal_text.h)"],
        )
        self.assertEqual(
            self.run_check('#include "../tui/composer.h"\n', [], module="app", source_name="terminal_text.cpp"),
            ["src/ava/app/terminal_text.cpp:1: implementation app_common -> tui (../tui/composer.h)"],
        )

    def test_only_app_tui_seam_may_include_tui(self) -> None:
        self.assertEqual(self.run_check('#include "ava/tui/composer.h"\n', [], module="app", source_name="tui/fixture.cpp"), [])
        self.assertEqual(
            self.run_check('#include "ava/tui/composer.h"\n', [], module="app"),
            ["src/ava/app/fixture.cpp:1: implementation app -> tui (ava/tui/composer.h)"],
        )

    def test_event_may_depend_on_core_and_debug(self) -> None:
        self.assertEqual(self.run_check('#include "ava/core/result.h"\n#include "ava/debug/print_members_on.h"\n', [], module="event"), [])

    def test_process_may_depend_only_on_core_and_debug_marker_plumbing(self) -> None:
        self.assertEqual(
            self.run_check('#include "ava/core/result.h"\n#include "ava/debug/print_members_on.h"\n', [], module="process"),
            [],
        )
        for target in ("app", "agent", "tools", "plugin", "mcp", "lsp", "diagnostics"):
            with self.subTest(target=target):
                self.assertEqual(
                    self.run_check(f'#include "ava/{target}/fixture.h"\n', [], module="process"),
                    [f"src/ava/process/fixture.cpp:1: implementation process -> {target} (ava/{target}/fixture.h)"],
                )

    def test_spawning_modules_may_depend_downward_on_process(self) -> None:
        for module in ("app", "http", "tools", "plugin", "mcp", "lsp"):
            with self.subTest(module=module):
                self.assertEqual(self.run_check('#include "ava/process/supervisor.h"\n', [], module=module), [])

    def test_event_cannot_depend_on_tui(self) -> None:
        self.assertEqual(
            self.run_check('#include "ava/tui/runtime.h"\n', [], module="event"),
            ["src/ava/event/fixture.cpp:1: implementation event -> tui (ava/tui/runtime.h)"],
        )

    def test_event_cannot_depend_on_command_or_context(self) -> None:
        for target in ("command", "context"):
            with self.subTest(target=target):
                self.assertEqual(
                    self.run_check(f'#include "ava/{target}/fixture.h"\n', [], module="event"),
                    [f"src/ava/event/fixture.cpp:1: implementation event -> {target} (ava/{target}/fixture.h)"],
                )

    def test_http_cannot_depend_on_higher_layers(self) -> None:
        for target in ("provider", "config", "tools", "agent", "mcp", "plugin", "event", "app"):
            with self.subTest(target=target):
                self.assertEqual(
                    self.run_check(f'#include "ava/{target}/fixture.h"\n', [], module="http"),
                    [f"src/ava/http/fixture.cpp:1: implementation http -> {target} (ava/{target}/fixture.h)"],
                )

    def test_forbidden_edge(self) -> None:
        self.assertEqual(
            self.run_check('#include "ava/provider/provider.h"\n', []),
            ['src/ava/config/fixture.cpp:1: implementation config -> provider (ava/provider/provider.h)'],
        )

    def test_angle_bracket_forbidden_edge(self) -> None:
        self.assertEqual(
            self.run_check("#include <ava/provider/provider.h>\n", []),
            ["src/ava/config/fixture.cpp:1: implementation config -> provider (ava/provider/provider.h)"],
        )

    def test_relative_forbidden_edge(self) -> None:
        self.assertEqual(
            self.run_check('#include "../provider/provider.h"\n', []),
            ["src/ava/config/fixture.cpp:1: implementation config -> provider (../provider/provider.h)"],
        )

    def test_noncanonical_internal_include(self) -> None:
        self.assertEqual(
            self.run_check('#include "ava/provider/../unknown/header.h"\n', []),
            ["src/ava/config/fixture.cpp:1: implementation config -> <noncanonical> (ava/provider/../unknown/header.h)"],
        )

    def test_exact_exception(self) -> None:
        exception = {
            "source": "src/ava/config/fixture.cpp",
            "include": "ava/provider/provider.h",
            "kind": "implementation",
            "reason": "Temporary characterization fixture.",
            "tracking": "test-only",
        }
        self.assertEqual(self.run_check('#include "ava/provider/provider.h"\n', [exception]), [])

    def test_stale_exception(self) -> None:
        exception = {
            "source": "src/ava/config/fixture.cpp",
            "include": "ava/config/model_config.h",
            "kind": "implementation",
            "reason": "Temporary characterization fixture.",
            "tracking": "test-only",
        }
        self.assertEqual(
            self.run_check('#include "ava/config/model_config.h"\n', [exception]),
            ["stale exception (not a currently forbidden include): src/ava/config/fixture.cpp implementation ava/config/model_config.h"],
        )

    def test_policy_rejects_unscanned_source_module_and_ungoverned_target(self) -> None:
        unscanned_source = {
            "source": "src/ava/command/fixture.cpp",
            "include": "ava/event/events.h",
            "kind": "implementation",
            "reason": "Invalid unscanned source fixture.",
            "tracking": "test-only",
        }
        with self.assertRaisesRegex(PolicyError, "unknown module"):
            self.run_check('#include "ava/provider/provider.h"\n', [unscanned_source])

        ungoverned_target = {
            "source": "src/ava/config/fixture.cpp",
            "include": "ava/core/result.h",
            "kind": "implementation",
            "reason": "Invalid ungoverned target fixture.",
            "tracking": "test-only",
        }
        with self.assertRaisesRegex(PolicyError, "not governed"):
            self.run_check('#include "ava/provider/provider.h"\n', [ungoverned_target])

    def test_policy_rejects_traversal_paths(self) -> None:
        include_traversal = {
            "source": "src/ava/config/fixture.cpp",
            "include": "ava/provider/../unknown/header.h",
            "kind": "implementation",
            "reason": "Invalid traversal fixture.",
            "tracking": "test-only",
        }
        with self.assertRaisesRegex(PolicyError, "canonical ava/<module>"):
            self.run_check('#include "ava/provider/provider.h"\n', [include_traversal])

        source_traversal = {
            "source": "src/ava/config/../provider/fixture.cpp",
            "include": "ava/provider/provider.h",
            "kind": "implementation",
            "reason": "Invalid traversal fixture.",
            "tracking": "test-only",
        }
        with self.assertRaisesRegex(PolicyError, "normalized relative production path"):
            self.run_check('#include "ava/provider/provider.h"\n', [source_traversal])

    def test_duplicate_exception(self) -> None:
        exception = {
            "source": "src/ava/config/fixture.cpp",
            "include": "ava/provider/provider.h",
            "kind": "implementation",
            "reason": "Temporary characterization fixture.",
            "tracking": "test-only",
        }
        with self.assertRaisesRegex(PolicyError, "duplicate exception"):
            self.run_check('#include "ava/provider/provider.h"\n', [exception, exception])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path)
    parser.add_argument("--policy", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ModuleDependencyRulesSelfTest)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.source is None:
        parser.error("--source is required unless --self-test is used")
    source = args.source.resolve()
    policy = args.policy.resolve() if args.policy else source / "tests" / "fixtures" / "module_dependency_rules.json"
    try:
        diagnostics = check(source, policy)
    except PolicyError as error:
        print(f"module dependency policy error: {error}", file=sys.stderr)
        return 1
    if diagnostics:
        print("\n".join(diagnostics), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
