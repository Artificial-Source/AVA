#!/usr/bin/env python3
"""Credential-free subprocess coverage for AVA doctor and support export."""

from __future__ import annotations

import argparse
from cli_test_support import isolated_environment, run_owned
from timeout_support import test_timeout
import json
from pathlib import Path
import socket
import stat
import subprocess
import tempfile

CANARY = "CANARY_DOCTOR_SUPPORT_SECRET_8f92"
REMOTE = "https://remote.invalid/CANARY_REMOTE_7a11"


def private_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.chmod(0o700)


def private_file(path: Path, text: str, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    path.write_text(text, encoding="utf-8")
    path.chmod(mode)


def run(ava: Path, args: list[str], env: dict[str, str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return run_owned([str(ava), *args],
    cwd=cwd,
    env=env,
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    timeout=test_timeout(10),
    check=False,)


def snapshot(root: Path) -> dict[str, tuple[int, int, str]]:
    result: dict[str, tuple[int, int, str]] = {}
    for path in sorted(root.rglob("*")):
        metadata = path.lstat()
        kind = "link" if stat.S_ISLNK(metadata.st_mode) else "dir" if stat.S_ISDIR(metadata.st_mode) else "file"
        result[str(path.relative_to(root))] = (stat.S_IMODE(metadata.st_mode), metadata.st_size, kind)
    return result


def assert_clean_output(completed: subprocess.CompletedProcess[str]) -> None:
    combined = completed.stdout + completed.stderr
    for forbidden in (CANARY, REMOTE, "marker-provider", "marker-plugin", "marker-mcp", "marker-lsp"):
        assert forbidden not in combined, f"private canary escaped CLI output: {forbidden}"


def seeded_environment(root: Path) -> tuple[dict[str, str], Path, socket.socket]:
    config_home = root / "config"
    state_home = root / "state"
    workspace = root / "workspace"
    ava_config = config_home / "ava"
    ava_state = state_home / "ava"
    config_target = root / "logical-config-target"
    state_target = root / "logical-state-target"
    for directory in (config_home, state_home, workspace, config_target, state_target):
        private_dir(directory)
    ava_config.symlink_to(config_target, target_is_directory=True)
    ava_state.symlink_to(state_target, target_is_directory=True)

    # The model parser sees private values, while public doctor output is counts only.
    private_file(
        ava_config / "models.json",
        json.dumps(
            {
                "default_provider": "openai",
                "default_model": "gpt-5.5",
                "canary": CANARY,
                "models": [
                    {
                        "provider": "openai",
                        "id": "private-canary-model",
                        "name": CANARY,
                        "api_family": "openai_responses",
                    }
                ],
            }
        ),
    )

    # Mode 000 is a content trap: doctor may inspect metadata but must not open it.
    private_file(ava_config / "auth.json", json.dumps({"token": CANARY, "remote": REMOTE}), mode=0o000)

    private_dir(ava_config / "plugins")
    plugin_dir = ava_config / "plugins" / "canary-plugin"
    private_dir(plugin_dir)
    private_file(
        plugin_dir / "plugin.json",
        json.dumps(
            {
                "schema_version": 1,
                "id": "canary.plugin",
                "name": CANARY,
                "version": "1.0.0",
                "api_version": "ava.plugin.v1",
                "description": REMOTE,
                "entrypoint": {"command": "/bin/sh", "args": ["-c", f"echo {CANARY} > marker-plugin"]},
                "capabilities": [],
            }
        ),
    )
    private_file(
        ava_config / "mcp.json",
        json.dumps(
            {
                "servers": [
                    {
                        "id": "private-mcp-id",
                        "name": CANARY,
                        "command": "/bin/sh",
                        "args": ["-c", f"echo {CANARY} > marker-mcp"],
                        "env": {"PRIVATE_CANARY": CANARY, "REMOTE_TEXT": REMOTE},
                    }
                ]
            }
        ),
    )
    private_file(
        ava_config / "lsp.json",
        json.dumps(
            {
                "version": 1,
                "builtin_servers": ["clangd"],
                "servers": [
                    {
                        "id": "private-lsp-id",
                        "argv": ["/bin/sh", "-c", f"echo {CANARY} > marker-lsp"],
                        "language_id": "private",
                    }
                ],
            }
        ),
    )
    private_file(
        ava_config / "permission-rules.json",
        json.dumps(
            {
                "schema_version": 2,
                "rules": [
                    {
                        "rule_id": "private-rule",
                        "scope": "global",
                        "workspace_dir": "",
                        "action": "deny",
                        "operation": "read",
                        "mode": "any",
                        "tool_name": "",
                        "target_path": str(root / CANARY),
                        "command": "",
                        "command_recipe_key": "",
                        "recipe_display": "",
                        "critical_acknowledged": False,
                        "reason": CANARY,
                        "actor": "test",
                        "created_at": "2026-01-01T00:00:00Z",
                    }
                ],
            }
        ),
    )

    diagnostics = ava_state / "diagnostics"
    private_dir(diagnostics)
    # Malformed optional records must become typed states, never copied payloads.
    private_file(diagnostics / "last-failure-v1.json", '{"schema_version":1,"private":"' + CANARY + '"}')
    private_file(diagnostics / "trace-counters-v1.json", '{"schema_version":1,"remote":"' + REMOTE + '"}')

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.setblocking(False)
    host, port = listener.getsockname()

    libcwd_rcfile = root / "libcwdrc"
    private_file(libcwd_rcfile, "silent = on\nchannels_default = off\n")

    env = isolated_environment(root)
    env.update(
        {
            "HOME": str(root / "home"),
            "XDG_CONFIG_HOME": str(config_home),
            "XDG_STATE_HOME": str(state_home),
            "XDG_DATA_HOME": str(root / "data"),
            "OPENAI_BASE_URL": f"http://{host}:{port}/marker-provider/{CANARY}",
            "OPENAI_API_KEY": CANARY,
            "ANTHROPIC_API_KEY": CANARY,
            "NO_COLOR": "1",
            "LIBCWD_RCFILE_NAME": str(libcwd_rcfile),
        }
    )
    return env, workspace, listener


def test_doctor_and_support(ava: Path, root: Path) -> None:
    env, workspace, listener = seeded_environment(root)
    with listener:
        before = snapshot(root)

        human = run(ava, ["doctor"], env, workspace)
        assert human.returncode == 0, human.stderr
        assert human.stdout.startswith("AVA doctor\n")
        assert "version_platform" in human.stdout and "summary pass=" in human.stdout
        assert_clean_output(human)
        assert snapshot(root) == before, "passive human doctor mutated the filesystem"

        machine = run(ava, ["doctor", "--json"], env, workspace)
        assert machine.returncode == 0, machine.stderr
        report = json.loads(machine.stdout)
        assert set(report) == {"schema_version", "checks", "summary"}
        assert [check["label"] for check in report["checks"]] == [
            "version_platform",
            "config_root",
            "state_root",
            "model_registry",
            "default_model",
            "auth_metadata",
            "plugin_configuration",
            "mcp_configuration",
            "lsp_configuration",
            "lsp_builtin_clangd",
            "permission_rules",
        ]
        checks = {check["label"]: check for check in report["checks"]}
        assert checks["plugin_configuration"]["items"] == 1 and checks["plugin_configuration"]["errors"] == 0
        assert checks["mcp_configuration"]["items"] == 1 and checks["mcp_configuration"]["enabled"] == 1
        assert checks["lsp_configuration"]["items"] == 1 and checks["lsp_configuration"]["enabled"] == 1
        assert checks["lsp_builtin_clangd"]["enabled"] == 1
        assert checks["lsp_builtin_clangd"]["code"] in {"ready", "missing_optional", "unsafe_metadata"}
        assert checks["permission_rules"]["items"] == 1
        assert checks["auth_metadata"]["code"] == "unsafe_metadata"
        assert_clean_output(machine)
        assert snapshot(root) == before, "passive JSON doctor mutated the filesystem"

        try:
            listener.accept()
            raise AssertionError("doctor accessed the provider network endpoint")
        except BlockingIOError:
            pass

        for marker in ("marker-provider", "marker-plugin", "marker-mcp", "marker-lsp"):
            assert not (workspace / marker).exists(), f"doctor launched configured process: {marker}"
        assert not (root / "state" / "ava" / "sessions").exists(), "doctor created a session"

        first = run(ava, ["support", "export"], env, workspace)
        second = run(ava, ["support", "export"], env, workspace)
        assert first.returncode == second.returncode == 0
        prefix = "Support artifact created: "
        assert first.stdout.startswith(prefix) and first.stdout.endswith("\n")
        assert second.stdout.startswith(prefix) and second.stdout.endswith("\n")
        assert first.stdout != second.stdout
        assert first.stderr == second.stderr == ""
        assert_clean_output(first)
        support_dir = root / "state" / "ava" / "support"
        artifacts = sorted(support_dir.glob("ava-support-v1-*.json"))
        reported = {Path(first.stdout[len(prefix):].strip()), Path(second.stdout[len(prefix):].strip())}
        assert set(artifacts) == reported, "support export did not report its exact unique no-replace files"
        assert len(artifacts) == 2, "support export did not publish unique no-replace files"
        for artifact_path in artifacts:
            metadata = artifact_path.stat()
            assert stat.S_IMODE(metadata.st_mode) == 0o600 and metadata.st_nlink == 1
            artifact = json.loads(artifact_path.read_text(encoding="utf-8"))
            assert set(artifact) == {"schema_version", "generated_at", "ava", "doctor", "trace", "last_failure"}
            assert set(artifact["ava"]) == {"version", "os", "arch"}
            assert artifact["trace"] == {"schema_version": 1, "state": "malformed"}
            assert artifact["last_failure"] == {"schema_version": 1, "state": "malformed"}
            text = artifact_path.read_text(encoding="utf-8")
            assert CANARY not in text and REMOTE not in text
        assert stat.S_IMODE(support_dir.stat().st_mode) == 0o700
        assert not list(support_dir.glob(".support-tmp-*")), "partial support publication remained"
        assert not (root / "state" / "ava" / "sessions").exists(), "support export created a session"
        for marker in ("marker-provider", "marker-plugin", "marker-mcp", "marker-lsp"):
            assert not (workspace / marker).exists(), f"support export launched configured process: {marker}"
        try:
            listener.accept()
            raise AssertionError("support export accessed the provider network endpoint")
        except BlockingIOError:
            pass
        listener.close()


def test_usage_and_required_failure(ava: Path, root: Path) -> None:
    config_home = root / "bad-config"
    state_home = root / "bad-state"
    workspace = root / "bad-workspace"
    target = root / "redirected-config"
    for directory in (config_home, state_home, workspace, target):
        private_dir(directory)
    (config_home / "ava").symlink_to(target, target_is_directory=True)
    libcwd_rcfile = root / "libcwdrc"
    private_file(libcwd_rcfile, "silent = on\nchannels_default = off\n")
    env = isolated_environment(root)
    env.update({
        "HOME": str(root / "bad-home"),
        "XDG_CONFIG_HOME": str(config_home),
        "XDG_STATE_HOME": str(state_home),
        "NO_COLOR": "1",
        "LIBCWD_RCFILE_NAME": str(libcwd_rcfile),
    })
    logical = run(ava, ["doctor", "--json"], env, workspace)
    assert logical.returncode == 0
    logical_report = json.loads(logical.stdout)
    logical_config = next(check for check in logical_report["checks"] if check["label"] == "config_root")
    assert logical_config["status"] == "pass" and logical_config["code"] == "ready"

    target.chmod(0o755)
    failed = run(ava, ["doctor", "--json"], env, workspace)
    assert failed.returncode == 1
    report = json.loads(failed.stdout)
    config_check = next(check for check in report["checks"] if check["label"] == "config_root")
    assert config_check["status"] == "fail" and config_check["code"] == "unsafe_metadata"
    assert str(target) not in failed.stdout + failed.stderr

    for args in (["doctor", "--unknown"], ["doctor", "--json", "--json"], ["support"], ["support", "export", "--json"], ["support", "unknown"]):
        completed = run(ava, list(args), env, workspace)
        assert completed.returncode == 2, (args, completed.returncode, completed.stdout, completed.stderr)
        assert completed.stdout == ""
        assert completed.stderr.startswith(f"Usage: {ava} ")

    outside_cwd = run(ava, ["--cwd", f"{workspace}-sibling", "--rpc"], env, workspace)
    assert outside_cwd.returncode == 2
    assert outside_cwd.stdout == ""
    assert outside_cwd.stderr == f"{ava}: --cwd must be equal to or inside the workspace directory\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", type=Path, required=True)
    parser.add_argument("--root", type=Path)
    args = parser.parse_args()
    parent = args.root.resolve() if args.root else None
    if parent is not None:
        private_dir(parent)
    with tempfile.TemporaryDirectory(prefix="ava-doctor-support-", dir=parent) as temporary:
        root = Path(temporary)
        test_doctor_and_support(args.ava.resolve(), root / "normal")
        test_usage_and_required_failure(args.ava.resolve(), root / "usage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
