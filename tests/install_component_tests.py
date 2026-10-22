#!/usr/bin/env python3
"""Verify the real CMake ava component install matches its package allowlist."""
from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import tempfile

from timeout_support import test_timeout


COMPONENT_FILES = {
    "bin/ava",
    "share/doc/ava/README.md",
    "share/doc/ava/LICENSE",
    "share/doc/ava/THIRD_PARTY_NOTICES.md",
    "share/doc/ava/docs/core/usage.md",
    "share/doc/ava/docs/core/configuration.md",
    "share/doc/ava/docs/core/context-resources.md",
    "share/doc/ava/docs/core/custom-providers.md",
    "share/doc/ava/docs/core/environment-variables.md",
    "share/doc/ava/docs/core/providers.md",
    "share/doc/ava/docs/core/subagents.md",
    "share/doc/ava/docs/core/thinking-modes.md",
    "share/doc/ava/docs/core/tools.md",
    "share/doc/ava/docs/interfaces/themes-keybindings.md",
    "share/doc/ava/docs/operations/testing.md",
    "share/doc/ava/docs/operations/terminal-setup.md",
    "share/doc/ava/docs/operations/troubleshooting.md",
    "share/doc/ava/docs/operations/diagnostics.md",
    "share/doc/ava/docs/operations/release-checklist.md",
    "share/doc/ava/docs/extensions/lsp.md",
    "share/doc/ava/docs/extensions/mcp.md",
    "share/doc/ava/docs/extensions/plugin-system.md",
    "share/doc/ava/docs/security/sandboxing.md",
    "share/doc/ava/docs/security/containment.md",
    "share/doc/ava/docs/development/session-versioning.md",
    "share/doc/ava/docs/development/side-effect-safety-checklist.md",
    "share/doc/ava/docs/headless-protocol.md",
    "share/doc/ava/docs/rpc-protocol.md",
    "share/doc/ava/docs/acp.md",
    "share/doc/ava/docs/acp-support.json",
    "share/doc/ava/docs/session-format.md",
    "share/doc/ava/docs/plugin-compatibility-policy.md",
    "share/doc/ava/docs/interop/evidence/README.md",
    "share/doc/ava/docs/interop/evidence/zed-1.9.0-2026-07-14.md",
    "share/doc/ava/docs/product/mvp-coverage-ledger.md",
    "share/doc/ava/docs/schema/theme.schema.json",
}


def installed_regular_files(prefix: pathlib.Path) -> set[str]:
    regular_files: set[str] = set()
    for path in prefix.rglob("*"):
        relative = path.relative_to(prefix).as_posix()
        if path.is_symlink():
            raise RuntimeError(f"CMake ava component installed a symlink: {relative}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise RuntimeError(f"CMake ava component installed a special file: {relative}")
        regular_files.add(relative)
    return regular_files


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, required=True)
    parser.add_argument("--ava", type=pathlib.Path, required=True)
    args = parser.parse_args()

    source = args.source.resolve()
    build = args.build.resolve()
    if not (source / "CMakeLists.txt").is_file() or not (build / "CMakeCache.txt").is_file():
        raise RuntimeError("install-component test requires configured source and build directories")
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    uses_libcwd = re.search(r"^OptionEnableLibcwd:[^=]+=ON$", cache, re.MULTILINE) is not None

    with tempfile.TemporaryDirectory(prefix="ava-install-component-") as directory:
        prefix = pathlib.Path(directory) / "prefix"
        result = subprocess.run(
            [args.cmake, "--install", str(build), "--prefix", str(prefix), "--component", "ava"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=test_timeout(60),
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"CMake ava component install failed ({result.returncode})\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        actual = installed_regular_files(prefix)
        if uses_libcwd:
            # Installing removes the build RPATH to the private debug library;
            # these builds still exercise the component's complete file allowlist.
            print("skipping installed AVA version probe: libcwd-enabled builds are not distributable; "
                  "the probe requires a non-libcwd build (including Release)")
        else:
            versions = []
            for binary in (args.ava.resolve(), prefix / "bin" / "ava"):
                version = subprocess.run(
                    [str(binary), "--version"], capture_output=True, text=True,
                    timeout=test_timeout(10), check=False,
                )
                if version.returncode != 0 or not version.stdout.strip():
                    raise RuntimeError(f"installed/build version probe failed for {binary}: {version.stderr}")
                versions.append(version.stdout.strip())
            if versions[0] != versions[1]:
                raise RuntimeError(f"installed AVA version differs from the build: {versions!r}")
    if actual != COMPONENT_FILES:
        raise RuntimeError(
            "CMake ava component allowlist mismatch\n"
            f"actual={sorted(actual)}\nexpected={sorted(COMPONENT_FILES)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
