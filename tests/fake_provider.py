#!/usr/bin/env python3
"""Shared credential-free fake-provider process owner and gate broker CLI.

Every Python, CMake, Node, and shell test harness launches the C++
``ava_fake_provider_server`` through this single owner so process-gate wiring,
bounded startup, and process-group cleanup stay identical everywhere.

Direct Python API::

    provider = launch_fake_provider(exe, directory, scenario="text-delayed")
    provider.wait_for_request(0, "first prompt reached provider")
    provider.release_request(0)
    provider.stop()

Non-Python harnesses spawn the same file as a line-based broker instead::

    python3 fake_provider.py broker --provider EXE --directory DIR \
        --prefix NAME --delay-ms 0 --scenario text-delayed --target unused

The broker prints ``ready port=<n>`` (or ``error <message>``) on stdout, then
answers one command per stdin line: ``wait <index> <timeout>``,
``release <index>``, ``finish <timeout>``, and ``stop``. Replies are ``ok`` or
``error <message>``. ``finish`` requires natural exit 0 after all scripted
requests; ``stop`` is cancellation/error cleanup. The broker owns provider
cleanup on EOF, signal, and finish failure, so no orphan provider or helper
process survives its caller.

RPC-client timeout tests may instead use ``with RpcTombstoneServer(root) as
server`` and launch ``AvaRpcClient(server.command)``. After the first request
times out, ``server.wait_for_request()`` acknowledges that it reached the fake;
``server.release_response()`` releases its late response. The client owns its
child process; close/kill it before leaving the server's socket-owning context.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import signal
import subprocess
import sys
import time
import json
import socket
import tempfile
from timeout_support import test_timeout
from cli_test_support import terminate_process_group

from process_gate import PROCESS_GATE_FD_ENV, ProcessGateSet, create_process_gate_pair
from test_timing_trace import timed_operation, timing_poll

_PROVIDER_STARTUP_TIMEOUT = 8.0
# Bound every broker command so a caller blocked on a reply always makes
# progress; matches the one-hour debug-timeout stretch ceiling.
_BROKER_COMMAND_TIMEOUT_CEILING = 3600.0


def _one_line(message: object, limit: int = 400) -> str:
    """Flatten a diagnostic so broker protocol replies stay single-line."""

    flattened = " / ".join(str(message).splitlines())
    return flattened[:limit]


class FakeProvider:
    """Own one fake-provider process and its harness-side control gates.

    The provider opens gate N after recording zero-based HTTP request N. Tests
    may also open gates in the reverse direction for scenario-specific barriers;
    stopping the provider closes the control endpoint after process cleanup.
    """

    def __init__(
        self,
        directory: pathlib.Path,
        prefix: str,
        process: subprocess.Popen[str],
        port_file: pathlib.Path,
        request_log: pathlib.Path,
        stdout_path: pathlib.Path,
        stderr_path: pathlib.Path,
        gates: ProcessGateSet,
        stdout: object,
        stderr: object,
    ) -> None:
        self.directory = directory
        self.prefix = prefix
        self.process = process
        self.port_file = port_file
        self.request_log = request_log
        self.stdout_path = stdout_path
        self.stderr_path = stderr_path
        self.gates = gates
        self._stdout = stdout
        self._stderr = stderr
        self._stopped = False

    @property
    def port(self) -> str:
        return self.port_file.read_text(encoding="utf-8").strip()

    @timed_operation("provider_gate", label_argument="label")
    def wait_for_request(self, request_index: int, label: str, timeout: float = 8.0) -> None:
        """Wait until the provider records zero-based ``request_index``."""

        self.gates.wait(request_index, timeout)

    def release_request(self, request_index: int) -> None:
        """Allow a gate-delayed zero-based provider request to respond."""

        self.gates.open(request_index)

    def stderr_text(self) -> str:
        if not self.stderr_path.exists():
            return ""
        return self.stderr_path.read_text(encoding="utf-8", errors="replace")

    @timed_operation("provider_wait", label_argument=None, default_label="fake provider exit")
    def finish(self, timeout: float = 8.0) -> None:
        """Require the fully consumed provider to exit 0 within ``timeout``."""

        try:
            status = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.stop()
            raise RuntimeError("fake provider did not finish after its scripted requests") from None
        error = None
        if status != 0:
            error = RuntimeError(f"fake provider exited with {status}\nstderr:\n{self.stderr_text()}")
        self._close_streams()
        self.gates.close()
        self._stopped = True
        if error is not None:
            raise error

    def _close_streams(self) -> None:
        if not self._stdout.closed:
            self._stdout.close()
        if not self._stderr.closed:
            self._stderr.close()

    @timed_operation("cleanup", label_argument="prefix", default_label="fake provider cleanup")
    def stop(self) -> None:
        """Stop executable group members, reap the leader, and close owned channels."""

        if self._stopped:
            return
        self._stopped = True
        try:
            # A reaped leader does not imply its private group has no surviving
            # descendants. Shared cleanup ignores zombies and bounds TERM/KILL.
            terminate_process_group(self.process.pid)
        finally:
            try:
                self.process.wait(timeout=test_timeout(3.0))
            finally:
                self._close_streams()
                self.gates.close()


@timed_operation("setup", label_argument="prefix", default_label="fake provider startup")
def launch_fake_provider(
    executable: str | pathlib.Path,
    directory: str | pathlib.Path,
    *,
    prefix: str = "provider",
    delay_ms: int = 0,
    scenario: str = "text-three",
    target: str | pathlib.Path = "",
    environment: dict[str, str] | None = None,
    startup_timeout: float = _PROVIDER_STARTUP_TIMEOUT,
) -> FakeProvider:
    """Launch an isolated provider with one inherited process-gate endpoint.

    ``directory`` holds the private fixture artifacts named after ``prefix``,
    ``delay_ms`` only paces scenarios that intentionally stream deltas,
    ``scenario`` configures responses and request barriers, and ``target``
    supplies an optional fixture path. The returned owner exposes persistent
    bidirectional gates and guarantees that only the child retains its endpoint
    after ``Popen`` succeeds. Startup is bounded and every partial-failure path
    cleans up the pair, streams, and child.
    """

    directory = pathlib.Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    port_file = directory / f"{prefix}.port"
    request_log = directory / f"{prefix}-requests.log"
    stdout_path = directory / f"{prefix}.out"
    stderr_path = directory / f"{prefix}.err"
    port_file.unlink(missing_ok=True)
    request_log.unlink(missing_ok=True)
    stdout = stdout_path.open("w", encoding="utf-8")
    stderr = stderr_path.open("w", encoding="utf-8")
    gate_pair = create_process_gate_pair()
    provider_environment = dict(environment) if environment is not None else os.environ.copy()
    provider_environment[PROCESS_GATE_FD_ENV] = str(gate_pair.child_fd)
    try:
        process = subprocess.Popen(
            [str(executable), str(port_file), str(request_log), str(delay_ms), scenario, str(target)],
            stdout=stdout,
            stderr=stderr,
            env=provider_environment,
            pass_fds=(gate_pair.child_fd,),
            start_new_session=True,
            text=True,
        )
    except BaseException:
        gate_pair.close()
        stdout.close()
        stderr.close()
        raise
    gate_pair.close_child_endpoint()
    provider = FakeProvider(directory, prefix, process, port_file, request_log, stdout_path, stderr_path, gate_pair.gates, stdout, stderr)
    deadline = time.monotonic() + startup_timeout
    while True:
        timing_poll()
        if port_file.exists() and port_file.stat().st_size > 0:
            return provider
        if process.poll() is not None:
            exit_status = process.returncode
            provider.stop()
            raise RuntimeError(
                f"fake provider exited with {exit_status} before writing its port\n"
                f"stdout:\n{stdout_path.read_text(encoding='utf-8', errors='replace')}\n"
                f"stderr:\n{stderr_path.read_text(encoding='utf-8', errors='replace')}"
            )
        if time.monotonic() >= deadline:
            provider.stop()
            raise RuntimeError("timed out waiting for fake provider port")
        time.sleep(0.05)


def _parse_broker_index(parts: list[str], command: str) -> int:
    if len(parts) < 2:
        raise ValueError(f"broker command '{command}' requires a zero-based request index")
    index = int(parts[1])
    if not 0 <= index < 64:
        raise ValueError(f"broker request index must be in [0, 63], got {index}")
    return index


def run_broker(argv: list[str]) -> int:
    """Own one provider behind a single-line stdin/stdout command protocol."""

    parser = argparse.ArgumentParser(prog="fake_provider.py broker")
    parser.add_argument("--provider", required=True, type=pathlib.Path)
    parser.add_argument("--directory", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", default="provider")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--scenario", default="text")
    parser.add_argument("--target", default="")
    args = parser.parse_args(argv)

    try:
        provider = launch_fake_provider(
            args.provider,
            args.directory,
            prefix=args.prefix,
            delay_ms=args.delay_ms,
            scenario=args.scenario,
            target=args.target,
        )
    except BaseException as error:
        print(f"error {_one_line(error)}", flush=True)
        return 1

    def handle_signal(signum: int, _frame: object) -> None:
        provider.stop()
        raise SystemExit(128 + signum)

    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, handle_signal)

    def reply(message: str) -> None:
        try:
            print(message, flush=True)
        except BrokenPipeError:
            provider.stop()
            raise SystemExit(1) from None

    reply(f"ready port={provider.port}")
    while True:
        line = sys.stdin.readline()
        if line == "":
            # EOF: the owner walked away; clean up without judging provider state.
            provider.stop()
            return 0
        parts = line.split()
        if not parts:
            continue
        command = parts[0]
        try:
            if command == "wait":
                index = _parse_broker_index(parts, command)
                if len(parts) < 3:
                    raise ValueError("broker command 'wait' requires a timeout in seconds")
                timeout = float(parts[2])
                if not 0.0 <= timeout <= _BROKER_COMMAND_TIMEOUT_CEILING:
                    raise ValueError(f"broker wait timeout must be in [0, {_BROKER_COMMAND_TIMEOUT_CEILING}], got {timeout}")
                provider.wait_for_request(index, f"broker wait for request {index}", timeout)
                reply("ok")
            elif command == "release":
                provider.release_request(_parse_broker_index(parts, command))
                reply("ok")
            elif command == "finish":
                if len(parts) < 2:
                    raise ValueError("broker command 'finish' requires a timeout in seconds")
                timeout = float(parts[1])
                if not 0.0 <= timeout <= _BROKER_COMMAND_TIMEOUT_CEILING:
                    raise ValueError(f"broker finish timeout must be in [0, {_BROKER_COMMAND_TIMEOUT_CEILING}], got {timeout}")
                try:
                    provider.finish(timeout)
                except Exception as error:
                    reply(f"error {_one_line(error)}")
                    return 1
                reply("ok")
                return 0
            elif command == "stop":
                natural_exit = provider.process.poll()
                provider.stop()
                if natural_exit is None or natural_exit == 0:
                    reply("ok")
                    return 0
                reply(f"error provider exited with status {natural_exit}")
                return 1
            else:
                reply(f"error unknown broker command '{_one_line(command, 40)}'")
        except SystemExit:
            raise
        except BaseException as error:
            reply(f"error {_one_line(error)}")


class RpcTombstoneServer:
    """Own the private control socket for one parent-gated fake RPC server.

    ``command`` is compatible with the unmodified RPC client's Popen boundary:
    no inherited descriptors or environment changes are required. Calls on one
    instance must be serialized. Socket closure wakes a blocked fake, but the
    caller still owns client/process cleanup.
    """

    def __init__(self, directory: str | pathlib.Path) -> None:
        self._directory = tempfile.TemporaryDirectory(prefix="rpc-gates-", dir=directory)
        self._socket_path = pathlib.Path(self._directory.name) / "control.sock"
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._gates: ProcessGateSet | None = None
        try:
            self._listener.bind(str(self._socket_path))
            self._listener.listen(1)
        except BaseException:
            self.close()
            raise

    @property
    def command(self) -> list[str]:
        """Return the fake's argv for AvaRpcClient; it withholds response one."""

        return [sys.executable, "-u", str(pathlib.Path(__file__).resolve()),
                "rpc-tombstone", "--control-socket", str(self._socket_path)]

    def wait_for_request(self, timeout: float = 8.0) -> None:
        """Acknowledge first-request consumption without allowing a response."""

        deadline = time.monotonic() + test_timeout(timeout)
        if self._gates is None:
            self._listener.settimeout(max(0.0, deadline - time.monotonic()))
            endpoint, _ = self._listener.accept()
            self._gates = ProcessGateSet(endpoint)
        self._gates.wait(0, max(0.0, deadline - time.monotonic()))

    def release_response(self) -> None:
        """Release response one only after wait_for_request has acknowledged it."""

        if self._gates is None:
            raise RuntimeError("wait_for_request must precede release_response")
        self._gates.open(0)

    def close(self) -> None:
        """Close owned endpoints and remove the private socket directory."""

        if self._gates is not None:
            self._gates.close()
            self._gates = None
        self._listener.close()
        self._directory.cleanup()

    def __enter__(self) -> RpcTombstoneServer:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


def run_rpc_tombstone(argv: list[str]) -> int:
    """Read two RPC requests, explicitly gate the first, and label generations."""

    parser = argparse.ArgumentParser(prog="fake_provider.py rpc-tombstone")
    parser.add_argument("--control-socket", required=True)
    args = parser.parse_args(argv)
    endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        endpoint.connect(args.control_socket)
    except BaseException:
        endpoint.close()
        raise
    gates = ProcessGateSet(endpoint)
    try:
        first = json.loads(sys.stdin.readline())
        gates.open(0)
        gates.wait(0, test_timeout(30.0))
        print(json.dumps({"id": first["id"], "type": "response", "success": True,
                          "result": {"generation": 1}}), flush=True)
        print(json.dumps({"schema_version": 1, "event_id": "late", "name": "late_response_sent",
                          "type": "late_response_sent", "payload": {}}), flush=True)
        second = json.loads(sys.stdin.readline())
        print(json.dumps({"id": second["id"], "type": "response", "success": True,
                          "result": {"generation": 2}}), flush=True)
        return 0
    finally:
        gates.close()


def main(argv: list[str]) -> int:
    if argv and argv[0] == "rpc-tombstone":
        return run_rpc_tombstone(argv[1:])
    if argv and argv[0] == "broker":
        return run_broker(argv[1:])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
