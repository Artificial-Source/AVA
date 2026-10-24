#!/usr/bin/env python3
"""Seal candidate bytes, verify transfers, and prepare owner-run draft commands."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import stat
import subprocess
import tarfile
import tempfile


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


def prepare_draft(args: argparse.Namespace) -> dict:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        raise ValueError("repository must be OWNER/REPO")
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision):
        raise ValueError("revision must be a full commit SHA")
    directory = args.directory.absolute()
    status = directory.lstat()
    if (not stat.S_ISDIR(status.st_mode) or status.st_uid != os.geteuid()
            or stat.S_IMODE(status.st_mode) != 0o700):
        raise ValueError("candidate directory must be owned by you with mode 0700")
    for entry in directory.iterdir():
        if not stat.S_ISREG(entry.lstat().st_mode):
            raise ValueError("candidate inputs must be regular files, not symlinks")
    candidate = verify_candidate(directory, args.manifest_sha256, args.revision)
    if {entry.name for entry in directory.iterdir()} != {*candidate["files"], "candidate.json"}:
        raise ValueError("candidate directory contains unexpected files")
    archive_name = next(name for name in candidate["files"] if name.endswith(".tar.gz"))
    version = archive_name.removeprefix("ava-").removesuffix("-linux-x64.tar.gz")
    tag = "v" + version
    local_tag = subprocess.run(
        ["git", "-C", str(args.repo), "show-ref", "--verify", "--quiet", "refs/tags/" + tag])
    if local_tag.returncode != 1:
        raise ValueError("local tag already exists or could not be checked")
    if git(args.repo, "rev-parse", args.revision + "^{commit}") != args.revision:
        raise ValueError("approved revision is not a local commit")
    body = args.body.read_text()
    if f"# AVA {version}\n" not in body:
        raise ValueError("release body version differs from archive")
    replacements = {
        "@AVA_COMMIT@": args.revision,
        "@AVA_ARCHIVE_SHA256@": candidate["files"][archive_name],
        "@AVA_CHECKSUM_SHA256@": candidate["files"][archive_name + ".sha256"],
        "@AVA_MANIFEST_SHA256@": args.manifest_sha256,
    }
    for placeholder, value in replacements.items():
        if placeholder not in body:
            raise ValueError(f"release body is missing {placeholder}")
        body = body.replace(placeholder, value)
    if re.search(r"@AVA_[A-Z_]+@", body):
        raise ValueError("release body contains an unresolved placeholder")
    # Read the executable only after manifest, archive and provenance verification.
    with tempfile.TemporaryDirectory(prefix="ava-draft-smoke-") as temp:
        root = pathlib.Path(temp)
        binary = root / "ava"
        with tarfile.open(directory / archive_name, "r:gz") as archive:
            with archive.extractfile(archive_name.removesuffix(".tar.gz") + "/bin/ava") as stream:
                binary.write_bytes(stream.read())
        binary.chmod(0o700)
        environment = {"PATH": "/usr/bin:/bin", "HOME": temp,
                       "XDG_CONFIG_HOME": str(root / "config"), "XDG_STATE_HOME": str(root / "state"),
                       "LANG": "C.UTF-8", "TERM": "xterm-256color"}
        output = subprocess.check_output([str(binary), "--version"], env=environment,
                                         text=True, timeout=30).strip()
        if output != "ava " + version:
            raise ValueError("extracted executable version differs from archive")
    destination = args.output.absolute()
    # mkdir and exclusive writes make a repeated dry-run fail without replacing
    # a previously reviewed body or command file.
    destination.mkdir(mode=0o700)
    notes = destination / "release-body.md"
    with notes.open("x") as stream:
        stream.write(body)
    notes_sha = digest(notes)
    q = shlex.quote
    repository = args.repository
    remote = "https://github.com/" + repository + ".git"
    script = pathlib.Path(__file__).resolve()
    commands = [
        "#!/usr/bin/env bash", "set -euo pipefail", "umask 077",
        "# Owner only: run after the packet and GitHub evidence are approved.",
        shlex.join(["python3", str(script), "verify", "--directory", str(directory),
                    "--revision", args.revision, "--manifest-sha256", args.manifest_sha256]),
        f"printf '%s\\n' {q(notes_sha + '  ' + str(notes))} | sha256sum --check --strict",
        f"remote_tags=$(git ls-remote --tags {q(remote)} {q('refs/tags/' + tag)} {q('refs/tags/' + tag + '^{}')})",
        'test -z "$remote_tags" || { echo "Remote tag already exists; stop" >&2; exit 1; }',
        f"release_tags=$(gh api --paginate {q('repos/' + repository + '/releases')} --jq '.[].tag_name')",
        f"if printf '%s\\n' \"$release_tags\" | grep -Fxq -- {q(tag)}; then",
        '  echo "Release or draft already exists; stop" >&2; exit 1', "fi",
        shlex.join(["git", "-C", str(args.repo.absolute()), "tag", "-a", tag,
                    args.revision, "-m", f"AVA {version}"]),
        shlex.join(["git", "-C", str(args.repo.absolute()), "push", remote, "refs/tags/" + tag]),
        shlex.join(["gh", "release", "create", tag, "--repo", repository, "--draft",
                    "--verify-tag", "--title", "AVA " + version, "--notes-file", str(notes),
                    str(directory / archive_name), str(directory / (archive_name + ".sha256"))]),
        "# Stop here. Verify the draft's downloaded assets and obtain final approval.",
        "# Publication is a separate owner command in docs/operations/publication.md.",
    ]
    with (destination / "draft-commands.sh").open("x") as stream:
        stream.write("\n".join(commands) + "\n")
    result = {**candidate, "version": version, "repository": repository,
              "manifest_sha256": args.manifest_sha256, "release_body_sha256": notes_sha,
              "commands": str(destination / "draft-commands.sh")}
    print("Dry-run passed; no remote commands were run. Owner commands:\n")
    print("\n".join(commands))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("seal", "verify-build", "record", "verify", "draft"))
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--directory", type=pathlib.Path)
    parser.add_argument("--manifest-sha256")
    parser.add_argument("--revision")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--repository")
    parser.add_argument("--body", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        if args.command == "draft":
            if not all((args.dry_run, args.directory, args.manifest_sha256, args.revision,
                        args.repository, args.body, args.output)):
                parser.error("draft requires --dry-run, --directory, --manifest-sha256, "
                             "--revision, --repository, --body and a new --output directory")
            result = prepare_draft(args)
        elif args.command == "verify":
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
    except (ValueError, KeyError, OSError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f"candidate verification failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
