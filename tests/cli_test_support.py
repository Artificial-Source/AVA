"""Hermetic environments and exception-safe process ownership for CLI tests."""

from __future__ import annotations

from contextlib import contextmanager
import ctypes
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import time
import threading

from timeout_support import test_timeout


ENVIRONMENT_ALLOWLIST = (
    "PATH", "LANG", "LC_ALL", "LC_CTYPE", "LD_LIBRARY_PATH", "LD_PRELOAD",
    "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "ASAN_OPTIONS",
    "UBSAN_OPTIONS", "LSAN_OPTIONS", "TSAN_OPTIONS", "MSAN_OPTIONS",
    "ASAN_SYMBOLIZER_PATH", "LLVM_SYMBOLIZER_PATH", "TMPDIR", "TZ",
    "AVA_DEBUG_NO_TIMEOUT", "AVA_DEBUG_NO_TIMEOUT_SECONDS",
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def isolated_environment(root: Path, tmpdir: Path | None = None) -> dict[str, str]:
    """Retain loader/sanitizer settings, never ambient credentials or proxies."""
    environment = {name: os.environ[name] for name in ENVIRONMENT_ALLOWLIST if name in os.environ}
    for name, child in (
        ("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
        ("XDG_STATE_HOME", "state"), ("XDG_DATA_HOME", "data"),
        ("XDG_CACHE_HOME", "cache"), ("XDG_RUNTIME_DIR", "runtime"),
    ):
        directory = root / child
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        directory.chmod(0o700)
        environment[name] = str(directory)
    temporary = tmpdir if tmpdir is not None else root / "tmp"
    temporary.mkdir(parents=True, exist_ok=True, mode=0o700)
    rcfile = root / "libcwdrc"
    rcfile.write_text("silent = on\nchannels_default = off\n", encoding="utf-8")
    rcfile.chmod(0o600)
    environment.update({
        "TERM": "dumb", "NO_COLOR": "1", "TMPDIR": str(temporary),
        "LIBCWD_RCFILE_NAME": str(rcfile), "no_proxy": "*", "NO_PROXY": "*",
    })
    return environment


def _require_procfs() -> None:
    """Fail before admitting a child when Linux descendant custody is unavailable."""
    if not sys.platform.startswith("linux"):
        raise RuntimeError("CLI descendant ownership requires Linux procfs and child subreapers")
    try:
        Path("/proc/self/stat").read_text()
        Path(f"/proc/self/task/{os.getpid()}/children").read_text()
    except OSError as error:
        raise RuntimeError(f"CLI descendant ownership requires readable procfs: {error}") from error


def _process_rows():
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
            yield int(entry.name), fields[0], int(fields[1]), int(fields[2])
        except (FileNotFoundError, ProcessLookupError):
            continue


def process_group_is_live(pgid: int) -> bool:
    """Linux zombies/dead members cannot execute and are not survivors."""
    try:
        return any(group == pgid and state not in ("Z", "X", "x")
                   for _pid, state, _parent, group in _process_rows())
    except OSError:
        # An unreadable stat file is not evidence of disappearance. killpg also
        # sees zombies, which deliberately makes this fallback conservative.
        try:
            os.killpg(pgid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True


def descendant_process_groups(pid: int) -> set[int]:
    """Snapshot groups belonging to an admitted CLI operation for leak checks."""
    rows = {child: (state, parent, group) for child, state, parent, group in _process_rows()}
    descendants = {pid}
    while True:
        discovered = {child for child, (_state, parent, _group) in rows.items() if parent in descendants}
        if discovered <= descendants:
            break
        descendants.update(discovered)
    return {group for child, (state, _parent, group) in rows.items()
            if child in descendants and state not in ("Z", "X", "x")}


def wait_for_process_groups_absent(groups: set[int], timeout: float) -> None:
    deadline = time.monotonic() + test_timeout(timeout)
    while True:
        live = {group for group in groups if process_group_is_live(group)}
        if not live:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(f"CLI left executable members in process groups {sorted(live)}")
        time.sleep(0.01)


def _pending_shell_leader(pid: int) -> bool:
    # A generated shell can be interrupted while its child is still opening the
    # input FIFO, before setsid runs. Only that shell's unreaped direct child is
    # eligible for the PID fallback; never signal an unrelated numeric PID.
    return pid != os.getpid() and any(
        child == pid and parent == os.getppid() and group != pid and state not in ("Z", "X", "x")
        for child, state, parent, group in _process_rows()
    )


def terminate_process_group(pgid: int) -> None:
    """Stop an owned private group, including descendants of an exited leader."""
    if pgid <= 1 or pgid == os.getpgrp():
        raise ValueError("refusing an unowned process group")
    for sig in (signal.SIGTERM, signal.SIGKILL):
        grouped = process_group_is_live(pgid)
        if not grouped and not _pending_shell_leader(pgid):
            return
        try:
            if grouped:
                os.killpg(pgid, sig)
            else:
                os.kill(pgid, sig)
        except ProcessLookupError:
            continue
        deadline = time.monotonic() + test_timeout(3)
        while process_group_is_live(pgid) or _pending_shell_leader(pgid):
            if time.monotonic() >= deadline:
                break
            time.sleep(0.01)
    if process_group_is_live(pgid) or _pending_shell_leader(pgid):
        raise RuntimeError(f"owned process group {pgid} still has executable members after SIGKILL")


def _owned_children() -> list[int]:
    return [int(pid) for pid in Path(f"/proc/self/task/{os.getpid()}/children").read_text().split()]


def _reap_adopted_zombies(leader: subprocess.Popen) -> list[int]:
    """Do init's reaping duty continuously, without terminating a live child."""
    reaped = []
    for pid in _owned_children():
        if pid == leader.pid:
            continue
        try:
            collected, _status = os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            continue
        if collected:
            reaped.append(collected)
    return reaped


def _reap_descendants(process: subprocess.Popen) -> list[int]:
    """A dedicated subreaper has no unrelated children; adoption retains custody."""
    term_deadline = time.monotonic() + test_timeout(0.25)
    kill_deadline = time.monotonic() + test_timeout(5)
    reaped = []
    signaled = set()
    while True:
        children = _owned_children()
        force = time.monotonic() >= term_deadline
        for pid in children:
            sig = signal.SIGKILL if force and (pid, signal.SIGTERM) in signaled else signal.SIGTERM
            if (pid, sig) in signaled:
                continue
            try:
                group = os.getpgid(pid)
                if group == pid:
                    os.killpg(group, sig)
                os.kill(pid, sig)
            except ProcessLookupError:
                pass
            signaled.add((pid, sig))
        while True:
            try:
                pid, status = os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                return reaped
            if pid == 0:
                break
            if pid == process.pid:
                process.returncode = os.waitstatus_to_exitcode(status)
            else:
                reaped.append(pid)
        if time.monotonic() >= kill_deadline:
            raise RuntimeError(f"CLI owner could not reap adopted descendants: {_owned_children()}")
        time.sleep(0.005)


def _custodian_main(command, control: socket.socket | None = None, pass_fds=()) -> int:
    """Run in a fresh interpreter, isolated from every other harness launch."""
    _require_procfs()
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), "cannot establish CLI descendant ownership")
    stop_signal = None

    def stop(number, _frame):
        nonlocal stop_signal
        stop_signal = number

    def publish(record):
        if control is not None:
            control.sendall(json.dumps(record).encode() + b"\n")

    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, stop)
    process = None
    reported = False
    reaped = []
    try:
        process = subprocess.Popen(command, start_new_session=True, pass_fds=pass_fds)
        publish({"started": process.pid})
        if control is not None:
            for descriptor in {*pass_fds, 0, 1, 2}:
                os.close(descriptor)
        while stop_signal is None:
            status = process.poll()
            reaped.extend(_reap_adopted_zombies(process))
            if status is not None and not reported:
                publish({"status": status})
                reported = True
            if control is None:
                if reported:
                    break
                time.sleep(0.01)
            else:
                readable, _, _ = select.select([control], [], [], 0.01)
                if readable:
                    request = control.recv(1)
                    if request in (b"", b"C"):
                        break
                    raise RuntimeError(f"invalid CLI owner command: {request!r}")
    finally:
        for number in (signal.SIGINT, signal.SIGTERM):
            signal.signal(number, signal.SIG_IGN)
        if process is not None:
            reaped.extend(_reap_descendants(process))
            if not reported:
                publish({"status": process.returncode})
            publish({"cleaned": reaped})
    return 128 + stop_signal if stop_signal is not None else process.returncode


class OwnedProcess:
    """Popen-like leader handle backed by a separate Linux descendant custodian.

    wait/poll never activate the safety net: product settlement assertions may
    inspect the real leader and its children before cleanup is requested.
    """

    def __init__(self, command, **options):
        _require_procfs()
        if "start_new_session" in options:
            raise ValueError("owned launches always establish private sessions")
        self.args = command
        self.returncode = None
        self.pid = None
        self.reaped_descendants = []
        self._cleaned = False
        self._buffer = b""
        self._io_thread = None
        self._io_result = None
        self._io_error = None
        self._lock = threading.RLock()
        self._control, child_control = socket.socketpair()
        inherited = tuple(options.pop("pass_fds", ()))
        try:
            self._owner = subprocess.Popen(
                [sys.executable, str(Path(__file__).resolve()), "own", str(child_control.fileno()),
                 json.dumps(inherited), *command],
                start_new_session=True, pass_fds=(*inherited, child_control.fileno()), **options,
            )
        except BaseException:
            self._control.close()
            raise
        finally:
            child_control.close()
        self.stdin = self._owner.stdin
        self.stdout = self._owner.stdout
        self.stderr = self._owner.stderr
        try:
            deadline = time.monotonic() + test_timeout(8)
            while self.pid is None:
                self._receive(max(0, deadline - time.monotonic()))
                if self.pid is None and time.monotonic() >= deadline:
                    raise RuntimeError("CLI custodian did not acknowledge launch")
        except BaseException as error:
            try:
                self.cleanup()
            except Exception as cleanup_error:
                raise RuntimeError(f"{error}; descendant cleanup failed: {cleanup_error}") from error
            raise

    def _receive(self, timeout):
        readable, _, _ = select.select([self._control], [], [], timeout)
        if not readable:
            return
        data = self._control.recv(65536)
        if not data:
            if self._cleaned:
                return
            raise RuntimeError("CLI custodian closed its control channel before cleanup")
        self._buffer += data
        while b"\n" in self._buffer:
            line, self._buffer = self._buffer.split(b"\n", 1)
            record = json.loads(line)
            if "error" in record:
                raise RuntimeError(f"CLI custodian failed: {record['error']}")
            if "started" in record:
                self.pid = record["started"]
            if "status" in record:
                self.returncode = record["status"]
            if "cleaned" in record:
                self.reaped_descendants = record["cleaned"]
                self._cleaned = True

    def poll(self):
        with self._lock:
            if self.returncode is None:
                self._receive(0)
            return self.returncode

    def wait(self, timeout=None):
        deadline = None if timeout is None else time.monotonic() + timeout
        with self._lock:
            while self.returncode is None:
                remaining = None if deadline is None else max(0, deadline - time.monotonic())
                self._receive(remaining)
                if self.returncode is None and deadline is not None and time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(self.args, timeout)
            return self.returncode

    def send_signal(self, number):
        if self.poll() is None:
            try:
                os.kill(self.pid, number)
            except ProcessLookupError:
                pass

    def terminate(self):
        self.send_signal(signal.SIGTERM)

    def kill(self):
        self.send_signal(signal.SIGKILL)

    def communicate(self, input=None, timeout=None):
        def collect():
            try:
                self._io_result = self._owner.communicate(input)
            except BaseException as error:
                self._io_error = error

        self._io_thread = threading.Thread(target=collect, name="cli-owner-io")
        self._io_thread.start()
        try:
            self.wait(timeout)
        finally:
            self.cleanup()
        if self._io_error is not None:
            raise self._io_error
        return self._io_result

    def cleanup(self):
        with self._lock:
            try:
                if not self._cleaned:
                    try:
                        self._control.sendall(b"C")
                    except BrokenPipeError:
                        pass
                    deadline = time.monotonic() + test_timeout(7)
                    while not self._cleaned:
                        self._receive(max(0, deadline - time.monotonic()))
                        if not self._cleaned and time.monotonic() >= deadline:
                            raise RuntimeError("CLI custodian exceeded its descendant cleanup deadline")
                status = self._owner.wait(timeout=test_timeout(3))
                if status != 0:
                    raise RuntimeError(f"CLI custodian exited {status}")
                if self._io_thread is not None:
                    self._io_thread.join(timeout=test_timeout(3))
                    if self._io_thread.is_alive():
                        raise RuntimeError("CLI output collector survived descendant cleanup")
            except BaseException:
                if self._owner.poll() is None:
                    self._owner.terminate()
                self._owner.wait(timeout=test_timeout(7))
                raise
            finally:
                if self._cleaned or self._owner.poll() is not None:
                    self._control.close()
                    for stream in (self.stdin, self.stdout, self.stderr):
                        if stream is not None:
                            stream.close()


def launch_owned(command, **options) -> OwnedProcess:
    return OwnedProcess(command, **options)


def cleanup_owned_processes(processes) -> None:
    """Finish every custodian before closing streams or deleting fixture roots."""
    errors = []
    for process in reversed(processes):
        try:
            process.cleanup()
        except Exception as error:
            errors.append(error)
    if errors:
        raise RuntimeError("owned child cleanup failed: " + "; ".join(map(str, errors)))


@contextmanager
def owned_popen(command, **options):
    process = launch_owned(command, **options)
    try:
        yield process
    finally:
        cleanup_owned_processes([process])


def run_owned(arguments, *, input=None, timeout=None, check=False, **options):
    """subprocess.run semantics with descendant custody on success and failure."""
    if input is not None:
        options["stdin"] = subprocess.PIPE
    with owned_popen(arguments, **options) as process:
        stdout, stderr = process.communicate(input, timeout=timeout)
        result = subprocess.CompletedProcess(arguments, process.returncode, stdout, stderr)
        if check:
            result.check_returncode()
        return result


def wait_for_response(path: Path, request_id: str, timeout: float) -> None:
    """Wait for a correlated terminal record, never a streamed text echo."""
    deadline = time.monotonic() + test_timeout(timeout)
    while True:
        text = path.read_text(encoding="utf-8") if path.exists() else ""
        for line in text.splitlines(keepends=True):
            if not line.endswith("\n"):
                continue
            record = json.loads(line)
            if record.get("type") == "response" and record.get("id") == request_id:
                return
        if time.monotonic() >= deadline:
            raise RuntimeError(f"no terminal RPC response for {request_id!r} in {path}:\n{text}")
        time.sleep(0.01)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "run":
        raise SystemExit(_custodian_main(sys.argv[2:]))
    if len(sys.argv) > 4 and sys.argv[1] == "own":
        channel = socket.socket(fileno=int(sys.argv[2]))
        try:
            _custodian_main(sys.argv[4:], channel, tuple(json.loads(sys.argv[3])))
        except BaseException as error:
            channel.sendall(json.dumps({"error": str(error)}).encode() + b"\n")
            raise SystemExit(1)
        finally:
            channel.close()
        raise SystemExit(0)
    if len(sys.argv) == 5 and sys.argv[1] == "wait-response":
        wait_for_response(Path(sys.argv[2]), sys.argv[3], float(sys.argv[4]))
        raise SystemExit(0)
    if len(sys.argv) != 3 or sys.argv[1] not in ("terminate-group", "group-live"):
        raise SystemExit("usage: cli_test_support.py {terminate-group|group-live} PGID | wait-response PATH ID SECONDS")
    try:
        group = int(sys.argv[2])
        if group <= 1 or group == os.getpgrp():
            raise ValueError("refusing an unowned process group")
        live = process_group_is_live(group) if sys.argv[1] == "group-live" else None
    except Exception as error:
        print(f"process-group fixture failed: {error}", file=sys.stderr)
        raise SystemExit(2)
    if live is not None:
        raise SystemExit(0 if live else 1)
    terminate_process_group(group)
