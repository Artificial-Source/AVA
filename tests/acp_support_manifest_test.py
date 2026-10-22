#!/usr/bin/env python3
"""Offline ACP v1 schema pin and exhaustive support-catalog check."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


def constants(definition):
    found = []
    if isinstance(definition, dict):
        if "const" in definition:
            found.append(definition["const"])
        for value in definition.values():
            found.extend(constants(value))
    elif isinstance(definition, list):
        for value in definition:
            found.extend(constants(value))
    return found


def names(entries):
    return {entry["name"] for entry in entries}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    source = Path(args.source)
    manifest = json.loads((source / "docs/acp-support.json").read_text())
    schema_path = source / manifest["schema"]["checkedFixture"]
    schema_bytes = schema_path.read_bytes()
    digest = hashlib.sha256(schema_bytes).hexdigest()
    assert digest == manifest["schema"]["sha256"], (digest, manifest["schema"]["sha256"])
    assert manifest["schema"]["protocolVersion"] == 1
    assert not manifest["schema"]["unstableSchemaUsed"]
    assert not manifest["schema"]["draftV2Used"]
    schema = json.loads(schema_bytes)
    defs = schema["$defs"]

    schema_methods = {value["x-method"] for value in defs.values() if isinstance(value, dict) and "x-method" in value}
    catalog_methods = {entry["name"] for entry in manifest["methods"]}
    assert catalog_methods == schema_methods, (sorted(schema_methods - catalog_methods), sorted(catalog_methods - schema_methods))
    assert "initialized" not in schema_methods  # ACP v1 initializes with one request/response; this is not MCP.

    exposure = manifest["exposure"]
    required_baseline = {"session/new", "session/prompt", "session/cancel", "session/update"}
    assert set(exposure["requiredBaseline"]) == required_baseline
    dispositions = {entry["name"]: entry["status"] for entry in manifest["methods"]}
    baseline_complete = all(dispositions[name] == "implemented" for name in required_baseline)
    assert exposure["successfulProtocolV1Negotiation"] == baseline_complete
    assert baseline_complete  # M3 may negotiate only with the complete required baseline.
    assert dispositions["initialize"] == "implemented"

    expected_enums = {
        "contentDiscriminators": ("ContentBlock", {"text", "image", "audio", "resource_link", "resource"}),
        "toolCallContentDiscriminators": ("ToolCallContent", {"content", "diff", "terminal"}),
        "toolKinds": ("ToolKind", {"read", "edit", "delete", "move", "search", "execute", "think", "fetch", "switch_mode", "other"}),
        "toolCallStatuses": ("ToolCallStatus", {"pending", "in_progress", "completed", "failed"}),
        "permissionOutcomes": ("RequestPermissionOutcome", {"cancelled", "selected"}),
        "permissionOptions": ("PermissionOptionKind", {"allow_once", "allow_always", "reject_once", "reject_always"}),
        "stopReasons": ("StopReason", {"end_turn", "max_tokens", "max_turn_requests", "refusal", "cancelled"}),
    }
    for section, (definition, expected) in expected_enums.items():
        actual_schema = set(constants(defs[definition]))
        assert actual_schema == expected, (definition, actual_schema)
        assert names(manifest[section]) == expected, section

    assert names(manifest["mcpTransports"]) == {"stdio", "http", "sse"}
    assert set(constants(defs["McpServer"])) == {"http", "sse"}  # stdio is the official implicit/default variant.
    stdio = next(entry for entry in manifest["mcpTransports"] if entry["name"] == "stdio")
    assert stdio["status"] == "implemented" and stdio["advertised"] is True
    assert all(entry["status"] != "implemented" for entry in manifest["mcpTransports"] if entry["name"] != "stdio")
    terminal_fs = {name for name in schema_methods if name.startswith("terminal/") or name.startswith("fs/")}
    assert names(manifest["terminalFilesystemMethods"]) == terminal_fs

    schema_error_codes = {value for value in constants(defs["ErrorCode"]) if isinstance(value, int)}
    manifest_error_codes = {entry["code"] for entry in manifest["jsonRpcErrors"] if isinstance(entry["code"], int)}
    assert manifest_error_codes == schema_error_codes

    expected_capabilities = {
        ("agent", "loadSession"),
        ("agent", "promptCapabilities.image"),
        ("agent", "promptCapabilities.audio"),
        ("agent", "promptCapabilities.embeddedContext"),
        ("agent", "mcpCapabilities.http"),
        ("agent", "mcpCapabilities.sse"),
        ("agent", "sessionCapabilities.list"),
        ("agent", "sessionCapabilities.delete"),
        ("agent", "sessionCapabilities.additionalDirectories"),
        ("agent", "sessionCapabilities.resume"),
        ("agent", "sessionCapabilities.close"),
        ("agent", "auth.logout"),
        ("client", "fs.readTextFile"),
        ("client", "fs.writeTextFile"),
        ("client", "terminal"),
        ("client", "session.configOptions.boolean"),
    }
    assert {(entry["side"], entry["name"]) for entry in manifest["capabilities"]} == expected_capabilities
    advertised_agent = {entry["name"] for entry in manifest["capabilities"] if entry["side"] == "agent" and entry.get("advertised") is True}
    assert advertised_agent == {"sessionCapabilities.list", "sessionCapabilities.resume", "sessionCapabilities.close"}
    image_capability = next(entry for entry in manifest["capabilities"] if entry["side"] == "agent" and entry["name"] == "promptCapabilities.image")
    assert image_capability["status"] == "implemented" and image_capability["advertised"] == "model-dependent"
    assert all(
        entry.get("advertised") is False
        for entry in manifest["capabilities"]
        if entry["side"] == "agent" and entry["name"] not in advertised_agent | {"promptCapabilities.image"}
    )
    assert dispositions["session/load"] == "deferred"
    assert dispositions["session/delete"] == "rejected"
    assert dispositions["session/request_permission"] == "implemented"

    session_update_schema = set(constants(defs["SessionUpdate"]))
    assert names(manifest["sessionUpdates"]) == session_update_schema
    implemented_updates = {entry["name"] for entry in manifest["sessionUpdates"] if entry["status"] == "implemented"}
    assert implemented_updates == {"agent_message_chunk", "agent_thought_chunk", "tool_call", "tool_call_update"}
    assert names(manifest["acpBuiltinTools"]) == {"read_file", "list_directory", "glob", "grep", "write_file", "edit_file", "apply_patch", "bash"}
    implemented_builtins = {entry["name"] for entry in manifest["acpBuiltinTools"] if entry["status"] == "implemented"}
    assert implemented_builtins == {"read_file", "list_directory", "glob", "grep", "write_file", "edit_file", "apply_patch", "bash"}
    assert all(entry["status"] == "implemented" for entry in manifest["terminalFilesystemMethods"])
    assert all(dispositions[name] == "implemented" for name in terminal_fs)

    outcome_source = (source / "src/ava/core/runtime_outcome.h").read_text()
    source_names = re.findall(
        r'RuntimeTerminalOutcomeCatalogEntry\{RuntimeTerminalOutcome::([A-Za-z]+), "([^"]+)"\}', outcome_source
    )
    disposition_names = {
        "Completed": "end_turn",
        "MaxTokens": "max_tokens",
        "MaxTurnRequests": "max_turn_requests",
        "Refusal": "refusal",
        "Cancelled": "cancelled",
        "Error": "protocol_error",
    }
    source_outcomes = {name: disposition_names[value] for value, name in source_names}
    manifest_outcomes = {entry["name"]: entry["disposition"] for entry in manifest["runtimeStopReasons"]}
    assert manifest_outcomes == source_outcomes, (manifest_outcomes, source_outcomes)
    assert {name for name, disposition in source_outcomes.items() if disposition == "protocol_error"} == {"error"}

    test_cmake = (source / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    registered = set(re.findall(r"add_test\(\s*NAME\s+([A-Za-z0-9_.-]+)", test_cmake))
    suites = re.search(r"set\(AVA_TEST_SUITES\s+(.*?)\)", test_cmake, re.DOTALL)
    assert suites, "CTest suite list is missing"
    registered.update(f"ava_tests.{suite}" for suite in suites.group(1).split())
    status_values = set(manifest["statusValues"])
    for section in (
        "methods", "capabilities", "contentDiscriminators", "toolCallContentDiscriminators", "toolKinds",
        "toolCallStatuses", "permissionOutcomes", "permissionOptions", "stopReasons", "terminalFilesystemMethods", "mcpTransports", "jsonRpcErrors",
        "sessionUpdates", "acpBuiltinTools"
    ):
        for entry in manifest[section]:
            assert entry["status"] in status_values, (section, entry)
            assert entry.get("handler") and entry.get("test"), (section, entry)
            for reference in entry["test"].split("/"):
                resolved = reference if "." in reference else f"ava_tests.{reference}"
                assert resolved in registered, (section, entry["name"], reference, "unregistered test reference")

    cmake = (source / "CMakeLists.txt").read_text()
    dependency = manifest["jsonDependency"]
    for pin in (dependency["fallbackRelease"], dependency["fallbackCommit"], dependency["sha256"]):
        if pin == dependency["fallbackCommit"]:
            continue  # CMake pins the release artifact; the manifest additionally records its peeled source commit.
        assert pin in cmake
    assert "find_package(nlohmann_json" in cmake and "URL_HASH" in cmake
    print("ACP support manifest and pinned stable v1 schema are exhaustive and consistent")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, ValueError) as error:
        print(f"ACP support manifest check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
