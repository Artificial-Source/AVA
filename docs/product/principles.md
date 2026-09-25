# AVA Product Principles

This current product document defines the authority for AVA's audience, product shape, safety posture, and scope decisions. Historical parity goals and reference-project comparisons do not override it.

## Audience and product

AVA is for developers who want an inspectable local coding agent in a terminal, with explicit control over provider access, tools, filesystem and process effects, and persisted sessions. It is a native C++23 application: the production product is the `ava` terminal executable and its local TUI, print, RPC, and ACP frontends.

AVA is terminal-first and local-first. Provider requests and explicitly approved network tools may leave the machine, but configuration, credentials, sessions, permission policy, diagnostics, and extension discovery remain local by default. AVA does not require a hosted control plane.

## Authority boundaries

- Backend modules own permissions, sessions, provider messages, tools, processes, trust, and extension semantics.
- The TUI renders backend-owned state and sends typed intent. Display labels, truncated IDs, terminal rows, and model-visible tool schemas never become execution authority.
- Tool visibility is not permission. Project trust permits selected project resources to load; it does not permit their operations.
- Persistent deny rules, malformed protected policy, ambiguous identity, stale authority, and unsupported safety states fail closed.
- Plugins and MCP/LSP processes remain bounded by explicit configuration, validation, process isolation, permissions, and output limits. No extension receives ambient terminal, editor, layout, credential, or policy authority.

## Engineering invariants

AVA favors narrow C++23 modules, RAII ownership, and explicit `Result<T>`/`VoidResult` failures over hidden lifetime or exception-based control flow. Filesystem and process side effects stay behind reviewed authority layers.

Sessions are append-only JSONL with strict validation, explicit leases/read/append authority, bounded projections, and deterministic recovery. Authoritative replay is never made lenient merely to accept malformed history.

Tests are deterministic and credential-free by default. Focused unit and whole-process CTest coverage establishes semantics; fake providers establish protocol and agent-loop behavior; tmux and PTY gates establish real terminal behavior. Live-provider checks are opt-in evidence and must distinguish AVA regressions from credential, provider, model, rate-limit, and network conditions.

## First official release scope

The first official release is intentionally narrow:

- native C++23 `ava` terminal executable;
- Linux x86-64 only, unless another architecture gains native exact-candidate evidence before the freeze closes;
- TUI, print, proprietary JSONL RPC v1, and the documented ACP profile;
- supported provider adapters, safe built-in tools, append-only sessions, bounded local plugins, local stdio MCP, and installed/configured LSP integration already described in current docs;
- one exact retained archive/checksum pair whose bytes passed the complete candidate gate.

Runtime version `1.0.0` is a candidate version in this repository, not proof of a published release. Current cut status and blockers live in the [release-readiness ledger](release-readiness.md).

## Supported-platform truth

A platform or architecture is supported for publication only when the exact candidate bytes have native build, full deterministic test, required sanitizer/terminal, install/package, dependency-floor, and retained-artifact evidence required by the release ledger. Source code branches, cross-compilation, static package provenance, or an older local run are not enough.

Best-effort source compatibility must be labeled as such. Unsupported and untested combinations are not advertised. Cross-compiled artifacts, multi-config builds, and architectures without native exact-candidate evidence cannot be published as supported.

## Non-goals

The first release does not pursue:

- feature-count or release-cadence parity with another agent;
- web, hosted cloud, public session sharing, telemetry, automatic downloads, self-update, or marketplace execution;
- permissionless tools, fail-open hooks/sandbox setup, ambient in-process extensions, or unauthenticated public service defaults;
- arbitrary extension render slots, native render callbacks, terminal bytes, key capture, themes, or editor control;
- package-manager breadth, non-Linux ports, broad task graphs, or a production desktop client;
- a claim that deterministic UI/protocol evidence measures model intelligence.

These exclusions can change only through an approved product and safety design, not because a reference project exposes the feature.

## Reference projects

Pi.dev, OpenCode, and Grok Build are comparative inputs. AVA may study capability shape, test vectors, release practices, and failure modes. It does not copy their source or architecture, inherit their trust model, or treat their current HEAD as a backlog.

A reference finding is admitted only when it exposes an AVA user need, contract violation, safety risk, or measurable quality gap under AVA's principles. Intentionally different implementations remain valid.

## Anti-parity-creep and freeze rule

Once a release cut is frozen, new P0/P1 work is admitted only with a reproducible first-release impact, exact AVA evidence, measurable acceptance criteria, and independent technical and scope review. Reference parity, an attractive feature, or a broad cleanup is insufficient. Work that does not meet that rule remains P2/P3 or is rejected; existing scope-review downgrades are not promoted without new evidence.
