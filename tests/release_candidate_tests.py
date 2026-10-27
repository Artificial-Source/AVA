#!/usr/bin/env python3
"""Black-box regressions for sealing, promotion and the artifact floor."""
from __future__ import annotations

import hashlib
import io
import json
import os
import pathlib
import re
import subprocess
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
CANDIDATE = ROOT / "scripts/release-candidate.py"
INSPECT = ROOT / "scripts/inspect-linux-artifact.py"


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ava-candidate-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.version = "9.8.7"
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.build = self.root / "build"
        (self.build / "tests").mkdir(parents=True)
        for args in (("init", "-q"), ("config", "user.name", "Fixture"),
                     ("config", "user.email", "fixture@example.invalid"),
                     ("commit", "-q", "--allow-empty", "-m", "Fixture")):
            subprocess.run(["git", "-C", str(self.repo), *args], check=True)
        self.revision = subprocess.check_output(["git", "-C", str(self.repo), "rev-parse", "HEAD"], text=True).strip()
        (self.build / "release-source-revision").write_text(self.revision + "\n")
        (self.build / "ava").write_bytes(b"tested executable bytes")
        (self.build / "tests/ava_fake_provider_server").write_bytes(b"tested helper bytes")
        (self.build / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.repo}\nCMAKE_BUILD_TYPE:STRING=Release\n"
            "AVA_RELEASE_ARTIFACT:BOOL=ON\nAVA_ENABLE_GITACHE:BOOL=OFF\nEnableLibcwd:BOOL=OFF\n"
            "FETCHCONTENT_FULLY_DISCONNECTED:BOOL=ON\nCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json:BOOL=ON\n")
        self.directory = self.root / "retained"
        self.directory.mkdir()

    def run_candidate(self, operation, *args, success=True):
        result = subprocess.run(["python3", str(CANDIDATE), operation, "--repo", str(self.repo),
                                 "--build-dir", str(self.build), "--directory", str(self.directory), *args],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def package(self):
        binary = (self.build / "ava").read_bytes()
        provenance = {"source": {"revision": self.revision}, "release_qualified": True,
                      "binary_sha256": hashlib.sha256(binary).hexdigest(), "architecture": "x86_64"}
        archive = self.directory / f"ava-{self.version}-linux-x64.tar.gz"
        with tarfile.open(archive, "w:gz") as stream:
            for name, data in (("bin/ava", binary), ("share/doc/ava/PROVENANCE.json", json.dumps(provenance).encode())):
                member = tarfile.TarInfo(f"ava-{self.version}-linux-x64/" + name)
                member.size = len(data)
                stream.addfile(member, io.BytesIO(data))
        archive.with_name(archive.name + ".sha256").write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n")
        self.run_candidate("record")
        return hashlib.sha256((self.directory / "candidate.json").read_bytes()).hexdigest()

    def test_unchanged_build_and_download_are_accepted(self):
        self.run_candidate("seal")
        self.run_candidate("verify-build")
        manifest = self.package()
        self.run_candidate("verify", "--manifest-sha256", manifest, "--revision", self.revision)

    def test_seal_cannot_be_silently_replaced(self):
        self.run_candidate("seal")
        self.run_candidate("seal", success=False)

    def test_changed_executable_is_rejected(self):
        self.run_candidate("seal")
        (self.build / "ava").write_bytes(b"rebuilt executable")
        self.assertIn("sealed build changed", self.run_candidate("verify-build", success=False).stderr)

    def test_changed_helper_is_rejected(self):
        self.run_candidate("seal")
        (self.build / "tests/ava_fake_provider_server").write_bytes(b"different helper")
        self.run_candidate("verify-build", success=False)

    def test_wrong_configure_commit_is_rejected(self):
        (self.build / "release-source-revision").write_text("0" * 40)
        self.run_candidate("seal", success=False)

    def test_dirty_source_is_rejected(self):
        (self.repo / "untracked.cpp").write_text("source")
        self.run_candidate("seal", success=False)

    def test_non_candidate_configuration_is_rejected(self):
        cache = self.build / "CMakeCache.txt"
        cache.write_text(cache.read_text().replace("Release", "Debug"))
        self.run_candidate("seal", success=False)

    def test_download_tampering_is_rejected(self):
        self.run_candidate("seal")
        manifest = self.package()
        archive = next(self.directory.glob("*.tar.gz"))
        archive.write_bytes(archive.read_bytes() + b"changed")
        self.run_candidate("verify", "--manifest-sha256", manifest, "--revision", self.revision, success=False)

    def test_wrong_manifest_or_commit_is_rejected(self):
        self.run_candidate("seal")
        manifest = self.package()
        self.run_candidate("verify", "--manifest-sha256", "0" * 64, "--revision", self.revision, success=False)
        self.run_candidate("verify", "--manifest-sha256", manifest, "--revision", "0" * 40, success=False)

    def draft_arguments(self):
        (self.build / "ava").write_text(f"#!/bin/sh\nprintf 'ava {self.version}\\n'\n")
        self.run_candidate("seal")
        manifest = self.package()
        self.directory.chmod(0o700)
        body = self.root / "release-body.md"
        body.write_text((ROOT / "docs/operations/release-body-template.md").read_text())
        return ["draft", "--dry-run", "--revision", self.revision,
                "--manifest-sha256", manifest, "--repository", "example/ava",
                "--body", str(body), "--output", str(self.root / "draft-plan")]

    def test_draft_dry_run_renders_notes_without_running_remote_commands(self):
        arguments = self.draft_arguments()
        manifest = arguments[5]
        output = self.root / "draft-plan"
        result = self.run_candidate(*arguments)
        self.assertIn("no remote commands were run", result.stdout)
        notes = (output / "release-body.md").read_text()
        self.assertIn(f"# AVA {self.version}\n", notes)
        self.assertIn(f"ava-{self.version}-linux-x64.tar.gz", notes)
        self.assertIn(f"`ava {self.version}`", notes)
        self.assertIn(self.revision, notes)
        self.assertIn(manifest, notes)
        self.assertNotIn("@AVA_", notes)
        commands = (output / "draft-commands.sh").read_text()
        self.assertIn(f"gh release create v{self.version} --repo example/ava --draft", commands)
        self.assertIn("--verify-tag", commands)
        self.assertIn("git ls-remote", commands)
        self.assertIn("gh api --paginate", commands)
        self.assertNotIn("--clobber", commands)
        self.assertNotIn("cmake", commands)
        self.assertFalse(subprocess.run(
            ["git", "-C", str(self.repo), "show-ref", "--verify", "--quiet",
             f"refs/tags/v{self.version}"]).returncode == 0)

    def test_draft_preflight_rejects_existing_release_in_large_listing(self):
        self.run_candidate(*self.draft_arguments())
        tools = self.root / "tools"
        tools.mkdir()
        releases = self.root / "remote-releases.txt"
        releases.write_text(f"v{self.version}\n" + "other-version\n" * 20000)
        (tools / "gh").write_text(f'#!/bin/sh\ncat "{releases}"\n')
        (tools / "git").write_text(
            '#!/bin/sh\nif [ "$1" = ls-remote ]; then exit 0; fi\n'
            'echo "unexpected remote write" >&2\nexit 99\n')
        for executable in tools.iterdir():
            executable.chmod(0o700)
        result = subprocess.run(
            ["bash", str(self.root / "draft-plan/draft-commands.sh")],
            env={**os.environ, "PATH": str(tools) + ":/usr/bin:/bin"},
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("Release or draft already exists", result.stderr)

    def test_draft_requires_explicit_dry_run(self):
        arguments = self.draft_arguments()
        arguments.remove("--dry-run")
        self.assertIn("requires --dry-run", self.run_candidate(*arguments, success=False).stderr)

    def test_draft_rejects_changed_manifest(self):
        arguments = self.draft_arguments()
        (self.directory / "candidate.json").write_text("{}\n")
        self.assertIn("manifest digest mismatch", self.run_candidate(*arguments, success=False).stderr)
        self.assertFalse((self.root / "draft-plan").exists())

    def test_draft_rejects_existing_plan_without_changing_it(self):
        arguments = self.draft_arguments()
        self.run_candidate(*arguments)
        notes = self.root / "draft-plan/release-body.md"
        before = notes.read_bytes()
        self.run_candidate(*arguments, success=False)
        self.assertEqual(notes.read_bytes(), before)

    def test_draft_rejects_existing_local_tag(self):
        arguments = self.draft_arguments()
        subprocess.run(["git", "-C", str(self.repo), "tag", f"v{self.version}"], check=True)
        self.assertIn("local tag already exists", self.run_candidate(*arguments, success=False).stderr)

    def test_draft_rejects_extra_or_symlink_inputs(self):
        arguments = self.draft_arguments()
        extra = self.directory / "unexpected"
        extra.write_text("not an approved release asset")
        self.assertIn("unexpected files", self.run_candidate(*arguments, success=False).stderr)
        extra.unlink()
        extra.symlink_to(self.root / "release-body.md")
        self.assertIn("not symlinks", self.run_candidate(*arguments, success=False).stderr)

    def test_draft_rejects_incomplete_notes(self):
        arguments = self.draft_arguments()
        (self.root / "release-body.md").write_text("# AVA @AVA_VERSION@\nNo identity\n")
        self.assertIn("missing @AVA_COMMIT@", self.run_candidate(*arguments, success=False).stderr)

    def test_draft_rejects_wrong_body_version(self):
        arguments = self.draft_arguments()
        body = self.root / "release-body.md"
        body.write_text(body.read_text().replace("# AVA @AVA_VERSION@", "# AVA 9.0.0"))
        self.assertIn("body version differs", self.run_candidate(*arguments, success=False).stderr)

    def test_draft_rejects_missing_version_placeholder(self):
        arguments = self.draft_arguments()
        body = self.root / "release-body.md"
        body.write_text(body.read_text().replace("@AVA_VERSION@", self.version))
        self.assertIn("missing @AVA_VERSION@", self.run_candidate(*arguments, success=False).stderr)

    def test_supplied_binary_cannot_claim_reused_build(self):
        result = subprocess.run([str(ROOT / "scripts/package-linux.sh"), "--build-dir", str(self.build),
                                 "--binary", str(self.build / "ava")], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot be combined", result.stderr)


class InspectionTests(unittest.TestCase):
    def setUp(self):
        binary = os.environ.get("AVA_RELEASE_TEST_BINARY")
        if not binary:
            self.skipTest("inspection mutations require the built AVA binary")
        self.binary = pathlib.Path(binary)
        self.temp = tempfile.TemporaryDirectory(prefix="ava-inspection-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def reject_instruction(self, data, diagnostic):
        section = self.root / "extension.bin"
        section.write_bytes(data)
        binary = self.root / "ava-mutated"
        # Mutate the first executable section so negative cases do not scan
        # AVA's complete text eight times before reaching the injected opcode.
        subprocess.run(["objcopy", "--update-section", f".init={section}",
                        str(self.binary), str(binary)], check=True, capture_output=True)
        result = subprocess.run(["python3", str(INSPECT), str(binary)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(diagnostic, result.stderr)

    def test_avx512_xmm_encoding_is_rejected(self):
        self.reject_instruction(bytes.fromhex("62 f1 7c 08 58 c0 c3"), "encoding exceeds")

    def test_aes_outside_v2_is_rejected(self):
        self.reject_instruction(bytes.fromhex("66 0f 38 dc c0 c3"), "outside reviewed")

    def test_bmi2_is_rejected(self):
        self.reject_instruction(bytes.fromhex("c4 e2 fb f5 c3 c3"), "encoding exceeds")

    def test_avx_is_rejected(self):
        self.reject_instruction(bytes.fromhex("c5 f8 77 c3"), "encoding exceeds")

    def test_tzcnt_is_rejected_without_guessing_about_fallback(self):
        self.reject_instruction(bytes.fromhex("f3 48 0f bc c0 c3"), "outside reviewed")

    def test_optional_rdtscp_benchmark_code_is_rejected(self):
        self.reject_instruction(bytes.fromhex("0f 01 f9 c3"), "outside reviewed")

    def test_unknown_instruction_is_rejected(self):
        self.reject_instruction(bytes.fromhex("0f 3f c3"), "outside reviewed")

    def test_newer_symbol_floor_is_rejected(self):
        original = self.binary.read_bytes()
        for family in (b"GLIBC", b"GLIBCXX", b"CXXABI"):
            with self.subTest(family=family):
                # Change the major version without changing ELF string offsets.
                match = re.search(family + rb"_([0-8])\.[0-9.]+", original)
                self.assertIsNotNone(match)
                old = match[0]
                new = family + b"_9" + old[len(family) + 2:]
                binary = self.root / "ava-newer-symbol"
                binary.write_bytes(original.replace(old, new))
                result = subprocess.run(["python3", str(INSPECT), str(binary)], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("exceeds", result.stderr)


if __name__ == "__main__":
    unittest.main()
