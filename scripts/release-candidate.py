#!/usr/bin/env python3
"""Seal a tested build and verify retained candidate bytes; never build or publish."""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import tarfile


def digest(path: pathlib.Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def git(repo: pathlib.Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()


def build_identity(repo: pathlib.Path, build: pathlib.Path) -> dict:
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        match = re.match(r"([^:#]+):[^=]+=(.*)", line)
        if match:
            cache[match[1]] = match[2]
    required = {"CMAKE_BUILD_TYPE": "Release", "AVA_RELEASE_ARTIFACT": "ON",
                "AVA_ENABLE_GITACHE": "OFF", "EnableLibcwd": "OFF",
                "FETCHCONTENT_FULLY_DISCONNECTED": "ON", "CMAKE_DISABLE_FIND_PACKAGE_nlohmann_json": "ON"}
    if any(cache.get(key) != value for key, value in required.items()):
        raise ValueError("build is not the canonical release-candidate configuration")
    if pathlib.Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != repo.resolve():
        raise ValueError("build belongs to another source checkout")
    revision = git(repo, "rev-parse", "HEAD")
    if (build / "release-source-revision").read_text().strip() != revision:
        raise ValueError("build was configured at another commit")
    if git(repo, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError("candidate source tree must be clean")
    return {"schema_version": 1, "source_revision": revision,
            "binary_sha256": digest(build / "ava"),
            "fake_provider_sha256": digest(build / "tests/ava_fake_provider_server")}


def verify_build(repo: pathlib.Path, build: pathlib.Path) -> dict:
    actual = build_identity(repo, build)
    if json.loads((build / "release-build.json").read_text()) != actual:
        raise ValueError("sealed build changed after qualification started")
    return actual


def package_identity(directory: pathlib.Path, revision: str, binary_sha256: str) -> dict:
    archives = list(directory.glob("ava-*-linux-x64.tar.gz"))
    if len(archives) != 1:
        raise ValueError("expected exactly one Linux x64 archive")
    archive = archives[0]
    if not re.fullmatch(r"ava-\d+\.\d+\.\d+-linux-x64.tar.gz", archive.name):
        raise ValueError("unexpected archive name")
    checksum = archive.with_name(archive.name + ".sha256")
    if archive.is_symlink() or checksum.is_symlink():
        raise ValueError("artifact pair must be regular files, not symlinks")
    archive_sha = digest(archive)
    if checksum.read_text() != f"{archive_sha}  {archive.name}\n":
        raise ValueError("archive checksum mismatch")
    prefix = archive.name.removesuffix(".tar.gz")
    with tarfile.open(archive, "r:gz") as stream:
        members = stream.getmembers()
        names = [m.name for m in members]
        if len(names) != len(set(names)):
            raise ValueError("duplicate archive members")
        for member in members:
            path = pathlib.PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or path.parts[0] != prefix or not (member.isfile() or member.isdir()):
                raise ValueError("unsafe archive member")
        with stream.extractfile(f"{prefix}/bin/ava") as binary:
            if hashlib.file_digest(binary, "sha256").hexdigest() != binary_sha256:
                raise ValueError("packaged executable differs from tested executable")
        with stream.extractfile(f"{prefix}/share/doc/ava/PROVENANCE.json") as provenance_file:
            provenance = json.load(provenance_file)
        if (provenance["source"]["revision"] != revision or not provenance["release_qualified"]
                or provenance["binary_sha256"] != binary_sha256 or provenance["architecture"] != "x86_64"):
            raise ValueError("package provenance differs from qualified build")
    return {"schema_version": 1, "source_revision": revision, "binary_sha256": binary_sha256,
            "files": {archive.name: archive_sha, checksum.name: digest(checksum)}, "members": names}


def verify_candidate(directory: pathlib.Path, expected_manifest: str, revision: str) -> dict:
    manifest = directory / "candidate.json"
    if not re.fullmatch(r"[0-9a-f]{64}", expected_manifest) or digest(manifest) != expected_manifest:
        raise ValueError("retained manifest digest mismatch")
    recorded = json.loads(manifest.read_text())
    actual = package_identity(directory, revision, recorded["binary_sha256"])
    if actual != recorded:
        raise ValueError("retained candidate differs from recorded digests/commit")
    return actual


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("seal", "verify-build", "record", "verify"))
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--directory", type=pathlib.Path)
    parser.add_argument("--manifest-sha256")
    parser.add_argument("--revision")
    args = parser.parse_args()
    try:
        if args.command == "verify":
            if not args.directory or not args.manifest_sha256 or not args.revision:
                parser.error("verify requires --directory, --manifest-sha256 and --revision")
            result = verify_candidate(args.directory, args.manifest_sha256, args.revision)
        else:
            if not args.build_dir:
                parser.error("build operation requires --build-dir")
            result = build_identity(args.repo, args.build_dir) if args.command == "seal" else verify_build(args.repo, args.build_dir)
            if args.command == "seal":
                with (args.build_dir / "release-build.json").open("x") as output:
                    json.dump(result, output, indent=2, sort_keys=True)
            if args.command == "record":
                if not args.directory:
                    parser.error("record requires --directory")
                result = package_identity(args.directory, result["source_revision"], result["binary_sha256"])
                with (args.directory / "candidate.json").open("x") as output:
                    json.dump(result, output, indent=2, sort_keys=True)
                    output.write("\n")
                print("manifest_sha256=" + digest(args.directory / "candidate.json"))
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, f"candidate verification failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
