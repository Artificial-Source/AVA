#!/usr/bin/env python3
# generate_print_members.py -- emit print_members definitions, split by namespace.
#
# Reads the JSON tags file ($BUILDDIR/generated/ctags/tags.json) in a
# single pass and writes one print_members.cpp per namespace bucket under
# <output_dir>/<bucket>/print_members.cpp.
#
# Which types are generated is read straight from tags.json: the ctags call
# passes `-D AVA_DEBUG_PRINT_MEMBERS_ON=void print_members_opt_in() { }`, so
# every class/struct that opted in (has that macro (and others like it) in its body)
# shows a `print_members_opt_in` member whose scope is that type's fully-qualified name.
# A generated definition only compiles for a type that declared print_members_opt_in, so
# only those types are emitted.
#
# Bucketing (collapse rule): a type in ava::<submodule>[::...] goes to bucket
# ava/<submodule>; any other top-level namespace gets its own bucket. Everything
# deeper folds into its bucket. The generated body is uniform (`<< member`); how
# each member is printed is resolved by C++ overload/concept dispatch at compile
# time. The declared type spelling is also used to omit NoLogString members;
# that alias is a source annotation, not a distinct C++ type.
#
# Usage: generate_print_members.py <tags.json> <output_dir>
import json
import os
import re
import sys
from collections import defaultdict


def label(member_name):
    # Member variables use a trailing-underscore convention; the printed label
    # drops a single trailing underscore (options_ -> "options:").
    return member_name[:-1] if member_name.endswith("_") else member_name


def bucket_for(scope):
    # Returns (bucket_namespace, bucket_dir). ava:: types collapse to their
    # first-level submodule; anything else uses its top-level namespace.
    if scope.startswith("ava::"):
        second = scope.split("::")[1]
        return ("ava::" + second, "ava/" + second)
    top = scope.split("::")[0]
    return (top, top)


def rel_qname(fqname, bucket_ns):
    # The print_members definition is emitted inside the bucket's namespace
    # block, so qualify the type relative to that namespace.
    prefix = bucket_ns + "::"
    return fqname[len(prefix):] if fqname.startswith(prefix) else fqname


def is_static_member(tag):
    # Return whether a ctags member tag describes class-wide storage rather
    # than per-object state. Universal Ctags emits C++ properties as a
    # comma-separated string when generate_ctags_json.sh enables that field.
    properties = tag.get("properties") or ""
    return "static" in properties.split(",")


def is_private_text_member(tag):
    # NoLogString is a string alias, so C++ overloads cannot distinguish it.
    # Honor its explicit source annotation before generating any expression.
    # Omitting the entire member also covers optional/vector and cv/reference
    # spellings without reading a secret or instantiating a content printer.
    return re.search(r"\bNoLogString\b", tag.get("typeref") or "") is not None


def load_tags(tags_path):
    classes = {}                       # fqname -> class/struct tag (name, scope, path)
    members_by_scope = defaultdict(list)  # fqname -> [(line, member_name)]
    opted = set()                      # fqnames that declare print_members_opt_in
    with open(tags_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            tag = json.loads(line)
            kind = tag.get("kind")
            name = tag.get("name")
            scope = tag.get("scope") or ""
            fqname = (scope + "::" + name) if (scope and name) else None
            if kind in ("class", "struct") and name and tag.get("path") and fqname:
                classes[fqname] = tag
            elif kind == "member" and scope and name and not is_static_member(tag) and not is_private_text_member(tag):
                # Keep the line so members can be emitted in declaration order
                # rather than tags.json's alphabetical order. Static members
                # describe the type rather than an individual object's state
                # and therefore do not belong in print_members output.
                members_by_scope[scope].append((tag.get("line", 0), name))
            if name == "print_members_opt_in" and scope:
                # Opt-in marker: the enclosing type declares print_members_opt_in.
                opted.add(scope)
    return classes, members_by_scope, opted


def emit_body(rel, members):
    # Build the print_members definition for one type as a string.
    lines = []
    lines.append("void " + rel + "::print_members(std::ostream& os, char const* prefix) const\n")
    lines.append("{\n")
    lines.append("  os << prefix;\n")
    lines.append("  AVA_USING_OSTREAM_PRELUDE(os)\n")
    if not members:
        # The AVA_USING_OSTREAM_PRELUDE(os) macro above ends with a bare `os`
        # that is meant to start a `<< member` chain. With no members it would
        # form a bare `os;` statement, which -Wunused-value flags (and -Werror
        # rejects). Stream an empty string instead: it is a function call (so
        # -Wunused-value stays quiet) without inspecting any object member.
        lines.append('      << "";\n')
    else:
        for idx, m in enumerate(members):
            sep = "" if idx == 0 else ", "
            # Preserve declared reference types; streaming the expression alone loses
            # that distinction and can traverse a borrowed content owner.
            lines.append('     << __write__("' + sep + label(m) + ':") << ::ava_utils::print_member<decltype(' + m + ')>(' + m + ")\n")
        lines.append("     ;\n")
    lines.append("}\n\n")
    return "".join(lines)


def build_bucket_content(bucket_ns, types):
    # Assemble the full text of one print_members.cpp bucket file.
    parts = []
    parts.append("// Auto-generated by generate_print_members.py. Do not edit.\n")
    parts.append("// print_members definitions for namespace " + bucket_ns + ".\n\n")
    parts.append('#include "sys.h"\n')
    # This header must be included first, because it must be included before cwds/debug_ostream_operators.h.
    parts.append('#include "ava/debug/debug_ostream_operators.h"\n')
    for h in sorted({h for _, _, h in types}):
        parts.append('#include "' + h + '"\n')
    parts.append("\n")
    for ns in bucket_ns.split("::"):
        parts.append("namespace " + ns + " {\n")
    parts.append("\n")
    for rel, members, _ in sorted(types, key=lambda t: t[0]):
        parts.append(emit_body(rel, members))
    for ns in reversed(bucket_ns.split("::")):
        parts.append("} // namespace " + ns + "\n")
    return "".join(parts)


def write_if_different(path, content):
    # Write content to path only when it differs from what is already on disk.
    #
    # This avoids bumping the file's mtime when the generated output is byte-identical,
    # which is essential for timestamp-based dependency trackers (CMake's
    # CMAKE_CONFIGURE_DEPENDS, ninja edges) so that no-op regenerations don't
    # trigger spurious reconfigures or rebuilds.
    #
    # Returns True when the file was (re)written, False when it was left untouched.
    try:
        with open(path, "r") as f:
            if f.read() == content:
                return False
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(content)
    return True


def remove_stale_outputs(out_dir, current_rel_paths):
    # Delete every print_members.cpp under out_dir that the current run does not
    # produce. These are orphans left over from a previous run whose bucket
    # disappeared, for example because a class toggled from opt-in to opt-out or
    # was removed entirely. Emptied bucket directories are removed as well.
    #
    # Only files named print_members.cpp are considered; source_files and any
    # other metadata are left untouched.
    current_set = {os.path.normpath(p) for p in current_rel_paths}
    for root, _dirs, files in os.walk(out_dir):
        for fname in files:
            if fname != "print_members.cpp":
                continue
            full = os.path.join(root, fname)
            rel = os.path.normpath(os.path.relpath(full, out_dir))
            if rel not in current_set:
                os.remove(full)
    # Remove now-empty directories bottom-up; rmdir only succeeds on empties.
    for root, dirs, _files in os.walk(out_dir, topdown=False):
        for d in dirs:
            try:
                os.rmdir(os.path.join(root, d))
            except OSError:
                pass


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: generate_print_members.py <tags.json> <output_dir>\n")
        return 2
    tags_path, out_dir = argv[1:2 + 1]

    classes, members_by_scope, opted = load_tags(tags_path)

    buckets = defaultdict(list)  # (bucket_ns, bucket_dir) -> [(rel, members, header)]
    for fqname in opted:
        cls = classes.get(fqname)
        if not cls:
            continue
        path = cls.get("path", "")
        # Only generate for header-defined types: a type defined in a .cpp (for
        # example a pimpl) cannot be referenced from a separate generated
        # translation unit, so its print_members must stay hand-written.
        if not path.endswith((".h", ".hpp", ".hxx", ".hh")):
            continue
        name = cls["name"]
        scope = cls.get("scope") or ""
        members = [n for _, n in sorted(members_by_scope.get(fqname, []))]
        header = cls.get("path", "")
        header_rel = header[4:] if header.startswith("src/") else header
        bucket_ns, bucket_dir = bucket_for(scope)
        rel = rel_qname(fqname, bucket_ns)
        buckets[(bucket_ns, bucket_dir)].append((rel, members, header_rel))

    # Write each bucket's print_members.cpp with write-if-different semantics so
    # that unchanged buckets keep their mtime and don't trigger recompilation.
    # First remove orphan files from previous runs whose bucket no longer exists.
    source_lines = []
    produced_rel_paths = []
    for (bucket_ns, bucket_dir), types in sorted(buckets.items()):
        rel_path = os.path.join(bucket_dir, "print_members.cpp")
        source_lines.append(rel_path + "\n")
        produced_rel_paths.append(rel_path)

    remove_stale_outputs(out_dir, produced_rel_paths)

    for (bucket_ns, bucket_dir), types in sorted(buckets.items()):
        rel_path = os.path.join(bucket_dir, "print_members.cpp")
        out_path = os.path.join(out_dir, rel_path)
        write_if_different(out_path, build_bucket_content(bucket_ns, types))

    # Write source_files last, also write-if-different. CMake watches this file
    # via CMAKE_CONFIGURE_DEPENDS; preserving its mtime on no-op regenerations
    # avoids a spurious cmake reconfigure.
    source_files_path = os.path.join(out_dir, "source_files")
    changed = write_if_different(source_files_path, "".join(source_lines))
    if changed:
        sys.stderr.write(source_files_path + " changed! A rebuild of the project is required!\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
