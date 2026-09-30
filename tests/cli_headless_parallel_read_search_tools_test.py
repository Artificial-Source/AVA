#!/usr/bin/env python3
"""Full-binary fake-provider coverage for the public parallel read/search opt-in."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from fake_provider import launch_fake_provider
from timeout_support import test_timeout

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

CALL_ORDER = ("call_read", "call_list", "call_glob", "call_grep", "call_write", "call_post_read")


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
    (root / "config" / "ava").mkdir(parents=True, exist_ok=True)
    (root / "config" / "ava").chmod(0o700)
    libcwd_rcfile = root / "libcwdrc"
    libcwd_rcfile.write_text("silent = on\nchannels_default = off\n", encoding="utf-8")
    libcwd_rcfile.chmod(0o600)
    environment.update(
        {
            "TERM": "dumb",
            "NO_COLOR": "1",
            "TMPDIR": str(tmpdir),
            "LIBCWD_RCFILE_NAME": str(libcwd_rcfile),
            "MOONSHOT_API_KEY": "test-key",
        }
    )
    return environment


def write_models(config_dir: pathlib.Path) -> None:
    models = {
        "default_provider": "moonshot",
        "default_model": "ava-headless-fake",
        "models": [
            {
                "provider": "moonshot",
                "id": "ava-headless-fake",
                "family": "fake",
                "api_family": "openai_chat_completions",
                "context_window_tokens": 8192,
                "max_output_tokens": 1024,
                "supports_tools": True,
                "supports_streaming": False,
                "supports_reasoning": False,
                "reports_usage": True,
            }
        ],
    }
    (config_dir / "ava" / "models.json").write_text(json.dumps(models), encoding="utf-8")


def prepare_workspace(workspace: pathlib.Path) -> None:
    workspace.mkdir(parents=True, exist_ok=True)
    (workspace / "notes.txt").write_text("alpha original\n", encoding="utf-8")
    (workspace / "extra.txt").write_text("alpha extra\n", encoding="utf-8")


def jsonl_records(text: str) -> list[dict[object, object]]:
    records: list[dict[object, object]] = []
    for line in text.splitlines():
        if not line.strip():
            continue
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(parsed, dict):
            records.append(parsed)
    return records


def event_call_ids(records: list[dict[object, object]], name: str) -> list[str]:
    ids: list[str] = []
    for record in records:
        if record.get("name") != name:
            continue
        payload = record.get("payload")
        if isinstance(payload, dict) and isinstance(payload.get("call_id"), str):
            ids.append(payload["call_id"])
            continue
        call_id = record.get("call_id")
        if isinstance(call_id, str):
            ids.append(call_id)
    return ids


def payload_for_call(records: list[dict[object, object]], name: str, call_id: str) -> dict[object, object] | None:
    for record in records:
        if record.get("name") != name:
            continue
        payload = record.get("payload")
        if isinstance(payload, dict) and payload.get("call_id") == call_id:
            return payload
        if record.get("call_id") == call_id and isinstance(record, dict):
            return record
    return None


def session_jsonl_text(state_root: pathlib.Path) -> str:
    sessions = state_root / "ava" / "sessions"
    if not sessions.exists():
        return ""
    chunks: list[str] = []
    for path in sorted(sessions.rglob("*.jsonl")):
        chunks.append(path.read_text(encoding="utf-8"))
    return "\n".join(chunks)


def assert_provider_order(records: list[dict[object, object]], session_text: str) -> None:
    starts = event_call_ids(records, "tool_start")
    results = event_call_ids(records, "tool_result")
    require(starts == list(CALL_ORDER), f"tool_start order {starts} != {list(CALL_ORDER)}")
    require(results == list(CALL_ORDER), f"tool_result order {results} != {list(CALL_ORDER)}")
    for call_id in CALL_ORDER:
        require(call_id in session_text, f"session replay is missing tool id {call_id}")
        start = session_text.find(call_id)
        require(start != -1, f"session replay missing {call_id}")
    positions = [session_text.find(call_id) for call_id in CALL_ORDER]
    require(positions == sorted(positions), f"session replay tool ids are not in provider order: {positions}")


def wait_for(predicate, timeout: float, message: str) -> None:
    deadline = time.monotonic() + test_timeout(timeout)
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise RuntimeError(message)


def run_print_case(ava: pathlib.Path, provider_exe: pathlib.Path, root: pathlib.Path) -> None:
    case = root / "print"
    tmpdir = case / "tmp"
    tmpdir.mkdir(parents=True, exist_ok=True)
    environment = isolated_environment(case, tmpdir)
    write_models(pathlib.Path(environment["XDG_CONFIG_HOME"]))
    workspace = case / "workspace"
    prepare_workspace(workspace)
    provider = launch_fake_provider(provider_exe, case, scenario="parallel-read-search", startup_timeout=test_timeout(8))
    environment["MOONSHOT_BASE_URL"] = f"http://127.0.0.1:{provider.port}"
    try:
        completed = subprocess.run(
            [
                str(ava),
                "--mode",
                "plan",
                "--print",
                "--json",
                "--allow",
                "read-only",
                "--parallel-read-search-tools",
                "--parallel-read-search-max-workers",
                "4",
                "inspect then mutate",
            ],
            cwd=workspace,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=test_timeout(30),
            check=False,
        )
        stdout = completed.stdout.decode("utf-8", errors="replace")
        stderr = completed.stderr.decode("utf-8", errors="replace")
        require(
            completed.returncode == 0,
            f"print parallel opt-in failed: rc={completed.returncode} stdout={stdout!r} stderr={stderr!r}",
        )
        records = jsonl_records(stdout)
        session_text = session_jsonl_text(pathlib.Path(environment["XDG_STATE_HOME"]))
        assert_provider_order(records, session_text)
        write_payload = payload_for_call(records, "tool_result", "call_write")
        require(write_payload is not None, "print run missing write tool_result")
        require(
            write_payload.get("status") == "error" and write_payload.get("category") == "permission_denied",
            f"plan-mode print write was not permission-denied: {write_payload}",
        )
        post_payload = payload_for_call(records, "tool_result", "call_post_read")
        require(post_payload is not None, "print run missing post-mutation tool_result")
        post_blob = json.dumps(post_payload)
        require("alpha original" in post_blob, f"denied write must leave original post-read content visible: {post_payload}")
        require("mutated-visible" not in post_blob, f"denied write must not expose mutated content: {post_payload}")
        require("permrule_" not in session_text, "print parallel opt-in must not persist new permission grants")
        provider.finish(test_timeout(8))
    finally:
        provider.stop()


def run_rpc_case(ava: pathlib.Path, provider_exe: pathlib.Path, root: pathlib.Path) -> None:
    case = root / "rpc"
    tmpdir = case / "tmp"
    tmpdir.mkdir(parents=True, exist_ok=True)
    environment = isolated_environment(case, tmpdir)
    write_models(pathlib.Path(environment["XDG_CONFIG_HOME"]))
    workspace = case / "workspace"
    prepare_workspace(workspace)
    provider = launch_fake_provider(provider_exe, case, scenario="parallel-read-search", startup_timeout=test_timeout(8))
    environment["MOONSHOT_BASE_URL"] = f"http://127.0.0.1:{provider.port}"
    process = subprocess.Popen(
        [
            str(ava),
            "--rpc",
            "--allow",
            "read-only",
            "--parallel-read-search-tools",
            "--parallel-read-search-max-workers",
            "4",
        ],
        cwd=workspace,
        env=environment,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    stdout_chunks: list[str] = []
    replied: set[str] = set()

    def stdout_text() -> str:
        return "".join(stdout_chunks)

    def pump() -> None:
        assert process.stdout is not None
        while True:
            line = process.stdout.readline()
            if line == "":
                break
            stdout_chunks.append(line)
            match = re.search(r'"resolver_request_id":"([^"]+)"', line)
            if match and match.group(1) not in replied and process.stdin is not None:
                resolver_id = match.group(1)
                replied.add(resolver_id)
                process.stdin.write(
                    json.dumps(
                        {
                            "id": f"reply-{resolver_id}",
                            "type": "permission_reply",
                            "request_id": resolver_id,
                            "correlation_id": "prompt",
                            "decision": "allow",
                            "reason": "approved for parallel-read-search barrier",
                        }
                    )
                    + "\n"
                )
                process.stdin.flush()

    pump_thread = None
    try:
        pump_thread = threading.Thread(target=pump, daemon=True)
        pump_thread.start()
        assert process.stdin is not None
        process.stdin.write(json.dumps({"id": "prompt", "type": "prompt", "protocol_version": 1, "message": "inspect then mutate"}) + "\n")
        process.stdin.flush()
        wait_for(
            lambda: "parallel-read-search complete" in stdout_text(),
            25,
            f"RPC parallel opt-in did not complete\nstdout={stdout_text()!r}",
        )
        process.stdin.close()
        status = process.wait(timeout=test_timeout(10))
        require(status == 0, f"RPC parallel opt-in exited {status}\nstdout={stdout_text()!r}\nstderr={process.stderr.read() if process.stderr else ''}")
        records = jsonl_records(stdout_text())
        session_text = session_jsonl_text(pathlib.Path(environment["XDG_STATE_HOME"]))
        assert_provider_order(records, session_text)
        post_payload = payload_for_call(records, "tool_result", "call_post_read")
        require(post_payload is not None, "RPC run missing post-mutation tool_result")
        post_blob = json.dumps(post_payload)
        require("mutated-visible" in post_blob, f"RPC post-mutation read did not observe barrier-visible content: {post_payload}")
        notes = (workspace / "notes.txt").read_text(encoding="utf-8")
        require("mutated-visible" in notes, f"workspace notes.txt was not mutated: {notes!r}")
        provider.finish(test_timeout(8))
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=test_timeout(3))
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=test_timeout(3))
        if pump_thread is not None:
            pump_thread.join(timeout=1)
        provider.stop()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True, type=pathlib.Path)
    parser.add_argument("--fake-provider", required=True, type=pathlib.Path)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    args = parser.parse_args()
    sys.path.insert(0, str(args.source / "tests"))

    ava = args.ava.resolve()
    provider_exe = args.fake_provider.resolve()
    require(ava.is_file(), f"AVA executable does not exist: {ava}")
    require(provider_exe.is_file(), f"fake provider does not exist: {provider_exe}")

    root = pathlib.Path(tempfile.mkdtemp(prefix="ava-cli-parallel-read-search.", dir="/tmp"))
    try:
        require(str(root).startswith("/tmp/ava-cli-parallel-read-search."), f"refusing unexpected fixture root: {root}")
        run_print_case(ava, provider_exe, root)
        run_rpc_case(ava, provider_exe, root)
    finally:
        shutil.rmtree(root)
        require(not root.exists(), f"owned fixture root was not removed: {root}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
