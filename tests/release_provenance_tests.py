#!/usr/bin/env python3
"""Focused deterministic checks for release provenance qualification."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock

SOURCE = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ava_release_provenance", SOURCE / "scripts" / "generate-release-provenance.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load release provenance generator")
PROVENANCE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PROVENANCE
SPEC.loader.exec_module(PROVENANCE)
AVA_VERSION = PROVENANCE.project_version(SOURCE)
if AVA_VERSION is None:
    raise RuntimeError("cannot read AVA version from CMakeLists.txt")


def clean_dependencies() -> list[dict[str, object]]:
    revision = "f" * 40
    return [
        {
            "name": name,
            "path": path,
            "usage": PROVENANCE.DEPENDENCY_USAGE[name],
            "gitlink_revision_actual": revision,
            "worktree_revision": revision,
            "worktree_state": "clean",
            "license_id": license_id,
            "license_file": license_file,
            "license_sha256_expected": PROVENANCE.EXPECTED_LICENSE_SHA256[name],
            "license_sha256_actual": PROVENANCE.EXPECTED_LICENSE_SHA256[name],
        }
        for name, path, license_file, license_id in PROVENANCE.DIRECT_DEPENDENCIES
    ]


def minimal_elf(
    *,
    elf_class: int = 2,
    endian: str = "<",
    machine: int = 62,
    needed: tuple[bytes, ...] = (b"libc.so.6",),
    include_dynamic: bool = True,
    dynamic_file_size: int | None = None,
    include_null: bool = True,
    string_table_address: int | None = None,
) -> bytes:
    """Build a sectionless ELF whose dynamic data is visible to the loader."""
    if elf_class == 1:
        header_size, program_size, dynamic_size = 52, 32, 8
        program_format, dynamic_format = endian + "IIIIIIII", endian + "iI"
        phentsize_offset, phnum_offset = 42, 44
    elif elf_class == 2:
        header_size, program_size, dynamic_size = 64, 56, 16
        program_format, dynamic_format = endian + "IIQQQQQQ", endian + "qQ"
        phentsize_offset, phnum_offset = 54, 56
    else:
        raise ValueError("unsupported ELF class")

    phnum = 2 if include_dynamic else 1
    phoff = header_size
    dynamic_offset = (phoff + phnum * program_size + dynamic_size - 1) // dynamic_size * dynamic_size
    base_address = 0x400000
    string_table = bytearray(b"\0")
    needed_offsets: list[int] = []
    for name in needed:
        needed_offsets.append(len(string_table))
        string_table.extend(name)
        string_table.append(0)

    entries: list[tuple[int, int]] = []
    if include_dynamic:
        string_offset = dynamic_offset + dynamic_size * (2 + len(needed_offsets) + int(include_null))
        entries = [(5, string_table_address if string_table_address is not None else base_address + string_offset),
                   (10, len(string_table))]
        entries.extend((1, offset) for offset in needed_offsets)
        if include_null:
            entries.append((0, 0))
        dynamic_data = b"".join(struct.pack(dynamic_format, tag, value) for tag, value in entries)
    else:
        dynamic_data = b""
        string_table = bytearray()

    total_size = dynamic_offset + len(dynamic_data) + len(string_table)
    data = bytearray(total_size)
    data[:9] = b"\x7fELF" + bytes((elf_class, 1 if endian == "<" else 2, 1, 0, 0))
    struct.pack_into(endian + "H", data, 18, machine)
    if elf_class == 1:
        struct.pack_into(endian + "I", data, 28, phoff)
    else:
        struct.pack_into(endian + "Q", data, 32, phoff)
    struct.pack_into(endian + "HH", data, phentsize_offset, program_size, phnum)

    if elf_class == 1:
        load_fields = (1, 0, base_address, 0, total_size, total_size, 5, 0x1000)
    else:
        load_fields = (1, 5, 0, base_address, 0, total_size, total_size, 0x1000)
    struct.pack_into(program_format, data, phoff, *load_fields)
    if include_dynamic:
        segment_file_size = len(dynamic_data) if dynamic_file_size is None else dynamic_file_size
        if elf_class == 1:
            dynamic_fields = (2, dynamic_offset, base_address + dynamic_offset, 0,
                              segment_file_size, segment_file_size, 4, dynamic_size)
        else:
            dynamic_fields = (2, 4, dynamic_offset, base_address + dynamic_offset, 0,
                              segment_file_size, segment_file_size, dynamic_size)
        struct.pack_into(program_format, data, phoff + program_size, *dynamic_fields)
        data[dynamic_offset:dynamic_offset + len(dynamic_data)] = dynamic_data
        data[dynamic_offset + len(dynamic_data):] = string_table
    return bytes(data)


def with_fabricated_empty_dynamic_section(contents: bytes) -> bytes:
    """Add the section-header pattern the previous parser incorrectly trusted."""
    data = bytearray(contents)
    section_offset = len(data)
    data.extend(bytes(3 * 64))
    struct.pack_into("<QHH", data, 40, section_offset, 64, 3)
    struct.pack_into("<IIQQQQIIQQ", data, section_offset + 64,
                     0, 6, 0, 0, section_offset, 0, 2, 0, 0, 0)
    struct.pack_into("<IIQQQQIIQQ", data, section_offset + 2 * 64,
                     0, 3, 0, 0, section_offset, 0, 0, 0, 0, 0)
    return bytes(data)


class ReleaseProvenanceTests(unittest.TestCase):
    def test_adopted_ciede2000_notice_is_distributed(self) -> None:
        notices = (SOURCE / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        section = notices.split("## CIEDE2000 — MIT\n", 1)[1].split("\n## ", 1)[0]
        self.assertIn("src/ava/tui/terminal/CIEDE2000.cxx", section)
        self.assertIn("https://github.com/gfiumara/CIEDE2000", section)
        self.assertIn("Copyright (c) 2015 Gregory Fiumara", section)
        self.assertIn("Covered by the MIT License text below.", section)
        self.assertIn("nlohmann JSON, and CIEDE2000)", notices)
        self.assertIn("Permission is hereby granted, free of charge", notices)

    def collect(self, *, build_mode: str = "source-build", qualification_mode: bool = True, binary_version: str | None = AVA_VERSION, host_architecture: str | None = "x86_64", architecture: str | None = "x86_64", needed: list[str] | None = None, dependency_reasons: list[str] | None = None) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "ava"
            binary.write_bytes(b"deterministic provenance fixture\n")
            with mock.patch.object(PROVENANCE, "run_git", return_value="f" * 40), mock.patch.object(
                PROVENANCE, "worktree_dirty", return_value=False
            ), mock.patch.object(
                PROVENANCE, "dependency_records", return_value=(clean_dependencies(), dependency_reasons or [])
            ):
                return PROVENANCE.collect_provenance(
                    SOURCE, binary, build_mode, host_architecture=host_architecture,
                    qualification_mode=qualification_mode,
                    binary_version=binary_version, architecture=architecture,
                    needed=needed if needed is not None else sorted(PROVENANCE.HOST_DYNAMIC_ALLOWLIST),
                )

    def test_schema_is_deterministic_and_private(self) -> None:
        first = self.collect()
        second = self.collect()
        self.assertEqual(first, second)
        self.assertEqual(first["schema_version"], 3)
        self.assertEqual(first["ava_version"], AVA_VERSION)
        self.assertEqual(first["host_architecture"], "x86_64")
        self.assertIs(first["host_architecture_matches_binary"], True)
        self.assertTrue(first["release_qualified"])
        self.assertNotIn(str(SOURCE), json.dumps(first))

    def test_dependency_and_source_worktree_state_fail_closed(self) -> None:
        dirty = self.collect(dependency_reasons=["dependency:utils:worktree-state-not-clean"])
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "ava"
            binary.write_bytes(b"source dirty fixture\n")
            with mock.patch.object(PROVENANCE, "run_git", return_value="f" * 40), mock.patch.object(
                PROVENANCE, "worktree_dirty", return_value=True
            ), mock.patch.object(PROVENANCE, "dependency_records", return_value=(clean_dependencies(), [])):
                source_dirty = PROVENANCE.collect_provenance(
                    SOURCE, binary, "source-build", host_architecture="x86_64",
                    qualification_mode=True, binary_version=AVA_VERSION, architecture="x86_64",
                    needed=sorted(PROVENANCE.HOST_DYNAMIC_ALLOWLIST),
                )
        self.assertFalse(dirty["release_qualified"])
        self.assertIn("dependency:utils:worktree-state-not-clean", dirty["qualification_reasons"])
        self.assertFalse(source_dirty["release_qualified"])
        self.assertIn("source-worktree-state-not-clean", source_dirty["qualification_reasons"])

    def test_license_hash_policy_mismatch_fails_closed(self) -> None:
        dependency = PROVENANCE.DIRECT_DEPENDENCIES[0]
        name, relative_path, license_path, _license_id = dependency
        with tempfile.TemporaryDirectory() as directory:
            repo = pathlib.Path(directory)
            (repo / "CMakeLists.txt").write_text("project(ava VERSION 1.0.0)\n", encoding="utf-8")
            license_file = repo / relative_path / license_path
            license_file.parent.mkdir(parents=True)
            license_file.write_text("changed license evidence\n", encoding="utf-8")
            binary = repo / "ava"
            binary.write_bytes(b"license hash fixture\n")
            with mock.patch.object(PROVENANCE, "DIRECT_DEPENDENCIES", (dependency,)), mock.patch.object(
                PROVENANCE, "gitlink_revision", return_value="f" * 40
            ), mock.patch.object(
                PROVENANCE, "run_git", return_value="f" * 40
            ), mock.patch.object(
                PROVENANCE, "worktree_dirty", return_value=False
            ):
                provenance = PROVENANCE.collect_provenance(
                    repo, binary, "source-build", host_architecture="x86_64",
                    qualification_mode=True, binary_version="1.0.0", architecture="x86_64",
                    needed=sorted(PROVENANCE.HOST_DYNAMIC_ALLOWLIST),
                )
        record = provenance["direct_dependencies"][0]
        self.assertEqual(record["gitlink_revision_actual"], "f" * 40)
        self.assertEqual(record["license_sha256_expected"], PROVENANCE.EXPECTED_LICENSE_SHA256[name])
        self.assertEqual(record["license_sha256_actual"], hashlib.sha256(b"changed license evidence\n").hexdigest())
        self.assertFalse(provenance["release_qualified"])
        self.assertIn(f"dependency:{name}:license-sha256-mismatch", provenance["qualification_reasons"])

    def test_worktree_revision_must_match_committed_gitlink(self) -> None:
        dependency = PROVENANCE.DIRECT_DEPENDENCIES[0]
        name, relative_path, license_path, _license_id = dependency
        with tempfile.TemporaryDirectory() as directory:
            repo = pathlib.Path(directory)
            license_file = repo / relative_path / license_path
            license_file.parent.mkdir(parents=True)
            license_file.write_bytes((SOURCE / relative_path / license_path).read_bytes())
            with mock.patch.object(PROVENANCE, "DIRECT_DEPENDENCIES", (dependency,)), mock.patch.object(
                PROVENANCE, "gitlink_revision", return_value="a" * 40
            ), mock.patch.object(PROVENANCE, "run_git", return_value="b" * 40), mock.patch.object(
                PROVENANCE, "worktree_dirty", return_value=False
            ):
                records, reasons = PROVENANCE.dependency_records(repo)
        self.assertEqual(records[0]["gitlink_revision_actual"], "a" * 40)
        self.assertEqual(records[0]["worktree_revision"], "b" * 40)
        self.assertIn(f"dependency:{name}:revision-mismatch", reasons)

    def test_worktree_state_includes_untracked_files(self) -> None:
        with mock.patch.object(PROVENANCE, "run_git", return_value="?? CMakeLists.txt\n") as run_git:
            self.assertTrue(PROVENANCE.worktree_dirty(SOURCE))
        run_git.assert_called_once_with(SOURCE, "status", "--porcelain", "--untracked-files=all")

    def test_dependency_metadata_covers_build_tool_usage(self) -> None:
        self.assertEqual(
            PROVENANCE.EXPECTED_LICENSE_SHA256["aicxx"],
            "dbe888a4dac5018ae7a4beb1ecfacd89de8d7abc7193024b95b1a0d2d6a45fe8",
        )
        records = clean_dependencies()
        self.assertEqual(next(record for record in records if record["name"] == "aicxx")["usage"], "build-tool")

    def test_elf_metadata_reads_loader_visible_needed_names_without_sections(self) -> None:
        # There are intentionally no section headers: the loader uses program
        # headers, so a valid PT_DYNAMIC must still qualify for parsing.
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "sectionless-dynamic-elf"
            binary.write_bytes(minimal_elf())
            self.assertEqual(PROVENANCE.elf_metadata(binary), ("x86_64", ["libc.so.6"]))

    def test_built_ava_dependencies_match_system_elf_inspection(self) -> None:
        binary = pathlib.Path(os.environ.get("AVA_RELEASE_TEST_BINARY", SOURCE / "build" / "ava"))
        readelf = shutil.which("readelf")
        if not sys.platform.startswith("linux") or not binary.is_file() or readelf is None:
            self.skipTest("built Linux AVA and readelf are required")

        architecture, needed = PROVENANCE.elf_metadata(binary)
        output = subprocess.run(
            [readelf, "--dynamic", str(binary)],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        inspected: list[str] = []
        for line in output.splitlines():
            if "(NEEDED)" not in line:
                continue
            start = line.find("[")
            end = line.find("]", start + 1)
            self.assertGreaterEqual(start, 0)
            self.assertGreater(end, start)
            inspected.append(line[start + 1 : end])

        self.assertIsNotNone(architecture)
        self.assertTrue(needed)
        self.assertEqual(needed, sorted(inspected))

    def test_elf_metadata_supports_elf32_and_big_endian(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "elf32-big-endian"
            binary.write_bytes(minimal_elf(elf_class=1, endian=">", machine=3))
            self.assertEqual(PROVENANCE.elf_metadata(binary), ("x86", ["libc.so.6"]))

    def test_elf_metadata_rejects_static_and_malformed_dynamic_segments(self) -> None:
        fixtures = {
            "static": minimal_elf(include_dynamic=False),
            "fabricated-empty-section": with_fabricated_empty_dynamic_section(minimal_elf(include_dynamic=False)),
            "zero-sized": minimal_elf(dynamic_file_size=0),
            "non-entry-aligned": minimal_elf(dynamic_file_size=1),
            "unterminated": minimal_elf(include_null=False),
            "unmapped-strtab": minimal_elf(string_table_address=0x900000),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name, contents in fixtures.items():
                with self.subTest(name=name):
                    binary = root / name
                    binary.write_bytes(contents)
                    self.assertEqual(PROVENANCE.elf_metadata(binary), ("x86_64", None))

    def test_elf_metadata_rejects_unterminated_or_non_utf8_needed_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            missing_nul = bytearray(minimal_elf())
            missing_nul[-1] = ord("x")
            invalid_utf8 = minimal_elf(needed=(b"\xff",))
            for name, contents in (("missing-nul", missing_nul), ("invalid-utf8", invalid_utf8)):
                with self.subTest(name=name):
                    binary = root / name
                    binary.write_bytes(contents)
                    self.assertEqual(PROVENANCE.elf_metadata(binary), ("x86_64", None))

    def test_elf_metadata_allows_zero_needed_with_complete_dynamic_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "zero-needed"
            binary.write_bytes(minimal_elf(needed=()))
            self.assertEqual(PROVENANCE.elf_metadata(binary), ("x86_64", []))

    def test_unexpected_loader_dependency_fails_qualification(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "unexpected-needed"
            binary.write_bytes(minimal_elf(needed=(b"libc.so.6", b"libsurprise.so.1")))
            with mock.patch.object(PROVENANCE, "run_git", return_value="f" * 40), mock.patch.object(
                PROVENANCE, "worktree_dirty", return_value=False
            ), mock.patch.object(
                PROVENANCE, "dependency_records", return_value=(clean_dependencies(), [])
            ):
                provenance = PROVENANCE.collect_provenance(
                    SOURCE, binary, "source-build", host_architecture="x86_64",
                    qualification_mode=True, binary_version=AVA_VERSION
                )
        self.assertEqual(provenance["elf_dt_needed"], ["libc.so.6", "libsurprise.so.1"])
        self.assertEqual(provenance["unexpected_dynamic_dependencies"], ["libsurprise.so.1"])
        self.assertFalse(provenance["release_qualified"])
        self.assertIn("unexpected-dynamic-dependency", provenance["qualification_reasons"])

    def test_matching_native_architectures_are_qualified(self) -> None:
        self.assertEqual(PROVENANCE.QUALIFIED_ARCHITECTURES, frozenset({"x86_64", "aarch64"}))
        for architecture in ("x86_64", "aarch64"):
            with self.subTest(architecture=architecture):
                provenance = self.collect(
                    host_architecture=architecture,
                    architecture=architecture,
                )
                self.assertEqual(provenance["host_architecture"], architecture)
                self.assertEqual(provenance["architecture"], architecture)
                self.assertIs(provenance["host_architecture_matches_binary"], True)
                self.assertTrue(provenance["release_qualified"])
                self.assertEqual(provenance["qualification_reasons"], [])

    def test_aarch64_loader_is_allowed_only_for_aarch64(self) -> None:
        loader = "ld-linux-aarch64.so.1"
        aarch64 = self.collect(
            host_architecture="aarch64",
            architecture="aarch64",
            needed=["libc.so.6", loader],
        )
        x86_64 = self.collect(needed=["libc.so.6", loader])
        self.assertTrue(aarch64["release_qualified"])
        self.assertIn(loader, aarch64["host_dynamic_dependency_allowlist"])
        self.assertEqual(aarch64["unexpected_dynamic_dependencies"], [])
        self.assertFalse(x86_64["release_qualified"])
        self.assertNotIn(loader, x86_64["host_dynamic_dependency_allowlist"])
        self.assertEqual(x86_64["unexpected_dynamic_dependencies"], [loader])

    def test_host_binary_architecture_mismatches_fail_closed(self) -> None:
        for host_architecture, binary_architecture in (
            ("x86_64", "aarch64"),
            ("aarch64", "x86_64"),
        ):
            with self.subTest(host=host_architecture, binary=binary_architecture):
                provenance = self.collect(
                    host_architecture=host_architecture,
                    architecture=binary_architecture,
                )
                self.assertEqual(provenance["host_architecture"], host_architecture)
                self.assertEqual(provenance["architecture"], binary_architecture)
                self.assertIs(provenance["host_architecture_matches_binary"], False)
                self.assertFalse(provenance["release_qualified"])
                self.assertEqual(
                    provenance["qualification_reasons"],
                    ["host-binary-architecture-mismatch"],
                )

    def test_missing_and_unsupported_architecture_evidence_fails_closed(self) -> None:
        fixtures = (
            ("absent-host", None, "x86_64", None, ["host-architecture-missing"]),
            (
                "unsupported-host",
                "riscv64",
                "x86_64",
                False,
                ["host-architecture-unsupported", "host-binary-architecture-mismatch"],
            ),
            ("missing-binary", "x86_64", None, None, ["binary-architecture-missing"]),
            (
                "unsupported-binary",
                "x86_64",
                "riscv64",
                False,
                ["binary-architecture-unsupported", "host-binary-architecture-mismatch"],
            ),
        )
        for name, host_architecture, binary_architecture, matches, reasons in fixtures:
            with self.subTest(name=name):
                provenance = self.collect(
                    host_architecture=host_architecture,
                    architecture=binary_architecture,
                )
                self.assertEqual(provenance["host_architecture"], host_architecture)
                self.assertEqual(provenance["architecture"], binary_architecture)
                self.assertIs(provenance["host_architecture_matches_binary"], matches)
                self.assertFalse(provenance["release_qualified"])
                self.assertEqual(provenance["qualification_reasons"], sorted(reasons))

    def test_dynamic_dependencies_are_a_qualification_gate(self) -> None:
        allowed_subset = self.collect(needed=["libc.so.6"])
        unexpected = self.collect(needed=["libc.so.6", "libsurprise.so.1"])
        self.assertTrue(allowed_subset["release_qualified"])
        self.assertEqual(allowed_subset["unexpected_dynamic_dependencies"], [])
        self.assertFalse(unexpected["release_qualified"])
        self.assertEqual(unexpected["unexpected_dynamic_dependencies"], ["libsurprise.so.1"])
        self.assertIn("unexpected-dynamic-dependency", unexpected["qualification_reasons"])

    def test_non_strict_and_supplied_binary_cannot_be_qualified(self) -> None:
        non_strict = self.collect(qualification_mode=False)
        self.assertFalse(non_strict["release_qualified"])
        self.assertIn("qualification-mode-not-requested", non_strict["qualification_reasons"])
        supplied = self.collect(build_mode="supplied-binary")
        self.assertFalse(supplied["release_qualified"])
        self.assertIn("build-mode-not-source-build", supplied["qualification_reasons"])
        wrong_version = self.collect(binary_version="9.9.9")
        self.assertFalse(wrong_version["release_qualified"])
        self.assertIn("binary-version-mismatch-or-unavailable", wrong_version["qualification_reasons"])
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                [str(SOURCE / "scripts" / "package-linux.sh"), "--binary", str(pathlib.Path(directory) / "ava"),
                 "--require-release-qualified", "--output-dir", str(pathlib.Path(directory) / "output")],
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rejects supplied-binary mode", result.stderr)


if __name__ == "__main__":
    program = unittest.main(exit=False)
    raise SystemExit(1 if not program.result.wasSuccessful() else 77 if program.result.skipped else 0)
