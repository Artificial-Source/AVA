#!/usr/bin/env python3
"""Verify the Mermaid notifier contract in one bounded real TUI child."""

from __future__ import annotations

import argparse
from pathlib import Path
import signal
import subprocess
import tempfile

from cli_test_support import isolated_environment
from timeout_support import test_timeout


# Progress must reach both attachments before the specific contract assertion fires.
EMPTY_BRIDGE_MARKER = b"R11_EMPTY_BRIDGE_OK\n"
ATTACH_MARKER = b"R11_MISSING_NOTIFIER_ATTACH\n"
RETURNED_MARKER = b"R11_MISSING_NOTIFIER_RETURNED\n"
ASSERTION_PREDICATE = (
    b"(!bridge_.enqueue && !bridge_.cancel && !bridge_.drain)"
    b" || bridge_.set_completion_notifier"
)
ATTACH_FUNCTION = b"RuntimeMermaidPresentationController::attach_loop"
OUTPUT_LIMIT = 256 * 1024


def require(condition: bool, message: str) -> None:
    """Keep every contract check active under Python optimization."""
    if not condition:
        raise AssertionError(message)


def run_case(executable: Path, root: Path) -> None:
    """Reap one isolated child and accept only the expected attachment assertion."""
    environment = isolated_environment(root)
    # File capture cannot block the abort diagnostic behind a full output pipe.
    # Preserve the existing helper's complete child environment without overrides.
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        process = subprocess.Popen(
            [str(executable), "tui_session", "mermaid_missing_notifier"],
            stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
            env=environment, cwd=root,
        )
        try:
            try:
                process.wait(timeout=test_timeout(10))
            except subprocess.TimeoutExpired as error:
                raise AssertionError("Mermaid bridge attachment did not terminate in time") from error
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=test_timeout(5))
        for name, stream in (("stdout", stdout), ("stderr", stderr)):
            stream.seek(0, 2)
            require(stream.tell() <= OUTPUT_LIMIT, f"unexpectedly large Mermaid contract {name}")
        stderr.seek(0)
        output = stderr.read(OUTPUT_LIMIT + 1)

    require(process.returncode == -signal.SIGABRT,
            f"expected Mermaid attachment SIGABRT, got {process.returncode}: {output!r}")
    require(output.count(EMPTY_BRIDGE_MARKER) == 1,
            f"empty Mermaid bridge did not attach and retire successfully: {output!r}")
    require(output.count(ATTACH_MARKER) == 1,
            f"child did not reach the missing-notifier attachment: {output!r}")
    require(RETURNED_MARKER not in output,
            f"missing-notifier Session construction unexpectedly returned: {output!r}")
    require(output.index(EMPTY_BRIDGE_MARKER) < output.index(ATTACH_MARKER),
            f"Mermaid attachment cases ran out of order: {output!r}")
    diagnostics = [line for line in output.splitlines()
                   if b"Assertion" in line and b"failed" in line
                   and ATTACH_FUNCTION in line and ASSERTION_PREDICATE in line]
    require(len(diagnostics) == 1,
            f"missing or repeated Mermaid notifier contract assertion: {output!r}")
    require(output.index(ATTACH_MARKER) < output.index(diagnostics[0]),
            f"Mermaid notifier assertion preceded the invalid attachment: {output!r}")


def main() -> int:
    """Give CTest a separate temporary state directory for this expected death."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava-tests", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    arguments = parser.parse_args()
    arguments.root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tui-mermaid-contract-", dir=arguments.root) as directory:
        run_case(arguments.ava_tests.resolve(), Path(directory).resolve())
    print("TUI Mermaid notifier contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
