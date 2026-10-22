#!/usr/bin/env python3
"""Full-binary ordering, permissions, write barriers, and gated print cancellation."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
from cli_test_support import (descendant_process_groups, isolated_environment, owned_popen, require, run_owned,
                              terminate_process_group, wait_for_process_groups_absent)
import json
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import threading

from fake_provider import launch_fake_provider
from timeout_support import test_timeout

CALL_ORDER = ("call_read", "call_list", "call_glob", "call_grep", "call_write", "call_post_read")


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
    (config_dir / "ava").mkdir(mode=0o700, exist_ok=True)
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
        except json.JSONDecodeError as error:
            raise RuntimeError(f"malformed CLI JSONL record: {line!r}") from error
        require(isinstance(parsed, dict), f"CLI record is not an object: {line!r}")
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
    for call_id in CALL_ORDER[:4]:
        result = payload_for_call(records, "tool_result", call_id)
        require(result is not None and result.get("status") == "success",
                f"read/search tool {call_id} did not succeed: {result}")
    first_read = payload_for_call(records, "tool_result", "call_read")
    require("alpha original" in json.dumps(first_read), f"first read did not return fixture bytes: {first_read}")
    for call_id in CALL_ORDER:
        require(call_id in session_text, f"session replay is missing tool id {call_id}")
        start = session_text.find(call_id)
        require(start != -1, f"session replay missing {call_id}")
    positions = [session_text.find(call_id) for call_id in CALL_ORDER]
    require(positions == sorted(positions), f"session replay tool ids are not in provider order: {positions}")


def run_print_case(ava: pathlib.Path, provider_exe: pathlib.Path, root: pathlib.Path) -> None:
    case = root / "print"
    tmpdir = case / "tmp"
    tmpdir.mkdir(parents=True, exist_ok=True)
    environment = isolated_environment(case, tmpdir)
    environment["MOONSHOT_API_KEY"] = "test-key"
    write_models(pathlib.Path(environment["XDG_CONFIG_HOME"]))
    workspace = case / "workspace"
    prepare_workspace(workspace)
    provider = launch_fake_provider(provider_exe, case, scenario="parallel-read-search", startup_timeout=test_timeout(8))
    environment["MOONSHOT_BASE_URL"] = f"http://127.0.0.1:{provider.port}"
    try:
        completed = run_owned([
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
        check=False,)
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
        require((workspace / "notes.txt").read_text(encoding="utf-8") == "alpha original\n",
                "denied print write changed the workspace file")
        require("permrule_" not in session_text, "print parallel opt-in must not persist new permission grants")
        provider.finish(test_timeout(8))
    finally:
        provider.stop()


def run_rpc_case(ava: pathlib.Path, provider_exe: pathlib.Path, root: pathlib.Path) -> None:
    case = root / "rpc"
    environment = isolated_environment(case)
    environment["MOONSHOT_API_KEY"] = "test-key"
    write_models(pathlib.Path(environment["XDG_CONFIG_HOME"]))
    workspace = case / "workspace"
    prepare_workspace(workspace)
    with ExitStack() as owners:
        provider = launch_fake_provider(provider_exe, case, scenario="parallel-read-search", startup_timeout=test_timeout(8))
        owners.callback(provider.stop)
        environment["MOONSHOT_BASE_URL"] = f"http://127.0.0.1:{provider.port}"
        readers: list[threading.Thread] = []

        def join_readers() -> None:
            for reader in readers:
                if reader.ident is None:
                    continue
                reader.join(timeout=test_timeout(5))
                require(not reader.is_alive(), f"CLI reader {reader.name} did not stop after child cleanup")

        owners.callback(join_readers)
        process = owners.enter_context(owned_popen(
            [str(ava), "--rpc", "--allow", "read-only", "--parallel-read-search-tools",
             "--parallel-read-search-max-workers", "4"],
            cwd=workspace,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        ))
        changed = threading.Condition()
        stdout_chunks: list[str] = []
        stderr_chunks: list[str] = []
        records: list[dict[object, object]] = []
        reader_errors: list[str] = []
        replied: set[str] = set()
        stdout_closed = False

        def pump_stdout() -> None:
            nonlocal stdout_closed
            try:
                require(process.stdout is not None, "RPC stdout pipe was not created")
                for line in process.stdout:
                    record = json.loads(line)
                    require(isinstance(record, dict), f"RPC record is not an object: {line!r}")
                    with changed:
                        stdout_chunks.append(line)
                        records.append(record)
                        changed.notify_all()
                    payload = record.get("payload")
                    if record.get("name") != "permission_requested" or not isinstance(payload, dict):
                        continue
                    resolver_id = payload.get("resolver_request_id")
                    require(isinstance(resolver_id, str), f"permission event lacks a resolver id: {record}")
                    if resolver_id in replied:
                        continue
                    replied.add(resolver_id)
                    require(process.stdin is not None, "RPC stdin pipe was not created")
                    process.stdin.write(json.dumps({
                        "id": f"reply-{resolver_id}", "type": "permission_reply", "request_id": resolver_id,
                        "correlation_id": "prompt", "decision": "allow",
                        "reason": "approved for parallel-read-search barrier",
                    }) + "\n")
                    process.stdin.flush()
            except Exception as error:
                with changed:
                    reader_errors.append(f"stdout reader: {error}")
            finally:
                with changed:
                    stdout_closed = True
                    changed.notify_all()

        def pump_stderr() -> None:
            try:
                require(process.stderr is not None, "RPC stderr pipe was not created")
                for chunk in iter(lambda: process.stderr.read(4096), ""):
                    with changed:
                        stderr_chunks.append(chunk)
            except Exception as error:
                with changed:
                    reader_errors.append(f"stderr reader: {error}")
                    changed.notify_all()

        readers.extend((threading.Thread(target=pump_stdout, name="rpc-stdout"),
                        threading.Thread(target=pump_stderr, name="rpc-stderr")))
        for reader in readers:
            reader.start()
        require(process.stdin is not None, "RPC stdin pipe was not created")
        process.stdin.write(json.dumps({"id": "prompt", "type": "prompt", "protocol_version": 1,
                                        "message": "inspect then mutate"}) + "\n")
        process.stdin.flush()

        def prompt_responses():
            return [record for record in records if record.get("type") == "response" and record.get("id") == "prompt"]

        with changed:
            observed = changed.wait_for(lambda: prompt_responses() or reader_errors or stdout_closed,
                                        timeout=test_timeout(25))
            require(observed and not reader_errors and len(prompt_responses()) == 1,
                    f"RPC prompt has no unique terminal response; readers={reader_errors} stdout={''.join(stdout_chunks)!r} stderr={''.join(stderr_chunks)!r}")
            terminal = prompt_responses()[0]
            require(terminal.get("success") is True, f"RPC prompt failed: {terminal}")
            require(terminal.get("result", {}).get("final_text") == "parallel-read-search complete",
                    f"RPC prompt returned the wrong final result: {terminal}")
        process.stdin.close()
        status = process.wait(timeout=test_timeout(10))
        join_readers()
        require(not reader_errors, f"RPC readers failed: {reader_errors}")
        stdout = "".join(stdout_chunks)
        stderr = "".join(stderr_chunks)
        require(status == 0, f"RPC parallel opt-in exited {status}\nstdout={stdout!r}\nstderr={stderr!r}")
        responses = [record for record in records if record.get("type") == "response"]
        require(all(record.get("success") is True for record in responses), f"unexpected RPC failure: {responses}")
        ids = [record.get("id") for record in responses]
        require(len(ids) == len(set(ids)), f"duplicate RPC terminal responses: {responses}")
        require(set(ids) == {"prompt", *(f"reply-{resolver_id}" for resolver_id in replied)},
                f"missing or unsolicited RPC terminal response: {responses}")
        session_text = session_jsonl_text(pathlib.Path(environment["XDG_STATE_HOME"]))
        assert_provider_order(records, session_text)
        post_payload = payload_for_call(records, "tool_result", "call_post_read")
        require(post_payload is not None, "RPC run missing post-mutation tool_result")
        require("mutated-visible" in json.dumps(post_payload),
                f"RPC post-mutation read did not observe barrier-visible content: {post_payload}")
        notes = (workspace / "notes.txt").read_text(encoding="utf-8")
        require(notes == "mutated-visible\n", f"workspace notes.txt was not mutated exactly: {notes!r}")
        provider.finish(test_timeout(8))


def run_print_cancel_case(ava: pathlib.Path, provider_exe: pathlib.Path, root: pathlib.Path,
                          stop_signal: signal.Signals, close_output: bool) -> None:
    case = root / f"print-cancel-{stop_signal.name}-closed-{close_output}"
    environment = isolated_environment(case)
    environment["MOONSHOT_API_KEY"] = "test-key"
    write_models(pathlib.Path(environment["XDG_CONFIG_HOME"]))
    workspace = case / "workspace"
    workspace.mkdir()
    prompt = f"persist interrupted {stop_signal.name} print"
    with ExitStack() as owners:
        provider = launch_fake_provider(provider_exe, case, scenario="text-delayed", startup_timeout=test_timeout(8))
        owners.callback(provider.stop)
        environment["MOONSHOT_BASE_URL"] = f"http://127.0.0.1:{provider.port}"
        errors = owners.enter_context(tempfile.TemporaryFile())
        process = owners.enter_context(owned_popen(
            [str(ava), "--print", *(["--json"] if close_output else []), prompt],
            cwd=workspace, env=environment, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=errors,
        ))
        provider.wait_for_request(0, "print provider request admitted", timeout=test_timeout(8))
        groups = descendant_process_groups(process.pid)
        for group in groups:
            owners.callback(terminate_process_group, group)
        require(process.stdout is not None, "print stdout pipe was not created")
        if close_output:
            process.stdout.close()
        process.send_signal(stop_signal)
        status = process.wait(timeout=test_timeout(10))
        errors.seek(0)
        diagnostic = errors.read().decode("utf-8", errors="replace")
        require(status == 130, f"{stop_signal.name} print exited {status}, closed-output={close_output}: {diagnostic}")
        if not close_output:
            require(process.stdout.read() == b"", "canceled print emitted an unreceived provider answer")
        # Check production settlement before the harness's safety-net cleanup.
        wait_for_process_groups_absent(groups, 5)

    replay = run_owned(
        [str(ava), "--rpc", "--continue"], cwd=workspace, env=environment,
        input='{"id":"validate","type":"validate_session"}\n{"id":"messages","type":"get_messages"}\n',
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=test_timeout(10),
    )
    require(replay.returncode == 0, f"interrupted session did not reopen: {replay.stderr}")
    responses = [record for record in jsonl_records(replay.stdout) if record.get("type") == "response"]
    require(len(responses) == 2 and {record.get("id") for record in responses} == {"validate", "messages"},
            f"interrupted session replay has missing/duplicate responses: {responses}")
    require(all(record.get("success") is True for record in responses), f"interrupted session replay failed: {responses}")
    by_id = {record["id"]: record["result"] for record in responses}
    require(by_id["validate"]["ok"] is True and by_id["validate"]["error_count"] == 0,
            f"interrupted print left an invalid session: {by_id['validate']}")
    users = [message for message in by_id["messages"]["messages"] if message.get("type") == "user_message"]
    require(len(users) == 1 and users[0].get("data", {}).get("text") == prompt,
            f"interrupted print did not persist exactly its admitted user prompt: {users}")


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
        for stop_signal in (signal.SIGINT, signal.SIGTERM):
            for close_output in (False, True):
                run_print_cancel_case(ava, provider_exe, root, stop_signal, close_output)
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
