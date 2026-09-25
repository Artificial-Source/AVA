#!/usr/bin/env python3
"""Credential-free CLI coverage for interactive TUI startup gates."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile


ENVIRONMENT_ALLOWLIST = (
    "PATH",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "LD_LIBRARY_PATH",
    "DYLD_LIBRARY_PATH",
    "DYLD_FALLBACK_LIBRARY_PATH",
    "ASAN_OPTIONS",
    "UBSAN_OPTIONS",
    "LSAN_OPTIONS",
    "TSAN_OPTIONS",
    "MSAN_OPTIONS",
    "ASAN_SYMBOLIZER_PATH",
    "LLVM_SYMBOLIZER_PATH",
    "TMPDIR",
    "TZ",
)

TTY_REQUIRED = b"interactive TUI requires a terminal on both stdin and stdout"
PRINT_OR_RPC = b"--print or --rpc"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def isolated_environment(root: pathlib.Path, tmpdir: pathlib.Path) -> dict[str, str]:
    environment = {name: os.environ[name] for name in ENVIRONMENT_ALLOWLIST if name in os.environ}
    directories = {
        "HOME": root / "home",
        "XDG_CONFIG_HOME": root / "config",
        "XDG_STATE_HOME": root / "state",
        "XDG_DATA_HOME": root / "data",
        "XDG_CACHE_HOME": root / "cache",
        "XDG_RUNTIME_DIR": root / "runtime",
    }
    for name, directory in directories.items():
        directory.mkdir(parents=True, exist_ok=True)
        environment[name] = str(directory)
    directories["XDG_RUNTIME_DIR"].chmod(0o700)
    tmpdir.mkdir(parents=True, exist_ok=True)
    libcwd_rcfile = root / "libcwdrc"
    libcwd_rcfile.write_text("silent = on\nchannels_default = off\n", encoding="utf-8")
    libcwd_rcfile.chmod(0o600)
    environment.update(
        {
            "TERM": "dumb",
            "NO_COLOR": "1",
            "TMPDIR": str(tmpdir),
            "LIBCWD_RCFILE_NAME": str(libcwd_rcfile),
        }
    )
    return environment


def run(
    ava: pathlib.Path,
    arguments: list[str],
    environment: dict[str, str],
    workspace: pathlib.Path,
    input_bytes: bytes = b"",
) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        [str(ava), *arguments],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=workspace,
        env=environment,
        timeout=15,
        check=False,
    )


def assert_no_runtime_state(state_root: pathlib.Path, label: str) -> None:
    ava_state = state_root / "ava"
    for name in ("sessions", "diagnostics"):
        require(not (ava_state / name).exists(), f"{label} created {name} runtime state")


def assert_no_sessions(state_root: pathlib.Path, label: str) -> None:
    sessions = state_root / "ava" / "sessions"
    found = sorted(path for path in sessions.rglob("*") if path.is_file()) if sessions.exists() else []
    require(not found, f"{label} created session state: {found}")


def run_cases(ava: pathlib.Path, workspace: pathlib.Path, environment: dict[str, str]) -> int:
    state_root = pathlib.Path(environment["XDG_STATE_HOME"])

    help_result = run(ava, ["--help"], environment, workspace)
    require(help_result.returncode == 0, f"--help failed: rc={help_result.returncode} stderr={help_result.stderr!r}")
    require(b"--line-shell" not in help_result.stdout, f"--help still advertises --line-shell: {help_result.stdout!r}")

    unknown = run(ava, ["--line-shell"], environment, workspace)
    require(
        unknown.returncode == 2 and unknown.stdout == b"" and b"unknown argument: --line-shell" in unknown.stderr,
        f"--line-shell was not rejected as an unknown flag: rc={unknown.returncode} stdout={unknown.stdout!r} stderr={unknown.stderr!r}",
    )
    assert_no_runtime_state(state_root, "unknown --line-shell")

    acp_unknown = run(ava, ["--acp", "--line-shell"], environment, workspace)
    require(
        acp_unknown.returncode == 2
        and acp_unknown.stdout == b""
        and b"use either --line-shell or --acp" not in acp_unknown.stderr
        and b"--acp is a standalone mode" in acp_unknown.stderr,
        f"--acp --line-shell did not use ordinary ACP extra-flag rejection: rc={acp_unknown.returncode} stdout={acp_unknown.stdout!r} stderr={acp_unknown.stderr!r}",
    )
    assert_no_runtime_state(state_root, "ACP extra --line-shell")

    both_non_tty = run(ava, ["--offline"], environment, workspace, b"/exit\n")
    require(
        both_non_tty.returncode == 2
        and both_non_tty.stdout == b""
        and TTY_REQUIRED in both_non_tty.stderr
        and PRINT_OR_RPC in both_non_tty.stderr,
        f"piped stdin and stdout did not reject interactive startup: rc={both_non_tty.returncode} stdout={both_non_tty.stdout!r} stderr={both_non_tty.stderr!r}",
    )
    assert_no_runtime_state(state_root, "both-non-TTY interactive startup")

    print_smoke = run(ava, ["--print", "--no-session", "--offline", "hi"], environment, workspace)
    require(
        print_smoke.returncode == 1
        and print_smoke.stdout == b""
        and b"offline mode is enabled; provider model calls are disabled" in print_smoke.stderr
        and TTY_REQUIRED not in print_smoke.stderr,
        f"explicit --print --no-session did not reach the offline provider gate: rc={print_smoke.returncode} stdout={print_smoke.stdout!r} stderr={print_smoke.stderr!r}",
    )
    assert_no_sessions(state_root, "explicit --print --no-session")

    positional = run(ava, ["--offline", "--no-session", "hello"], environment, workspace)
    require(
        positional.returncode == 1
        and positional.stdout == b""
        and b"offline mode is enabled; provider model calls are disabled" in positional.stderr
        and TTY_REQUIRED not in positional.stderr,
        f"positional prompt did not reach the offline provider gate: rc={positional.returncode} stdout={positional.stdout!r} stderr={positional.stderr!r}",
    )
    assert_no_sessions(state_root, "positional print prompt")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    ava = pathlib.Path(args.ava).resolve()
    require(ava.is_file(), f"AVA executable does not exist: {ava}")
    root = pathlib.Path(args.root).resolve()
    if root.exists():
        require(root.name == "interactive-cli", f"refusing to clear unexpected test root: {root}")
        shutil.rmtree(root)
    root.mkdir(parents=True, mode=0o700)
    workspace = root / "workspace"
    workspace.mkdir()
    with tempfile.TemporaryDirectory(prefix="ava-interactive-cli-", dir=root) as tmp:
        return run_cases(ava, workspace, isolated_environment(root, pathlib.Path(tmp)))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
