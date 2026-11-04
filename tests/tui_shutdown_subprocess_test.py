#!/usr/bin/env python3
"""Supervise real TUI shutdown failures, including expected fatal termination."""

from __future__ import annotations

import argparse
from pathlib import Path
import signal
import subprocess
import tempfile

from cli_test_support import isolated_environment
from timeout_support import test_timeout


# An exception's content must never reach stderr, including terminate's handler.
PRIVATE_SENTINEL = b"R9_PRIVATE_PROMPT_AND_CREDENTIAL_SENTINEL"
# This error is still current when complete() invokes shutdown from its catch.
OUTER_SENTINEL = b"R9_PRIVATE_OUTER_COMPLETION_SENTINEL"
# Every release stage is attempted before termination, even after an earlier fault.
STAGE_MARKERS = (
    b"R9_STAGE_STOP\n", b"R9_STAGE_QUEUED\n",
    b"R9_STAGE_ACTIVE\n", b"R9_STAGE_PLUGIN\n",
)
# Environment selectors map only to production's fixed, non-content stage labels.
FAULT_STAGES = {
    "queue_receipts": b"queue receipts",
    "queue_returned_error": b"queue receipts",
    "completion_returned_error": b"queue receipts",
    "stop_request": b"stop request",
    "queued_prompts": b"queued prompts",
    "active_prompt": b"active prompt",
    "plugin_waiters": b"plugin waiters",
    "outer_exception": b"queued prompts",
}


def require(condition: bool, message: str) -> None:
    """Keep checks active even when Python runs with optimization enabled."""
    if not condition:
        raise AssertionError(message)


def run_case(executable: Path, root: Path, fault: str) -> None:
    """Start one fresh Application, bound its wait, and always reap the child."""
    case_root = root / fault
    case_root.mkdir()
    environment = isolated_environment(case_root)
    environment["AVA_TUI_SHUTDOWN_FAULT"] = fault
    # File-backed capture avoids a full stderr pipe blocking an abort diagnostic;
    # reject unexpected large output before reading any contents into memory.
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        process = subprocess.Popen(
            [str(executable), "tui_session", "shutdown_cleanup"],
            stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr,
            env=environment, cwd=case_root,
        )
        try:
            try:
                process.wait(timeout=test_timeout(10))
            except subprocess.TimeoutExpired as error:
                raise AssertionError(
                    f"{fault}: shutdown failed to finish; a held worker may have been joined"
                ) from error
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=test_timeout(5))
        stderr.seek(0, 2)
        require(stderr.tell() <= 256 * 1024, f"{fault}: unexpectedly large stderr")
        stderr.seek(0)
        output = stderr.read()

    # Returned errors protect the same exit/lifetime guarantees as exceptions.
    returned_error = fault in ("queue_returned_error", "completion_returned_error")
    prefix = b"TUI cleanup failed at " if returned_error else b"TUI shutdown cleanup failed at "
    diagnostic = prefix + FAULT_STAGES[fault] + b"\n"
    require(output.count(diagnostic) == 1, f"{fault}: missing or repeated fixed diagnostic: {output!r}")
    require(PRIVATE_SENTINEL not in output and OUTER_SENTINEL not in output, f"{fault}: exception content leaked to stderr")
    if fault == "outer_exception":
        require(output.count(b"R9_OUTER_EXCEPTION_ACTIVE\n") == 1,
                f"{fault}: shutdown did not run inside the real completion catch: {output!r}")
    elif fault != "completion_returned_error":
        require(b"R9_WORKER_WAITING\n" in output and b"R9_WORKER_HELD\n" in output,
                f"{fault}: child did not establish its held-worker handshake: {output!r}")
    require(b"R9_WORKER_MISSING\n" not in output and b"R9_WORKER_FALLBACK\n" not in output,
            f"{fault}: worker synchronization or fallback fired: {output!r}")
    positions = []
    for stage in STAGE_MARKERS:
        if fault == "completion_returned_error":
            require(stage not in output, f"{fault}: ordinary completion unexpectedly entered shutdown cleanup: {output!r}")
            continue
        require(output.count(stage) == 1, f"{fault}: release stage missing or repeated: {stage!r}: {output!r}")
        positions.append(output.index(stage))
    require(positions == sorted(positions), f"{fault}: release stages ran out of order: {output!r}")

    if fault == "queue_receipts" or returned_error:
        require(process.returncode == 0, f"{fault}: ordinary regression assertions failed: {output!r}")
        require(b"R9_SESSION_EXIT_1\n" in output, f"{fault}: Session did not return exit status 1: {output!r}")
        for marker in (b"R9_WORKER_RETURNED\n", b"R9_QUEUE_FINISH\n", b"R9_INBOX_RETIRED\n"):
            require(output.count(marker) == 1, f"{fault}: missing post-join cleanup evidence: {marker!r}: {output!r}")
        require(output.index(b"R9_WORKER_RETURNED\n") < output.index(b"R9_QUEUE_FINISH\n") < output.index(b"R9_INBOX_RETIRED\n"),
                f"{fault}: post-join cleanup happened out of order: {output!r}")
    else:
        require(process.returncode == -signal.SIGABRT,
                f"{fault}: expected terminate/SIGABRT, got {process.returncode}: {output!r}")
        forbidden_markers = (b"R9_INBOX_RETIRED\n", b"R9_SESSION_EXIT_1\n")
        if fault == "outer_exception":
            # This worker completed normally before queue finish threw. Require
            # that sequence, separately from the four held-worker death cases.
            for marker in (b"R9_WORKER_RETURNED\n", b"R9_QUEUE_FINISH\n"):
                require(output.count(marker) == 1 and output.index(marker) < positions[0],
                        f"{fault}: completion did not precede catch-triggered shutdown: {output!r}")
        else:
            forbidden_markers += (b"R9_WORKER_RETURNED\n", b"R9_QUEUE_FINISH\n")
        for forbidden in forbidden_markers:
            require(forbidden not in output,
                    f"{fault}: fatal shutdown continued past its release boundary: {forbidden!r}: {output!r}")


def main() -> int:
    """Run one requested CTest case with independent temporary state."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava-tests", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--fault", required=True, choices=FAULT_STAGES)
    arguments = parser.parse_args()
    arguments.root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tui-shutdown-", dir=arguments.root) as directory:
        run_case(arguments.ava_tests.resolve(), Path(directory).resolve(), arguments.fault)
    print(f"TUI shutdown regression passed: {arguments.fault}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
