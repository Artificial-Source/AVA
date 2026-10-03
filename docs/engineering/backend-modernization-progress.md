# Backend Modernization Progress

This ledger is the source of truth for AVA's backend modernization. A task is marked
`completed` only when its acceptance evidence is present in the repository and the
recorded commands have passed.

## Project context

- Implementation branch: `backend-modernization`
- Starting commit: `c94ac8631419`
- Started: 2026-08-30
- Coordinator: principal implementation agent
- Historical working-tree note: the branch was created while unrelated primary-agent
  work was preserved separately. That feature later arrived through the prioritized
  upstream lineage and is now part of the integrated base rather than an M0/M1 commit.
- Prioritized upstream base: `origin/develop` at `3924ef03` was statically audited and
  made an ancestor of this branch on 2026-09-02; the 33 modernization commits were
  replayed above it. The original M0 hashes remain the evidence identity for the
  pre-upstream `c94ac8631419` cohort and are not reinterpreted as measurements of the
  rebased runtime.
- Dependency policy: upstream added the pinned MIT `memory` submodule at
  `4ddb41469f323e32c170637aa413a132e83727be`. Its source, license, build/runtime
  integration, and static security properties were reviewed; it is attributed in
  `THIRD_PARTY_NOTICES.md`. Submodule bootstrap was restored to exact-gitlink checkout
  and rejects mismatches rather than following mutable branches.

## Status legend

`not started` · `active` · `blocked` · `completed`

## Milestones

| Task | Description | Owner | Status | Files changed | Tests / benchmark evidence | Commit | Remaining risks |
|---|---|---|---|---|---|---|---|
| M0.1 | Map backend ownership, process sites, lifecycles, cancellation, and retained data | Architecture Cartographer + coordinator | completed | `docs/engineering/backend-current-state.md` | Static inventory plus source-link and structure checks | `3d4349ef` | Re-verify if excluded concurrent work is later integrated |
| M0.2 | Reproducible startup, RSS, dispatch, plugin, catalog, session, cancellation, cleanup, and memory harness | Performance Engineer | completed | `scripts/benchmark-backend.py`; `tests/backend_benchmark_helper.cpp`; `tests/backend_benchmark_test.py`; `tests/CMakeLists.txt`; `docs/development/backend-benchmarking.md` | Release and BetaTest `ava_benchmark.harness_self_test` + `ava_benchmark.smoke`; five-run baseline schema v2 | `8fab13fe`, `971327fb` | Machine-cold startup, persistent-worker warmth, selective routing, and indexed sessions remain explicitly unsupported before their owning milestones |
| M0.3 | Establish honest machine-recorded baseline and calibrated budgets | Performance Engineer + coordinator | completed | `docs/engineering/performance-baseline.md`; `docs/engineering/backend-performance-baseline-2026-08-30.json` | Release baseline, 5 runs, exact artifacts/hashes/source/build/host identity, raw RSS snapshots, median/p95/max | `6999441f` | Same-host 20% investigation trigger is non-gating; provenance is best-effort and cold-cache control unavailable |
| M0.4 | Add lightweight CI performance/reliability smoke | Performance Engineer + coordinator | completed | `tests/CMakeLists.txt`; harness smoke checks | Default CTest registration; Release and BetaTest smoke pass; helper omission has a negative test | `8fab13fe`, `971327fb` | Coarse catastrophic ceilings intentionally do not gate microbenchmark deltas |
| M1.1 | Define process ownership identifiers and supervisor API | Process Lifecycle Architect + coordinator | completed | [`docs/engineering/process-supervisor-adr.md`](process-supervisor-adr.md) | Accepted ADR; source Markdown links, documentation structure, and textual diff checks passed | `44f28d1a` | Implementation evidence remains in M1.2–M1.5 |
| M1.2 | Implement managed Linux/POSIX process groups and tree cleanup; retain the future Windows contract without a support claim | Process Lifecycle Engineer | active | `src/ava/process/`; [`docs/engineering/process-supervisor-adr.md`](process-supervisor-adr.md) | Reservation-before-fork, gated exec/adoption, exact environments, readiness, pidfd/adaptive monitor, immutable deadlines, exact reap, scopes, shutdown, and opaque anchored executable/cwd capabilities are implemented; process suites, repeated runs, ASan/UBSan, TSan, and process benchmark smoke pass | `09312277`–`dcf9a569`, `eb3f2690` | LSP provenance mapping and Bash's async-signal-safe containment refactor are the next production prerequisites; `setsid` escape, unkillable-process limits, conservative POSIX runtime evidence, and unsupported Windows execution remain explicit |
| M1.3 | Migrate every process-spawning subsystem | Process Lifecycle Engineer + subsystem owners | active | Curl transport, Plugin runner, clipboard helpers, and MCP stdio client; migration matrix in [`docs/engineering/process-supervisor-adr.md`](process-supervisor-adr.md#api-and-authority-contract) | Curl, Plugin, clipboard, and MCP now use explicit application/session/run/operation Supervisor scopes as applicable, exact role environments, bounded output/deadlines, managed-group cleanup, and static no-legacy-authority contracts. MCP launches mint `ava-mcp-explicit-v1` only (trusted PATH, synthetic PWD, explicit config `env`; no ambient inheritance) and settle through Supervisor rather than leader `waitpid`. MCP family benchmark authority is supervised. No MCP speedup is claimed. | prior Curl/Plugin/clipboard commits; MCP integration `6f4a8dd1`–`cd27fa9e` | LSP, Bash, Mermaid, browser opener, and external editor remain legacy-local; no mixed ownership or runtime fallback is permitted |
| M1.4 | Add bounded, secret-safe process diagnostics | Process Lifecycle Engineer | active | Process snapshots, launch framing, readiness, benchmark redaction | Closed content-free process reasons/snapshots and benchmark redaction are implemented and tested | M1 foundation commits | Application trace/support-export adaptation remains incomplete; no raw PID/PGID/fd/path/argv/environment content may be added |
| M1.5 | Add leak, cancellation, timeout, descendant, and shutdown tests | Process Test Engineer | active | Process, capability, Curl, Plugin, clipboard, static-contract, and schema-v3 benchmark suites | Exact settlement/reap, leader-first descendants, refusal escalation, race gates, environment canaries, descriptor replacement/FD hygiene, deadline-vs-cancel ordering, bounded Plugin protocol queues, pidfd/fallback, repeated races, ASan/UBSan, TSan, and full 168-test BetaTest pass | M1 foundation and migration commits | Real PTY gates remain for editor migration; separate conservative-POSIX CI and future Windows tests remain required |
| M2.1 | Document and compatibility-test existing plugin protocol | Plugin Runtime Engineer | not started | pending | pending | Legacy protocol ambiguity |
| M2.2 | Implement lazy, persistent, scoped plugin worker manager | Plugin Runtime Engineer | not started | pending | pending | Concurrency, restart, and trust-scope isolation |
| M2.3 | Preserve one-shot plugin compatibility | Plugin Runtime Engineer | not started | pending | pending | Legacy behavior parity |
| M2.4 | Add versioned lifecycle/resource manifest settings | Plugin Runtime Engineer | not started | pending | pending | Manifest compatibility and safe defaults |
| M2.5 | Add worker health/restart/failure lifecycle tests | Plugin Runtime Engineer | not started | pending | pending | Process leakage and malformed protocol handling |
| M2.6 | Benchmark cold/warm workers, eviction, crashes, and runtime cost | Performance Engineer | not started | pending | pending | Runtime availability on test host |
| M3.1 | Build normalized installed/available/selected tool catalog | Tool Routing Engineer | not started | pending | pending | Stable IDs and source-version invalidation |
| M3.2 | Implement deterministic native lexical/fuzzy search | Tool Routing Engineer | not started | pending | pending | Recall versus bounded prompt cost |
| M3.3 | Define always-visible base tool set | Tool Routing Engineer + coordinator | not started | pending | pending | Workflow compatibility |
| M3.4 | Add bounded built-in tool-discovery tool | Tool Routing Engineer | not started | pending | pending | Permission-safe advertising |
| M3.5 | Add request-specific selection and catalog budgets | Tool Routing Engineer | not started | pending | pending | Provider request compatibility |
| M3.6 | Preserve normal direct-call fallback | Tool Routing Engineer | not started | pending | pending | Multi-turn discovery reliability |
| M3.7 | Add deterministic routing evaluation/scale suite | Performance Engineer + Tool Routing Engineer | not started | pending | pending | Representative query corpus |
| M4.1 | Audit storage, compaction, recovery, migrations, and RAM behavior | Session Engineer | not started | pending | pending | Existing format invariants |
| M4.2 | Define bounded active session state | Session Engineer | not started | pending | pending | Context assembly compatibility |
| M4.3 | Add versioned on-disk session index | Session Engineer | not started | pending | pending | Crash consistency and migration |
| M4.4 | Add pagination, lazy loading, metadata search, and rebuild | Session Engineer | not started | pending | pending | Large-branch behavior |
| M4.5 | Add content-addressed large-payload storage | Session Engineer | not started | pending | pending | Reference-safe GC and path safety |
| M4.6 | Persist compact versioned turn receipts | Session Engineer | not started | pending | pending | Reproducibility without secrets |
| M4.7 | Strengthen recovery, migration, and writer authority | Session Engineer | not started | pending | pending | Partial records and stale indexes |
| M4.8 | Benchmark large sessions and bounded RAM | Performance Engineer + Session Engineer | not started | pending | pending | 100k-event fixture cost |
| M5.1 | Extend tool capability/resource metadata | Scheduler Engineer | not started | pending | pending | Dynamic resource declaration fidelity |
| M5.2 | Implement symlink-safe conflict detection | Scheduler Engineer | not started | pending | pending | TOCTOU and conservative ordering |
| M5.3 | Add fair bounded execution pool and limits | Scheduler Engineer | not started | pending | pending | Cancellation and starvation |
| M5.4 | Durably persist each completed parallel result | Scheduler + Session Engineers | not started | pending | pending | Provider ordering and stale generations |
| M5.5 | Guarantee exactly-once tool-call settlement | Scheduler Engineer | not started | pending | pending | Crash/replay semantics |
| M5.6 | Add bounded loop/repetition protection | Scheduler Engineer | not started | pending | pending | False positives |
| M6.1 | Formalize versioned machine-readable plugin protocol schema | SDK Engineer + Plugin Runtime Engineer | not started | pending | pending | Single-source generation strategy |
| M6.2 | Implement TypeScript, Python, Go, and C++ SDKs | SDK Engineer | not started | pending | pending | Packaging without startup runtimes |
| M6.3 | Add plugin init/test/validate/package commands | SDK Engineer | not started | pending | pending | Existing CLI conventions |
| M6.4 | Add six examples for each required language | SDK Engineer | not started | pending | pending | Runtime/toolchain availability |
| M6.5 | Run common protocol conformance vectors across SDKs | SDK Engineer | not started | pending | pending | Cross-language cancellation behavior |
| M6.6 | Measure shared-host value; implement or document adapter/ADR | SDK + Performance Engineers | not started | pending | pending | Trust/isolation complexity |
| M7.1 | ADR comparing trusted-native interface options | Architecture + Security Reviewers | not started | pending | pending | ABI and trust risk |
| M7.2 | Define narrow versioned C ABI | Native Plugin Engineer | not started | pending | pending | Ownership, threading, allocator contract |
| M7.3 | Add disabled-by-default explicit trust gates | Native Plugin Engineer | not started | pending | pending | Project-controlled activation prevention |
| M7.4 | Add reference native plugin and compatibility tests | Native Plugin Engineer | not started | pending | pending | Crash containment is impossible in-process |
| M7.5 | Benchmark native versus persistent out-of-process paths | Performance Engineer | not started | pending | pending | Honest significance threshold |
| M8.1 | Add cross-subsystem fault-injection suite | Reliability Engineer | not started | pending | pending | Determinism and runtime cost |
| M8.2 | Add parser/path fuzz targets or adversarial equivalents | Reliability Engineer | not started | pending | pending | CI fuzz duration |
| M8.3 | Run ASan/UBSan/TSan/LSan as supported | Coordinator | not started | pending | pending | Existing unrelated findings |
| M8.4 | Establish smoke and scheduled CI performance budgets | Performance Engineer | not started | pending | pending | Host noise and baseline updates |
| M8.5 | Record dependency, license, binary, startup, memory, security deltas | Coordinator | not started | pending | pending | Platform-specific binary measurements |
| M9.1 | Update final architecture and trust-boundary documentation | Documentation Owner | not started | pending | pending | Documentation drift |
| M9.2 | Write manifest/session/tool/native migration guide | Documentation Owner | not started | pending | pending | Legacy fixture coverage |
| M9.3 | Write operations and diagnostics guide | Documentation Owner | not started | pending | pending | Command/API stability |
| M9.4 | Produce evidence-backed final technical report | Coordinator | not started | `docs/engineering/backend-modernization-final-report.md` (planned) | pending | pending | No unsupported performance claims |
| M9.5 | Add non-technical outcome summary | Documentation Owner | not started | pending | pending | Clarity |
| M9.6 | Clean full integration, conformance, migration, fault, sanitizer, performance, docs, and examples gate | Integration Reviewer + coordinator | not started | pending | pending | Cross-platform checks depend on CI |

## Review finding ledger

The integrated M0 review used the default reviewer on the complete mapping, harness,
and baseline delta. M1 planning findings record accepted design gates; `fixed` means the
design blocker is resolved, not that implementation is complete. Findings keep stable
IDs and one of: `open`, `fixed`, `rejected`, or `deferred`.

| Finding | Milestone | Status | Evidence / resolution |
|---|---|---|---|
| M0-R1 | M0 | fixed | Schema v2 smoke requires an executable helper, unsupported required seams fail checks, and negative harness tests cover omission/non-executable helpers. |
| M0-R2 | M0 | fixed | Benchmark children now receive a fixed minimal environment; tests prove provider/base-URL/database/loader/askpass/Kerberos/Docker/Python variables do not survive; docs explicitly deny OS network containment. |
| M0-R3 | M0 | fixed | `backend-current-state.md` now records that plugin/MCP normal shutdown reaps only the leader and can leave a same-group descendant. |
| M0-R4 | M0 | fixed | Schema v2 retains the complete memory-helper output and every RSS snapshot, aggregates per-run maxima, and measures Linux current RSS delta with peak details separate. |
| M0-R5 | M0 | fixed | Schema v2 records SHA-256/size/mode/mtime for all artifacts, source commit/tree/dirty state, CMake/compiler/feature metadata, and explicitly unverified provenance. |
| M0-R6 | M0 | fixed | Native registry timing now performs exact lookup of the final built-in and emits `ns_per_lookup`; catalog timing is labeled composite. |
| M0-R7 | M0 | fixed | M0 rows now identify final files, tests, benchmark evidence, commits, and calibrated residual risks. |
| M0-D1 | M0 | fixed | A full-suite-only `ESRCH` was traced to `/proc/<pid>/stat` disappearing between open and read in `parallel_test_runner_test.py`; the test now accepts only `ENOENT`/`ESRCH` as successful process disappearance. The focused test passed 100 consecutive runs and the full suite passed afterward. |
| M1-GATE-001 | M1 | fixed | [Foreground editor job control](process-supervisor-adr.md#m1-gate-001) is accepted: verified private group, RAII foreground-terminal transfer/restoration, bounded unsupported suspension, and real PTY gates. |
| M1-GATE-002 | M1 | fixed | [Environment and credential inheritance](process-supervisor-adr.md#m1-gate-002) is accepted: exact profiles, ambient proxy/CA only for curl, explicit MCP env authority, minimal plugin env, and positive/negative canary tests. |
| M1-GATE-003 | M1 | fixed | [Dependency enforcement](process-supervisor-adr.md#m1-gate-003) is accepted: `process` is scanned, may depend only on core, consumers may depend on it, and the exception fixture remains empty. |
| M1-CAP-001 | M1 | fixed | Opaque anchored executable/cwd capabilities, physical-parent external `AnchorOpen`, descriptor exec, final pre-fork freshness, and mandatory secure-adoption cwd landed inert in `eb3f2690`. The severe filesystem/exec security review found no material issue; deterministic replacement, script, policy, content-redaction, and descriptor-hygiene tests pass. |
| M1-PLUG-001 | M1 | fixed | Runtime Plugin event hooks and model tools share the admitted run scope and guard cancellation; direct commands remain session-scoped and each child derives one operation. A SessionRunController-only cancellation test proves one Canceled/complete settlement and no later hook side effect. |
| M1-PLUG-002 | M1 | fixed | Plugin outbound records, individual inbound records, incomplete tails, aggregate queued stdout, queued record count, and retained stderr/stdout tails have hard bounds. Flood and oversized-frame tests settle as OutputLimit with complete byte/truncation accounting. |
| M1-PLUG-003 | M1 | fixed | Bare PATH, absolute, `./`, nested-relative, and parent-relative executable spellings retain compatibility: spawn resolves only slash-relative executables against the absolute plugin cwd while preserving manifest `argv[0]`. |
| M1-PLUG-004 | M1 | fixed | One absolute startup budget spans supervised exec and initialization. Parent cancellation is observed without a helper thread, while already-due execution/startup deadlines win in protocol, terminal snapshot, and returned error; gated children never exec after cancellation. |
| M1-PLUG-005 | M1 | fixed | Static scanners forbid raw fork/exec/pipe/dup/signal/wait authority and have negative self-tests; live-destructor, late-settlement, Linux descendant, authority isolation, and source-owned benchmark claim tests close migration-gate false negatives. |
| MCP-IO-01 | M1 | fixed | `d62fb825` maps only `EPIPE`/`ECONNRESET` to neutral pipe EOF, preserving MCP peer-close context and bounded stderr-byte metadata; closed-reader and initialized-notification regressions pass. |
| MCP-TEST-02 | M1 | fixed | `d62fb825` denial fixtures observe no new process records after permitted registry discovery and zero live records, rather than treating permitted discovery as forbidden dispatch. |
| MCP-IO-03 | M1 | fixed | `1e982024` adds owned-handle read-only committed-stop observation before MCP requests, I/O, waits, and buffered response acceptance. Gated owner-stop tests prove no later `tools/list`; complete natural-exit responses remain readable. |
| BENCH-MCP-01 | M1 | fixed | `b666309b` admits the already-adapted MCP benchmark driver to the supervised-family guard. Existing thresholds, provenance, managed-group cleanup, finished-record and single-settlement requirements remain unchanged. |
| BENCH-001 | M1 | fixed | Comparison now requalifies live recorded worktrees, commits/trees/cleanliness, exact runtime references, regenerated non-overlapping family/shared ownership scopes, authority source, harness script, AVA/helper build roots, and common fixtures. Malformed, missing, hostile-path, or stale provenance returns bounded structured unsupported evidence rather than throwing or measuring. |
| BENCH-002 | M1 | fixed | Plugin uses immediate reviewed before `13fb0cef` and after `2a30f40e`; Curl is supervised in both, so exactly one Plugin authority/scope transition is measured. Multiple or reverse transitions are rejected. |
| BENCH-003 | M1 | fixed | Source attribution uses complete recursive Plugin/MCP/LSP scopes, deterministic Curl/Bash pathspec scopes, and a separate complete shared process-plumbing scope. Independent AVA/helper CMake provenance must bind both binaries to the measured root and source-owned authority map. |
| BENCH-004 | M1 | fixed | Additive split-harness/helper/scope provenance remains standalone-compatible with historical schema v3; legacy v3 comparison returns a specific unsupported reason, while partial new provenance is invalid. |
| UPSTREAM-SEC-001 | Integration | fixed | Static audit found no embedded malware, binary payload, obfuscation, persistence, credential exfiltration, or unexplained network behavior in `c94ac8631419..3924ef03` or the exact aicxx/utils/memory pins. The mutable maintainer-bootstrap path found during that audit was removed; `.gitmodules` is checkout-only and `autogen.sh` initializes exact gitlinks and rejects `+`/`U` states. |
| UPSTREAM-SEC-002 | Integration | fixed | Project-primary provenance is typed, effective untrusted transitions are serialized across one manager/workspace, active runs/appends/jobs/deliveries block persistence, old controllers retire irreversibly, stale capsules are purged, and session construction/refresh holds a navigation reservation from before trust resolution through publication. Deterministic both-order races and the severe security re-review passed. Separate AVA processes/managers observe persisted trust only at their own open/reload boundary. |
| UPSTREAM-DEP-001 | Integration | fixed | The new pinned MIT `memory` source was reviewed as a compiled production dependency, attributed in `THIRD_PARTY_NOTICES.md`, and included in release provenance. Its application pool is currently constructed but unused; no performance improvement is claimed. |

## Prioritized upstream integration evidence

- Audited top-level range: `c94ac863141975806bbab52e950a2f2499108b65..3924ef03a81ae991f5116c9321b05bc7d2f016b4` (24 commits, 89 net-changed files).
- Audited dependency changes: aicxx `411eae31..15c31e10`, utils `5ed11a17..07c67a53`, and new memory pin `4ddb41469f323e32c170637aa413a132e83727be`. All commits were unsigned; unsigned status was recorded as provenance, not treated as evidence of malware.
- Integration shape: `3924ef03` is the exact ancestor of the replayed modernization chain. Range comparison found one manual conflict in `tests/CMakeLists.txt`; its union retains Carlo's process-gate/timing tests and the modernization benchmark/process registrations.
- Integrated BetaTest build passed after regenerating stale build-tree print-member output. Focused integration/security/process tests passed, four concurrency suites passed 20 repetitions, and the complete 164-test CTest run passed with only documented opt-in skips.
- Focused ASan/UBSan passed 9/9 for runtime authority and process/Curl/clipboard paths. Focused TSan passed 4/4 for the session controller, subagent coordinator/delivery manager, and runtime trust races.
- `ava_benchmark.smoke` and `ava_benchmark.process_smoke` passed. These are reliability checks, not latency or memory improvement claims; family performance evidence uses immediate reviewed migration pairs rather than reinterpreting the historical M0 source.

## Current M1 capability evidence

- `eb3f2690` adds move-only, native-handle-free executable/cwd capabilities and keeps them inert for production: Curl and clipboard remain path launches, while LSP/Bash/Mermaid consume nothing yet.
- Linux descriptor execution uses typed `execveat(AT_EMPTY_PATH)` framing; the conservative POSIX `fexecve` branch is compiled by contract but has not run on a separate POSIX host. Windows remains unsupported.
- `ava_tests.process_capability_posix` passed 20 consecutive runs; focused process/core/tool/containment and both benchmark smoke suites passed.
- The full BetaTest suite passed 165/165 with documented opt-in skips. Focused ASan/UBSan passed 4/4 and the TSan capability suite passed.

## Current M1 Plugin migration evidence

- Commits `46deea54`, `7f8dcdc4`, `25ed6a21`, and `2a30f40e` transfer one-shot Plugin launch, exact environment, pipe I/O, cancellation, group cleanup, and reaping exclusively to Supervisor authority. Persistent workers/restart/state remain M2 work.
- The full BetaTest suite passed 168/168 with documented opt-in skips. Plugin/process/runtime suites passed five repeated waves; focused ASan/UBSan passed 7/7 and focused TSan passed 6/6. Both benchmark smokes and the static scanner plus negative self-test pass.
- The pinned harness is `9332a106`; both five-run Release pairs use measured before `13fb0cef` and after `2a30f40e`, identical harness/fixture bytes, clean source-bound AVA/helper builds, exact Plugin-only family attribution, and opposite collection orders.
- Forward raw evidence: [before](process-plugin-forward-before-2026-09-02.json), [after](process-plugin-forward-after-2026-09-02.json), and [comparison](process-plugin-forward-comparison-2026-09-02.json). Plugin median changed from 11.291063 ms to 24.247045 ms: +12.955982 ms (+114.745%).
- Reversed-order raw evidence: [before](process-plugin-reverse-before-2026-09-02.json), [after](process-plugin-reverse-after-2026-09-02.json), and [comparison](process-plugin-reverse-comparison-2026-09-02.json). Plugin median changed from 11.355040 ms to 24.320772 ms: +12.965732 ms (+114.185%).
- Both orderings therefore reproduce the non-gating latency investigation trigger. Investigation traced the changed lifecycle boundary to Supervisor gate/exec confirmation and two quiet-group proofs before exact reap; Linux `verified_group_has_live_member` scans procfs for same-group survivors. Tracing perturbs timings, so no percentage is assigned to an individual mechanism. The safety-preserving one-shot cost is recorded rather than optimized speculatively; M2 persistent workers are expected to amortize process lifecycle work but have no measured claim yet.

## MCP and headless integration evidence — 2026-09-30

- Validated implementation snapshot: `cd27fa9e474287cc6b72f84b5805f9ee8ecce0ff`,
  after 18 local backend commits above reviewed incoming `102a99da`. Carlo's
  incoming 11 commits and 23 changed paths, including executable build/test code
  and the exact utils gitlink, were audited before the local fast-forward. No
  malware indicators were identified; this is not proof of malware absence.
- This increment implements MCP Supervisor migration and follow-up environment,
  consent, EOF, deadline, and committed-stop fixes; issue #79 single submission
  resolution with passive recognition and typed cancellation; issue #82 private
  owned fixture isolation; and explicit default-off print/RPC read/search
  scheduling with worker bounds and no child-loop inheritance. It does not
  complete M1 or the broader backend backlog.
- The single integrated Sol-high security gate reviewed committed
  `102a99da..cd27fa9e` under the agreed operator/project trust model and found no
  material security issue. It was a source-level gate, not an independent runtime
  test run. Trusted MCP grants authorize evolving unsandboxed server/script
  paths, not immutable script bytes; unkeyed composition hashes are not encryption.
- Rebuilt developer (`BetaTest`, debug enabled), Release, and ASan/UBSan trees.
  Full CTest results were respectively **154 passed / 29 skipped / 0 failed**,
  **152 passed / 29 skipped / 0 failed**, and **156 passed / 29 skipped / 0 failed**.
  Skips were the opt-in live-provider, real-clangd, and terminal/tmux smokes; they
  are not passes. The Release SDK interop test initially could not start because
  of a removed cached Node executable. Updating only `AVA_NODE_EXECUTABLE` to the
  installed Node path made the focused test and full suite pass.
- Reproduction commands, after configuring the documented build trees:

  ```sh
  export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
  scripts/build.sh --build-dir build --jobs 4 --target generate-print-members
  scripts/build.sh --build-dir build --jobs 4
  scripts/run-tests.sh --build-dir build --jobs 4
  scripts/build.sh --build-dir build-release --jobs 4
  scripts/run-tests.sh --build-dir build-release --jobs 4
  scripts/build.sh --build-dir build-sanitize --jobs 2 --target generate-print-members
  scripts/build.sh --build-dir build-sanitize --jobs 2
  scripts/run-tests.sh --build-dir build-sanitize --jobs 2
  scripts/run-tests.sh --build-dir build --jobs 4 --repeat until-fail:3 \
    -R '^ava_tests\.(mcp|app_command_registry|app_interactive|process_supervisor_posix|agent_loop|acp|app_rpc)$|^ava_cli\.(headless_e2e_model_smoke|headless_parallel_read_search_tools)$'
  ```

- All nine critical tests above passed three consecutive executions (27 passes).
  The #82 headless smoke additionally passed ten consecutive executions while an
  owned concurrent harness created/removed 99,551 unpredictable empty directories
  directly beneath the former build-tree fixture ancestor. No churn errors or
  owned directories remained. This is ancestor-churn evidence, not a claim that
  the original intermittent failure was reproduced before the fix.
- All 21 process benchmark smoke checks passed in all three trees. MCP was
  measured as `supervised`, with `managed_group` cleanup, a finished Supervisor
  record, and one settlement. LSP and Bash remain `legacy_local`. Binary source
  commit embedding is explicitly unverified; artifact/source/cache identity is
  best-effort provenance, not clean-release qualification. No MCP speedup is claimed.
- Local command logs are `build/ava-backend-final-dev.log`,
  `build/ava-backend-final-repeat.log`, `build/ava-backend-final-churn.log`,
  `build-release/ava-backend-final-release.log`, and
  `build-sanitize/ava-backend-final-sanitize.log`; these build artifacts are not
  committed evidence fixtures. Process smoke JSON/Markdown remain in each tree.
- Changed ranges in 45 C++ files pass clang-format 22.1.3. Whole-file checks found
  only preexisting untouched violations in `tests/app_rpc_commands_tests.cpp` and
  `tests/command_tests.cpp`; unrelated formatting was preserved. Whitespace gates
  pass. Clang-tidy is unavailable on PATH and was not run; no TSan rerun or new
  platform qualification is claimed.
- Remaining in-scope tracks are LSP/Bash supervision prerequisites, bounded
  session reads/catalog pagination, selective tool advertising/discovery, and
  typed settings/reload research and approval. No storage migration, persistent
  plugin worker, broader scheduler, SDK, or native-plugin completion is implied.
  All integration commits are local; nothing was pushed and unrelated desktop/docs
  working-tree changes were left untouched.

## M0 validation evidence

- `cmake --preset release` and the Release `ava`, benchmark-helper, and fake-provider
  targets built successfully.
- `cmake --preset dev` and the complete BetaTest build succeeded.
- Release and BetaTest `ava_benchmark.harness_self_test` and
  `ava_benchmark.smoke` passed.
- The schema-v2 baseline completed five runs for every currently measurable family;
  the committed JSON passed the harness schema/identity validator.
- `ctest --repeat until-fail:100 -R '^ava_build\.parallel_test_runner$'` passed
  after the procfs-disappearance race fix.
- The complete BetaTest CTest run passed all 147 registered tests with zero failures;
  credential/live-provider and opt-in terminal tests reported their expected skips.
- Source Markdown links, documentation structure, Python compilation, C++ formatting,
  and `git diff --check` passed.
- No production runtime source or production dependency changed in M0.

## Commit ledger

| Milestone | Commit | Scope |
|---|---|---|
| M0 | `3d4349ef`, `8fab13fe`, `971327fb`, `6999441f` | Current-state map, benchmark harness/smoke, review fixes, baseline evidence, and reliability-test fix |
| M1 | `09312277`–`dcf9a569`, `eb3f2690`, `d33c1be5`, `a6e6465c`, `f135c476`, `ffda314f`, `93d991c7`, `46deea54`, `7f8dcdc4`, `25ed6a21`, `2a30f40e` | Supervisor foundation, inert anchored capabilities, and Curl/Plugin/clipboard authority migrations; six production spawn families remain |
| M1 benchmark | `223cf75d`–`45a41ad2`; pinned harness `9332a106` | Fail-closed split source/harness provenance, AVA/helper build binding, complete family/shared attribution, forward/reverse Plugin Release evidence |
| Integration | `ac29b0ef`, `bb99f5c4`, `544898be`, `7fc1c201`, `6ff6499e` | Pinned dependency bootstrap/notice and process-local workspace trust-revocation linearization above prioritized `3924ef03` |
| M2 | pending | Persistent plugin workers |
| M3 | pending | Selective tool catalog and router |
| M4 | pending | Bounded sessions, index, payloads, receipts |
| M5 | pending | Resource-aware durable scheduler |
| M6 | pending | Multi-language plugin SDKs |
| M7 | pending | Experimental trusted-native interface |
| M8 | pending | Fault injection, hardening, performance gates |
| M9 | pending | Documentation, migration, release readiness |
