#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys


ALLOWED = {
    "LD_LIBRARY_PATH": "/fixture/loader;[still-loader=1",
    "AVA_DEBUG_NO_TIMEOUT": "function-shaped;[debug=value",
    "AVA_TEST_NAME": "allowed title;[equals==yes\nsecond line",
    "AVA_SESSION_TITLES": "preserved;[title=value",
}
FORBIDDEN_NAMES = (
    "AVA_CLI_FORBIDDEN",
    "BASH_FUNC_ava_cli_fixture%%",
    "AVA;SEMICOLON",
    "AVA[BRACKET]",
    "AVA\nNEWLINE",
    "AVA\\BACKSLASH",
    "ZZZ_AVA_CLI_FORBIDDEN",
)


def inject_and_run(argv: list[str]) -> int:
    """Run argv with hostile inherited values and return the child exit status."""
    env = os.environ.copy()
    env.update(ALLOWED)
    env.update(
        {
            "HOME": "/must/not/win",
            "XDG_CONFIG_HOME": "/must/not/win/config",
            "AVA_CLI_FORBIDDEN": "secret=value;[unterminated\nMORE=poison",
            "BASH_FUNC_ava_cli_fixture%%": (
                "() {  printf '%s\\n' value;\n"
                "AVA_CLI_FAKE[;SPLIT=value==still-value\n"
                "}"
            ),
            "AVA;SEMICOLON": "odd-name-value=1;[",
            "AVA[BRACKET]": "odd-name-value=2;[",
            "AVA\nNEWLINE": "odd-name-value=3;[",
            "AVA\\BACKSLASH": "odd-name-value=4;[",
            # Inserted after the multiline poison so the former parser could
            # silently lose this name while constructing its CMake list.
            "ZZZ_AVA_CLI_FORBIDDEN": "must-not-reach-child",
        }
    )
    return subprocess.run(argv, env=env, check=False).returncode


def check(root_arg: str) -> int:
    """Verify fixture paths and environment policy, reporting mismatches on stderr."""
    # Match CMake's ABSOLUTE path handling without resolving symlinks: a build
    # directory reached through a symlink must retain the caller's spelling.
    root = Path(root_arg).absolute()
    expected = {
        "HOME": str(root / "home"),
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_STATE_HOME": str(root / "state"),
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_CACHE_HOME": str(root / "cache"),
        "XDG_RUNTIME_DIR": str(root / "runtime"),
        "NO_COLOR": "1",
        "http_proxy": "",
        "HTTPS_PROXY": "",
        "no_proxy": "*",
        "NO_PROXY": "*",
        **ALLOWED,
    }
    failures = [
        f"{name}: expected {value!r}, got {os.environ.get(name)!r}"
        for name, value in expected.items()
        if os.environ.get(name) != value
    ]
    failures.extend(
        f"forbidden variable survived: {name!r}"
        for name in FORBIDDEN_NAMES
        if name in os.environ
    )
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


def main() -> int:
    """Dispatch environment injection or child checks; reject invalid arguments."""
    if len(sys.argv) >= 2 and sys.argv[1] == "inject":
        return inject_and_run(sys.argv[2:])
    if len(sys.argv) == 3 and sys.argv[1] == "check":
        return check(sys.argv[2])
    print("usage: cli_isolated_env_test.py inject COMMAND... | check ROOT", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
