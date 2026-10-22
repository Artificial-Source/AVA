#!/usr/bin/env python3
"""Behavioral regressions for the bounded live ACP dogfood launcher."""

import argparse
from cli_test_support import isolated_environment, run_owned
from timeout_support import test_timeout
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import textwrap


ZED_COMMIT = "0" * 40
EXPECTED_ZED_FILE_LIMIT = 64 * 1024 * 1024


def write_executable(path, body):
    path.write_text(body, encoding="utf-8")
    path.chmod(0o700)


def clean_environment(root):
    inherited = isolated_environment(root)
    env = {
        "HOME": str(root / "launcher-home"),
        "PATH": "/usr/bin:/bin",
        "TMPDIR": str(root),
        "LANG": "C.UTF-8",
        "LC_ALL": "C.UTF-8",
        # Match the libcwd suppression applied to ava_tests.*: this clean env
        # does not inherit os.environ, so set the pair explicitly to keep ava's
        # debug initialization from writing to the streams this test inspects.
        "LIBCWD_NO_STARTUP_MSGS": "1",
        "AVA_NO_DEBUG_OUTPUT": "1",
    }
    env = {**inherited, **env}
    # Preserve only explicit debug-routing inputs from the parent; all other
    # developer environment remains excluded from this launcher fixture.
    for name in ("AVA_TEST_NAME", "AVA_DEBUG_OUTPUT_DIR", "LIBCWD_RCFILE_NAME", "LIBCWD_RCFILE_OVERRIDE_NAME"):
        value = os.environ.get(name)
        if value:
            env[name] = value
    return env


def run_launcher(script, arguments, root, stdin=""):
    (root / "launcher-home").mkdir(exist_ok=True)
    return run_owned([str(script), *arguments],
    input=stdin,
    text=True,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    timeout=test_timeout(30),
    check=False,
    env=clean_environment(root),)


def common_arguments(ava, provider, zed, root_parent, preflight):
    return [
        "zed",
        "run",
        "--ava",
        str(ava),
        "--fake-provider",
        str(provider),
        "--zed",
        str(zed),
        "--acknowledge-dedicated-display",
        "--confinement",
        "sandbox",
        "--confinement-description",
        "isolated launcher regression test",
        "--preflight-evidence",
        str(preflight),
        "--root-parent",
        str(root_parent),
        "--operator-timeout-seconds",
        "60",
    ]


def assert_display_validation(script, root):
    socket_path = root / "wayland-test"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(str(socket_path))
        listener.listen(1)
        preflight = root / "preflight.txt"
        preflight.write_text("reviewed test boundary\n", encoding="utf-8")
        base = common_arguments(Path("/bin/true"), Path("/bin/true"), Path("/bin/true"), root, preflight)

        both = run_launcher(script, [*base, "--display", ":98", "--wayland-display", str(socket_path)], root)
        assert both.returncode != 0
        assert "declare exactly one of --display or --wayland-display" in both.stderr

        authority = root / "authority"
        authority.write_text("test\n", encoding="utf-8")
        wayland_with_xauth = run_launcher(
            script,
            [*base, "--wayland-display", str(socket_path), "--display-auth-file", str(authority)],
            root,
        )
        assert wayland_with_xauth.returncode != 0
        assert "--display-auth-file is valid only with --display" in wayland_with_xauth.stderr

        socket_link = root / "wayland-link"
        socket_link.symlink_to(socket_path)
        linked = run_launcher(script, [*base, "--wayland-display", str(socket_link)], root)
        assert linked.returncode != 0
        assert "absolute non-symlink socket path" in linked.stderr

        valid = run_launcher(script, [*base, "--wayland-display", str(socket_path)], root)
        assert valid.returncode != 0
        assert "--wayland-display must" not in valid.stderr
        assert "could not parse a Zed version" in valid.stderr


def assert_startup_and_file_limit(script, root):
    ava = Path("/bin/true")
    provider_release = root / "startup-provider-release"
    os.mkfifo(provider_release, 0o600)
    provider = root / "fake-provider.py"
    write_executable(
        provider,
        """#!/usr/bin/python3
import os
from pathlib import Path
import signal
import sys
port = Path(sys.argv[1])
port.write_text("12345\\n", encoding="utf-8")
port.with_name("provider-pid").write_text(f"{os.getpid()}\\n", encoding="utf-8")
if len(sys.argv) > 4 and sys.argv[4] == "text-delayed":
    with Path(RELEASE_FIFO).open("rb", buffering=0) as release:
        release.read(1)
    raise SystemExit(42)
signal.pause()
""".replace("RELEASE_FIFO", repr(str(provider_release))),
    )
    preflight = root / "preflight.txt"
    preflight.write_text("reviewed test boundary\n", encoding="utf-8")

    exit_zed = root / "zed-exit.py"
    write_executable(
        exit_zed,
        f"""#!/usr/bin/python3
import sys
if len(sys.argv) > 1 and sys.argv[1] == "--version":
    print("Zed 1.9.0 {ZED_COMMIT}")
    raise SystemExit(0)
raise SystemExit(0)
""",
    )
    exit_root = root / "exit-root"
    exit_root.mkdir()
    exited = run_launcher(
        script,
        [*common_arguments(ava, provider, exit_zed, exit_root, preflight), "--display", ":98"],
        root,
    )
    assert exited.returncode != 0
    assert "exited or became a zombie during startup" in exited.stderr
    assert "raw/zed.stderr" in exited.stderr

    limit_file = root / "zed-limit.txt"
    limit_zed = root / "zed-limit.py"
    write_executable(
        limit_zed,
        f"""#!/usr/bin/python3
from pathlib import Path
import os
import resource
import signal
import sys
if len(sys.argv) > 1 and sys.argv[1] == "--version":
    print("Zed 1.9.0 {ZED_COMMIT}")
    raise SystemExit(0)
with Path({str(limit_file)!r}).open("a", encoding="utf-8") as output:
    output.write(f"{{resource.getrlimit(resource.RLIMIT_FSIZE)[0]}}\\n")
def stop(_signal, _frame):
    print("tail-on-term", flush=True)
    raise SystemExit(0)
signal.signal(signal.SIGTERM, stop)
print("capture-started", flush=True)
if Path(os.environ["AVA_ZED_DOGFOOD_PHASE_ROOT"]).name == "cancellation":
    with Path({str(provider_release)!r}).open("wb", buffering=0) as release:
        release.write(b"1")
signal.pause()
""",
    )
    limit_root = root / "limit-root"
    limit_root.mkdir()
    limited = run_launcher(
        script,
        [*common_arguments(ava, provider, limit_zed, limit_root, preflight), "--display", ":98"],
        root,
        stdin="incomplete\n",
    )
    assert limited.returncode != 0
    limits = [int(value) for value in limit_file.read_text(encoding="utf-8").splitlines()]
    assert limits == [EXPECTED_ZED_FILE_LIMIT, EXPECTED_ZED_FILE_LIMIT], limits

    for run_root in (exit_root, limit_root):
        phase_roots = list(run_root.glob("ava-zed-dogfood.*/phases/*"))
        assert phase_roots
        for phase_root in phase_roots:
            for log_name in ("zed.stdout", "zed.stderr"):
                log = phase_root / "raw" / log_name
                if log.exists():
                    assert log.stat().st_size <= 16 * 1024 * 1024
            if run_root == limit_root:
                assert "tail-on-term" in (phase_root / "raw/zed.stdout").read_text(encoding="utf-8")

    capped_zed = root / "zed-capped.py"
    write_executable(
        capped_zed,
        f"""#!/usr/bin/python3
import signal
import sys
if len(sys.argv) > 1 and sys.argv[1] == "--version":
    print("Zed 1.9.0 {ZED_COMMIT}")
    raise SystemExit(0)
sys.stdout.write("x" * (17 * 1024 * 1024))
sys.stdout.flush()
signal.pause()
""",
    )
    capped_root = root / "capped-root"
    capped_root.mkdir()
    capped = run_launcher(
        script,
        [*common_arguments(ava, provider, capped_zed, capped_root, preflight), "--display", ":98"],
        root,
    )
    assert capped.returncode != 0
    assert "capture exited nonzero" in capped.stderr
    capped_logs = list(capped_root.glob("ava-zed-dogfood.*/phases/*/raw/zed.stdout"))
    assert capped_logs and all(log.stat().st_size <= 16 * 1024 * 1024 for log in capped_logs)


def assert_cancellation_waits_for_provider_gate(script, root):
    preflight = root / "preflight.txt"
    preflight.write_text("reviewed test boundary\n", encoding="utf-8")
    provider = root / "gated-provider.py"
    write_executable(
        provider,
        textwrap.dedent(
            """\
            #!/usr/bin/python3
            import os
            from pathlib import Path
            import signal
            import socket
            import sys

            server = socket.socket()
            server.bind(("127.0.0.1", 0))
            server.listen(1)
            Path(sys.argv[1]).write_text(f"{server.getsockname()[1]}\\n", encoding="utf-8")
            if sys.argv[4] != "text-delayed":
                signal.pause()
            connection, _ = server.accept()
            connection.recv(4096)
            gate = socket.socket(fileno=int(os.environ["AVA_TEST_CONTROL_FD"]))
            gate.sendall(bytes((0,)))
            gate.recv(1)
            connection.close()
            """
        ),
    )
    zed = root / "requesting-zed.py"
    write_executable(
        zed,
        textwrap.dedent(
            f"""\
            #!/usr/bin/python3
            import json
            import os
            from pathlib import Path
            import signal
            import socket
            import sys
            from urllib.parse import urlparse

            if len(sys.argv) > 1 and sys.argv[1] == "--version":
                print("Zed 1.9.0 {ZED_COMMIT}")
                raise SystemExit(0)
            if Path(os.environ["AVA_ZED_DOGFOOD_PHASE_ROOT"]).name == "cancellation":
                settings = json.loads((Path(os.environ["XDG_CONFIG_HOME"]) / "zed/settings.json").read_text(encoding="utf-8"))
                endpoint = settings["agent_servers"]["AVA M6 dogfood (cancellation)"]["env"]["MOONSHOT_BASE_URL"]
                port = urlparse(endpoint).port
                connection = socket.create_connection(("127.0.0.1", port))
                connection.sendall(b"POST / HTTP/1.1\\r\\nHost: 127.0.0.1\\r\\nContent-Length: 2\\r\\n\\r\\n{{}}")
            signal.pause()
            """
        ),
    )
    success_root = root / "gated-success"
    success_root.mkdir()
    completed = run_launcher(
        script,
        [*common_arguments(Path("/bin/true"), provider, zed, success_root, preflight), "--display", ":98"],
        root,
        stdin="fail\nlifecycle intentionally skipped\npass\ncancellation observed after provider gate\n",
    )
    submission = completed.stderr.index("Send exactly: cancel this delayed deterministic M6 turn")
    confirmation = completed.stderr.index("PROVIDER REQUEST 0 CONFIRMED")
    cancellation = completed.stderr.index("Cancel the in-flight turn from Zed")
    outcome = completed.stderr.index("Record cancellation phase outcome")
    assert submission < confirmation < cancellation < outcome, completed.stderr
    reports = list(success_root.glob("ava-zed-dogfood.*/operator-observations.tsv"))
    assert len(reports) == 1
    assert "cancellation\tpass\tcancellation observed after provider gate" in reports[0].read_text(encoding="utf-8")

    missing_release = root / "missing-request-release"
    os.mkfifo(missing_release, 0o600)
    no_request_provider = root / "no-request-provider.py"
    write_executable(
        no_request_provider,
        textwrap.dedent(
            """\
            #!/usr/bin/python3
            from pathlib import Path
            import signal
            import socket
            import sys

            server = socket.socket()
            server.bind(("127.0.0.1", 0))
            server.listen(1)
            Path(sys.argv[1]).write_text(f"{server.getsockname()[1]}\\n", encoding="utf-8")
            if sys.argv[4] == "text-delayed":
                with Path(RELEASE_FIFO).open("rb", buffering=0) as release:
                    release.read(1)
                raise SystemExit(24)
            signal.pause()
            """.replace("RELEASE_FIFO", repr(str(missing_release)))
        ),
    )
    idle_zed = root / "idle-zed.py"
    write_executable(
        idle_zed,
        textwrap.dedent(
            f"""\
            #!/usr/bin/python3
            import os
            from pathlib import Path
            import signal
            import sys
            if len(sys.argv) > 1 and sys.argv[1] == "--version":
                print("Zed 1.9.0 {ZED_COMMIT}")
                raise SystemExit(0)
            if Path(os.environ["AVA_ZED_DOGFOOD_PHASE_ROOT"]).name == "cancellation":
                with Path({str(missing_release)!r}).open("wb", buffering=0) as release:
                    release.write(b"1")
            signal.pause()
            """
        ),
    )
    missing_root = root / "missing-request"
    missing_root.mkdir()
    missing = run_launcher(
        script,
        [*common_arguments(Path("/bin/true"), no_request_provider, idle_zed, missing_root, preflight), "--display", ":98"],
        root,
        stdin="fail\nlifecycle intentionally skipped\npass\nthis pass must not be recorded\n",
    )
    assert missing.returncode != 0
    assert "no cancellation outcome was requested" in missing.stderr
    assert "PROVIDER REQUEST 0 CONFIRMED" not in missing.stderr
    assert "Record cancellation phase outcome" not in missing.stderr
    reports = list(missing_root.glob("ava-zed-dogfood.*/operator-observations.tsv"))
    assert len(reports) == 1
    assert "cancellation\tpass" not in reports[0].read_text(encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    source = Path(args.source).absolute()
    script = source / "scripts/live-acp-dogfood.sh"
    assert script.is_file() and os.access(script, os.X_OK)

    with tempfile.TemporaryDirectory(prefix="ava-live-acp-dogfood-test-") as temporary:
        root = Path(temporary).absolute()
        display_root = root / "display"
        display_root.mkdir()
        assert_display_validation(script, display_root)
        startup_root = root / "startup"
        startup_root.mkdir()
        assert_startup_and_file_limit(script, startup_root)
        cancellation_root = root / "cancellation"
        cancellation_root.mkdir()
        assert_cancellation_waits_for_provider_gate(script, cancellation_root)

    print("live ACP dogfood launcher display, startup, file-limit, and cancellation-gate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
