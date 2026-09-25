#!/usr/bin/env python3
"""Credential-free real-process checks for ava --acp stdio purity and recovery."""

import argparse
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import tempfile
import time

from fake_provider import FakeProvider, launch_fake_provider
from timeout_support import test_timeout


OWNED_PROCESSES = set()
OWNED_PROVIDERS: list[FakeProvider] = []
OWNED_DIRECTORIES: list[Path] = []


def environment(root):
    root.mkdir(parents=True, exist_ok=True)
    temporary = root / "tmp"
    temporary.mkdir(parents=True, exist_ok=True)
    # The bash command planner resolves the trusted home from HOME, so the
    # isolated HOME must exist and be owner-only (as a real user home would be)
    # for command sealing's safe-directory check to pass.
    home = root / "home"
    home.mkdir(parents=True, exist_ok=True)
    os.chmod(home, 0o700)
    libcwd_rcfile = (root / "libcwdrc").absolute()
    libcwd_rcfile.write_text(
        "silent = on\nchannels_default = off\n", encoding="utf-8")
    env = {
        "HOME": str(home),
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_STATE_HOME": str(root / "state"),
        "XDG_CACHE_HOME": str(root / "cache"),
        "TMPDIR": str(temporary),
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        "NO_COLOR": "1",
        "AVA_SESSION_TITLES": "off",
        # Match the libcwd suppression applied to ava_tests.* so ava's debug
        # initialization cannot write to the streams this test parses. The
        # libcwdrc below silences channels; AVA_NO_DEBUG_OUTPUT additionally
        # skips NAMESPACE_DEBUG::init() in ava::app::debug_init().
        "LIBCWD_NO_STARTUP_MSGS": "1",
        "AVA_NO_DEBUG_OUTPUT": "1",
        # Debug builds must remain protocol-quiet even when an isolated HOME
        # has no developer libcwd configuration.
        #FIXME: remove this? LIBCWD_NO_STARTUP_MSGS should be enough. "LIBCWD_RCFILE_NAME": str(libcwd_rcfile),
    }
    # Preserve explicit per-test debug routing despite the otherwise isolated
    # environment. The fixed base rcfile stays silent unless an override was
    # deliberately exported by the developer.
    for name in ("AVA_TEST_NAME", "AVA_DEBUG_OUTPUT_DIR", "AVA_DEBUG_NO_TIMEOUT", "AVA_DEBUG_NO_TIMEOUT_SECONDS", "LIBCWD_RCFILE_OVERRIDE_NAME"):
        value = os.environ.get(name)
        if value is not None:
            env[name] = value
    return env


def owned_popen(*arguments, **options):
    options["start_new_session"] = True
    process = subprocess.Popen(*arguments, **options)
    OWNED_PROCESSES.add(process)
    return process


def cleanup_owned_processes():
    for process in reversed(tuple(OWNED_PROCESSES)):
        if process.poll() is not None:
            continue
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            continue
    deadline = time.monotonic() + 2
    for process in reversed(tuple(OWNED_PROCESSES)):
        if process.poll() is not None:
            continue
        try:
            process.wait(timeout=max(0.01, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    for process in tuple(OWNED_PROCESSES):
        if process.poll() is None:
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass
        for stream in (process.stdin, process.stdout, process.stderr):
            if stream is not None and not stream.closed:
                stream.close()
    OWNED_PROCESSES.clear()
    for provider in reversed(OWNED_PROVIDERS):
        provider.stop()
    OWNED_PROVIDERS.clear()
    for directory in reversed(OWNED_DIRECTORIES):
        if directory.is_symlink() or directory.is_file():
            directory.unlink(missing_ok=True)
            continue
        try:
            shutil.rmtree(directory)
        except FileNotFoundError:
            pass
    OWNED_DIRECTORIES.clear()


def signal_cleanup(signum, _frame):
    cleanup_owned_processes()
    raise SystemExit(128 + signum)


def start(ava, root, cwd=None, extra_env=None):
    env = environment(root)
    # Mimic the launching shell, which exports PWD as the lexical path the user
    # works with (including symlinked components). ava uses PWD rather than the
    # kernel-resolved current_path() so a workspace configured through a symlink
    # keeps its configured path throughout the session.
    if cwd is not None:
        env["PWD"] = str(cwd)
    if extra_env:
        env.update(extra_env)
    return owned_popen(
        [ava, "--acp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=env, cwd=cwd, bufsize=0
    )


def start_fake_provider(executable, root, delay_ms=0, scenario="text-three", target="unused"):
    provider = launch_fake_provider(
        executable,
        root,
        prefix="provider",
        delay_ms=delay_ms,
        scenario=scenario,
        target=target,
        environment=environment(root / "environment"),
        startup_timeout=test_timeout(10),
    )
    OWNED_PROVIDERS.append(provider)
    return provider


def configure_fake_model(root, supports_tools=False, input_modalities=None):
    config = root / "config" / "ava"
    session_root = root / "state" / "ava" / "sessions"
    config.mkdir(parents=True, exist_ok=True)
    session_root.mkdir(parents=True, exist_ok=True)
    # Local model commands seal both roots. Keep the subprocess fixture's
    # config and session-parent hierarchy owner-private, like AVA requires.
    for directory in (root, root / "config", config, root / "state", root / "state" / "ava", session_root):
        os.chmod(directory, 0o700)
    (config / "models.json").write_text(json.dumps({
        "default_provider": "moonshot",
        "default_model": "acp-fake",
        "models": [{
            "provider": "moonshot", "id": "acp-fake", "name": "ACP Fake", "family": "fake",
            "context_window_tokens": 8192, "max_output_tokens": 1024,
            "supports_tools": supports_tools, "supports_streaming": False,
            "input_modalities": input_modalities or ["text"], "output_modalities": ["text"],
        }],
    }))


def send(process, payload, ending=b"\n"):
    process.stdin.write(payload + ending)
    process.stdin.flush()


def read_line(process, timeout=10.0, operation="ACP response"):
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    ready = selector.select(test_timeout(timeout))
    selector.close()
    if not ready:
        raise AssertionError(
            f"timed out waiting for {operation}; process.poll()={process.poll()!r}")
    line = process.stdout.readline()
    assert line, f"ACP stdout closed during {operation}; process.poll()={process.poll()!r}"
    return json.loads(line)


def read_until_id(process, request_id, timeout=15.0, operation="ACP request completion"):
    records = []
    deadline = time.monotonic() + test_timeout(timeout)
    while time.monotonic() < deadline:
        record = read_line(
            process, max(0.05, deadline - time.monotonic()), operation)
        records.append(record)
        if record.get("id", object()) == request_id:
            return records
    raise AssertionError(
        f"timed out waiting for {operation}; process.poll()={process.poll()!r}")


def expect_no_stdout(process, timeout=0.12):
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    ready = selector.select(timeout)
    selector.close()
    assert not ready, "unexpected ACP stdout record"


def stop_cleanly(process):
    process.stdin.close()
    assert process.wait(timeout=test_timeout(10)) == 0
    assert process.stdout.read() == b""
    assert process.stderr.read() == b""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True)
    parser.add_argument("--fake-provider", required=True)
    parser.add_argument("--fake-mcp", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()
    # Command plans validate workspace ancestry. Use a private mkdtemp root
    # (honors TMPDIR) rather than the build tree, whose checkout ancestor can
    # deliberately be shared. The directory is removed during process cleanup.
    root = Path(tempfile.mkdtemp(prefix="ava-acp-subprocess-"))
    OWNED_DIRECTORIES.append(root)

    conflict = subprocess.run([args.ava, "--acp", "--rpc"], input=b"", capture_output=True, env=environment(root), timeout=test_timeout(10))
    assert conflict.returncode != 0 and conflict.stdout == b"" and b"standalone" in conflict.stderr

    parent_secret_name = "AVA_ACP_SUBPROCESS_PARENT_SECRET"
    previous_parent_secret = os.environ.get(parent_secret_name)
    os.environ[parent_secret_name] = "must-not-reach-child"
    process = start(args.ava, root / "recovery")
    child_environment = Path(f"/proc/{process.pid}/environ").read_bytes().split(b"\0")
    assert not any(item.startswith(parent_secret_name.encode() + b"=") for item in child_environment)
    if previous_parent_secret is None:
        os.environ.pop(parent_secret_name, None)
    else:
        os.environ[parent_secret_name] = previous_parent_secret
    send(process, b"{")
    parse_error = read_line(process)
    assert parse_error["error"]["code"] == -32700 and parse_error["id"] is None
    send(process, b"\xff")
    utf8_error = read_line(process)
    assert utf8_error["error"]["code"] == -32700
    send(process, b"[]", ending=b"\r\n")
    batch_error = read_line(process)
    assert batch_error["error"]["code"] == -32600
    send(process, json.dumps({"jsonrpc": "2.0", "id": "init", "method": "initialize", "params": {"protocolVersion": 99}}).encode(), ending=b"\r\n")
    initialized = read_line(process)
    assert initialized["id"] == "init"
    assert initialized["result"]["protocolVersion"] == 1
    capabilities = initialized["result"]["agentCapabilities"]
    assert capabilities["loadSession"] is False
    assert capabilities["sessionCapabilities"] == {"list": {}, "resume": {}, "close": {}}
    assert capabilities["promptCapabilities"] == {"image": True, "audio": False, "embeddedContext": False}
    assert "delete" not in capabilities["sessionCapabilities"]
    assert initialized["result"]["authMethods"] == []
    send(process, json.dumps({"jsonrpc": "2.0", "id": "list-omitted", "method": "session/list"}).encode())
    assert read_line(process)["result"]["sessions"] == []
    send(process, json.dumps({"jsonrpc": "2.0", "id": "list-null", "method": "session/list", "params": None}).encode())
    assert read_line(process)["result"]["sessions"] == []
    send(process, json.dumps({"jsonrpc": "2.0", "id": "load-deferred", "method": "session/load", "params": {}}).encode())
    assert read_line(process)["error"]["code"] == -32601
    send(process, json.dumps({"jsonrpc": "2.0", "method": "unknown", "params": {}}).encode())
    expect_no_stdout(process)
    send(process, json.dumps({"jsonrpc": "2.0", "id": 4, "method": "unknown", "params": {}}).encode())
    missing = read_line(process)
    assert missing["id"] == 4 and missing["error"]["code"] == -32601
    send(process, json.dumps({"jsonrpc": "2.0", "id": None, "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    null_init = read_line(process)
    assert null_init["id"] is None and null_init["error"]["code"] == -32600
    for duplicate in (
        b'{"jsonrpc":"2.0","id":5,"id":6,"method":"initialize","params":{"protocolVersion":1}}',
        b'{"jsonrpc":"2.0","id":5,"method":"initialize","m\\u0065thod":"other","params":{"protocolVersion":1}}',
        b'{"jsonrpc":"2.0","id":5,"method":"initialize","params":{"protocolVersion":1,"future":{"x":1,"x":2}}}',
        b'{"jsonrpc":"2.0","jsonrpc":"2.0","id":5,"method":"initialize","params":{"protocolVersion":1}}',
    ):
        send(process, duplicate)
        assert read_line(process)["error"]["code"] == -32600
    stop_cleanly(process)

    preinit = start(args.ava, root / "preinit")
    send(preinit, json.dumps({"jsonrpc": "2.0", "id": 1, "method": "session/new", "params": {}}).encode())
    rejected = read_line(preinit)
    assert rejected["error"]["code"] == -32600
    stop_cleanly(preinit)

    eof = start(args.ava, root / "eof")
    stop_cleanly(eof)

    loops = start(args.ava, root / "loop-prevention")
    malformed_response_intent = (
        b'{"jsonrpc":"1.0","id":9,"result":{}}',
        b'{"jsonrpc":"2.0","id":9,"error":{"code":"bad","message":1}}',
        b'{"jsonrpc":"2.0","id":10}',
        b'{"jsonrpc":"2.0","id":null}',
        b'{"jsonrpc":"2.0","id":11,"result":{},"error":{"code":1,"message":"bad"}}',
    )
    for record in malformed_response_intent:
        send(loops, record)
    expect_no_stdout(loops)

    deep_result = b"[" * 80 + b"0" + b"]" * 80
    for record in (
        b'{"jsonrpc":"2.0","id":20,"result":' + deep_result + b"}",
        b'{"jsonrpc":"2.0","result":' + deep_result + b',"id":null}',
    ):
        send(loops, record)
    expect_no_stdout(loops)

    oversized_value = b"x" * (1024 * 1024 + 64)
    for record in (
        b'{"jsonrpc":"2.0","id":21,"result":"' + oversized_value + b'"}',
        b'{"jsonrpc":"2.0","id":null,"result":"' + oversized_value + b'"}',
        b'{"jsonrpc":"2.0","result":"' + oversized_value + b'","id":22}',
        b'{"jsonrpc":"2.0","result":"' + oversized_value + b'","id":null}',
    ):
        send(loops, record)
    expect_no_stdout(loops)

    oversized_notification = (
        b'{"jsonrpc":"2.0","params":{"blob":"' + oversized_value
        + b'"},"method":"unknown"}'
    )
    send(loops, oversized_notification)
    expect_no_stdout(loops)

    oversized_request = (
        b'{"jsonrpc":"2.0","params":{"blob":"' + oversized_value
        + b'"},"id":22,"method":"initialize"}'
    )
    send(loops, oversized_request)
    request_error = read_line(loops)
    assert request_error["id"] is None and request_error["error"]["code"] == -32700

    send(loops, b'{"jsonrpc":"2.0","id":"usable","method":"initialize","params":{"protocolVersion":1}}')
    assert read_line(loops)["id"] == "usable"
    loops.stdin.close()
    assert loops.wait(timeout=test_timeout(10)) == 0
    assert loops.stdout.read() == b""
    diagnostics = loops.stderr.read().splitlines()
    response_intent_records = len(malformed_response_intent) + 6
    assert 1 <= len(diagnostics) <= response_intent_records
    assert all(b"response" in line and b"result" not in line and b"error" not in line for line in diagnostics)

    lifecycle_root = root / "lifecycle-env"
    workspace = root / "workspace"
    nested = workspace / "nested"
    outside = root / "outside"
    nested.mkdir(parents=True, exist_ok=True)
    outside.mkdir(parents=True, exist_ok=True)
    os.chmod(root, 0o700)
    os.chmod(workspace, 0o700)
    configure_fake_model(lifecycle_root)
    provider = start_fake_provider(args.fake_provider, root / "provider", scenario="text-three-delayed-first")
    lifecycle = start(
        args.ava, lifecycle_root, cwd=workspace,
        extra_env={"MOONSHOT_BASE_URL": f"http://127.0.0.1:{provider.port}", "MOONSHOT_API_KEY": "fake-acp-key"},
    )
    send(lifecycle, json.dumps({"jsonrpc": "2.0", "id": "i", "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    lifecycle_init = read_line(lifecycle, operation="lifecycle initialize response")["result"]
    assert lifecycle_init["protocolVersion"] == 1
    assert lifecycle_init["agentCapabilities"]["loadSession"] is False
    assert lifecycle_init["agentCapabilities"]["promptCapabilities"]["image"] is False

    def new_session(request_id, cwd):
        send(lifecycle, json.dumps({
            "jsonrpc": "2.0", "id": request_id, "method": "session/new",
            "params": {"cwd": str(cwd), "mcpServers": []},
        }).encode())
        return read_line(lifecycle, operation="session/new lifecycle response")

    traversal = new_session("traversal", workspace / ".." / "outside")
    assert traversal["error"]["code"] == -32602 and "traversal" in traversal["error"]["message"]
    link = workspace / "linked"
    try:
        link.symlink_to(nested, target_is_directory=True)
        linked = new_session("symlink", link)
        assert linked["error"]["code"] == -32602 and "symlink" in linked["error"]["message"]
    except OSError:
        pass
    send(lifecycle, json.dumps({
        "jsonrpc": "2.0", "id": "mcp-http", "method": "session/new",
        "params": {"cwd": str(workspace), "mcpServers": [{"type": "http", "name": "remote", "url": "https://example.invalid", "headers": []}]},
    }).encode())
    assert read_line(lifecycle)["error"]["code"] == -32602
    send(lifecycle, json.dumps({
        "jsonrpc": "2.0", "id": "mcp-duplicate", "method": "session/new",
        "params": {"cwd": str(workspace), "mcpServers": [
            {"name": "duplicate", "command": str(Path(args.fake_mcp).absolute()), "args": [], "env": []},
            {"name": "duplicate", "command": str(Path(args.fake_mcp).absolute()), "args": [], "env": []},
        ]},
    }).encode())
    assert read_line(lifecycle)["error"]["code"] == -32602
    send(lifecycle, json.dumps({
        "jsonrpc": "2.0", "id": "mcp-stdio", "method": "session/new",
        "params": {"cwd": str(workspace), "mcpServers": [{
            "name": "stdio-demo", "command": str(Path(args.fake_mcp).absolute()), "args": [],
            "env": [{"name": "AVA_MCP_EXPLICIT", "value": "session-local"}],
        }]},
    }).encode())
    stdio_session = read_line(lifecycle)["result"]["sessionId"]
    send(lifecycle, json.dumps({"jsonrpc": "2.0", "id": "close-stdio", "method": "session/close", "params": {"sessionId": stdio_session}}).encode())
    assert read_line(lifecycle)["result"] == {}

    first = new_session("new-a", workspace)
    second = new_session("new-b", nested)
    session_a = first["result"]["sessionId"]
    session_b = second["result"]["sessionId"]
    assert session_a != session_b

    send(lifecycle, json.dumps({
        "jsonrpc": "2.0", "id": "text-model-image", "method": "session/prompt",
        "params": {"sessionId": session_b, "prompt": [
            {"type": "image", "data": "iVBORw0KGgo=", "mimeType": "image/png"},
        ]},
    }).encode())
    assert read_line(lifecycle)["error"]["code"] == -32602

    prompt = lambda rid, sid, text: send(lifecycle, json.dumps({
        "jsonrpc": "2.0", "id": rid, "method": "session/prompt",
        "params": {"sessionId": sid, "prompt": [{"type": "text", "text": text}]},
    }).encode())
    prompt("p1", session_a, "first")
    provider.wait_for_request(0, "active prompt provider request", timeout=test_timeout(10))
    prompt("same-active", session_a, "must reject")
    active_rejection = read_line(lifecycle)
    assert active_rejection["id"] == "same-active" and active_rejection["error"]["code"] == -32600
    provider.release_request(0)
    first_records = read_until_id(lifecycle, "p1", operation="first session prompt completion")
    assert first_records[-1]["result"]["stopReason"] == "end_turn"
    assert first_records[-2]["method"] == "session/update"

    prompt("pa", session_a, "independent a")
    prompt("pb", session_b, "independent b")
    independent = []
    completed = set()
    while completed != {"pa", "pb"}:
        record = read_line(lifecycle, 10, "parallel session prompt completion")
        independent.append(record)
        if record.get("id") in {"pa", "pb"}:
            assert record["result"]["stopReason"] == "end_turn"
            completed.add(record["id"])
    assert all(record.get("error") is None for record in independent)

    send(lifecycle, json.dumps({"jsonrpc": "2.0", "id": "close-a", "method": "session/close", "params": {"sessionId": session_a}}).encode())
    assert read_line(lifecycle)["result"] == {}
    send(lifecycle, json.dumps({
        "jsonrpc": "2.0", "id": "load-a", "method": "session/load",
        "params": {"sessionId": session_a, "cwd": str(workspace), "mcpServers": []},
    }).encode())
    load_rejected = read_line(lifecycle)
    assert load_rejected["id"] == "load-a" and load_rejected["error"]["code"] == -32601
    expect_no_stdout(lifecycle)

    send(lifecycle, json.dumps({"jsonrpc": "2.0", "id": "list", "method": "session/list", "params": {}}).encode())
    listed = read_line(lifecycle)["result"]["sessions"]
    assert {item["sessionId"] for item in listed} >= {session_a, session_b}
    send(lifecycle, json.dumps({
        "jsonrpc": "2.0", "id": "resume-a", "method": "session/resume",
        "params": {"sessionId": session_a, "cwd": str(workspace), "mcpServers": []},
    }).encode())
    assert read_line(lifecycle)["result"] == {}
    send(lifecycle, json.dumps({"jsonrpc": "2.0", "id": "close-resumed", "method": "session/close", "params": {"sessionId": session_a}}).encode())
    assert read_line(lifecycle)["result"] == {}
    stop_cleanly(lifecycle)
    provider.finish(timeout=test_timeout(10))

    # A real bidirectional client must be able to answer a permission request
    # while session/prompt is still in flight. Tool updates remain ordered
    # around the request and the terminal response is last.
    permission_root = root / "permission-env"
    configure_fake_model(permission_root, supports_tools=True)
    permission_target = workspace / "permission-read.txt"
    permission_target.write_text("ACP_PERMISSION_CONTENT")
    permission_provider = start_fake_provider(
        args.fake_provider, root / "permission-provider", scenario="read-tool", target=permission_target
    )
    permission_process = start(
        args.ava, permission_root, cwd=workspace,
        extra_env={"MOONSHOT_BASE_URL": f"http://127.0.0.1:{permission_provider.port}", "MOONSHOT_API_KEY": "fake-acp-key"},
    )
    send(permission_process, json.dumps({"jsonrpc": "2.0", "id": "pi", "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    permission_init = read_line(permission_process)["result"]["agentCapabilities"]
    assert permission_init["promptCapabilities"]["image"] is False and permission_init["loadSession"] is False
    send(permission_process, json.dumps({
        "jsonrpc": "2.0", "id": "pn", "method": "session/new",
        "params": {"cwd": str(workspace), "mcpServers": []},
    }).encode())
    permission_session = read_line(permission_process)["result"]["sessionId"]
    send(permission_process, json.dumps({
        "jsonrpc": "2.0", "id": "permission-prompt", "method": "session/prompt",
        "params": {"sessionId": permission_session, "prompt": [{"type": "text", "text": "read the permission target"}]},
    }).encode())
    permission_records = []
    permission_request = None
    while True:
        record = read_line(permission_process, 10, "permission session lifecycle response")
        permission_records.append(record)
        if record.get("method") == "session/request_permission":
            permission_request = record
            assert record["params"]["sessionId"] == permission_session
            assert record["params"]["toolCall"]["toolCallId"] == "call_read"
            assert record["params"]["toolCall"]["status"] == "pending"
            assert [option["optionId"] for option in record["params"]["options"]] == [
                "allow_once", "allow_always", "reject_once", "reject_always"
            ]
            assert "ACP_PERMISSION_CONTENT" not in json.dumps(record)
            send(permission_process, json.dumps({
                "jsonrpc": "2.0", "id": record["id"],
                "result": {"outcome": {"outcome": "selected", "optionId": "allow_once"}},
            }).encode())
        if record.get("id") == "permission-prompt":
            break
    update_kinds = [
        record["params"]["update"]["sessionUpdate"] for record in permission_records
        if record.get("method") == "session/update"
    ]
    assert permission_request is not None
    permission_index = permission_records.index(permission_request)
    tool_updates = [
        (index, record["params"]["update"]) for index, record in enumerate(permission_records)
        if record.get("method") == "session/update" and record["params"]["update"].get("toolCallId") == "call_read"
    ]
    statuses = [(index, update["status"]) for index, update in tool_updates]
    assert [status for _, status in statuses] == ["pending", "in_progress", "completed"]
    assert statuses[0][0] < permission_index < statuses[1][0] < statuses[2][0]
    assert update_kinds[-1] == "agent_message_chunk"
    assert permission_records[-1]["result"]["stopReason"] == "end_turn"
    stop_cleanly(permission_process)
    permission_provider.finish(timeout=test_timeout(10))
    assert "ACP_PERMISSION_CONTENT" in permission_provider.request_log.read_text(errors="replace")

    # M5 client filesystem routing is exercised through the real stdio peer,
    # including permission-before-read and the exact outbound DTO.
    client_fs_root = root / "client-fs-env"
    configure_fake_model(client_fs_root, supports_tools=True)
    client_fs_target = workspace / "client-fs.txt"
    client_fs_target.write_text("LOCAL_CONTENT_MUST_NOT_WIN")
    client_fs_provider = start_fake_provider(
        args.fake_provider, root / "client-fs-provider", scenario="read-tool", target=client_fs_target
    )
    client_fs = start(
        args.ava, client_fs_root, cwd=workspace,
        extra_env={"MOONSHOT_BASE_URL": f"http://127.0.0.1:{client_fs_provider.port}", "MOONSHOT_API_KEY": "fake-acp-key"},
    )
    send(client_fs, json.dumps({
        "jsonrpc": "2.0", "id": "fsi", "method": "initialize",
        "params": {"protocolVersion": 1, "clientCapabilities": {"fs": {"readTextFile": True, "writeTextFile": True}}},
    }).encode())
    assert read_line(client_fs)["result"]
    send(client_fs, json.dumps({
        "jsonrpc": "2.0", "id": "fsn", "method": "session/new", "params": {"cwd": str(workspace), "mcpServers": []},
    }).encode())
    client_fs_session = read_line(client_fs)["result"]["sessionId"]
    send(client_fs, json.dumps({
        "jsonrpc": "2.0", "id": "fsp", "method": "session/prompt",
        "params": {"sessionId": client_fs_session, "prompt": [{"type": "text", "text": "read through client fs"}]},
    }).encode())
    client_fs_methods = []
    while True:
        record = read_line(client_fs, 10, "client filesystem session lifecycle response")
        method = record.get("method")
        if method == "session/request_permission":
            client_fs_methods.append(method)
            send(client_fs, json.dumps({
                "jsonrpc": "2.0", "id": record["id"],
                "result": {"outcome": {"outcome": "selected", "optionId": "allow_once"}},
            }).encode())
        elif method == "fs/read_text_file":
            client_fs_methods.append(method)
            assert record["params"] == {
                "sessionId": client_fs_session,
                "path": str(client_fs_target),
                "line": 1,
                "limit": 201,
            }
            send(client_fs, json.dumps({
                "jsonrpc": "2.0", "id": record["id"], "result": {"content": "REMOTE_CLIENT_FS_CONTENT"},
            }).encode())
        if record.get("id") == "fsp":
            assert record["result"]["stopReason"] == "end_turn"
            break
    assert client_fs_methods == ["session/request_permission", "fs/read_text_file"]
    stop_cleanly(client_fs)
    client_fs_provider.finish(timeout=test_timeout(10))
    client_fs_requests = client_fs_provider.request_log.read_text(errors="replace")
    assert "REMOTE_CLIENT_FS_CONTENT" in client_fs_requests and "LOCAL_CONTENT_MUST_NOT_WIN" not in client_fs_requests

    # A negotiated terminal follows the complete remote lifecycle and never
    # starts a local process in the real ava --acp process.
    terminal_root = root / "terminal-env"
    configure_fake_model(terminal_root, supports_tools=True)
    terminal_provider = start_fake_provider(
        args.fake_provider, root / "terminal-provider", scenario="terminal-tool"
    )
    terminal_process = start(
        args.ava, terminal_root, cwd=workspace,
        extra_env={"MOONSHOT_BASE_URL": f"http://127.0.0.1:{terminal_provider.port}", "MOONSHOT_API_KEY": "fake-acp-key"},
    )
    send(terminal_process, json.dumps({
        "jsonrpc": "2.0", "id": "ti", "method": "initialize",
        "params": {"protocolVersion": 1, "clientCapabilities": {"terminal": True}},
    }).encode())
    assert read_line(terminal_process)["result"]
    send(terminal_process, json.dumps({
        "jsonrpc": "2.0", "id": "tn", "method": "session/new", "params": {"cwd": str(workspace), "mcpServers": []},
    }).encode())
    terminal_session = read_line(terminal_process)["result"]["sessionId"]
    send(terminal_process, json.dumps({
        "jsonrpc": "2.0", "id": "tp", "method": "session/prompt",
        "params": {"sessionId": terminal_session, "prompt": [{"type": "text", "text": "run through client terminal"}]},
    }).encode())
    terminal_methods = []
    while True:
        record = read_line(terminal_process, 10, "terminal session lifecycle response")
        method = record.get("method")
        if method == "session/request_permission":
            terminal_methods.append(method)
            send(terminal_process, json.dumps({
                "jsonrpc": "2.0", "id": record["id"],
                "result": {"outcome": {"outcome": "selected", "optionId": "allow_once"}},
            }).encode())
        elif method and method.startswith("terminal/"):
            terminal_methods.append(method)
            if method == "terminal/create":
                assert record["params"]["command"] == "touch"
                assert record["params"]["args"] == ["terminal-e2e-marker"]
                assert record["params"]["env"] == []
                assert record["params"]["cwd"] == str(workspace)
                result = {"terminalId": "subprocess-terminal"}
            elif method == "terminal/wait_for_exit":
                result = {"exitCode": 0, "signal": None}
            elif method == "terminal/output":
                result = {"output": "SUBPROCESS_TERMINAL_OUTPUT", "truncated": False}
            else:
                result = {}
            send(terminal_process, json.dumps({"jsonrpc": "2.0", "id": record["id"], "result": result}).encode())
        if record.get("id") == "tp":
            assert record["result"]["stopReason"] == "end_turn"
            break
    assert terminal_methods == [
        "session/request_permission", "terminal/create", "terminal/wait_for_exit", "terminal/output", "terminal/release"
    ]
    stop_cleanly(terminal_process)
    terminal_provider.finish(timeout=test_timeout(10))
    assert "SUBPROCESS_TERMINAL_OUTPUT" in terminal_provider.request_log.read_text(errors="replace")
    assert not (workspace / "terminal-e2e-marker").exists()

    image_root = root / "image-env"
    configure_fake_model(image_root, input_modalities=["text", "image"])
    image_provider = start_fake_provider(args.fake_provider, root / "image-provider", scenario="text")
    image_process = start(
        args.ava, image_root, cwd=workspace,
        extra_env={"MOONSHOT_BASE_URL": f"http://127.0.0.1:{image_provider.port}", "MOONSHOT_API_KEY": "fake-acp-key"},
    )
    send(image_process, json.dumps({"jsonrpc": "2.0", "id": "ii", "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    image_init = read_line(image_process)["result"]["agentCapabilities"]
    assert image_init["promptCapabilities"]["image"] is True and image_init["loadSession"] is False
    send(image_process, json.dumps({
        "jsonrpc": "2.0", "id": "in", "method": "session/new",
        "params": {"cwd": str(workspace), "mcpServers": []},
    }).encode())
    image_session = read_line(image_process)["result"]["sessionId"]
    send(image_process, json.dumps({
        "jsonrpc": "2.0", "id": "image-prompt", "method": "session/prompt",
        "params": {"sessionId": image_session, "prompt": [
            {"type": "text", "text": "inspect the image"},
            {"type": "image", "data": "iVBORw0KGgo=", "mimeType": "image/png", "uri": None},
        ]},
    }).encode())
    image_records = read_until_id(image_process, "image-prompt", operation="image session prompt completion")
    assert image_records[-1]["result"]["stopReason"] == "end_turn"
    stop_cleanly(image_process)
    image_provider.finish(timeout=test_timeout(10))
    image_requests = image_provider.request_log.read_text(errors="replace")
    assert "iVBORw0KGgo=" in image_requests and "image/png" in image_requests

    cancel_root = root / "cancel-env"
    configure_fake_model(cancel_root)
    cancel_provider = start_fake_provider(args.fake_provider, root / "cancel-provider", scenario="text")
    cancel_process = start(
        args.ava, cancel_root, cwd=workspace,
        extra_env={"MOONSHOT_BASE_URL": f"http://127.0.0.1:{cancel_provider.port}", "MOONSHOT_API_KEY": "fake-acp-key"},
    )
    send(cancel_process, json.dumps({"jsonrpc": "2.0", "id": "ci", "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    assert read_line(cancel_process)["result"]
    send(cancel_process, json.dumps({
        "jsonrpc": "2.0", "id": "cn", "method": "session/new", "params": {"cwd": str(workspace), "mcpServers": []},
    }).encode())
    cancel_session = read_line(cancel_process)["result"]["sessionId"]
    send(cancel_process, json.dumps({
        "jsonrpc": "2.0", "method": "session/cancel", "params": {"sessionId": cancel_session},
    }).encode())
    send(cancel_process, json.dumps({
        "jsonrpc": "2.0", "id": "idle-survives", "method": "session/prompt",
        "params": {"sessionId": cancel_session, "prompt": [{"type": "text", "text": "idle cancellation must not leak"}]},
    }).encode())
    idle_records = read_until_id(cancel_process, "idle-survives", operation="idle cancellation session prompt completion")
    assert idle_records[-1]["result"]["stopReason"] == "end_turn"

    stop_cleanly(cancel_process)
    cancel_provider.finish(timeout=test_timeout(10))

    # Active cancellation is gated: the delayed provider holds the prompt's HTTP
    # request behind gate 0, so session/cancel provably lands while the request
    # is in flight instead of racing a time-based delay.
    cancel_active_root = root / "cancel-active-env"
    configure_fake_model(cancel_active_root)
    active_provider = start_fake_provider(args.fake_provider, root / "cancel-active-provider", scenario="text-delayed")
    active_process = start(
        args.ava, cancel_active_root, cwd=workspace,
        extra_env={"MOONSHOT_BASE_URL": f"http://127.0.0.1:{active_provider.port}", "MOONSHOT_API_KEY": "fake-acp-key"},
    )
    send(active_process, json.dumps({"jsonrpc": "2.0", "id": "ci", "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    assert read_line(active_process)["result"]
    send(active_process, json.dumps({
        "jsonrpc": "2.0", "id": "cn", "method": "session/new", "params": {"cwd": str(workspace), "mcpServers": []},
    }).encode())
    active_session = read_line(active_process)["result"]["sessionId"]
    send(active_process, json.dumps({
        "jsonrpc": "2.0", "id": "cancel-prompt", "method": "session/prompt",
        "params": {"sessionId": active_session, "prompt": [{"type": "text", "text": "cancel the in-flight request"}]},
    }).encode())
    active_provider.wait_for_request(0, "active cancellation provider request", timeout=test_timeout(10))
    send(active_process, json.dumps({
        "jsonrpc": "2.0", "method": "session/cancel", "params": {"sessionId": active_session},
    }).encode())
    canceled_records = read_until_id(active_process, "cancel-prompt", operation="active session cancellation completion")
    assert canceled_records[-1]["result"]["stopReason"] == "cancelled"
    active_provider.release_request(0)
    stop_cleanly(active_process)
    # Cancellation closed the provider HTTP connection, so the delayed response
    # write may fail after the gate release; only prove the process is gone.
    active_provider.stop()

    auth_root = root / "auth-env"
    configure_fake_model(auth_root)
    auth = start(args.ava, auth_root, cwd=workspace, extra_env={"MOONSHOT_BASE_URL": "http://127.0.0.1:1"})
    send(auth, json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    assert read_line(auth, operation="auth lifecycle initialize response")["result"]
    send(auth, json.dumps({"jsonrpc": "2.0", "id": 2, "method": "session/new", "params": {"cwd": str(workspace), "mcpServers": []}}).encode())
    auth_session = read_line(auth, operation="auth lifecycle session construction")["result"]["sessionId"]
    send(auth, json.dumps({
        "jsonrpc": "2.0", "id": 3, "method": "session/prompt",
        "params": {"sessionId": auth_session, "prompt": [{"type": "text", "text": "needs auth"}]},
    }).encode())
    auth_error = read_line(auth, operation="auth lifecycle prompt response")
    assert auth_error["error"]["code"] == -32000
    assert "moonshot" in auth_error["error"]["message"] and "hint" in auth_error["error"]["message"]
    stop_cleanly(auth)

    model_root = root / "model-env"
    model_config = model_root / "config" / "ava"
    model_config.mkdir(parents=True, exist_ok=True)
    (model_config / "models.json").write_text(json.dumps({
        "default_provider": "bogus", "default_model": "missing",
        "models": [{"provider": "bogus", "id": "missing", "name": "Missing", "family": "fake", "supports_tools": False}],
    }))
    model_process = start(args.ava, model_root, cwd=workspace)
    assert model_process.wait(timeout=test_timeout(10)) != 0
    assert model_process.stdout.read() == b""
    model_diagnostic = model_process.stderr.read()
    assert b"startup provider is not registered" in model_diagnostic and b"bogus" in model_diagnostic
    model_process.stdin.close()

    broken = start(args.ava, root / "broken")
    broken.stdout.close()
    send(broken, json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": 1}}).encode())
    assert broken.wait(timeout=test_timeout(10)) != 0
    broken.stdin.close()
    diagnostic = broken.stderr.read()
    assert diagnostic and b"protocolVersion" not in diagnostic and b"initialize" not in diagnostic

    print("real ava --acp subprocess checks passed")
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal_cleanup)
    signal.signal(signal.SIGTERM, signal_cleanup)
    try:
        raise SystemExit(main())
    finally:
        cleanup_owned_processes()
