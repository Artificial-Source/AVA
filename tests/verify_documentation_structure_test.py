#!/usr/bin/env python3
"""Focused tests for scripts/verify-documentation-structure.py."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


FIXED_ROOT_DOCS = (
    "AGENTS.md",
    "acp.md",
    "acp-support.json",
    "rpc-protocol.md",
    "headless-protocol.md",
    "session-format.md",
    "plugin-compatibility-policy.md",
)
REQUIRED_INDEXES = (
    "core/README.md",
    "interfaces/README.md",
    "desktop/README.md",
    "extensions/README.md",
    "operations/README.md",
    "development/README.md",
    "development/internals/README.md",
    "engineering/README.md",
    "security/README.md",
    "product/README.md",
    "plans/README.md",
    "roadmap/README.md",
    "history/README.md",
    "schema/README.md",
    "goals/README.md",
    "versions/README.md",
    "interop/README.md",
    "interop/evidence/README.md",
)
REQUIRED_LLMS = (
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
)


class DocumentationStructureTests(unittest.TestCase):
    script: Path

    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name) / "repo"
        self.root.mkdir()
        self.create_valid_repository()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def write(self, relative: str, content: str = "# Document\n") -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def create_valid_repository(self) -> None:
        for relative in FIXED_ROOT_DOCS:
            content = "{}\n" if relative.endswith(".json") else "# Document\n"
            self.write(f"docs/{relative}", content)
        for relative in REQUIRED_INDEXES:
            self.write(f"docs/{relative}")
        self.write(
            "docs/development/README.md",
            "# Development\n\n[internals](internals/README.md)\n",
        )
        self.write(
            "docs/engineering/backend-modernization-progress.md",
        )
        self.write(
            "docs/engineering/README.md",
            "# Engineering\n\n[backend modernization](backend-modernization-progress.md)\n",
        )
        self.write(
            "docs/interop/README.md",
            "# Interop\n\n[evidence](evidence/README.md)\n",
        )

        spine_targets = [*FIXED_ROOT_DOCS, *REQUIRED_INDEXES]
        self.write(
            "docs/README.md",
            "# Documentation\n\n"
            + "".join(f"- [{target}]({target})\n" for target in spine_targets),
        )
        self.write(
            "llms.txt",
            "# Documentation map\n\n"
            + "".join(f"- [{target}]({target})\n" for target in REQUIRED_LLMS),
        )

    def run_checker(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(self.script), str(self.root)],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def test_happy_path(self) -> None:
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("documentation structure: verified (", result.stdout)
        self.assertIn("18 required indexes, 22 llms links", result.stdout)

    def test_missing_required_index(self) -> None:
        (self.root / "docs/core/README.md").unlink()
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing required category index: docs/core/README.md", result.stderr)

    def test_unexpected_root_document(self) -> None:
        self.write("docs/loose.md")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("unexpected top-level documentation file: docs/loose.md", result.stderr)

    def test_unexpected_root_category(self) -> None:
        self.write("docs/misc/README.md")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("unexpected top-level documentation category: docs/misc", result.stderr)

    def test_unreachable_document(self) -> None:
        self.write("docs/core/nested/orphan.md")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("unreachable first-party document: docs/core/nested/orphan.md", result.stderr)

    def test_engineering_index_must_link_immediate_document(self) -> None:
        self.write("docs/engineering/new-program.md")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "docs/engineering/README.md: category index omits immediate document: docs/engineering/new-program.md",
            result.stderr,
        )

    def test_engineering_category_is_unreachable_without_human_spine_link(self) -> None:
        spine = self.root / "docs/README.md"
        spine.write_text(
            spine.read_text(encoding="utf-8").replace("engineering/README.md", "missing-engineering-index.md"),
            encoding="utf-8",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "unreachable first-party document: docs/engineering/README.md",
            result.stderr,
        )

    def test_desktop_index_must_link_immediate_document(self) -> None:
        self.write("docs/desktop/architecture.md")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "docs/desktop/README.md: category index omits immediate document: docs/desktop/architecture.md",
            result.stderr,
        )

    def test_desktop_category_is_unreachable_without_human_spine_link(self) -> None:
        spine = self.root / "docs/README.md"
        spine.write_text(
            spine.read_text(encoding="utf-8").replace("desktop/README.md", "missing-desktop-index.md"),
            encoding="utf-8",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "unreachable first-party document: docs/desktop/README.md",
            result.stderr,
        )

    def test_category_index_must_link_immediate_document(self) -> None:
        self.write("docs/core/usage.md")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "docs/core/README.md: category index omits immediate document: docs/core/usage.md",
            result.stderr,
        )

    def test_category_index_must_link_immediate_child_index(self) -> None:
        self.write("docs/core/nested/README.md")
        spine = self.root / "docs/README.md"
        spine.write_text(
            spine.read_text(encoding="utf-8")
            + "- [nested](core/nested/README.md)\n",
            encoding="utf-8",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "docs/core/README.md: category index omits immediate document: docs/core/nested/README.md",
            result.stderr,
        )

    def test_json_document_is_a_reachable_terminal(self) -> None:
        self.write("docs/core/catalog.json", '"[not a link](missing.md)"\n')
        self.write(
            "docs/core/README.md",
            "# Core\n\n[catalog](catalog.json)\n",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_llms_rejects_broken_local_link(self) -> None:
        llms = self.root / "llms.txt"
        llms.write_text(llms.read_text(encoding="utf-8") + "[missing](docs/missing.md)\n", encoding="utf-8")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("llms.txt: missing local link target: docs/missing.md", result.stderr)

    def test_llms_rejects_escaping_local_link(self) -> None:
        llms = self.root / "llms.txt"
        llms.write_text(llms.read_text(encoding="utf-8") + "[outside](../outside.md)\n", encoding="utf-8")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1)
        self.assertIn("llms.txt: link escapes repository: ../outside.md", result.stderr)

    def test_reference_code_is_excluded(self) -> None:
        self.write("docs/reference-code/vendor/orphan.md", "[broken](missing.md)\n")
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_artifact_template_is_reachable_but_not_traversed(self) -> None:
        self.write(
            "docs/operations/release-artifact-readme.md",
            "# Artifact\n\n[relocated target](docs/core/missing.md)\n",
        )
        self.write(
            "docs/operations/README.md",
            "# Operations\n\n[artifact](release-artifact-readme.md)\n",
        )
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True, type=Path)
    arguments = parser.parse_args()
    DocumentationStructureTests.script = arguments.script.resolve(strict=True)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(DocumentationStructureTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
