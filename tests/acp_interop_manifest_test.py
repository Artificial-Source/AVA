#!/usr/bin/env python3
"""Offline cross-check for pinned M6 ACP interoperability metadata and wiring."""

import argparse
import base64
import json
from pathlib import Path
import re
import sys

EXPECTED = {
    "@agentclientprotocol/sdk": {
        "version": "1.2.1",
        "license": "Apache-2.0",
        "gitHead": "26da1ae7ab66fae0f5e77272dee3e5d562d24aee",
        "shasum": "c98952123d2b202a143ab5ec68782eec2775003a",
        "integrity": "sha512-jwYUdOQR7tc+Zfch53VL4JJyUNK/46q03uUTYb+PjECsmnNl94XFXOfYLJ8RBpMNidXd1rpOAVgb0vqD98xImA==",
    },
    "zod": {
        "version": "4.4.3",
        "license": "MIT",
        "integrity": "sha512-ytENFjIJFl2UwYglde2jchW2Hwm4GJFLDiSXWdTrJQBIN9Fcyp7n4DhxJEiWNAJMV1/BqWfW/kkg71UDcHJyTQ==",
    },
    "acpx": {
        "version": "0.12.0",
        "license": "MIT",
        "gitHead": "6a24a546d2349cbe71ed032d52d07cab611e320c",
        "shasum": "584e2c99cae2312607e0691bba6f4fa6dd8a89fb",
        "integrity": "sha512-APYpN04XFWrCGuSBvM4HTKWWFH8uSIuzc+qI7aCGeVdP9o4euZeBosFEkmNUHvBOop0XBemg6d8RsNvzXN3Mgw==",
    },
}


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8"))


def validate_sri(value):
    algorithm, encoded = value.split("-", 1)
    assert algorithm == "sha512", value
    assert len(base64.b64decode(encoded, validate=True)) == 64, value


def validate_lock(lock, direct_dependencies):
    assert lock["lockfileVersion"] == 3
    assert lock["requires"] is True
    assert lock["packages"][""]["dependencies"] == direct_dependencies
    for location, package in lock["packages"].items():
        if not location:
            continue
        assert package.get("version"), location
        assert package.get("resolved", "").startswith("https://registry.npmjs.org/"), location
        validate_sri(package["integrity"])


def package_entry(lock, name):
    return lock["packages"][f"node_modules/{name}"]


def assert_option_off(cmake, option):
    pattern = rf'option\({re.escape(option)}\s+"[^"]+"\s+OFF\)'
    assert re.search(pattern, cmake), option


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    source = Path(args.source)

    sdk_dir = source / "tests/acp-sdk"
    acpx_dir = source / "tests/acp-acpx"
    sdk_package = load_json(sdk_dir / "package.json")
    sdk_lock = load_json(sdk_dir / "package-lock.json")
    acpx_package = load_json(acpx_dir / "package.json")
    acpx_lock = load_json(acpx_dir / "package-lock.json")

    assert sdk_package["private"] is True and sdk_package["type"] == "module"
    assert "scripts" not in sdk_package
    sdk_dependencies = {"@agentclientprotocol/sdk": "1.2.1", "zod": "4.4.3"}
    assert sdk_package["dependencies"] == sdk_dependencies
    validate_lock(sdk_lock, sdk_dependencies)

    assert acpx_package["private"] is True and acpx_package["type"] == "module"
    assert "scripts" not in acpx_package
    acpx_dependencies = {"acpx": "0.12.0"}
    assert acpx_package["dependencies"] == acpx_dependencies
    validate_lock(acpx_lock, acpx_dependencies)

    for name in ("@agentclientprotocol/sdk", "zod"):
        locked = package_entry(sdk_lock, name)
        expected = EXPECTED[name]
        assert locked["version"] == expected["version"]
        assert locked["license"] == expected["license"]
        assert locked["integrity"] == expected["integrity"]
    locked_acpx = package_entry(acpx_lock, "acpx")
    assert locked_acpx["version"] == EXPECTED["acpx"]["version"]
    assert locked_acpx["license"] == EXPECTED["acpx"]["license"]
    assert locked_acpx["integrity"] == EXPECTED["acpx"]["integrity"]
    assert package_entry(acpx_lock, "@agentclientprotocol/sdk")["version"] == "1.2.1"
    assert package_entry(acpx_lock, "zod")["version"] == "4.4.3"

    support = load_json(source / "docs/acp-support.json")
    interop = support["interoperability"]
    assert interop["milestone"] == "M6 production interoperability and captured evidence"
    assert interop["productionNodeRequired"] is False
    assert interop["ciNodeVersion"] == "24.13.1"
    labels = set(interop["evidenceLabels"])
    assert labels == {
        "automated", "manual verified", "configuration documented but not executed",
        "deferred", "unsupported",
    }
    metadata = {entry["name"]: entry for entry in interop["dependencies"]}
    assert set(metadata) == set(EXPECTED)
    for name, expected in EXPECTED.items():
        entry = metadata[name]
        for field, value in expected.items():
            assert entry[field] == value, (name, field)
        assert entry["testOnly"] is True
        validate_sri(entry["integrity"])
        if "gitHead" in expected:
            assert re.fullmatch(r"[0-9a-f]{40}", entry["gitHead"])
            assert re.fullmatch(r"[0-9a-f]{40}", entry["shasum"])
        for field in ("packagePath", "lockPath"):
            assert (source / entry[field]).is_file(), (name, field)
        if "harnessPath" in entry:
            assert (source / entry["harnessPath"]).is_file(), name

    sdk_meta = metadata["@agentclientprotocol/sdk"]
    assert sdk_meta["testName"] == "ava_cli.acp_sdk_interop"
    assert sdk_meta["cmakeOption"] == "AVA_REQUIRE_ACP_SDK_INTEROP"
    assert sdk_meta["evidenceLabel"] == "automated"
    assert sdk_meta["status"] == "automated"
    assert "mandatory dedicated CI" in sdk_meta["gate"]
    acpx_meta = metadata["acpx"]
    assert acpx_meta["testName"] == "ava_cli.acpx_interop"
    assert acpx_meta["cmakeOption"] == "AVA_ENABLE_ACPX_INTEROP"
    assert acpx_meta["evidenceLabel"] == "automated"
    assert acpx_meta["status"] == "automated, opt-in alpha compatibility smoke, not official conformance"
    assert "opt-in" in acpx_meta["gate"] and "default CTest" in acpx_meta["gate"]

    dogfood = interop["dogfood"]
    assert dogfood["scriptPath"] == "scripts/live-acp-dogfood.sh"
    assert dogfood["modes"] == ["sdk", "acpx", "zed"]
    assert dogfood["packageDownloadAtRuntime"] is False
    assert dogfood["evidencePolicyPath"] == "docs/interop/evidence/README.md"
    zed_report = "docs/interop/evidence/zed-1.9.0-2026-07-14.md"
    assert dogfood["completedRealClientEvidence"] == [zed_report]
    assert (source / zed_report).is_file()
    confinement = dogfood["zedConfinementRequirements"]
    assert confinement["profileIsolationIsSandbox"] is False
    assert confinement["ordinaryAccountProfileOnlyAllowed"] is False
    assert confinement["dedicatedDisplayRequired"] is True
    assert len(confinement["allowedModes"]) == 2
    assert "SSH-agent" in confinement["forbiddenEnvironment"]
    assert "private temporary" in confinement["rawArtifactLocation"]
    for field in ("scriptPath", "evidencePolicyPath"):
        assert (source / dogfood[field]).is_file(), field

    clients = {entry["name"]: entry for entry in interop["clients"]}
    assert set(clients) == {"Zed", "JetBrains", "CodeCompanion.nvim"}
    zed = clients["Zed"]
    assert zed["detectedVersion"] == "1.9.0"
    assert zed["detectedCommit"] == "ced90fc636c4ede05402befc38a63bae7fd741bd"
    assert zed["evidenceLabel"] == "manual verified"
    assert "manual verified" in zed["status"]
    assert zed["configurationDocumentationPath"] == "docs/acp.md#zed-custom-agent"
    assert zed["dogfoodScriptPath"] == dogfood["scriptPath"]
    assert zed["evidencePolicyPath"] == dogfood["evidencePolicyPath"]
    assert zed["completedEvidenceReports"] == [zed_report]
    assert zed["lastVerified"] == "2026-07-14"
    assert zed["configurationLastChecked"] == "2026-07-14"
    assert zed["confinementRequired"] is True
    assert "no documented headless ACP mode" in zed["headlessMode"]

    jetbrains = clients["JetBrains"]
    assert jetbrains["detectedVersion"] is None
    assert jetbrains["documentedVersion"] == "2026.1+"
    assert jetbrains["evidenceLabel"] == "configuration documented but not executed"
    assert jetbrains["configurationDocumentationPath"] == "docs/acp.md#jetbrains-20261-custom-agent"
    assert jetbrains["configurationPath"] == "~/.jetbrains/acp.json"
    assert jetbrains["configurationLastChecked"] == "2026-07-13"
    assert jetbrains["completedEvidenceReports"] == []

    codecompanion = clients["CodeCompanion.nvim"]
    assert codecompanion["detectedVersion"] is None
    assert codecompanion["evidenceLabel"] == "configuration documented but not executed"
    assert codecompanion["configurationDocumentationPath"] == "docs/acp.md#codecompanionnvim-custom-acp-adapter"
    assert codecompanion["configurationLastChecked"] == "2026-07-13"
    assert codecompanion["completedEvidenceReports"] == []
    assert "terminal=false" in codecompanion["terminalLimitation"]
    assert [entry["name"] for entry in interop["clients"] if entry["evidenceLabel"] == "manual verified"] == ["Zed"]
    assert jetbrains["completedEvidenceReports"] == [] and codecompanion["completedEvidenceReports"] == []
    for entry in clients.values():
        docs_path = entry["configurationDocumentationPath"].split("#", 1)[0]
        assert (source / docs_path).is_file(), entry["name"]
    acp_configuration_docs = (source / "docs/acp.md").read_text(encoding="utf-8")
    for heading in (
        "## Zed custom agent",
        "## JetBrains 2026.1+ custom agent",
        "## CodeCompanion.nvim custom ACP adapter",
    ):
        assert heading in acp_configuration_docs, heading
    captured_reports = sorted(
        path.name for path in (source / "docs/interop/evidence").glob("*.md")
        if path.name != "README.md"
    )
    assert captured_reports == ["zed-1.9.0-2026-07-14.md"]
    report_path = source / zed_report
    report_bytes = report_path.read_bytes()
    assert 0 < len(report_bytes) <= 64 * 1024 and b"\0" not in report_bytes
    report = report_bytes.decode("utf-8")
    report_lines = report.splitlines()
    assert report_lines and report_lines[0] == "---" and "---" in report_lines[1:]
    frontmatter_end = report_lines[1:].index("---") + 1
    frontmatter = {}
    for line in report_lines[1:frontmatter_end]:
        key, value = line.split(":", 1)
        frontmatter[key.strip()] = value.strip().strip('"')
    assert frontmatter == {
        "report_status": "complete",
        "evidence_label": "manual verified",
        "client": "Zed",
        "client_version": zed["detectedVersion"],
        "client_commit": zed["detectedCommit"],
        "protocol": "ACP stable v1 schema-v1.19.0",
        "date_utc": zed["lastVerified"],
        "operator": "repository maintainer session",
    }
    for token in (
        "ACP schema source commit: `a213df5240048f96d2b23f644984bb20c188a234`",
        "ACP schema fixture SHA-256: `92c1dfcda10dd47e99127500a3763da2b471f9ac61e12b9bf0430c32cf953796`",
        "Fake-provider version/source: AVA 1.0.0 test helper",
        "Fake-provider binary SHA-256: `61584947f1661a8b027c238c4c3283e1cddd8f252c19fdb754b98379af3ece76`",
        "AVA binary SHA-256: `31969596f1236872aa8faad8ad6e4e4648dc1585a04055765c4ad3d835192283`",
        "exact inherited phase-tag scan found no PID",
        "Unsupported/unobserved:",
    ):
        assert token in report, token
    assert report.count("- Outcome: `pass`") == 2
    assert "[REQUIRED" not in report and "- [ ]" not in report
    assert not re.search(r"/(?:home|Users)/[^/\s`<>]+", report)
    assert not re.search(r"/(?:tmp|var/tmp)/[^\s`<>]*ava-(?:acp|zed|dogfood)", report, re.I)
    assert not re.search(r"!\[[^]]*\]\([^)]*\)", report)

    cmake = (source / "tests/CMakeLists.txt").read_text(encoding="utf-8")
    assert_option_off(cmake, "AVA_REQUIRE_ACP_SDK_INTEROP")
    assert_option_off(cmake, "AVA_ENABLE_ACPX_INTEROP")
    for test_name in ("ava_cli.acp_sdk_interop", "ava_cli.acpx_interop", "ava_tests.acp_interop_manifest"):
        assert test_name in cmake
    assert "SKIP_RETURN_CODE 77" in cmake and "--required" in cmake
    assert "--root ${CMAKE_BINARY_DIR}/acp-sdk-interop" not in cmake
    assert cmake.index("if(AVA_ENABLE_ACPX_INTEROP)") < cmake.index("NAME ava_cli.acpx_interop")
    assert "acp-acpx/node_modules/.bin/acpx" in cmake

    sdk_harness = (sdk_dir / "interop.mjs").read_text(encoding="utf-8")
    assert "ndJsonStream(Writable.toWeb(ava.child.stdin), Readable.toWeb(ava.child.stdout))" in sdk_harness
    assert ".connectWith(" in sdk_harness
    assert "process.exitCode = 77" in sdk_harness
    assert "[\"--acp\"]" in sdk_harness
    assert "env.MOONSHOT_API_KEY = FAKE_KEY" in sdk_harness
    assert "scrubParentEnvironment();" in sdk_harness
    assert "...process.env" not in sdk_harness

    acpx_harness = (acpx_dir / "interop.py").read_text(encoding="utf-8")
    for token in ("EXPECTED_NODE_VERSION = \"v24.13.1\"", "EXPECTED_ACPX_VERSION = \"0.12.0\"", "\"--agent\"", "\"exec\"", "\"--json-strict\"", "\"--approve-all\""):
        assert token in acpx_harness
    assert "start_new_session=True" in acpx_harness
    assert "os.environ.clear()" in acpx_harness

    dogfood_script_path = source / dogfood["scriptPath"]
    assert dogfood_script_path.stat().st_mode & 0o111
    dogfood_script = dogfood_script_path.read_text(encoding="utf-8")
    for token in (
        "run_ctest_gate sdk AVA_REQUIRE_ACP_SDK_INTEROP",
        "run_ctest_gate acpx AVA_ENABLE_ACPX_INTEROP",
        "--confinement sandbox",
        "--wayland-display",
        "--acknowledge-disposable-credential-free",
        "ordinary-account profile-only execution is rejected",
        "XDG_CONFIG_HOME=\"$phase_root/xdg-config\"",
        "AVA_ZED_DOGFOOD_PHASE_ROOT=\"$phase_root\"",
        "ZED_ALLOW_EMULATED_GPU=1",
        "MAX_ZED_FILE_BLOCKS=65536",
        "process_alive_non_zombie",
        "open(f\"/proc/{pid}/environ\"",
        "signal.pidfd_send_signal(pidfd, signal_number)",
        "cleanup_phase_tagged_processes \"$phase_root\"",
        "cleanup_zed_phase_processes \"$phase_root\" || status=1",
        "wait_for_capture_exit",
        "capture exited nonzero",
        "mkfifo -- \"$zed_stdout_fifo\" \"$zed_stderr_fifo\"",
        "bounded residual scan found no PID",
        "\"command\": ava",
        "\"args\": [\"--acp\"]",
        "end-to-end-workflow",
        "scenario=text",
        "report_status: incomplete",
        "zed sanitize-copy",
        "sanitization rejected: fake key value is present",
    ):
        assert token in dogfood_script, token
    assert "npm ci" not in dogfood_script and "npx" not in dogfood_script
    assert "pgrep -f" not in dogfood_script

    policy = (source / dogfood["evidencePolicyPath"]).read_text(encoding="utf-8")
    for token in (
        "Zed 1.9.0 is **manual verified only for the flows captured",
        "observed",
        "inferred",
        "Raw GUI logs",
        "report_status: incomplete",
        "configuration documented but not executed",
    ):
        assert token in policy, token

    acp_docs = (source / "docs/acp.md").read_text(encoding="utf-8")
    for token in (
        "Official `@agentclientprotocol/sdk` | 1.2.1",
        "`acpx` | 0.12.0",
        "automated, opt-in alpha compatibility smoke, not official conformance",
        "`Zed` | 1.9.0",
        "`manual verified`",
        "zed-1.9.0-2026-07-14.md",
        "`JetBrains` | 2026.1+",
        "configuration documented but not executed",
        "$XDG_CONFIG_HOME/zed/settings.json",
        "~/.jetbrains/acp.json",
        "CodeCompanion.nvim",
        "terminal=false",
        "npm ci --ignore-scripts --no-audit --no-fund",
        "Node 24.13.1",
        "AVA_REQUIRE_ACP_SDK_INTEROP",
        "AVA_ENABLE_ACPX_INTEROP",
    ):
        assert token in acp_docs, token

    workflow = (source / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    for sha in (
        "3d3c42e5aac5ba805825da76410c181273ba90b1",
        "55cc8345863c7cc4c66a329aec7e433d2d1c52a9",
        "820762786026740c76f36085b0efc47a31fe5020",
    ):
        assert sha in workflow
    assert "permissions:\n  contents: read" in workflow
    assert workflow.count("persist-credentials: false") == workflow.count("actions/checkout@")
    assert "node-version: 24.13.1" in workflow
    assert workflow.count("npm ci --ignore-scripts --no-audit --no-fund") >= 2
    assert "AVA_REQUIRE_ACP_SDK_INTEROP=ON" in workflow
    assert "AVA_ENABLE_ACPX_INTEROP=ON" in workflow
    assert "workflow_dispatch:" in workflow and "run_acpx:" in workflow
    assert "env -i" in workflow
    sanitizer_job = workflow.split("\n  sanitizer:\n", 1)[1].split("\n  acp-sdk-interop:", 1)[0]
    assert "id: sanitizer-test" in sanitizer_job
    assert "if: always() && steps.sanitizer-test.outcome != 'skipped'" in sanitizer_job
    assert "summarize-test-results.py" in sanitizer_job
    assert "ava-junit-sanitize.xml" in sanitizer_job

    print("ACP M6 interoperability package locks, metadata, CMake, and CI are consistent")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, KeyError, ValueError, UnicodeDecodeError) as error:
        print(f"ACP interoperability manifest check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
