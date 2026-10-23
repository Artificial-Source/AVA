# AVA Development

These current maintainer references describe the source checkout and engineering policy. Production source, CMake ownership, tests, and fixed contracts remain authoritative where they define behavior.

- [Contributing guide](contributing.md): prerequisites, build workflow, checks, and review expectations.
- [Architecture](architecture.md): system boundaries, data flow, and module ownership.
- [Codebase guide](codebase-guide.md): implementation map and common change paths.
- [Frontend state boundary](frontend-state-boundary.md): retained interactive, history, and service contracts during the ordinary-state migration.
- [Backend benchmarking](backend-benchmarking.md): reproducible offline benchmark suites, methodology, and interpretation.
- [Documentation policy](documentation-policy.md): information authority, style, freshness, and validation.
- [C++ safety rules](cpp-safety-rules.md): ownership, errors, concurrency, and systems-programming constraints.
- [Side-effect safety checklist](side-effect-safety-checklist.md): required review questions for mutating or external operations.
- [Session architecture](session-architecture.md): conceptual overview of stores, leases, lease-bound authorities, and ephemeral sessions.
- [Session versioning](session-versioning.md): normative maintainer policy for persisted-session compatibility.
- [Internals index](internals/README.md): focused runtime ownership references.
- [`docs/AGENTS.md`](../AGENTS.md): documentation maintenance instructions for humans and agents.
