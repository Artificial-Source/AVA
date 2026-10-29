#!/usr/bin/env python3
"""Prove each process-ownership scanner rejects every forbidden source token.

Run the untouched sources first so a scanner that always fails cannot pass the
negative cases. Mutations are private copies; no repository file is changed.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile
import unittest

from timeout_support import test_timeout


class ProcessContractTests(unittest.TestCase):
    args: argparse.Namespace

    def scan(self, source: Path):
        family = self.args.family.upper()
        command = [self.args.cmake, f"-DAVA_{family}_SOURCE={source}",
                   f"-DAVA_{family}_HEADER={self.args.header}"]
        if self.args.tui is not None:
            command.append(f"-DAVA_INTERACTIVE_TUI_SOURCE={self.args.tui}")
        return subprocess.run([*command, "-P", str(self.args.scanner)], capture_output=True,
                              text=True, check=False, timeout=test_timeout(10))

    def test_each_forbidden_token_is_detected(self):
        baseline = self.scan(self.args.source)
        self.assertEqual(baseline.returncode, 0, baseline.stdout + baseline.stderr)
        tokens = ["fork (", "kill\n(", "waitpid(", "waitid (", "poll\t(", "pollfd",
                  "#include <poll.h>", "#include <signal.h>", "#include <sys/wait.h>"]
        if self.args.family == "clipboard_image":
            tokens.extend(["#include <csignal>", "#include <sys/types.h>", "#include <unistd.h>", "#include <fcntl.h>"])
        original = self.args.source.read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="ava-static-contract-") as temporary:
            mutated = Path(temporary) / self.args.source.name
            for token in tokens:
                with self.subTest(token=token):
                    mutated.write_text(original + "\n" + token + "\n", encoding="utf-8")
                    result = self.scan(mutated)
                    self.assertNotEqual(result.returncode, 0, token)
                    self.assertIn("regained legacy process ownership", result.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--family", choices=("curl_transport", "clipboard_image"), required=True)
    parser.add_argument("--scanner", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--tui", type=Path)
    args = parser.parse_args()
    if args.family == "clipboard_image" and args.tui is not None and args.tui.name == "interactive_tui.cpp":
        args.tui = args.tui.with_name("TuiFrontEnd.cpp")
    ProcessContractTests.args = args
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ProcessContractTests)
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
