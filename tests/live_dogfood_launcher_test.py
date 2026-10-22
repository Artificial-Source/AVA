#!/usr/bin/env python3
"""Offline destructive-root and privacy regressions for live dogfood launchers."""

import argparse
from contextlib import ExitStack
from cli_test_support import isolated_environment, owned_popen, run_owned
from timeout_support import test_timeout
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import sys


DUMMY_CREDENTIAL = "AVA_LIVE_DOGFOOD_DUMMY_KEY_NOT_A_SECRET"

LAUNCHERS = (
    ("model", "live-model-dogfood.sh", "AVA_LIVE_DOGFOOD_ROOT", "AVA_LIVE_DOGFOOD_KEEP", "ava-live-dogfood."),
    (
        "coding",
        "live-coding-dogfood.sh",
        "AVA_LIVE_CODING_DOGFOOD_ROOT",
        "AVA_LIVE_CODING_DOGFOOD_KEEP",
        "ava-live-coding-dogfood.",
    ),
)


def write_executable(path, body):
    path.write_text(body, encoding="utf-8")
    path.chmod(0o700)


def make_private_dir(path):
    path.mkdir(parents=True)
    path.chmod(0o700)
    return path


def clean_environment(home, temporary, fake_ava, marker):
    env = isolated_environment(home.parent, temporary)
    env.update({
        "HOME": str(home),
        "PATH": "/usr/bin:/bin",
        "TMPDIR": str(temporary),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "AVA_LIVE_PROVIDER_SMOKE": "1",
        "OPENAI_API_KEY": DUMMY_CREDENTIAL,
        "AVA_EXE": str(fake_ava),
        "FAKE_AVA_EXECUTION_MARKER": str(marker),
        "LIBCWD_NO_STARTUP_MSGS": "1",
        "AVA_NO_DEBUG_OUTPUT": "1",
    })
    return env


def run_launcher(script, env, cwd):
    result = run_owned(["/bin/sh", str(script)],
    cwd=cwd,
    env=env,
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=test_timeout(15),
    check=False,)
    combined = result.stdout + result.stderr
    assert DUMMY_CREDENTIAL not in combined
    return result


def private_mode(path):
    return stat.S_IMODE(path.lstat().st_mode)


def assert_private_tree(root):
    assert private_mode(root) == 0o700, (root, oct(private_mode(root)))
    for path in root.rglob("*"):
        mode = private_mode(path)
        assert mode & 0o077 == 0, (path, oct(mode))
    for log_name in ("rpc-output.jsonl", "rpc-error.log", "replied-permissions.txt"):
        log = root / log_name
        assert log.is_file(), log
        assert private_mode(log) == 0o600, (log, oct(private_mode(log)))


def assert_success_and_parent_survival(source, fake_ava, root, launcher):
    name, script_name, root_variable, keep_variable, child_prefix = launcher
    script = source / "scripts" / script_name
    case_root = make_private_dir(root / f"{name}-success")
    home = make_private_dir(case_root / "home")
    temporary = make_private_dir(case_root / "tmp")
    parent = make_private_dir(case_root / "private-parent")
    canary = parent / "caller-canary.txt"
    canary.write_text("caller-owned\n", encoding="utf-8")
    neighbor = case_root / "caller-neighbor.txt"
    neighbor.write_text("neighbor\n", encoding="utf-8")
    marker = case_root / "fake-ava-ran.txt"

    env = clean_environment(home, temporary, fake_ava, marker)
    env[root_variable] = str(parent)
    result = run_launcher(script, env, source)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert "classification=passed" in result.stdout
    assert canary.read_text(encoding="utf-8") == "caller-owned\n"
    assert neighbor.read_text(encoding="utf-8") == "neighbor\n"
    assert list(parent.iterdir()) == [canary]
    assert marker.is_file()

    marker.unlink()
    env[keep_variable] = "1"
    retained = run_launcher(script, env, source)
    assert retained.returncode == 0, (retained.stdout, retained.stderr)
    evidence_lines = [line for line in retained.stdout.splitlines() if line.startswith("evidence_root=")]
    assert len(evidence_lines) == 1, retained.stdout
    evidence_root = Path(evidence_lines[0].split("=", 1)[1])
    assert evidence_root.parent == parent.resolve()
    assert evidence_root.name.startswith(child_prefix)
    assert evidence_root.is_dir() and not evidence_root.is_symlink()
    assert canary.read_text(encoding="utf-8") == "caller-owned\n"
    assert neighbor.read_text(encoding="utf-8") == "neighbor\n"
    assert_private_tree(evidence_root)


def assert_rejections_precede_execution(source, fake_ava, root, launcher):
    name, script_name, root_variable, _, _ = launcher
    script = source / "scripts" / script_name
    case_root = make_private_dir(root / f"{name}-rejections")
    home = make_private_dir(case_root / "home")
    temporary = make_private_dir(case_root / "tmp")
    valid = make_private_dir(case_root / "valid")
    insecure = case_root / "insecure"
    insecure.mkdir()
    insecure.chmod(0o775)
    symlink = case_root / "parent-link"
    symlink.symlink_to(valid, target_is_directory=True)
    marker = case_root / "fake-ava-ran.txt"
    env = clean_environment(home, temporary, fake_ava, marker)

    rejection_cases = (
        ("relative-parent", "absolute"),
        (str(case_root / "missing"), "existing"),
        (str(symlink), "symlink"),
        (str(symlink) + "/", "symlink"),
        (str(home), "HOME"),
        (str(source), "checkout"),
        ("/", "root"),
        (str(insecure), "0700"),
    )
    for candidate, expected in rejection_cases:
        marker.unlink(missing_ok=True)
        attempted = dict(env)
        attempted[root_variable] = candidate
        result = run_launcher(script, attempted, source)
        assert result.returncode != 0, (candidate, result.stdout, result.stderr)
        assert expected in result.stderr, (candidate, expected, result.stderr)
        assert not marker.exists(), candidate

    wrong_owner = None
    if os.geteuid() == 0:
        wrong_owner = make_private_dir(case_root / "wrong-owner")
        wrong_owner.chown(65534, 65534)
    else:
        system_private = Path("/root")
        if (
            system_private.is_dir()
            and not system_private.is_symlink()
            and system_private.stat().st_uid != os.geteuid()
            and private_mode(system_private) == 0o700
        ):
            wrong_owner = system_private
    if wrong_owner is not None:
        attempted = dict(env)
        attempted[root_variable] = str(wrong_owner)
        result = run_launcher(script, attempted, source)
        assert result.returncode != 0
        assert not marker.exists()

    else:
        print("SKIP: wrong-owner parent rejection requires root or a visible private directory owned by another uid", file=sys.stderr)

    assert (valid.exists() and symlink.is_symlink() and insecure.exists())


def matrix_environment(home, temporary, fake_ava, marker, summary, target):
    env = clean_environment(home, temporary, fake_ava, marker)
    env.update(
        {
            "AVA_LIVE_PROVIDER_MATRIX_TARGET": target,
            "AVA_LIVE_PROVIDER_MATRIX_SUMMARY": str(summary),
            "AVA_LIVE_PROVIDER_MATRIX_KEEP": "1",
        }
    )
    return env


def assert_matrix_result(result, summary, target, marker):
    stdout, stderr = result.communicate(timeout=20)
    assert result.returncode == 0, (target, stdout, stderr)
    assert DUMMY_CREDENTIAL not in stdout + stderr
    assert "matrix_classification=passed" in stdout
    assert marker.is_file()
    assert summary.is_file() and private_mode(summary) == 0o600
    summary_text = summary.read_text(encoding="utf-8")
    assert "openai\topenai\tOPENAI_API_KEY" in summary_text
    assert f"\t{target}\tpassed\t0\t" in summary_text
    roots = [Path(line.split("=", 1)[1]) for line in stdout.splitlines() if line.startswith("log_root=")]
    assert len(roots) == 1
    run_root = roots[0]
    assert run_root.is_dir() and private_mode(run_root) == 0o700
    case_parent = run_root / "openai"
    assert case_parent.is_dir() and private_mode(case_parent) == 0o700
    children = list(case_parent.iterdir())
    assert len(children) == 1 and children[0].is_dir()
    assert_private_tree(children[0])
    assert private_mode(run_root / "openai.out") == 0o600
    return run_root


def assert_parallel_matrix_integration(source, fake_ava, root):
    matrix = source / "scripts/live-provider-matrix.sh"
    case_root = make_private_dir(root / "matrix")
    with ExitStack() as children:
        processes = []
        expectations = []
        for index, target in enumerate(("model-dogfood", "coding-dogfood")):
            invocation = make_private_dir(case_root / str(index))
            home = make_private_dir(invocation / "home")
            temporary = make_private_dir(invocation / "tmp")
            marker = invocation / "fake-ava-ran.txt"
            summary = invocation / "summary.tsv"
            env = matrix_environment(home, temporary, fake_ava, marker, summary, target)
            process = children.enter_context(owned_popen(
                ["/bin/sh", str(matrix)],
                cwd=source,
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            ))
            processes.append(process)
            expectations.append((summary, target, marker))

        run_roots = [
            assert_matrix_result(process, summary, target, marker)
            for process, (summary, target, marker) in zip(processes, expectations)
        ]
        assert run_roots[0] != run_roots[1]


def create_fake_ava(path):
    write_executable(
        path,
        r'''#!/usr/bin/python3
import json
import os
from pathlib import Path
import sys

marker_path = Path(os.environ["FAKE_AVA_EXECUTION_MARKER"])
marker_path.write_text("executed\n", encoding="utf-8")
coding = (Path.cwd() / "src/task.txt").is_file()
marker = "AVA_LIVE_CODING_DONE_4172"


def emit(value):
    print(json.dumps(value, separators=(",", ":")), flush=True)


if coding:
    target = Path.cwd() / "src/task.txt"
    target.write_text("# Live Coding Dogfood\n\nstatus: " + marker + "\n", encoding="utf-8")
    session = Path(os.environ["XDG_STATE_HOME"]) / "ava/sessions/fake/session.jsonl"
    session.parent.mkdir(parents=True, exist_ok=True)
    records = [
        {"type": "assistant_output_item", "kind": "function_call", "tool_name": "skill", "operation": "edit"},
        {"type": "assistant_turn_commit"},
        {"type": "tool_result", "assistant_output_entry_id": "entry", "resolution": "allow", "tool_name": "apply_patch", "marker": marker},
        {"type": "permission_decision", "tool_name": "skill", "resolution": "allow"},
        {"type": "permission_decision", "tool_name": "apply_patch", "operation": "edit", "resolution": "allow"},
    ]
    session.write_text("".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records), encoding="utf-8")

for raw_line in sys.stdin:
    try:
        request = json.loads(raw_line)
    except json.JSONDecodeError:
        continue
    if request.get("type") == "prompt":
        if coding:
            emit({"name": "tool_start", "tool": "skill"})
            emit({"name": "tool_result", "tool": "skill", "status": "success"})
            emit({"name": "tool_start", "tool": "read_file"})
            emit({"name": "tool_result", "tool": "read_file", "status": "success"})
            emit({"name": "permission_requested", "resolver_request_id": "fake-skill", "tool_name": "skill"})
            emit({"name": "permission_replied", "resolver_request_id": "fake-skill", "tool_name": "skill", "decision": "allow"})
            emit({"name": "tool_start", "tool": "apply_patch"})
            emit({"name": "tool_result", "tool": "apply_patch", "status": "success"})
            emit({"final_text": marker})
        else:
            emit({"name": "tool_start", "tool": "read_file"})
            emit({"name": "tool_result", "tool": "read_file", "status": "success"})
            emit({"final_text": "ava-live-dogfood-marker"})
    elif request.get("id") == "validate-after":
        emit({"id": "validate-after", "ok": True})
    elif request.get("id"):
        emit({"id": request["id"], "ok": True})
''',
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    source = Path(args.source).resolve()

    for _, script_name, _, _, _ in LAUNCHERS:
        script = source / "scripts" / script_name
        assert script.is_file()

    with tempfile.TemporaryDirectory(prefix="ava-live-dogfood-launcher-test-") as temporary_name:
        root = Path(temporary_name).resolve()
        root.chmod(0o700)
        fake_ava = root / "fake-ava.py"
        create_fake_ava(fake_ava)
        for launcher in LAUNCHERS:
            assert_success_and_parent_survival(source, fake_ava, root, launcher)
            assert_rejections_precede_execution(source, fake_ava, root, launcher)
        assert_parallel_matrix_integration(source, fake_ava, root)
        shutil.rmtree(root / "matrix", ignore_errors=False)

    print("live dogfood launcher private-parent, cleanup, mode, rejection, and matrix tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
