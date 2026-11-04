#!/usr/bin/env python3
# Protect generated member selection and omission of private text annotations.

import argparse
import json
import os
import subprocess
import sys
import tempfile


def main(argv):
    """Protect static exclusion, declared-type dispatch, and private-text omission."""
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

    # Omit the complete annotated field, including wrappers that would otherwise
    # instantiate a string/container printer. Similar names are not annotations.
    private_types = [
        "ava::core::NoLogString",
        "std::optional<ava::core::NoLogString>",
        "std::vector<ava::core::NoLogString>",
        "ava::core::NoLogString const",
        "ava::core::NoLogString const &",
    ]
    private_names = []
    for index, spelling in enumerate(private_types):
        name = f"private_text_{index}_"
        private_names.append(name)
        tags.append({
            "_type": "tag", "name": name, "path": "src/ava/example/Example.h",
            "line": 10 + index, "kind": "member", "scope": "ava::example::Example",
            "typeref": "typename:" + spelling,
        })
    for index, (name, spelling) in enumerate([
        ("ordinary_text_", "std::string"),
        ("similar_name_", "NoLogStringView"),
    ]):
        tags.append({
            "_type": "tag", "name": name, "path": "src/ava/example/Example.h",
            "line": 20 + index, "kind": "member", "scope": "ava::example::Example",
            "typeref": "typename:" + spelling,
        })
    # A class containing only private text must still receive a valid empty
    # printer; omitting every field must not create a bare ostream expression.
    tags.extend([
        {"_type": "tag", "name": "PrivateOnly", "path": "src/ava/example/Example.h",
         "line": 30, "kind": "class", "scope": "ava::example"},
        {"_type": "tag", "name": "only_secret_", "path": "src/ava/example/Example.h",
         "line": 31, "kind": "member", "scope": "ava::example::PrivateOnly",
         "typeref": "typename:NoLogString"},
        {"_type": "tag", "name": "print_members_opt_in", "path": "src/ava/example/Example.h",
         "line": 32, "kind": "function", "scope": "ava::example::PrivateOnly"},
    ])

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
    for name in private_names + ["only_secret_"]:
        if name in generated:
            sys.stderr.write("private text member was emitted: " + name + "\n" + generated)
            return 1
    for name in ["ordinary_text_", "similar_name_"]:
        if "print_member<decltype(" + name + ")>(" + name + ")" not in generated:
            sys.stderr.write("ordinary member was incorrectly hidden: " + name + "\n" + generated)
            return 1
    private_body = generated.split("void PrivateOnly::print_members", 1)[1]
    if '<< "";' not in private_body or "print_member<" in private_body:
        sys.stderr.write("private-only class did not receive an empty printer:\n" + private_body)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
