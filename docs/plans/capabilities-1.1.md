# AVA 1.1 Backend Candidate Capabilities

This document captures post-1.0 backend candidate work discovered from AVA code/docs audits plus behavior comparison against local reference repositories under `docs/reference-code/`. The current product MVP baseline and Pi/OpenCode gap map lives in `docs/product/mvp-baseline.md`. Reference repositories are product-behavior inputs only; do not copy their source code or architecture into AVA.

The 1.0 backend MVP baseline is declared by the current runtime/docs. This file is the backend planning surface for 1.1 and later; it is not a promise that every row ships in one release. Frontend/TUI planning is Carlo-owned and intentionally excluded.

Priority legend:

- `P0`: urgent backend correctness or contract gap.
- `P1`: strong 1.1 backend candidate that unlocks major workflows.
- `P2`: likely 1.2+ or needs a separate design/security pass.
- `Research`: keep on roadmap, but decide only after deeper product or security design.

## Reference Findings

### Backend And Product Parity

Backend/product capabilities that AVA does not fully match yet:

- Session tree product workflows: backend/RPC fork, clone, tree inspection, labels, names, caller-supplied branch summaries, and a documented session-versioning policy exist; provider-generated branch summaries are deferred until branch navigation UX needs them.
- Multimodal/image support: image prompt parts, image-capable model metadata, attachment storage, fork/clone copy, provider payload support, RPC local-path/inline-upload input, and TUI import flows exist; broader attachment UX remains follow-up work.
- Broader provider/auth matrix: completed backend Anthropic OAuth request/refresh handling intentionally stays limited to stored/env bearer credentials until Anthropic publishes a documented third-party flow; Gemini native Google Generative AI support exists; opt-in live-smoke harnessing now exists; GitHub Copilot OAuth, Bedrock, Vertex, Azure, and larger generated model catalogs remain separate follow-up work, not part of the current hardening batch.
- Subagents/task workers: configurable native subagents, foreground/background child sessions, and background job tracking have landed; live jobs, results, and pending delivery are process-local for the current AVA process and are not recovered or delivered after close, crash, or restart, while child-session JSONL remains durable and inspectable; a public default-off print/RPC launch opt-in supports preflight-proven non-interactive builtin read/search calls. Default-on execution, interactive/ACP exposure, broader tool eligibility, chained task graphs, and plugin-contributed subagent packages remain follow-up work.
- Plugin ecosystem scale: broader package management, remote package sources, dynamic resources, and plugin-defined providers remain follow-up work. Global `providers.json` already supplies startup-time custom providers for OpenAI Chat Completions, OpenAI Responses, and Anthropic Messages; additional protocol families and hot runtime re-registration are separate deferred work.
- Plugin core-service proxy: plugins asking AVA to perform file, shell, network, and session operations through AVA permissions instead of doing side effects directly.
- Stronger sandbox options: arbitrary-plugin, unrestricted-shell, container, and cross-platform isolation remain follow-up work beyond the shipped Linux development-command containment slice on supported kernels with a verified local executor.
- Full LSP/code intelligence: pull/publish diagnostics, document/workspace symbols, definitions, references, bounded full-text on-disk sync, and one globally exact-opt-in installed-only identity-bound `clangd` recipe now have a backend slice; this is the sole automatic LSP recipe, every other server requires explicit configuration, and unsaved-buffer sync remains follow-up scope.
- Unified settings: global plus project merge, validation, migrations, hot reload, and config write locking.

## 1.1 Candidate Table

| Capability | Priority | Scope | Current AVA State | 1.1 Acceptance Direction |
| --- | --- | --- | --- | --- |
| Session tree, fork, clone, labels, names, branch summaries | P1 | Backend + RPC | Backend/RPC commands for naming, labels, tree inspection, fork, clone, and caller-supplied branch summaries are implemented over append-only session entries; session-versioning policy is documented. | Add richer frontend/TUI workflow polish without rewriting session storage. Revisit provider-generated branch summaries only after branch UX defines model-call permission, budget, and stale-session behavior. |
| Persistent permission rules | P1 | Backend + RPC | Durable allow/deny rule storage, management commands, exact matching, fail-closed validation, and RPC lifecycle events are implemented. | Add richer rule-management UX, broader policy categories, and long-term migration/diagnostic polish. |
| Unified settings and reload | P1 | Backend + RPC | Config is domain-specific; keybinds/model/auth/prompt settings exist separately. | Define global/project merge semantics, validation, diagnostics, safe writes, reload commands, and explicit changed/skipped/error reporting. |
| Full LSP code intelligence | P1 | Backend tools + RPC | Pull/publish diagnostics, document/workspace symbols, definitions, references, bounded full-text on-disk sync, explicit config, and one globally exact-opt-in installed-only identity-bound `clangd` recipe exist as capability-gated backend tools. | Keep installed-only `clangd` as the sole automatic LSP recipe and require explicit configuration for every other server; add unsaved-buffer sync and richer RPC/headless presentation without broadening executable discovery. |
| Multimodal/image attachments | P1 | Backend + provider + RPC | Provider-neutral metadata/replay validation, sanitized RPC/export output, AVA-managed bytes, fork/clone copy, provider serialization, RPC path/inline-upload input, and TUI import are implemented. | Continue UX polish while preserving AVA-managed storage, size/MIME/hash validation, and safe textual fallback metadata. |
| Provider/auth breadth | P1 | Backend | OpenAI and Kimi have live evidence; Anthropic native path has deterministic API-key plus stored/env OAuth bearer request/refresh handling, while interactive OAuth is deferred pending an official third-party flow; Gemini native Google Generative AI support has deterministic protocol coverage and a credential-gated smoke entry; compatible shims have deterministic fake coverage and an opt-in credential-gated live-smoke harness. Global `providers.json` startup-time custom providers support OpenAI Chat Completions, OpenAI Responses, and Anthropic Messages through the application-scoped catalog (`src/ava/provider/catalog.cpp`). | Treat Copilot, Bedrock, Vertex, Azure, generated model catalogs, plugin-defined providers, additional protocol families, and hot runtime re-registration as separately scoped follow-up decisions with provider-specific auth/security designs. |
| Plugin core-service proxy | P1 | Backend security/extensibility | Plugins can request AVA-mediated file read/search and read-only `session.status` through capability-gated `proxy.request` records that reuse AVA permissions, cancellation, audits, and output limits. Shell/network/edit/session mutation proxies remain deferred. | Add any broader shell/network/session mutation proxy operations only with explicit permission categories, cancellation, audits, and output limits. |
| HTML/richer export | P2 | Backend/RPC | Markdown export exists. | Consider HTML export only if sharing/readability demand is concrete; keep markdown as canonical inspectable export. |
| HTTP/server daemon mode | P2 | Backend protocol | Stdio JSONL RPC is the 1.0 contract. | Design only after stdio RPC remains stable; keep auth, local binding, and permission boundaries explicit. |
| Configurable task subagents and background jobs | Done / follow-up polish | Backend + sessions | Native `task` tool, built-in `general`/`explore`, custom Markdown subagents, child sessions, automatically allowed/audited task launch, foreground nested Ask UI, fail-closed background nested Ask actions, background job registry, cancellation, and bounded retained snapshots are implemented. Live jobs, results, and pending delivery are process-local for the current AVA process and are not recovered or delivered after close, crash, or restart; child-session JSONL remains durable and inspectable. | Keep docs/tests current, add RPC/TUI polling/cancel UI when needed, and keep plugin-contributed subagent packages out of core until package trust exists. |
| Parallel/chained task orchestration | P2/Research | Backend + plugins + sessions | Broader orchestration remains deferred beyond the native `task` subagent slice. Public `--parallel-read-search-tools` and optional `--parallel-read-search-max-workers N` (1..4) are shipped for print/RPC only, default off, and cover preflight-proven non-interactive builtin `read_file`, `list_directory`, `glob`, and `grep` epochs. A full-binary headless opt-in smoke is registered in `tests/CMakeLists.txt`; see [the parallel-tools plan](parallel-tools-plan.md). | Default-on execution, interactive/ACP exposure, broader ordinary-tool eligibility, and chained task graphs remain deferred. Define their replay ordering, permission-audit ordering, cancellation semantics, and UX before expanding the shipped slice. |
| Plugin package manager/marketplace | P2 | Extensibility | Manual plugin install/discovery exists. | Add only after plugin compatibility, signing/trust, version pinning, and remote install security are designed. |
| OS/container sandbox option | P2/Research | Security/tools/plugins | Linux development-command containment is implemented for eligible sealed local commands using Landlock/seccomp, only on supported kernels and with verified installation by the local executor (`src/ava/tools/bash_tool.cpp` and `src/ava/containment/containment.h`). Unavailable containment downgrades policy to Ask; installation failure does not start user code. This is not arbitrary-plugin or unrestricted-shell isolation. | Evaluate broader container, arbitrary-plugin, unrestricted-shell, and cross-platform isolation separately without weakening portability or extending the verified command-containment guarantee. |
| Advanced MCP | P2 | Extensibility | Stdio tools/prompts and read-style text MCP resources are implemented; remote transports and advanced resource behavior are deferred. | Evaluate HTTP/OAuth/subscriptions/sampling/resource templates/binary resources and richer pagination diagnostics separately without weakening `mcp.resource.read` boundaries. |

## Not Counted As Gaps

- `/compact`, `/export`, and `/bash` already have AVA command paths.
- AVA already has prompt commands and skills; the remaining gap is broader settings/package-driven resource management.
- Slack bots, GPU pod orchestration, web artifact workspaces, iframe sandboxes, and browser DOM injection are out of scope for AVA's local terminal-first product unless the product direction changes.
- In-process extension APIs are not a default target. AVA's out-of-process plugin boundary is an intentional safety choice.

## Validation Expectations

1. Backend P1 work should include RPC/headless contract tests, session replay/export validation, permission audit coverage, and failure-mode tests.
2. Provider/auth work should include fake-provider contract tests first and live smokes only when credentials are available.
3. Plugin/security work should receive a security review before any side-effect proxy, package install, or sandbox claim ships.
