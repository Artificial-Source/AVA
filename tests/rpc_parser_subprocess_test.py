#!/usr/bin/env python3
"""Real-process RPC parser depth/duplicate-key recovery regressions."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from cli_test_support import isolated_environment
from timeout_support import test_timeout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ava", required=True)
    parser.add_argument("--root", required=True)
    args = parser.parse_args()

    root = Path(args.root)
    root.mkdir(parents=True, exist_ok=True)
    temporary = tempfile.TemporaryDirectory(prefix="rpc-parser-", dir=root)
    root = Path(temporary.name)
    libcwd_rcfile = (root / "libcwdrc").absolute()
    libcwd_rcfile.write_text(
        "silent = on\nchannels_default = off\n", encoding="utf-8")
    env = isolated_environment(root)
    env.update({
        "HOME": str(root / "home"),
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_STATE_HOME": str(root / "state"),
        "XDG_CACHE_HOME": str(root / "cache"),
        "NO_COLOR": "1",
        # Debug builds must remain protocol-quiet even when an isolated HOME
        # has no developer libcwd configuration.
        "LIBCWD_RCFILE_NAME": str(libcwd_rcfile),
    })
    # LIBCWD_RCFILE_OVERRIDE_NAME is read by libcwd *after* LIBCWD_RCFILE_NAME
    # and overrides its channel settings. A developer host may have it set to
    # a file that turns NOTICE on, which would leak debug output to stderr and
    # fail the protocol-quiet assertion below. Strip it so the silent rcfile
    # above is the final word.
    env.pop("LIBCWD_RCFILE_OVERRIDE_NAME", None)
    process = subprocess.Popen(
        [args.ava, "--rpc", "--no-session", "--offline"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
    )

    depth = 100_000
    deep = b'{"id":"deep","type":"get_protocol","future":' + (b'[' * depth) + (b']' * depth) + b'}\n'
    duplicate_records = [
        b'{"id":"first","id":"second","type":"get_protocol"}\n',
        b'{"id":"type","type":"get_protocol","t\\u0079pe":"get_state"}\n',
        b'{"id":"method","type":"get_protocol","method":1,"method":2}\n',
        b'{"id":"jsonrpc","type":"get_protocol","jsonrpc":"2.0","jsonrpc":"1.0"}\n',
        b'{"id":"nested","type":"get_protocol","future":{"unique":1,"unique":2}}\n',
    ]
    payload = deep + b"".join(duplicate_records) + b'{"id":"usable","type":"get_protocol","future":{"unique":true}}\n'
    try:
        stdout, stderr = process.communicate(input=payload, timeout=test_timeout(5))
        assert process.returncode == 0, (process.returncode, stderr)
        lines = stdout.splitlines()
        assert len(lines) == 7, f"expected seven complete responses, got {stdout!r}; stderr={stderr!r}"
        assert stdout.endswith(b"\n"), f"unterminated RPC response: {stdout!r}"
        responses = [json.loads(line) for line in lines]
        assert responses[0]["id"] == "deep"
        assert responses[0]["error"]["code"] == "invalid_request"
        for response in responses[1:6]:
            assert response["success"] is False
            assert response["error"]["code"] == "invalid_request"
        assert responses[6]["id"] == "usable" and responses[6]["success"] is True
        assert stderr == b"", stderr
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=test_timeout(5))
        temporary.cleanup()
    print("real RPC strict-parser recovery checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
