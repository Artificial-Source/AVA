#!/usr/bin/env python3
# Verify that generate_print_members.py emits only per-object data members.

import argparse
import json
import os
import subprocess
import sys
import tempfile


def main(argv):
    """Protect static-member exclusion and declared-type dispatch for borrowed members."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--generator", required=True)
    args = parser.parse_args(argv)

    tags = [
        {
            "_type": "tag",
            "name": "Example",
            "path": "src/ava/example/Example.h",
            "line": 1,
            "kind": "class",
            "scope": "ava::example",
        },
        {
            "_type": "tag",
            "name": "shared_default",
            "path": "src/ava/example/Example.h",
            "line": 3,
            "kind": "member",
            "scope": "ava::example::Example",
            "properties": "static,constexpr",
        },
        {
            "_type": "tag",
            "name": "value_",
            "path": "src/ava/example/Example.h",
            "line": 4,
            "kind": "member",
            "scope": "ava::example::Example",
        },
        {
            "_type": "tag",
            "name": "print_members_opt_in",
            "path": "src/ava/example/Example.h",
            "line": 5,
            "kind": "function",
            "scope": "ava::example::Example",
        },
    ]

    with tempfile.TemporaryDirectory(prefix="ava-print-members-") as temp_dir:
        tags_path = os.path.join(temp_dir, "tags.json")
        output_dir = os.path.join(temp_dir, "generated")
        with open(tags_path, "w", encoding="utf-8") as tags_file:
            for tag in tags:
                tags_file.write(json.dumps(tag) + "\n")

        result = subprocess.run(
            [sys.executable, args.generator, tags_path, output_dir],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            return result.returncode

        output_path = os.path.join(output_dir, "ava", "example", "print_members.cpp")
        with open(output_path, encoding="utf-8") as output_file:
            generated = output_file.read()

    if "shared_default" in generated:
        sys.stderr.write("static member shared_default was emitted:\n" + generated)
        return 1
    if '__write__("value:") << ::ava_utils::print_member<decltype(value_)>(value_)' not in generated:
        sys.stderr.write("instance member value_ was not emitted:\n" + generated)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
