# Engineering Programs

This category contains active, evidence-backed engineering programs and the
reproducible ledgers, baselines, ADRs, and report sets that support them. These
records must identify their scope and repeatable evidence; they are not a
replacement for current architecture or policy.

- [TUI architecture](tui-architecture.md): issue #93 implementation guide for
  ownership, loop order, worker publication and safe feature changes.
- [Backend modernization progress](backend-modernization-progress.md): active
  program ledger and its acceptance-evidence record.
- [Backend current state](backend-current-state.md): M0.1 ownership, process,
  tool-call, session, cancellation, and memory-retention inventory.
- [Process supervisor ADR](process-supervisor-adr.md): accepted M1 ownership,
  lifecycle, platform, environment, migration, and verification contract.
- [Backend performance baseline](performance-baseline.md): M0.3 measured results,
  calibrated regression policy, and unsupported-result disclosures.
- [Backend performance baseline JSON](backend-performance-baseline-2026-08-30.json):
  complete versioned samples and machine/build identity for the M0 run.
- [Plugin process forward before](process-plugin-forward-before-2026-09-02.json)
  and [after](process-plugin-forward-after-2026-09-02.json): five-run Release
  raw samples for the legacy-to-supervised Plugin pair.
- [Plugin process forward comparison](process-plugin-forward-comparison-2026-09-02.json):
  qualified before-then-after Plugin investigation result.
- [Plugin process reverse before](process-plugin-reverse-before-2026-09-02.json)
  and [after](process-plugin-reverse-after-2026-09-02.json): fresh five-run
  Release samples collected in after-then-before order.
- [Plugin process reverse comparison](process-plugin-reverse-comparison-2026-09-02.json):
  qualified reversed-order confirmation of the Plugin investigation trigger.

Current architecture and engineering policy remain under
[`development/`](../development/), while completed ledgers remain under
[`history/`](../history/).
