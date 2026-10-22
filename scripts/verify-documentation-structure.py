#!/usr/bin/env python3
"""Verify AVA's documentation taxonomy, indexes, and reachability."""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import sys
import urllib.parse

LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(\s*(<[^>]+>|[^\s)]+)(?:\s+[^)]*)?\)")
REFERENCE_DEFINITION_RE = re.compile(
    r"^[ ]{0,3}\[([^\]\n]{1,999})\]:[ \t]*(?:<([^>\n]*)>|([^ \t\n]+))",
    re.MULTILINE,
)
REFERENCE_USAGE_RE = re.compile(r"(?<!!)\[([^\]\n]{1,999})\]\[([^\]\n]{0,999})\]")
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")

FIXED_ROOT_FILES = frozenset(
    {
        "README.md",
        "AGENTS.md",
        "acp.md",
        "acp-support.json",
        "rpc-protocol.md",
        "headless-protocol.md",
        "session-format.md",
        "plugin-compatibility-policy.md",
    }
)
APPROVED_DIRECTORIES = frozenset(
    {
        "core",
        "interfaces",
        "desktop",
        "extensions",
        "operations",
        "development",
        "engineering",
        "security",
        "product",
        "plans",
        "roadmap",
        "goals",
        "history",
        "versions",
        "interop",
        "schema",
        "reference-code",
    }
)
REQUIRED_INDEXES = (
    "docs/core/README.md",
    "docs/interfaces/README.md",
    "docs/desktop/README.md",
    "docs/extensions/README.md",
    "docs/operations/README.md",
    "docs/development/README.md",
    "docs/development/internals/README.md",
    "docs/engineering/README.md",
    "docs/security/README.md",
    "docs/product/README.md",
    "docs/plans/README.md",
    "docs/roadmap/README.md",
    "docs/history/README.md",
    "docs/schema/README.md",
    "docs/goals/README.md",
    "docs/versions/README.md",
    "docs/interop/README.md",
    "docs/interop/evidence/README.md",
)
REQUIRED_LLMS_TARGETS = frozenset(
    {
        "docs/README.md",
        "docs/core/README.md",
        "docs/interfaces/README.md",
        "docs/desktop/README.md",
        "docs/extensions/README.md",
        "docs/operations/README.md",
        "docs/security/README.md",
        "docs/development/README.md",
        "docs/engineering/README.md",
        "docs/product/README.md",
        "docs/plans/README.md",
        "docs/roadmap/README.md",
        "docs/goals/README.md",
        "docs/history/README.md",
        "docs/versions/README.md",
        "docs/interop/README.md",
        "docs/interop/evidence/README.md",
        "docs/acp.md",
        "docs/rpc-protocol.md",
        "docs/headless-protocol.md",
        "docs/session-format.md",
        "docs/plugin-compatibility-policy.md",
    }
)
ARTIFACT_TEMPLATE = pathlib.PurePosixPath(
    "docs/operations/release-artifact-readme.md"
)


def markdown_without_fenced_code(text: str) -> str:
    lines: list[str] = []
    fence_character: str | None = None
    fence_length = 0
    for line in text.splitlines():
        match = FENCE_RE.match(line)
        if match:
            marker = match.group(1)
            if fence_character is None:
                fence_character = marker[0]
                fence_length = len(marker)
                lines.append("")
                continue
            if (
                marker[0] == fence_character
                and len(marker) >= fence_length
                and not line[match.end() :].strip()
            ):
                fence_character = None
                fence_length = 0
                lines.append("")
                continue
        lines.append("" if fence_character else line)
    return "\n".join(lines)


def normalize_reference_label(label: str) -> str:
    return " ".join(label.split()).casefold()


def markdown_targets(path: pathlib.Path) -> tuple[list[str], list[str]]:
    text = markdown_without_fenced_code(path.read_text(encoding="utf-8"))
    targets = [match.group(1).strip("<>") for match in LINK_RE.finditer(text)]
    definitions: dict[str, str] = {}
    definition_spans: list[tuple[int, int]] = []
    for match in REFERENCE_DEFINITION_RE.finditer(text):
        label = normalize_reference_label(match.group(1))
        target = match.group(2) if match.group(2) is not None else match.group(3)
        if label:
            definitions.setdefault(label, target)
        line_end = text.find("\n", match.end())
        definition_spans.append((match.start(), len(text) if line_end == -1 else line_end))

    characters = list(text)
    for start, end in definition_spans:
        for index in range(start, end):
            if characters[index] != "\n":
                characters[index] = " "

    failures: list[str] = []
    text_without_definitions = "".join(characters)
    for match in REFERENCE_USAGE_RE.finditer(text_without_definitions):
        raw_label = match.group(2) or match.group(1)
        target = definitions.get(normalize_reference_label(raw_label))
        if target is None:
            failures.append(f"undefined reference link label: {raw_label}")
        else:
            targets.append(target)
    return targets, failures


def documentation_nodes(root: pathlib.Path) -> set[pathlib.PurePosixPath]:
    docs = root / "docs"
    nodes: set[pathlib.PurePosixPath] = set()
    for directory, directory_names, file_names in os.walk(docs, followlinks=False):
        current = pathlib.Path(directory)
        relative_directory = current.relative_to(root)
        if relative_directory.parts[:2] == ("docs", "reference-code"):
            directory_names[:] = []
            continue
        directory_names.sort()
        for name in sorted(file_names):
            if pathlib.Path(name).suffix not in {".md", ".json"}:
                continue
            nodes.add(pathlib.PurePosixPath((current / name).relative_to(root).as_posix()))
    return nodes


def local_candidate(
    raw_target: str,
    source: pathlib.Path,
    root: pathlib.Path,
) -> tuple[pathlib.Path | None, str | None]:
    parsed = urllib.parse.urlsplit(raw_target)
    if parsed.scheme or parsed.netloc or raw_target.startswith("//") or not parsed.path:
        return None, None
    decoded_path = urllib.parse.unquote(parsed.path)
    candidate = (source.parent / decoded_path).resolve(strict=False)
    try:
        candidate.relative_to(root)
    except ValueError:
        return None, f"link escapes repository: {raw_target}"
    if candidate.is_dir():
        candidate = candidate / "README.md"
    return candidate, None


def verify(root: pathlib.Path) -> tuple[list[str], int, int, int]:
    failures: list[str] = []
    docs = root / "docs"
    if not docs.is_dir():
        return ["missing documentation directory: docs"], 0, 0, 0

    for entry in sorted(docs.iterdir(), key=lambda item: item.name):
        if entry.is_dir() and not entry.is_symlink():
            if entry.name not in APPROVED_DIRECTORIES:
                failures.append(f"unexpected top-level documentation category: docs/{entry.name}")
        elif entry.name not in FIXED_ROOT_FILES:
            failures.append(f"unexpected top-level documentation file: docs/{entry.name}")

    nodes = documentation_nodes(root)
    for relative in REQUIRED_INDEXES:
        if pathlib.PurePosixPath(relative) not in nodes:
            failures.append(f"missing required category index: {relative}")

    edges: dict[pathlib.PurePosixPath, set[pathlib.PurePosixPath]] = {
        node: set() for node in nodes
    }
    for node in sorted(nodes):
        if node.suffix != ".md" or node == ARTIFACT_TEMPLATE:
            continue
        source = root / node
        targets, parse_failures = markdown_targets(source)
        failures.extend(f"{node}: {failure}" for failure in parse_failures)
        for raw_target in targets:
            candidate, failure = local_candidate(raw_target, source, root)
            if failure:
                failures.append(f"{node}: {failure}")
                continue
            if candidate is None:
                continue
            relative = pathlib.PurePosixPath(candidate.relative_to(root).as_posix())
            if relative in nodes:
                edges[node].add(relative)

    ownership_indexes = sorted(
        node
        for node in nodes
        if node.name == "README.md"
        and node != pathlib.PurePosixPath("docs/README.md")
    )
    for index in ownership_indexes:
        index_directory = root / index.parent
        required_owned: set[pathlib.PurePosixPath] = set()
        for child in sorted(index_directory.iterdir(), key=lambda item: item.name):
            if child.is_file() and child.suffix in {".md", ".json"} and child.name != "README.md":
                required_owned.add(
                    pathlib.PurePosixPath(child.relative_to(root).as_posix())
                )
            elif child.is_dir() and not child.is_symlink():
                child_index = child / "README.md"
                child_relative = pathlib.PurePosixPath(
                    child_index.relative_to(root).as_posix()
                )
                if child_relative in nodes:
                    required_owned.add(child_relative)
        for omitted in sorted(required_owned - edges[index]):
            failures.append(f"{index}: category index omits immediate document: {omitted}")

    start = pathlib.PurePosixPath("docs/README.md")
    reachable: set[pathlib.PurePosixPath] = set()
    pending = [start] if start in nodes else []
    while pending:
        node = pending.pop()
        if node in reachable:
            continue
        reachable.add(node)
        pending.extend(sorted(edges[node] - reachable, reverse=True))
    for unreachable in sorted(nodes - reachable):
        failures.append(f"unreachable first-party document: {unreachable}")

    llms = root / "llms.txt"
    llms_targets: set[pathlib.PurePosixPath] = set()
    llms_link_count = 0
    if not llms.is_file():
        failures.append("missing robot documentation entry point: llms.txt")
    else:
        targets, parse_failures = markdown_targets(llms)
        failures.extend(f"llms.txt: {failure}" for failure in parse_failures)
        for raw_target in targets:
            candidate, failure = local_candidate(raw_target, llms, root)
            if failure:
                failures.append(f"llms.txt: {failure}")
                continue
            if candidate is None:
                continue
            llms_link_count += 1
            relative = pathlib.PurePosixPath(candidate.relative_to(root).as_posix())
            llms_targets.add(relative)
            if not candidate.exists():
                failures.append(f"llms.txt: missing local link target: {raw_target}")
        required_llms_paths = {
            pathlib.PurePosixPath(target) for target in REQUIRED_LLMS_TARGETS
        }
        for missing in sorted(required_llms_paths - llms_targets):
            failures.append(f"llms.txt: missing required documentation link: {missing}")

    return failures, len(nodes), len(reachable), llms_link_count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=pathlib.Path)
    args = parser.parse_args()

    root = args.root.resolve(strict=True)
    if not root.is_dir():
        parser.error(f"repository root is not a directory: {root}")

    failures, document_count, reachable_count, llms_link_count = verify(root)
    if failures:
        print("documentation structure verification failed:", file=sys.stderr)
        for failure in sorted(set(failures)):
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "documentation structure: verified "
        f"({document_count} documents, {reachable_count} reachable, "
        f"{len(REQUIRED_INDEXES)} required indexes, {llms_link_count} llms links)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
