# Backend Benchmarking

AVA's M0 backend harness records the pre-modernization architecture with AVA in offline mode, an allowlisted child environment, and repository-owned local fixtures. Results are observations of one exact machine, build, source tree, and set of artifact bytes. They are not portable performance claims.

The harness is not an operating-system network sandbox. AVA runs with `--offline`, and the repository-owned todo plugin fixture performs no network I/O, but an arbitrary replacement plugin is not thereby prevented from using the network.

## Build and run

Build the application and test-only helper first:

```sh
cmake --preset release
cmake --build build-release --target ava ava_backend_benchmark_helper
```

Run the quick suite used by CTest:

```sh
scripts/benchmark-backend.py \
  --ava build-release/ava \
  --benchmark-helper build-release/tests/ava_backend_benchmark_helper \
  --suite smoke --runs 1 \
  --output /tmp/ava-backend-smoke.json \
  --report /tmp/ava-backend-smoke.md
```

`--suite smoke` requires an executable `--benchmark-helper`; argument validation fails before any benchmark runs if it is absent. The registered smoke therefore exercises native dispatch, plugin cleanup, manifest discovery without process spawn, cancellation acknowledgement, session opening, and repeated-call memory. An unsupported result for one of those required helper seams fails the smoke checks rather than passing silently.

Use `--suite baseline --runs 5` for machine-specific baseline evidence. Use `--suite stress --runs 10` only for opt-in detailed work; it includes larger catalog/session fixtures and more one-shot plugin calls. Baseline and stress may omit the C++ helper, in which case helper-backed families remain present as structured `unsupported` entries and smoke checks are not applied. `--fake-provider` records a supplied local fake-provider artifact, although the harness does not execute a provider. `--memory-helper` defaults to [`scripts/benchmark-memory.py`](../../scripts/benchmark-memory.py), and `--sample-plugin` defaults to the repository's todo plugin.

## Methodology

- Python uses `time.monotonic_ns`; the C++ helper uses `std::chrono::steady_clock`.
- Processes launched directly by the backend harness receive a fresh environment rather than a filtered copy of the host environment. Its complete allowlist is a fixed `PATH` (`/usr/local/bin:/usr/bin:/bin`), `LANG`/`LC_ALL=C.UTF-8`, isolated `HOME`/XDG/`TMPDIR`, `TERM=dumb`, `NO_COLOR=1`, disabled Git prompting, AVA offline/debug-suppression controls, and `AVA_SESSION_TITLES=off`. Host `PYTHONPATH`, loader variables, askpass/Kerberos/Docker/database/provider/base-URL/credential variables, proxies, and arbitrary variables do not survive. The memory helper derives its nested AVA environment from that already-allowlisted map and applies only its fixed PTY terminal settings.
- `run_process` owns subprocess cleanup on success, timeout, and cancellation, reaping descendants through a dedicated Linux subreaper (including children that call `setsid`); other POSIX hosts use a private process group. Linux timings exclude the owner's Python startup and cleanup grace.
- SHA-256 (computed with Python's standard library), byte size, path, mtime, and executable mode identify AVA, the benchmark helper, a supplied fake provider, the memory helper, the sample plugin manifest and entrypoint, and the benchmark script. AVA also gets a bounded `--version` probe; plugin manifest identity records its ID/version/API and entrypoint declaration.
- Source identity records repository path, exact commit, committed tree, and dirty state separately from artifact byte identity. Build metadata records the nearest CMake cache/source root, build type, compiler ID/version, sanitizer/TSan/debug/libcwd settings when present, and relevant C++ flags.
- Build provenance and freshness remain best effort. Cache paths and mtimes can show a mismatch, but the harness does not claim that an executable embeds the recorded Git commit or prove that the executable was produced from that source state. Build the requested targets immediately before collecting evidence.
- Linux idle RSS reuses the PTY and `/proc/*/smaps_rollup` process-tree logic in `benchmark-memory.py`. Each backend run value is the maximum RSS among that run's raw snapshots, so the result-level maximum is supported by retained observations. The backend JSON retains the complete memory-helper output, including warm-up, every measured snapshot, process names, PSS/USS/swap/process/thread details, and the helper summaries.
- The repeated-call C++ seam reads current Linux resident RSS from `/proc/self/statm` before and after the direct no-op call loop, converting resident pages with the runtime page size. Peak `getrusage` high-water readings remain separate details. macOS uses an explicitly named peak-high-water delta fallback; other platforms report that seam as unsupported.
- The repeated-call RSS delta is a narrow direct-call seam. It is not evidence of product-wide memory stability, long-lived worker behavior, allocator reclamation, or process-tree stability.
- Session fixtures are generated through `SessionStore` with current valid records. Plugin measurements reuse the sample todo plugin. Manifest discovery asserts that no child becomes waitable.
- The current plugin application path is one-shot. First and repeated plugin results are labeled `current_one_shot`; they are not claims about persistent worker warmth.
- Native registry lookup builds the current built-in registry before timing, chooses its final registered entry, and times exact linear lookup for the requested iterations. It emits `ns_per_lookup`, target name, and entry count; schema materialization is not part of that measurement.
- Synthetic catalog measurements use a deliberately composite timed boundary: registry construction, full schema materialization, and lookup of the final synthetic entry. The nonexistent selective router remains explicitly unsupported.
- A reproducible machine-cold start would require cache controls the harness does not assume. `cold_startup` is therefore unsupported; `warm_startup` records full offline RPC initialization after one warm-up.

## Interpretation and CI

The JSON is the authoritative harness output; Markdown is only a readable rendering. Compare distributions and inspect all raw samples before discussing a delta. Keep source identity, artifact hashes, host, compiler, CMake source root, and build settings with any claim, and rerun on the same quiet host.

`parameters.exact_command` records the benchmark script's `sys.argv`. An external wrapper such as `/usr/bin/time` is outside that argument vector, so wrapper-only observations are not represented by the JSON and must not be described as part of its recorded evidence.

The registered `ava_benchmark.smoke` CTest uses broad reliability ceilings only: completion within a practical timeout, required helper seams measured, no plugin child left waitable, no child from manifest discovery, and no catastrophic RSS delta. It does **not** gate pull requests on microbenchmark deltas. Detailed baseline and stress suites are intentionally opt-in and unregistered.

Both `ava_benchmark.smoke` (`TIMEOUT 75`) and `ava_benchmark.process_smoke` (`TIMEOUT 120`) use ordinary parallel CTest scheduling. They gate broad reliability ceilings, not microperformance comparisons, so detailed benchmark measurements should be launched in isolation explicitly. Fixture-specific locks remain limited to tests that share those fixtures.

Comparator fixtures in `ava_tests.benchmark_driver` and `ava_benchmark.process_harness_self_test` are synthetic-only. Live-Git cases run separately as `ava_benchmark.live_git_self_test` (`TIMEOUT 180`) and retain the shared Git-fixture lock. `ava_benchmark.historical_artifact` is optional (`TIMEOUT 15`) and returns skip code 77 when the retained historical M0 artifact is absent.

On non-Linux hosts, helper timing measurements can still run, but procfs idle process-tree RSS is explicitly unsupported. The macOS repeated-call result is a labeled peak-high-water fallback rather than current RSS; a platform with neither supported behavior reports `unsupported`, which intentionally does not satisfy the required smoke seam.

## M1 process-supervision extension

M1 extends the M0 harness; it does not reinterpret M0 artifacts. `smoke`, `baseline`, and `stress` still emit `ava.backend-benchmark.v2`, and `ava_benchmark.historical_artifact` validates the historical M0 JSON when present. The additional suites are:

- `process-smoke`: the default registered catastrophic-correctness CTest, with one helper invocation per run;
- `process-baseline`: opt-in raw distributions suitable for a same-host family migration pair.

They emit `ava.backend-benchmark.v3`. New helper cases emit one `ava.backend-benchmark-helper.v2` object. Every helper observation is retained as `{run, observation, value, metrics, checks}`; observations in one helper invocation remain visibly correlated and are not described as independent repetitions. Primary and numeric metric summaries use median, nearest-rank p95, and maximum.

The ordered result set always contains application startup/RSS, scope construction, first and warm spawn commit, concurrent 1/8/64 records, natural/leader-first/TERM-refusal settlement, shared-budget shutdown64, pidfd and forced POSIX fallback monitor 1/8/64, and Curl/Plugin/MCP/LSP/Bash lifecycle results. Unsupported entries stay in place with a closed reason such as `source_architecture_absent`, `caller_not_migrated`, `pidfd_unavailable`, or `fixture_unavailable`; the harness never substitutes zero or synthetic measurements.

### Build and quick process smoke

Use one build recipe for both members of a pair. For example:

```sh
cmake -S . -B /tmp/ava-process-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/ava-process-build --target \
  ava ava_backend_benchmark_helper ava_fake_process_child \
  ava_fake_mcp_server ava_fake_lsp_server
ctest --test-dir /tmp/ava-process-build --output-on-failure \
  -R '^ava_benchmark\.(process_harness_self_test|process_smoke)$'
```

The equivalent direct command is:

```sh
runtime_reference=$(git rev-parse --verify \
  'dd4460348658c4127606db679da873cbf3dba274^{commit}')
git diff --exit-code "$runtime_reference" -- \
  src CMakeLists.txt cmake config.h.in
scripts/benchmark-backend.py \
  --ava /tmp/ava-process-build/ava \
  --benchmark-helper /tmp/ava-process-build/tests/ava_backend_benchmark_helper \
  --fake-process-child /tmp/ava-process-build/tests/ava_fake_process_child \
  --fake-mcp-server /tmp/ava-process-build/tests/ava_fake_mcp_server \
  --fake-lsp-server /tmp/ava-process-build/tests/ava_fake_lsp_server \
  --memory-helper scripts/benchmark-memory.py \
  --sample-plugin examples/plugins/todo \
  --runtime-reference "$runtime_reference" \
  --suite process-smoke --runs 1 \
  --output /tmp/ava-process-smoke.json \
  --report /tmp/ava-process-smoke.md
```

A build that contains the M1 process target but omits its fake child cannot pass process smoke: process cases return `fixture_unavailable`, and required-case checks fail. A source tree that genuinely predates the process architecture instead returns `source_architecture_absent` for every neutral supervisor/monitor result; this is the explicit portability state, not a performance result. Family lifecycle drivers still compile and measure there.

### Pinned-harness paired baseline recipe

A comparison uses three independent, clean worktrees: one pinned harness and two measured source cohorts. The harness for this contract is exactly `9332a106ec51504dd9fc85b014870b48ac25b99c`. Run **both** cohorts with `harness_src/scripts/benchmark-backend.py`; a family migration commit may predate `--measured-source-root` or another evidence-integrity check and is never the harness authority merely because it supplies a measured binary.

The measured and harness worktrees are live comparison inputs, not collection-only paths. Keep all three present, pinned at their recorded commits and trees, and Git-clean through the forward comparison and any reversed-order comparison. Cleanup is permitted only after the last comparison has completed.

For the current Plugin migration, the reviewed pair is:

```sh
export AVA_FAMILY_BEFORE_COMMIT=13fb0cef5925368fa12f8bcf693235281bce099f
export AVA_FAMILY_MIGRATION_COMMIT=2a30f40ec562b49915c3b09369cf4e6897de3d4d
export AVA_FAMILY_BEFORE_AUTHORITIES='curl=supervised,plugin=legacy_local,mcp=legacy_local,lsp=legacy_local,bash=legacy_local'
export AVA_FAMILY_AFTER_AUTHORITIES='curl=supervised,plugin=supervised,mcp=legacy_local,lsp=legacy_local,bash=legacy_local'
```

The before anchor already has supervised Curl and legacy Plugin. The after anchor has supervised Curl and Plugin. Consequently, this pair isolates the Plugin transition; MCP, LSP, and Bash stay legacy in both cohorts. Its recursive Plugin scope changes, the Bash pathspec scope is identical, and the separately recorded shared process-plumbing scope changes. A qualified comparison reports those facts in `source_attribution` and reaches `measured`.

For a later family, set `AVA_FAMILY_BEFORE_COMMIT` and `AVA_FAMILY_MIGRATION_COMMIT` to separately reviewed full commit IDs and set both expected authority maps explicitly. The before commit must be the immediate reviewed baseline for that family, not an all-legacy historical carrier. The two commits may differ elsewhere only after review; the comparator requires exactly one `legacy_local` to `supervised` authority transition, isolates changes in the family-owned scopes, and records shared process-plumbing changes separately.

#### Create and verify the three worktrees

Run from a clean repository that contains all three pinned objects:

```sh
harness_commit=9332a106ec51504dd9fc85b014870b48ac25b99c
: "${AVA_FAMILY_BEFORE_COMMIT:?set the reviewed full before commit ID}"
: "${AVA_FAMILY_MIGRATION_COMMIT:?set the reviewed full migration commit ID}"
: "${AVA_FAMILY_BEFORE_AUTHORITIES:?set the reviewed before authority map}"
: "${AVA_FAMILY_AFTER_AUTHORITIES:?set the reviewed after authority map}"

for commit in \
  "$harness_commit" \
  "$AVA_FAMILY_BEFORE_COMMIT" \
  "$AVA_FAMILY_MIGRATION_COMMIT"
do
  test "$(printf %s "$commit" | wc -c)" -eq 40
  test "$(git rev-parse --verify "$commit^{commit}")" = "$commit"
done

harness_src=/tmp/ava-process-harness-src
before_src=/tmp/ava-process-before-src
after_src=/tmp/ava-process-after-src
harness_build=/tmp/ava-process-harness-build
before_build=/tmp/ava-process-before-build
after_build=/tmp/ava-process-after-build
benchmark_python=$(command -v python3)
test -x "$benchmark_python"

test ! -e "$harness_src"
test ! -e "$before_src"
test ! -e "$after_src"
rm -rf "$harness_build" "$before_build" "$after_build"
git worktree add --detach "$harness_src" "$harness_commit"
git worktree add --detach "$before_src" "$AVA_FAMILY_BEFORE_COMMIT"
git worktree add --detach "$after_src" "$AVA_FAMILY_MIGRATION_COMMIT"

for source in "$harness_src" "$before_src" "$after_src"
do
  git -C "$source" submodule update --init --recursive
  test -z "$(git -C "$source" status --porcelain --untracked-files=normal)"
done

test "$(git -C "$harness_src" rev-parse HEAD)" = "$harness_commit"
test "$(git -C "$before_src" rev-parse HEAD)" = "$AVA_FAMILY_BEFORE_COMMIT"
test "$(git -C "$after_src" rev-parse HEAD)" = "$AVA_FAMILY_MIGRATION_COMMIT"

check_authority_map() {
  "$benchmark_python" - "$1" "$2" <<'PY'
import re
import sys
from pathlib import Path

families = ("curl", "plugin", "mcp", "lsp", "bash")
source = Path(sys.argv[1]) / "tests" / "backend_benchmark_authorities.cmake"
expected = dict(item.split("=", 1) for item in sys.argv[2].split(","))
matches = re.findall(
    r"set\(AVA_BENCHMARK_(CURL|PLUGIN|MCP|LSP|BASH)_AUTHORITY +(legacy_local|supervised)\)",
    source.read_text(encoding="utf-8"),
)
actual = {name.lower(): authority for name, authority in matches}
if tuple(expected) != families or actual != expected:
    raise SystemExit(f"authority map mismatch for {source}: expected={expected}, actual={actual}")
print(f"verified {source}: {actual}")
PY
}

check_authority_map "$before_src" "$AVA_FAMILY_BEFORE_AUTHORITIES"
check_authority_map "$after_src" "$AVA_FAMILY_AFTER_AUTHORITIES"

"$benchmark_python" - "$AVA_FAMILY_BEFORE_AUTHORITIES" "$AVA_FAMILY_AFTER_AUTHORITIES" <<'PY'
import sys

before = dict(item.split("=", 1) for item in sys.argv[1].split(","))
after = dict(item.split("=", 1) for item in sys.argv[2].split(","))
changes = [name for name in before if before[name] != after.get(name)]
if len(changes) != 1 or (before[changes[0]], after[changes[0]]) != ("legacy_local", "supervised"):
    raise SystemExit(f"expected exactly one legacy_local->supervised transition, got {changes}")
print(f"verified family transition: {changes[0]}")
PY
```

Initializing submodules in linked worktrees shares the superproject's submodule object and configuration storage. Users who need stronger isolation may use three local clones instead. This linked-worktree recipe must not deinitialize submodules, because doing so can mutate the shared primary checkout configuration.

The source-file check occurs before measurement and prevents stale cache or command-line authority values from defining the claim. The harness also records that file's Git object, byte hash, and exact map from the measured source root. The capability probe, measured AVA build provenance, independent helper build provenance, and family results must all agree with that source-owned map.

#### Build once per source and collect the forward pair

Use one generator, compiler, build type, and feature recipe for the measured cohorts. The pinned harness supplies one byte-identical process child, fake provider, fake MCP server, fake LSP server, memory helper, and sample plugin to both runs, and both invocations must use the same Python executable. Each measured build supplies only its own AVA and authority-bearing benchmark helper; those two expected-to-differ artifacts come from the same measured source and CMake build tree in this recipe. Helper dependencies may build local fixture copies, but the commands below do not use them as evidence.

```sh
cmake -S "$harness_src" -B "$harness_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$harness_build" --target \
  ava_fake_process_child ava_fake_provider_server \
  ava_fake_mcp_server ava_fake_lsp_server

cmake -S "$before_src" -B "$before_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$before_build" --target \
  ava ava_backend_benchmark_helper

cmake -S "$after_src" -B "$after_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$after_build" --target \
  ava ava_backend_benchmark_helper

"$benchmark_python" "$harness_src/scripts/benchmark-backend.py" \
  --measured-source-root "$before_src" \
  --ava "$before_build/ava" \
  --benchmark-helper "$before_build/tests/ava_backend_benchmark_helper" \
  --fake-process-child "$harness_build/tests/ava_fake_process_child" \
  --fake-provider "$harness_build/tests/ava_fake_provider_server" \
  --fake-mcp-server "$harness_build/tests/ava_fake_mcp_server" \
  --fake-lsp-server "$harness_build/tests/ava_fake_lsp_server" \
  --memory-helper "$harness_src/scripts/benchmark-memory.py" \
  --sample-plugin "$harness_src/examples/plugins/todo" \
  --runtime-reference "$AVA_FAMILY_BEFORE_COMMIT" \
  --run-order before_then_after \
  --suite process-baseline --runs 5 \
  --output /tmp/ava-process-before.json \
  --report /tmp/ava-process-before.md

"$benchmark_python" "$harness_src/scripts/benchmark-backend.py" \
  --measured-source-root "$after_src" \
  --ava "$after_build/ava" \
  --benchmark-helper "$after_build/tests/ava_backend_benchmark_helper" \
  --fake-process-child "$harness_build/tests/ava_fake_process_child" \
  --fake-provider "$harness_build/tests/ava_fake_provider_server" \
  --fake-mcp-server "$harness_build/tests/ava_fake_mcp_server" \
  --fake-lsp-server "$harness_build/tests/ava_fake_lsp_server" \
  --memory-helper "$harness_src/scripts/benchmark-memory.py" \
  --sample-plugin "$harness_src/examples/plugins/todo" \
  --runtime-reference "$AVA_FAMILY_MIGRATION_COMMIT" \
  --run-order before_then_after \
  --suite process-baseline --runs 5 \
  --output /tmp/ava-process-after.json \
  --report /tmp/ava-process-after.md \
  --compare-to /tmp/ava-process-before.json \
  --comparison-output /tmp/ava-process-comparison.json
```

Both build target sets are appropriate to their source trees, while fixture bytes and harness bytes are common. `--measured-source-root` remains explicit and different for the two runs, so checkout, runtime-reference, family-source, and CMake provenance describe the binary under measurement rather than the harness checkout.

#### Reversed-order confirmation after a trigger

A forward item with `investigation_trigger: true` is a request to investigate, not a regression verdict. Collect a fresh pair in the reverse temporal order—after first, before second—on the same host and boot. Reusing or relabeling the forward documents is not confirmation.

```sh
"$benchmark_python" "$harness_src/scripts/benchmark-backend.py" \
  --measured-source-root "$after_src" \
  --ava "$after_build/ava" \
  --benchmark-helper "$after_build/tests/ava_backend_benchmark_helper" \
  --fake-process-child "$harness_build/tests/ava_fake_process_child" \
  --fake-provider "$harness_build/tests/ava_fake_provider_server" \
  --fake-mcp-server "$harness_build/tests/ava_fake_mcp_server" \
  --fake-lsp-server "$harness_build/tests/ava_fake_lsp_server" \
  --memory-helper "$harness_src/scripts/benchmark-memory.py" \
  --sample-plugin "$harness_src/examples/plugins/todo" \
  --runtime-reference "$AVA_FAMILY_MIGRATION_COMMIT" \
  --run-order after_then_before \
  --suite process-baseline --runs 5 \
  --output /tmp/ava-process-reverse-after.json \
  --report /tmp/ava-process-reverse-after.md

"$benchmark_python" "$harness_src/scripts/benchmark-backend.py" \
  --measured-source-root "$before_src" \
  --ava "$before_build/ava" \
  --benchmark-helper "$before_build/tests/ava_backend_benchmark_helper" \
  --fake-process-child "$harness_build/tests/ava_fake_process_child" \
  --fake-provider "$harness_build/tests/ava_fake_provider_server" \
  --fake-mcp-server "$harness_build/tests/ava_fake_mcp_server" \
  --fake-lsp-server "$harness_build/tests/ava_fake_lsp_server" \
  --memory-helper "$harness_src/scripts/benchmark-memory.py" \
  --sample-plugin "$harness_src/examples/plugins/todo" \
  --runtime-reference "$AVA_FAMILY_BEFORE_COMMIT" \
  --run-order after_then_before \
  --suite process-baseline --runs 5 \
  --output /tmp/ava-process-reverse-before.json \
  --report /tmp/ava-process-reverse-before.md

"$benchmark_python" - \
  "$harness_src/scripts/benchmark-backend.py" \
  /tmp/ava-process-reverse-before.json \
  /tmp/ava-process-reverse-after.json \
  /tmp/ava-process-reverse-comparison.json <<'PY'
import importlib.util
import json
import sys
from pathlib import Path

script, before_path, after_path, output_path = map(Path, sys.argv[1:])
spec = importlib.util.spec_from_file_location("ava_backend_benchmark", script)
if spec is None or spec.loader is None:
    raise SystemExit(f"cannot import pinned harness: {script}")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
before = json.loads(before_path.read_text(encoding="utf-8"))
after = json.loads(after_path.read_text(encoding="utf-8"))
comparison = module.compare_process_documents(before, after)
comparison["provenance"] = {
    "before_commit": before["provenance"]["measured_checkout"]["commit"],
    "after_commit": after["provenance"]["measured_checkout"]["commit"],
    "run_order_confirmation": "reversed_order_required",
}
comparison["artifacts"] = {
    "before_document": module.file_identity_v3(before_path),
    "after_document": module.file_identity_v3(after_path),
}
module.validate_comparison_document(comparison)
output_path.write_text(json.dumps(comparison, indent=2, sort_keys=True) + "\n", encoding="utf-8")
if comparison["status"] != "measured":
    raise SystemExit(f"reverse comparison unsupported: {comparison['reason_code']}")
print(f"Comparison: {output_path}")
PY
```

The comparator keeps semantic before/after order even though collection order is reversed. A repeatable claim still requires inspection of both pairs and their raw samples; the JSON intentionally never promotes one pair to `repeatable_claim: true`.

#### Cleanup and the historical first/Curl carrier

Keep evidence JSON/Markdown as needed. Cleanup is deliberately last and remains fail-closed: first prove every linked worktree is still clean, then remove each one with the single force form that handles initialized submodules without deinitializing shared configuration. Any failed status, worktree-removal, or build-tree-removal command stops cleanup:

```sh
for source in "$after_src" "$before_src" "$harness_src"
do
  if ! status=$(git -C "$source" status --porcelain --untracked-files=normal); then
    printf 'cannot verify worktree status: %s\n' "$source" >&2
    exit 1
  fi
  if test -n "$status"; then
    printf 'refusing to remove non-clean worktree: %s\n%s\n' "$source" "$status" >&2
    exit 1
  fi
done

# All three status commands succeeded and all three worktrees are clean before
# any removal begins.
for source in "$after_src" "$before_src" "$harness_src"
do
  if ! git worktree remove --force "$source"; then
    printf 'cannot remove worktree: %s\n' "$source" >&2
    exit 1
  fi
done

# These are external build trees, not worktree paths; remove them separately.
if ! rm -rf "$harness_build" "$before_build" "$after_build"; then
  printf 'cannot remove external benchmark build trees\n' >&2
  exit 1
fi
```

Do not run `git submodule deinit` from these linked worktrees, raw-remove their paths, or otherwise edit the primary checkout's shared submodule configuration during cleanup. Offline re-comparison after removing the measured or harness source worktrees is intentionally unsupported unless the exact worktrees are recreated at their recorded paths and commits and are again clean.

The historical `971327fb66fc372f5828c5f5967e118d9374f9da` instrumentation carrier (with `dd7cb260d58beb6f2d69bc07dc0bb604d65bd3ef` and `789b100c728bd9f95a68324e8eaa8012d9b09cdb` applied) still represents production paths from all-legacy `c94ac863141975806bbab52e950a2f2499108b65`. It is useful only for a separately reviewed first/Curl migration pair. It is **not** the Plugin baseline: comparing that all-legacy carrier to the current Plugin after commit introduces Curl and Plugin transitions, which the comparator rejects as `single_authority_transition_required`. Use `13fb0cef5925368fa12f8bcf693235281bce099f` as the Plugin before anchor instead.

In the pinned Plugin after cohort above, Curl and Plugin are source-owned `supervised`; MCP, LSP, and Bash are `legacy_local`. That map belongs to the recorded comparison cohort, not today's driver. The current source-owned map in `tests/backend_benchmark_authorities.cmake` marks Curl, Plugin, and MCP `supervised`; only LSP and Bash remain `legacy_local` in this five-family benchmark. Stale cache entries and command-line values cannot assert migration. A supervised family driver emits `supervisor_record_finished=true`, `supervisor_settlement_once=true`, and `cleanup_scope=managed_group` only after verifying its finished record and complete cleanup. Changing a remaining declaration without adapting its driver still returns structured `caller_not_migrated`.

### Driver boundaries and cleanup evidence

Neutral process modes use only the public Supervisor and narrow test telemetry APIs:

- idle scope constructs `Supervisor` plus application scope, proving no monitor, no live record, no Linux thread/immediate-child delta, and recording current RSS delta;
- spawn commit includes exact-environment mint, reservation, launch, and confirmed exec; warm and concurrent cases retain every child observation;
- natural, leader-first descendant, TERM refusal, and shutdown64 time their stated settlement boundaries and verify expected reason, cleanup, endpoint EOF where applicable, and settlement count one;
- monitor modes use ready idle children and `CLOCK_PROCESS_CPUTIME_ID`, normalized to `cpu_ns_per_wall_second`. Automatic mode requires actual pidfd selection and zero periodic fallback probes. Forced fallback reaches the logarithmic buckets and one-second cap before its fixed hold.

After every measured process driver, endpoints and consumers are closed before Supervisor destruction, retained handles are waited, records are Finished with settlement count one, `live_records` is zero, shutdown is complete, and monitor resources are gone. Only then does the helper use `waitpid(-1, WNOHANG) == ECHILD` as an **immediate-child guard**. Descendant cleanup is evidenced by Supervisor settlement and inherited-endpoint EOF; the immediate-child guard is never presented as descendant evidence. Legacy family checks are explicitly labeled `immediate_children_only`.

The fixed family boundaries are one stdlib-Python loopback Curl request; todo sample plugin initialize/call/shutdown; fake MCP initialize/tools-list/shutdown; fake LSP initialize/diagnostics/destruction; and a benign direct-argv command through normal sealed Bash planning and execution. Curl, Plugin, and MCP measure supervised managed-group lifecycles; only LSP and Bash retain legacy immediate-child boundaries. The MCP driver checks exactly one finished, completely cleaned Supervisor record and one settlement before emitting its managed-group evidence. All retain content-free compatibility checks only.

### Provenance, redaction, and comparison

V3 records the measured source root and checkout commit/tree/dirty state; runtime reference and production-path equality; the independent harness repository, commit/tree/dirty state, contract, and script hash; and non-overlapping source ownership scopes from the measured root. Each scope records its kind, canonical paths and Git pathspecs, every recursively resolved entry's mode/type/object/path, entry count, and a deterministic SHA-256 digest over the complete declaration and entry set. Plugin, MCP, and LSP own their complete dedicated module trees. Curl owns every `src/ava/http/curl_transport*` entry, and Bash owns every entry selected by `:(glob)src/ava/tools/bash_tool*`, so newly split files are included automatically while shared `src/ava/tools/CMakeLists.txt` and `ToolContext` plumbing are not attributed to Bash.

The remaining production source paths form a separately declared shared process-plumbing scope. It includes the agent, app, process, and non-Bash tools paths (as well as top-level CMake/configuration paths) while excluding every family-owned scope. Generation fails if the family and shared scopes overlap or do not completely cover `src`, `CMakeLists.txt`, `cmake`, and `config.h.in`. V3 also records the exact source-owned authority file object/hash/map and independent nearest-cache build provenance for both measured AVA and the benchmark helper. Each build record includes CMake generator/version/cache hash/source root/build type/features, compiler path/hash/ID/version/flags, and relevant CMake/C++ flags. Every used binary/script/fixture/plugin retains hash, size, mode, and mtime alongside OS/kernel/machine/CPU/count/RAM/page/Python, hashed boot ID, limits, monotonic resolution, start/end load, and exact driver commands and scale parameters. Paths occur only under provenance or artifacts.

Build provenance remains best effort: **neither AVA nor the benchmark helper embeds a verified source commit**. Comparison nevertheless requires both identities to resolve, each binary to be inside its recorded CMake tree, both source roots to match the measured root, and the helper's cache recipe, generator, build type, compiler, flags, and features to equal AVA's recorded configuration (the same cache used below is the strongest simple case; a separately recorded equivalent configuration is also accepted). A helper from another checkout or an inconsistent cache yields structured `comparison_provenance_required`, never a measurement.

Historical V3 documents without the independent measured/harness split, the newer additive authority/helper-build binding, or the complete source-scope format remain valid standalone V3 artifacts. Comparison still returns structured `provenance_split_required` for the missing split and `comparison_provenance_required` for unresolved or absent comparison-only binding or ownership scopes; partial provenance groups are invalid.

Results and samples contain no PID, PGID, raw owner ID, descriptor, argv/command, executable/cwd path, URL, environment value, child output, protocol frame, prompt, or tool content. Redaction checks tokenize composite sample keys, so names such as `child_pid`, `request_url`, and `command_argv` are rejected without rejecting closed aggregates such as `pidfd_successes`, `stdout_bytes`, `record_count`, and `endpoint_eof`. Primary samples must be non-negative. Helper checks and metrics accept only numbers, booleans, and validated closed labels. Malformed, truncated, multi-object, dynamically reasoned, or content-bearing helper output is rejected.

Optional comparison output uses `ava.backend-benchmark-comparison.v1`. Standalone process smoke may retain a developer-dirty source or harness tree, including `null` when a Git status command fails; cleanliness is a comparison gate, not a smoke usability gate. Unknown status is never recorded as false-clean. Before comparing, each cohort must have resolved full measured checkout and runtime-reference identities, clean measured production paths, exact runtime production-path equality, qualified AVA/helper build binding, and a clean resolved harness whose recorded script hash matches the script artifact. The recorded measured and harness repository paths must still be Git worktrees whose current HEAD, tree, and clean state match the recorded checkout; the recorded commits and runtime-reference trees must still resolve there.

Recorded source entries and digests are never accepted on their own. The pinned harness regenerates every canonical family scope and the shared process scope from each cohort's validated full measured commit, re-enforcing non-overlap and exact union coverage of all production paths, and requires exact deep equality with every recorded identity. It also regenerates the source-owned family-authority object, byte hash, and map from that commit. A missing or replaced worktree, Git failure, checkout mutation, unresolved or wrong commit/tree, omitted or extra scope entry, forged digest, or altered authority identity returns stable structured `comparison_provenance_required` mismatches rather than an exception or measurement.

The comparator recomputes summaries from raw samples and also refuses cohorts unless host and hashed boot, build recipe, compiler, non-authority features, units/boundaries, pinned harness identity, and every common fixture hash match. Common fixtures include the benchmark and memory scripts, Python executable, process child, optional fake provider, fake MCP/LSP servers, Curl/direct-argv executables, and sample-plugin manifest/entrypoint; measured AVA and authority-bearing helper bytes are expected to differ. Exactly one source-owned authority may change, and it must be `legacy_local` to `supervised`. The complete scope signature for exactly that transitioned family must change, while every other family-owned scope must match. Multiple or reverse/unexpected transitions, a capability/build/result authority attributed to the wrong source map, or another family-owned scope change are unsupported.

The shared process-plumbing signature is not assigned to any family and is not an isolation failure. Comparison records whether it changed as `source_attribution.shared_process_scope_changed`; it may change to support the one transitioned family. A family result is comparable only when both cohorts contain every required compatibility check and every check is true. Reports never require M1 to be faster. Investigation triggers are latency greater than both 20% and 100 microseconds, RSS greater than both 20% and 4 MiB, and monitor CPU greater than both 25% and 5 ms/s. They are non-gating; after any trigger, a repeatable claim requires the fresh reversed-order pair described above.
