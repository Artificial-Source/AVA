# AVA Release Readiness

This is the single current cut line for AVA's first official release. It records the frozen decision and actionable ledger; dated audit detail is preserved in the [2026-08-23 audit history](../history/2026-08-23-release-readiness-audit.md). Runtime version `1.0.0` is **not** a published release, and this audit creates no tag, release, or official artifact.

> **Integration status:** the audit scope and the isolated `AVA-REL-001`, `AVA-REL-002`, `AVA-REL-003`, and `AVA-REL-014` fixes are integrated on `develop`. The sanitizer policy is applied centrally to `ava_terminal`; no terminal frontend source was changed for that integration. GitHub Actions run `32795413575` passed every required job at commit `546fa0aa7a9faece8adb9937796464b01a5d86f9`. This is still not an exact retained release candidate.

## Candidate decision

| Field | Frozen value |
| --- | --- |
| Candidate version | `1.0.0` |
| Baseline commit | `c43794e91bb8d4f706ad4c916387f5487fca14ee` |
| Audit integration state | Safety, documentation, package-policy, and CI fixes are integrated through `546fa0aa7a9faece8adb9937796464b01a5d86f9`; exact retained candidate bytes are still required |
| Freeze date | 2026-08-23 |
| Verdict | **READY AFTER LISTED BLOCKERS** |
| Open release blockers | `AVA-REL-011`, then `AVA-REL-013`, then `AVA-REL-012` as ordered below |
| First-publication target | Linux x86-64 only; no other architecture may publish without native exact-candidate evidence |
| Publication state | No official release or tag exists; none is created by this audit |

The comparison audit used fetched pins Pi.dev `460191cfcf27d60ff81fc0178812f4ff09e8df06` (`0.84.2`), OpenCode `03bba464d46f3eddf74195919b1344aa937f7b11` (`1.18.21`), and Grok Build `07b2f7144fd5c5c9d3dd1966937a87852d2dbdb8` (core `1.0.8`, npm wrapper `0.1.220-alpha.4`). The repository's separately preserved comparison checkouts remain at Pi.dev `936aff00918de1187f085f123c2812d8f2d67745`, OpenCode `38e10eb1408feb700021b8e8766fb0ab41bf84e2`, and Grok Build `8a14c91d88875a831a38b3a066b1683116bcb31c`; they were not moved to perform the audit.

## Integration status

| Item | Current status |
| --- | --- |
| Scope, principles, audit history, and publication design | Integrated on `develop` |
| `AVA-REL-001` persistent deny precedence | Integrated as an isolated backend unit; focused 6/6 and clean-series full 143/143 CTest passed |
| `AVA-REL-002` truncated provider tool calls | Integrated as an isolated backend unit; focused 4/4 and clean-series full 143/143 CTest passed |
| `AVA-REL-014` destructive dogfood roots | Integrated as an isolated script/test unit; direct harness, focused 1/1, and clean-series full 143/143 CTest passed |
| `AVA-REL-003` canonical sanitizer gate | Integrated centrally without terminal source edits; clean local 146/146 and GitHub sanitizer jobs passed |
| CI | Run `32795413575` passed Debug, Release, sanitizer, focused TSan, ACP SDK interoperability, and native AArch64 build/test/static-package jobs |
| Publication | Commits were pushed to `develop`; no tag, official package, or release was created |

## Frozen scope and admission rule

The frozen P0 scope is `AVA-REL-001` and `AVA-REL-002`; both are fixed on `develop`. The frozen P1 scope contains integrated `AVA-REL-003` and `AVA-REL-014`, plus open `AVA-REL-011`, `AVA-REL-012`, and `AVA-REL-013`. Documentation closure `AVA-REL-015` is a completed P2 audit deliverable, not a release blocker. `AVA-REL-010` is not a separate blocker: its exact-candidate terminal gate is merged into `AVA-REL-011`. All former release candidates `AVA-REL-004` through `AVA-REL-009` remain downgraded to P2 after independent scope review.

After this freeze, a new P0/P1 requires all of: a reproducible impact on the narrow Linux x86-64 first release, exact AVA source/runtime evidence, measurable acceptance, and independent technical and scope approval. Competitor parity, breadth, cleanup value, or an untested concern is insufficient. Existing downgrades stay downgraded unless new evidence satisfies the same rule.

## Dependency-ordered milestones

| Milestone | Status | Exit condition |
| --- | --- | --- |
| M0 — Documentation audit closure | Completed and validated in this consolidation | `AVA-REL-015` current docs, history, navigation, package inventory, and checks agree |
| M1 — Exact-byte and support qualification | Open | `AVA-REL-011` and `AVA-REL-013` close for the same retained Linux x64 candidate bytes |
| M2 — Publication | Blocked by M1 | `AVA-REL-012` closes through reviewed, no-clobber official publication and post-publication verification |
| M3 — Post-1.0 work | Deferred | P2/P3 items receive separate approved goals; none blocks M1 or M2 |

## P0, completed P1, and audit-deliverable ledger

### AVA-REL-001 — Persistent deny precedence

- **Tier/status/owner:** P0; integrated on `develop`; Andrés.
- **Subsystem:** permissions and file/search tools.
- **Exact evidence:** pre-fix whole-process reproductions are `$AUDIT_ROOT/logs/p0-reproductions/ava-br-001-{valid,malformed}-summary.json`; fixed-tree reruns are the matching `ava-br-001-fixed-*` summaries. The fix and adversarial cases are in `src/ava/permissions/permission_rule_{matching,path}.cpp`, `src/ava/tools/file_tools.*`, `src/ava/tools/search_tools.cpp`, and `tests/permission_rules_tests.cpp`. The tests cover dot/trailing spellings plus contained intermediate/final symlinks, hardlinks, and missing targets while keeping persistent Allows lexical-only. `ava_tests.permission_rules` and adjacent tool/ACP/agent cases passed inside the final integrated runs; `$AUDIT_ROOT/logs/final-after-review-normal/tests.log` records 143/143 PASS and `$AUDIT_ROOT/logs/final-after-review-sanitize/tests.log` records 146/146 PASS, each with 30 declared expected skips.
- **Impact:** a durable operator deny could otherwise allow workspace content acquisition, provider transmission, or mutation through policy auto-Allow or an equivalent path alias.
- **Measurable acceptance:** exact denies block `read_file`, `list_directory`, `glob`, `grep`, write, and edit before content access or mutation across equivalent lexical/physical aliases; denied per-file search results and write previews do not leak names, bytes, or diffs; no fallback prompt runs; audits identify `persistent_rule` and the rule ID; malformed protected global/workspace rule storage and required deny-identity failures fail closed; persistent Allows are not broadened across aliases.
- **Verification:** `ava_tests.permission_rules`, tool/permission regression assertions, normal full CTest, and sanitizer full CTest.
- **Dependencies/size/milestone:** none; S; M0 completed safety input.
- **Intentional difference:** AVA retains operation permissions and durable deny precedence rather than adopting permissionless or broadly ambient reference behavior.

### AVA-REL-002 — Truncated provider tool calls never execute

- **Tier/status/owner:** P0; integrated on `develop`; Andrés.
- **Subsystem:** provider normalization, agent loop, and tool dispatch.
- **Exact evidence:** `tests/agent_loop_resilience_tests.cpp` covers buffered and streaming truncation for OpenAI Responses, OpenAI-compatible Chat Completions, Anthropic Messages, and Gemini GenerateContent. It proves zero permission/preflight calls, timeline events, registry dispatches, and side effects while binding bounded `provider_output_truncated` results for continuation. `ava_tests.agent_loop_resilience` passed inside the final integrated normal 143/143, sanitizer 146/146, and Release 141/141 runs under `$AUDIT_ROOT/logs/final-after-review-{normal,sanitize,release}/`.
- **Impact:** valid-looking but incomplete destructive arguments could otherwise execute after a provider length limit.
- **Measurable acceptance:** all four families, buffered and streamed, convert every `MaxTokens`/length tool call to a bounded non-retryable error result; session replay validates; continuation includes exact call IDs/results; tool-call count remains zero; unknown incomplete outcomes fail before persistence or dispatch.
- **Verification:** `ava_tests.agent_loop_resilience`, all provider-family suites, full normal CTest, and full sanitizer CTest.
- **Dependencies/size/milestone:** none; M; M0 completed safety input.
- **Intentional difference:** Pi.dev supplied a useful test vector, but AVA's fail-closed incomplete-output rule is the authority; this is not parity work.

### AVA-REL-003 — Canonical sanitizer gate

- **Tier/status/owner:** P1; integrated on `develop`; Andrés.
- **Subsystem:** CMake, CI, sanitizer tests, terminal library, and death-test handling.
- **Exact evidence:** the initial fresh sanitizer default build failed on uninstrumented consumers and exposed libcwd/death-test instability. The integrated change makes the `sanitize` preset/CI path canonical BetaTest plus debug instrumentation, makes UBSan non-recovering, propagates sanitizer runtime from static libraries, instruments `ava_terminal` centrally, adds static-consumer/UB/command contract tests, and disables core dumps in intentional abort children. A clean local run passed 146/146, and GitHub Actions run `32795413575` passed its sanitizer job. `$AUDIT_ROOT/logs/final-after-review-sanitize/tests.log` preserves the audit's 146/146 closure evidence.
- **Impact:** a partial or recoverable sanitizer configuration could report success while production terminal objects or violations escaped the gate.
- **Measurable acceptance:** fresh default sanitizer configure/build succeeds; compile/link commands instrument all first-party production targets; the UB fixture must fail internally and therefore pass its `WILL_FAIL` CTest; full sanitizer CTest passes with only declared skips.
- **Verification:** `cmake --preset sanitize`, complete locked build/test at two jobs, sanitizer contract tests, compile-command inspection, and repeat of core-mode/libcwd-sensitive tests.
- **Dependencies/size/milestone:** none; M; M0 completed safety input.
- **Intentional difference:** one AVA-owned canonical configuration is preferred over carrying nominally equivalent but materially different sanitizer recipes.

### AVA-REL-014 — Destructive live-dogfood roots

- **Tier/status/owner:** P1; integrated on `develop`; Andrés.
- **Subsystem:** live-provider dogfood launchers.
- **Exact evidence:** `scripts/live-model-dogfood.sh`, `scripts/live-coding-dogfood.sh`, and `scripts/live-provider-matrix.sh` now treat overrides as validated private parents, allocate mode-0700 invocation-owned unpredictable children, and remove only those children. `tests/live_dogfood_launcher_test.py` exercises canary, symlink, root/home/checkout overlap, mode, ownership, cleanup, retention, and matrix cases; its direct invocation and registered `ava_tests.live_dogfood_launchers` CTest passed during consolidation.
- **Impact:** the old override contract could recursively delete a caller-supplied path after a typo or hostile value.
- **Measurable acceptance:** no caller-supplied parent or pre-existing neighbor is recursively removed; `/`, home, checkout/descendants, symlinks, and wrong owner/mode are rejected; exactly the invocation-created child is cleaned or retained with private modes.
- **Verification:** `python3 tests/live_dogfood_launcher_test.py --source .`, shell syntax checks for all edited launchers, and its registered CTest.
- **Dependencies/size/milestone:** none; S; M0 completed safety input.
- **Intentional difference:** live evidence stays opt-in and classified; convenience never weakens local filesystem authority.

### AVA-REL-015 — One truthful documentation spine

- **Tier/status/owner:** P2 audit deliverable; completed by this consolidation; shared.
- **Subsystem:** documentation, build guidance, package inventory, and release operations.
- **Exact evidence:** this ledger and [principles](principles.md) are canonical; the dated audit is under history; publication has a current runbook; the completed TUI plan moved to history and left the runtime artifact; current build/platform/package statements and closed goals were reconciled. Direct source links verified 128 files; structure verified 108/108 reachable documents; four focused documentation CTests passed 4/4; Release provenance/install/package CTests passed 3/3; the live-dogfood CTest and direct harness passed; Python/JSON/shell syntax, assertion comments, exact 32-document inventory, old-path/stale-command searches, C++ formatting, Release build, focused TUI CTest, and final `git diff --check` passed.
- **Impact:** contradictory current docs could direct unsafe builds, advertise unsupported artifacts, revive closed parity work, or ship history as runtime guidance.
- **Measurable acceptance:** every current page links to one principles/cut authority; runtime `1.0.0` is not called published; package payload is exactly 32 documents (30 Markdown, 2 JSON) and artifact index matches; all required gates pass.
- **Verification:** exact gates named above.
- **Dependencies/size/milestone:** fixed code/test work remains authoritative; M; M0.
- **Intentional difference:** history and comparison evidence remain preserved but cannot become current product authority.

## Open P1 blockers

### AVA-REL-011 — Qualify and retain the exact release bytes

- **Tier/status/owner:** P1; open blocker; shared.
- **Subsystem:** CI, native terminal qualification, artifact retention, and package promotion.
- **Exact evidence:** exact-baseline GitHub run `32653859737` failed before job creation because `runner.temp` was invalid in job-level workflow environment expressions. That parser defect and subsequent GCC 16, AArch64 BMI2, sanitizer, and architecture-specific loader-policy failures were fixed. Run `32795413575` then passed Debug, Release, sanitizer, focused TSan, ACP SDK interoperability, and native AArch64 build/test/static-package jobs. Local strict x64 packaging and 23 tmux plus four PTY gates also passed, but CI still rebuilds for packaging, does not run the 27 terminal gates on the exact packaged binary, and retains no promotable archive/checksum pair. See `$AUDIT_ROOT/logs/{dev-gcc,release-gcc-ninja,strict-package,tmux,pty}/`.
- **Impact:** rebuilding after tests can publish bytes that CI never tested, while non-retained logs/artifacts cannot be independently promoted or verified.
- **Measurable acceptance:** a successful exact-candidate native Linux x64 CI run builds the final binary once; that binary receives full deterministic CTest, canonical sanitizer evidence, focused TSan, all exact candidate 23 tmux and four PTY gates with zero skips, install/provenance/package checks, checksum and extracted CLI/fake-provider smoke; the unchanged archive/checksum pair is retained with explicit retention and recorded digests; promotion re-verifies those digests and never rebuilds. No architecture without equivalent native evidence is emitted or advertised.
- **Verification:** retained workflow/run IDs, job conclusions, test reports, terminal evidence, package manifest, archive/checksum digests before and after promotion, and extracted `bin/ava --version` from the retained archive.
- **Dependencies/size/milestone:** fixed `AVA-REL-001/002/003/014/015`; L; M1.
- **Intentional difference:** AVA keeps deep package/headless/terminal gates rather than reducing qualification to a version smoke or competitor release pattern.

### AVA-REL-013 — Truthful supported artifact floor

- **Tier/status/owner:** P1; open blocker; shared.
- **Subsystem:** build support, runtime portability, architecture, and documentation.
- **Exact evidence:** Ubuntu 24.04.4 x86-64 with GCC 13.3 passed BetaTest/Unix Makefiles and Release/Ninja full runs. GitHub run `32795413575` added successful GCC 16 Debug/Release and native AArch64 Release build/test/static-package evidence; AArch64 remains outside the first-publication target because it has no exact retained-byte terminal/minimum-host qualification. Clang 18 was BLOCKED by scanner/default GCC 16 and Clang/libstdc++ C++23 `std::expected` incompatibilities. MSVC, macOS, and Windows were not qualified. The audited x64 binary contains BMI2 instructions and requires symbol floors `GLIBC_2.38`, `GLIBCXX_3.4.32`, and `CXXABI_1.3.13`, plus `libncursesw.so.6`, `libtinfo.so.6`, and `curl`; CMake timeout properties require CMake 3.27. Multi-config is not release-qualified.
- **Impact:** users can receive an artifact that will not start on an older CPU/runtime or infer support from source branches that were never natively tested.
- **Measurable acceptance:** first-publication docs and release body name Linux x64 only; exact retained bytes are inspected for ISA, symbol versions, and dynamic dependencies; extracted smoke passes on the chosen minimum supported host/CPU profile; CMake minimum is 3.27 everywhere; tested/best-effort/unsupported matrices agree; every future architecture has independent native exact-candidate evidence before publication.
- **Verification:** `file`, `readelf`, `objdump`, `ldd`/loader inspection without broadening dependency claims; native minimum-host extraction, checksum, `--version`, `--help`, `doctor`, and fake-provider smoke; documentation gates.
- **Dependencies/size/milestone:** `AVA-REL-011` exact bytes; M; M1.
- **Intentional difference:** support is evidence-driven, not inferred from portable-looking CMake conditionals or another project's matrix.

### AVA-REL-012 — Official publication lifecycle and reviewed 1.0 notes

- **Tier/status/owner:** P1; open blocker; shared.
- **Subsystem:** release operations, changelog/release notes, publication, rollback, and withdrawal.
- **Exact evidence:** no `v1.0.0` tag or official release exists; `CHANGELOG.md` has no reviewed 1.0 release section; current automation ends at local no-clobber package publication. The required but unimplemented design is [publication.md](../operations/publication.md).
- **Impact:** an ad hoc first release can publish the wrong commit or bytes, overwrite assets, omit limitations, or leave no correction/withdrawal procedure.
- **Measurable acceptance:** owners review the exact version/commit, 1.0 changelog and release body, supported floor, known limits, checksums, approval record, immutable correction policy, post-publication checks, rollback/withdrawal decision tree, and supported-version policy; a no-clobber draft dry-run uses the retained `AVA-REL-011` assets; final tag/commit equality and remote assets are verified. This audit itself creates no tag or release.
- **Verification:** reviewed release packet, dry-run record, approvals, remote tag/commit/API checks, downloaded asset hashes, extracted smoke, clean-install smoke, and withdrawal/patch drill evidence.
- **Dependencies/size/milestone:** `AVA-REL-011` and `AVA-REL-013`; L; M2.
- **Intentional difference:** corrections use a new patch release; published version/tag/assets are never silently replaced.

### AVA-REL-010 — Terminal qualification consolidation

- **Tier/status/owner:** P1; scope merged into `AVA-REL-011`, no separate open blocker; shared.
- **Subsystem:** TUI terminal release evidence.
- **Exact evidence:** audit evidence was effectively 23/23 tmux after one AF_UNIX long-path environment rerun and 4/4 PTY; ordinary CI could skip all 27. The frozen review moved exact-candidate recurring execution to `AVA-REL-011` and moved long socket-root robustness to P2 `AVA-REL-106`.
- **Impact:** keeping a second blocker would duplicate acceptance and obscure which exact bytes must pass.
- **Measurable acceptance/verification:** satisfied only through `AVA-REL-011`'s exact-candidate zero-skip 23+4 gate and retained reports.
- **Dependencies/size/milestone:** `AVA-REL-011`; S consolidation; M1.
- **Intentional difference:** renderer tests remain necessary but do not substitute for the bounded real-terminal wave.

## P2 ledger — post-release hardening

Every P2 row is non-blocking for the frozen cut. Procedures are acceptance plus verification; any scope change requires a separate approved goal.

| ID | Status; owner | Subsystem | Exact evidence | Impact | Measurable acceptance and verification | Dependencies | Size | Milestone | Intentional-difference note |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `AVA-REL-004` | Deferred P2; Andrés | Session attachments/branches | `attachments.cpp` and `session_branch.cpp` validate names then reopen with streams; tests cover pre-existing symlinks, not replacement seams | Same-UID replacement can redirect attachment reads/truncation | Use held descriptor-relative no-follow opens for import, replay, destination and branch copy; replacement seams preserve outside canaries; run attachment/branch/import/export suites, full CTest, sanitizer | None | M | M3 | Preserve AVA descriptor authority; no reference storage rewrite |
| `AVA-REL-005` | Deferred P2; Andrés | Ordinary session reads | TUI/print/RPC null limits select `legacy_unbounded_session_read_limits()`; ACP already has finite limits | Large valid/corrupt sessions can exhaust memory or latency | Set finite byte/line/entry defaults before materialization/provider use; bounded remediation and explicit finite compatibility override; verify startup/session/RPC/print/agent suites and full CTest | Session compatibility decision | M | M3 | No requirement to adopt competitor pagination/database architecture |
| `AVA-REL-006` | Deferred P2; Andrés | Provider parsers | OpenAI Responses enforces shared parser budgets; Anthropic/Gemini/compatible paths lack equivalent cumulative checks; transport still caps 8 MiB | Family-specific expansion can violate the advertised 256 KiB parser seam | Enforce record/event/array/fragment budgets terminally in every family; oversized/fragmented tests in all provider suites, full CTest and sanitizer | None | M | M3 | Contract parity across AVA providers, not provider-count parity |
| `AVA-REL-007` | Deferred P2; Andrés | HTTP streaming retries | Curl can deliver non-empty error bodies before status; retry suppression sees delivered bytes; only empty-429 test exists | Ordinary 429/503 bodies suppress safe retry | Buffer/gate failed-attempt bodies; non-empty 429/503 then success retries, bytes never reach parser/frontend, accepted partial 2xx remains non-retryable; HTTP/provider/fake-E2E/full tests | Transport design | M | M3 | Keep AVA retry settlement semantics rather than copying a router |
| `AVA-REL-008` | Deferred P2; Andrés | XDG/trust/auth/session roots | Bounded audit found caller-controlled XDG overlap, not a default repository-controlled escalation | Misconfiguration can place authority state inside a workspace | Descriptor-check physical disjointness before authority load; ancestor/descendant/symlink tests fail closed with canaries; config/trust/runtime/full tests | Root migration UX | M | M3 | Project trust still never replaces operation permission |
| `AVA-REL-009` | Deferred P2; Carlo | Backend session grants | Reusable grant semantics live in `tui/session_grants.*`; TUI consumes that state; RPC has separate state; no bypass reproduced | Frontends can diverge and TUI becomes policy owner | Move eligibility/key/cap/lifecycle to backend permissions; TUI/RPC use one narrow resolver; run permission/RPC/TUI PTY/full tests | None | M | M3 | Preserve stronger AVA permission model; do not adopt ambient grants |
| `AVA-REL-101` | Deferred P2; shared | Static analysis | `.clang-tidy` is strict, but clang-tidy/analyzer tools and whole-project CI evidence were absent | Untested safety branches and diagnostics can regress | Pin toolchain, generate fresh compile DB, run safety-sensitive first-party TUs with zero permanent noise, retain report; verify config and negative harness | Build matrix | M | M3 | Actionable baseline, not warning-count theater |
| `AVA-REL-102` | Deferred P2; Andrés | Parser fuzzing | No first-party fuzz targets/corpora for strict JSON, sessions, RPC/ACP, SSE, rules, paths, manifests | Parser edge cases rely on hand vectors | Add narrow side-effect-free libFuzzer targets/corpora, bounded ASan/UBSan smoke, retained minimized reproducers; verify each target rejects seeded faults | `AVA-REL-006` where shared budgets change | L | M3 | No broad dependency or architecture transplant |
| `AVA-REL-103` | Deferred P2; shared | Coverage | LLVM coverage tools/first-party reports unavailable during audit | Missing safety branches are hard to prioritize | Add Clang source coverage by subsystem/branch, publish missingness and safety gaps without arbitrary repository percentage; verify reproducible report generation | Static toolchain | M | M3 | Coverage guides tests; it is not a release score |
| `AVA-REL-104` | Deferred P2; Andrés | ThreadSanitizer | Focused TSan passed 4/4 coordinator/ACP tests; scheduler, titles, branch summaries, observer, RPC, Mermaid/TUI shutdown not covered | Shared-state races can escape the narrow set | Add prioritized focused cases and full instrumentation; each passes under TSan with finite cleanup; retain logs | Stable test runtimes | L | M3 | Focus on AVA ownership seams, not competitor suite counts |
| `AVA-REL-105` | Deferred P2; shared | Reproducibility/SBOM/signing | Two strict builds had identical extracted regular files and build IDs but different archive hashes from timestamps/metadata; no standard SBOM or detached signature | Mirrors and rebuild verification lack deterministic/authenticated artifacts | Normalize epoch/order/owner/mode/tar/gzip metadata; produce bit-identical archives twice; generate SPDX/CycloneDX SBOM; define detached signing/verification and advisory policy; verify hashes/SBOM/signature in clean environment | Exact-byte pipeline | L | M3 | GitHub attestation may complement, not replace, mirror policy |
| `AVA-REL-106` | Deferred P2; Carlo | tmux harness paths | One 108-byte socket path failed before AVA; isolated short-root rerun passed | Arbitrary long build roots cause false negatives | Allocate guarded short private socket paths while retaining evidence in scenario roots; pass 23/23 from deliberately long build path and prove cleanup | None | S | M3 | Harness robustness only; no TUI redesign |
| `AVA-REL-108` | Deferred P2; Andrés | Cross-build integration roots | Several integration tests use fixed `/tmp` roots and wrappers serialize only one build tree | Concurrent normal/sanitizer trees can collide | Per-invocation owner-validated private roots; simultaneous-tree stress passes with no process/path leaks | Test harness review | M | M3 | Keep deterministic tests without global mutable paths |
| `AVA-REL-109` | Deferred P2; Andrés | Top-level exception boundary | Sampled workers contain exceptions; no sanitized process-wide boundary/cleanup injection exists | Unexpected exceptions may skip terminal/resource cleanup | Assess and, if justified, add one sanitized boundary that preserves typed failures; injected terminal/process/session cleanup tests pass | Ownership review | M | M3 | Results remain normal control flow; no broad exception conversion |
| `AVA-REL-110` | Deferred P2; shared | C++ ownership policy | Policy bans raw ownership/manual `new`, while reviewed factory/shared ownership idioms need explicit rationale | Written policy and accepted code can drift | Inventory factories/shared ownership, choose and document narrow exceptions or refactor, add enforceable checker with zero unexplained hits | None | M | M3 | Apply AVA's needs; do not import guidelines wholesale |
| `AVA-REL-111` | Deferred P2; shared | Build-system cleanup | Multi-config is unqualified; experiment targets, generated print-member transitions, direct dependencies, in-source builds, and config selection have unresolved edges | Builds can select ambiguous outputs or require extra regeneration | Reject or fully support multi-config packaging; add in-source guard; make generated-source transitions one-build; tighten target dependencies; Ninja/Make positive and multi-config negative tests pass | CMake 3.27 floor | L | M3 | Single-config support may remain the deliberate product boundary |

## P3 ledger — research and future products

P3 rows are not implied commitments. Each requires product approval before implementation.

| ID | Status; owner | Subsystem | Exact evidence and impact | Measurable acceptance and verification | Dependencies | Size | Milestone | Intentional-difference note |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `AVA-REL-201` | Research; unassigned | Remote MCP/OAuth | Current MCP is bounded local stdio; remote auth/transport expands network and credential authority | Threat model, OAuth state/redirect rules, transport bounds and fake-server conformance before opt-in implementation | Security design | L | M3 | Local stdio remains valid without parity |
| `AVA-REL-202` | Research; unassigned | Richer LSP | Current installed/configured slice lacks unsaved-buffer, hover and implementation breadth | Approve capabilities, bounded document lifecycle and fake/real-server tests with no downloads | LSP ownership | L | M3 | No automatic server marketplace |
| `AVA-REL-203` | Research; unassigned | Session index | Strict append-only sessions have no derivative full-text index | Rebuildable non-authoritative index, corruption isolation, bounded query and session tests | Finite reads | L | M3 | Never weaken authoritative replay |
| `AVA-REL-204` | Research; unassigned | Durable jobs | Jobs are intentionally process-local; child JSONL persists | Define crash/restart ownership, reconciliation and cancellation semantics; deterministic restart tests | Session/job design | L | M3 | No broad task graph implied |
| `AVA-REL-205` | Research; unassigned | Structured plan review | Plan mode enforces backend write policy but has no approval/comment state machine | Backend-owned artifact/state covers tools, shell, extensions and children; adversarial tests before TUI | Permission design | L | M3 | Never add approval theater without authority |
| `AVA-REL-206` | Research; Carlo | TUI LaTeX | Markdown is bounded; LaTeX is excluded from first release | Approve literal parser/render fallback, bounds and terminal tests without arbitrary renderer authority | TUI design | M | M3 | Richer reference rendering is not a requirement |
| `AVA-REL-207` | Research; unassigned | Accessibility | Keyboard/plain TUI coverage is not screen-reader certification | Named assistive-technology matrix, task scripts, findings and remediations; retain truthful limits | Product research | L | M3 | Do not overclaim keyboard TUI coverage as assistive-technology certification |
| `AVA-REL-208` | Research; Andrés | MemorySanitizer | Not configured; official guidance requires instrumented dependency closure | Reproducible Clang/libc++ dependency closure and clean focused/full MSan evidence | Clang support | L | M3 | ASan/UBSan remains the current canonical gate |
| `AVA-REL-209` | Research; unassigned | Non-Linux ports | macOS/Windows/MSVC were not qualified; Linux-specific authority paths exist | Platform threat/support contract, native CI, tests, terminal and exact artifacts per OS/arch | Product approval | L | M3 | Portable-looking branches do not imply support |
| `AVA-REL-210` | Research; unassigned | Package manager/marketplace/self-update | Remote code and automatic downloads conflict with current provenance/trust boundary | Source identity, lifecycle scripts, signing, compatibility, rollback, permissions and offline tests before any enablement | Supply-chain design | L | M3 | Manual local resources remain supported |
| `AVA-REL-211` | Research; unassigned | HTTP/OpenAPI/SSE | Current automation is local stdio RPC/ACP | Authenticated bounded service contract, threat model, conformance, cancellation and shutdown tests | Protocol approval | L | M3 | No unauthenticated ambient daemon |
| `AVA-REL-212` | Research; unassigned | Web client | No production web surface exists | Separate approved client/API/accessibility/security design and end-to-end tests | `AVA-REL-211` | L | M3 | Terminal-first remains canonical |
| `AVA-REL-213` | Research; unassigned | Desktop product | Qt/QML target is an experimental prototype, not release integration | Explicit runtime ownership, packaging, native evidence and UI tests | Product approval | L | M3 | Prototype presence is not support |
| `AVA-REL-214` | Research; unassigned | Cloud sharing | Local HTML/JSONL export exists; public sharing is intentionally deferred | Privacy/retention/auth/deletion/consent design plus adversarial and withdrawal tests | Hosted-service approval | L | M3 | No ambient public upload |
| `AVA-REL-215` | Research; unassigned | Broad task graphs | Bounded task subagents/jobs exist; workflow graphs do not | Backend authority, budgets, recursion/cancellation/recovery and deterministic graph tests | Durable-job design | L | M3 | Bounded delegation need not become orchestration parity |
| `AVA-REL-216` | Research; shared | Dependency advisories | Exact 2026-08-23 OSV/GitHub/npm/Canonical review found no demonstrated blocker, but five niche gitlinks and destination runtimes lack complete authoritative mappings | Define a bounded release advisory record or automation that preserves exact pins, component boundaries, empty-result uncertainty, and accepted exceptions; rerun against the exact candidate without uploading source | Supply-chain policy | M | M3 | No empty advisory query is a safety claim |

## Rejected and intentionally different

The cut rejects a parity treadmill, reference source or architecture copying, permissionless/fail-open/ambient extension patterns, telemetry, automatic downloads, marketplace execution, public cloud sharing, and lenient authoritative replay. Tool visibility never grants execution authority; project trust never replaces operation policy. These decisions are governed by [product principles](principles.md), not reopened by a reference release.
