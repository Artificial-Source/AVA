# AVA Documentation

This is the human documentation spine for AVA. Follow it in task order, or use the category indexes to browse a subject.

## How to read these docs

- **Current** pages describe the implemented product or source tree and must change with the behavior they describe.
- **Normative** contracts define compatibility or verified guarantees; their fixed paths and conformance coverage change together.
- **Plans**, roadmaps, and goals describe intended work, not shipped behavior.
- **History and evidence** preserve dated claims and observations; they are not proof of current behavior by themselves.

## Start, configure, and use AVA

Start with [usage](core/usage.md), then [configuration](core/configuration.md), [providers](core/providers.md), and [environment variables](core/environment-variables.md). The [core index](core/README.md) also covers context resources, model-visible tools, subagents, and thinking modes.

## Interfaces and contracts

- [Interface index](interfaces/README.md): the terminal customization guide.
- [RPC protocol](rpc-protocol.md): normative proprietary AVA RPC v1 contract.
- [ACP endpoint](acp.md) and [machine-readable support profile](acp-support.json): stable ACP integration contract and support declaration.
- [Headless protocol](headless-protocol.md): normative behavior shared by print and RPC modes.
- [Session format](session-format.md): normative append-only persisted-session contract.
- [Plugin compatibility policy](plugin-compatibility-policy.md): normative plugin and MCP compatibility rules.
- [Theme schema](schema/README.md): machine-readable theme contract.
- [Session architecture](development/session-architecture.md): conceptual overview of sessions, `SessionStore`, leases, authorities, and sessionless mode.
- [Session run controller](development/internals/session-run-controller.md): run-controller and append-routing contract.
- [Run observer](development/internals/run-observer.md): trace events and the non-authoritative observation attachment on `SessionStore`.
- [Backend implementation ledger](history/backend-implementation-ledger.md): historical backend milestone and implementation ledger.

These fixed contract paths remain at the `docs/` root (or their established schema/security locations) so external consumers do not need to follow taxonomy changes.

## Desktop planning

- [Desktop index](desktop/README.md): greenfield Electron planning and research. This is not a shipped UI, not a first-release host artifact, and not a baseline taken from the existing optional client in `apps/desktop/`.

## Extensions

Use the [extensions index](extensions/README.md) for local plugins, MCP servers, and optional LSP integration. Extension guides are current descriptive or authoring references; the fixed compatibility and protocol documents above own normative promises.

## Operations

The [operations index](operations/README.md) covers builds, tests, Docker, terminals, diagnostics, troubleshooting, local release artifacts, and the owner-run [official publication runbook](operations/publication.md). The runbook is prepared; approval and the exact-candidate GitHub run are still required. Start with the symptom-first [troubleshooting guide](operations/troubleshooting.md) when AVA or its build fails.

## Security

Read the [security index](security/README.md). [Sandboxing and trust guidance](security/sandboxing.md) is practical current guidance; [containment](security/containment.md) is the fixed normative statement of verified command-containment scope and limitations.

## Development

The [development index](development/README.md) links contribution, architecture, codebase, C++ safety, side-effect, session-versioning, and internal runtime references. Documentation maintainers must also follow [`docs/AGENTS.md`](AGENTS.md) and the [documentation policy](development/documentation-policy.md).

## Engineering programs

The [engineering index](engineering/README.md) contains active, evidence-backed engineering programs and their reproducible ledgers, baselines, ADRs, and reports. Current architecture and policy remain under `development/`; completed ledgers remain under `history/`.

## Product and current status

The [product index](product/README.md) contains canonical [product principles](product/principles.md), the single current [release-readiness cut](product/release-readiness.md), feature/capability baselines, and evidence mapping. Runtime version `1.0.0` is not a published release; complete candidate qualification and publication require the release ledger, [release checklist](operations/release-checklist.md), and publication runbook.

## Plans

The [plans index](plans/README.md) contains proposed or staged work for 1.1 capabilities, tooling, packages, and parallel tool execution. Plans do not establish current product behavior.

## Roadmap and goals

- [Roadmap index](roadmap/README.md): sequencing, approval ledgers, maturity targets, and dated frontend/backend qualification state.
- [Goal packages](goals/README.md): bounded execution packages with acceptance criteria.

Roadmaps and goals are planning surfaces even when they record completed slices; use current docs and tests to determine present behavior.

## History and evidence

- [History index](history/README.md): dated audits, completed planning records, and historical implementation ledgers.
- [Version index](versions/README.md): release-position journals and historical capability claims.
- [Interoperability index](interop/README.md): the ACP evidence category, linking the [evidence policy and reports](interop/evidence/README.md) with dated, scoped interoperability observations.

Local `reference-code/pi/`, `reference-code/opencode/`, and `reference-code/grok-build/` trees are comparative behavior references only. They are not AVA architecture, implementation, documentation authority, build input, or part of documentation verification.
