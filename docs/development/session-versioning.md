# Session Versioning Policy

AVA sessions are append-only JSONL files. The storage format is intentionally inspectable and conservative: normal operation does not rewrite validated records. The only startup mutation exceptions are narrow, exclusively leased recovery of a final torn record and of a proven complete-line uncommitted v4 staging suffix; migration work remains explicit, test-backed, and copy-forward.

## Current Contract

- Session entries use the top-level `version` field. The current writer version is `4` (`kCurrentSessionEntryVersion`).
- Entries without a `version` field are treated as legacy version `0` for read compatibility. New files should not write explicit `version:0` entries.
- Readers accept missing-version legacy entries and explicit versions `1..kCurrentSessionEntryVersion`, then reject future versions with an actionable session error. Replay validation also treats legacy version `0` as supported.
- Payloads that need independent evolution use `data.schema_version`. Session metadata, branch summary, `assistant_output_item`, and `assistant_turn_commit` payloads use `schema_version:1`. Tool-result `structured_result` objects also use `schema_version:1` inside their parent entry.
- Compaction payloads predate this payload-versioning convention and do not currently carry `data.schema_version`; a future incompatible compaction payload change should add and validate a schema version before relying on this policy.
- Unknown or unsupported payload schema versions are replay-validation errors, not silent best-effort reads.

## Compatibility Rules

- New fields should be additive whenever possible. Older readers may ignore fields they do not understand, but they must reject a newer top-level entry `version` when the shape cannot be safely interpreted.
- Do not bump the top-level session entry version for cosmetic fields, optional metadata, or display-only additions that current validation can safely ignore.
- Bump `data.schema_version` for one payload type when only that payload changes incompatibly.
- Bump `kCurrentSessionEntryVersion` only when the common entry envelope, parent/id semantics, replay ordering, or provider-replay meaning changes for multiple entry types.
- A version bump must update protocol/docs text, replay validation, session export if affected, and focused session tests before release.

## Version 4 Ordered Assistant Output

Version 4 adds two private physical entry types: `assistant_output_item` and `assistant_turn_commit`. Both require envelope version `4` and strict payload `schema_version:1`; unknown, duplicate, variant-incompatible, leading-zero integer, or out-of-bound fields are rejected. Items are staged in contiguous order and a matching commit makes them visible. The commit alone carries provider/model, finish metadata, and optional usage/cost; its `item_count` must match dense item sequences `0..item_count-1` (maximum 4096 items/indexes).

A complete final staged suffix is not a migration shortcut: it is an incomplete-turn warning and has no logical replay, stats, compaction, or public-export meaning. A v4 function result is valid only in its immediate post-commit window: after that turn's commit and before the next user message, assistant message, v4 item/commit, or compaction boundary (audit/bookkeeping entries may intervene). Any non-final staged group, malformed commit, sparse/duplicate identity, or other classifier diagnostic is an error. Import rejects even the warning with source-recovery guidance. Branch/fork/clone classify the selected prefix before copying and reject every v4 diagnostic, preventing a target from ending inside either a staged or committed group while still permitting an explicit earlier target before a later staged suffix.

Readers support mixed v0–v3/v4 files without rewriting old records. A valid committed v4 turn projects to the legacy logical shape for stats: one assistant message plus its reasoning/function items, with accounting read once from the commit. A v4 function result must bind the exact `assistant_output_entry_id` and occur after the commit; this remains strict even when legacy v3 tool-result pairing is configured leniently.

AgentLoop writes v4 transactions additively while provider replay reconstructs their ordered private items. Read-side consumers deliberately have three views: RPC history uses the legacy-compatible assistant/reasoning/tool projection with additive private-free `ordered_output` and text phase; Markdown/HTML, transcript, compaction, and token estimation use a per-item ordered public projection; portable JSONL retains committed physical v4 item/commit records and exact tool-result bindings. The portable view strips provider IDs/output indexes and native/signature/redacted values, validates before return, and remains importable without claiming provider-native lossless replay. Incomplete final staging is ignored by these read views and every other classifier diagnostic fails closed; mixed v0–v3 records remain compatible.

## Narrow Torn-Tail Recovery

A resumed runtime acquires the session's existing exclusive `SessionLease` before loading. Startup `--fork` and an RPC branch from a different source acquire a temporary exclusive source lease and hold it through branch creation. With that lease held, `SessionStore::recover_torn_tail` verifies that the read-only lease descriptor and a separate read/write descriptor identify the same canonical regular file, then strictly scans the fixed initial size. Every newline-terminated record must be strict JSON with bounded nesting, unique object keys, a supported entry version, and a supported session-entry shape.

Recovery never changes validated record bytes and never adds an entry or schema field:

- A file already ending in LF is unchanged.
- A complete, strict-valid, supported final record without LF receives exactly one LF followed by a data sync.
- A strict-valid but semantically unsupported, unknown, or future-version final record fails unchanged. Duplicate-key, over-nested, oversized, newline-terminated malformed, and middle-corrupt records also fail unchanged.
- Only a strict-JSON `Invalid` final unterminated suffix is treated as torn framing. Its exact bytes are first written to a unique sibling `<session>.torn-tail.<unique>.bin` with mode `0600`, data-synced, and made directory-durable. Only after that succeeds is the source truncated to the last validated LF and data-synced. A quarantine creation, write, or sync failure leaves the source untruncated.

Session listing is read-only. Normal and bounded/ACP listing may summarize the validated complete-record prefix when the only bad bytes are an `Invalid` final unterminated suffix; a complete valid no-LF record is included. Listing never performs recovery and does not extend this tolerance to framed corruption, duplicate keys, excessive nesting, semantic errors, or future versions.

The quarantine is an operator recovery aid, not an automatically replayed entry. To inspect or restore one, first stop AVA writers and preserve both the current session and quarantine. Do not append a quarantined suffix after newer records. Reconstruct the prior byte sequence only in a separate offline copy using the known pre-recovery validated prefix plus the exact quarantine bytes, repair the intended record there, and strictly validate the result before any deliberate replacement. Retain the original files until the reconstructed copy has been verified.

Cooperating AVA writers share the existing per-path append mutex, and append refuses a nonempty file whose last byte is not LF. Advisory leases and the in-process mutex cannot stop an uncooperative external process from modifying bytes during recovery or between the append framing check and write. Closing that residual external-writer race requires future descriptor-pinned writer hardening; the current policy must not be described as a sandbox or as durability against arbitrary writers.

## Complete Staged-Suffix Recovery

After torn-tail handling, opening/resuming under the same exact active lease also inspects the complete v4 assistant-output suffix. Only a structurally valid final contiguous `assistant_output_item` group with no commit is recoverable. Persistent recovery first quarantines its exact complete-line bytes in a unique owner-only sibling, syncs that artifact and its directory, then truncates and syncs only that proven suffix. Ephemeral recovery removes only the corresponding in-memory suffix. A committed turn, an interior group, an invalid item/commit, sparse sequence, duplicate identity, or any unrelated boundary is never removed and fails closed unchanged. This is recovery, not migration, and it is distinct from torn-tail framing repair.

## Migration Expectations

- Normal AVA startup opens old supported sessions in place and appends new entries with the current writer version, subject only to leased torn-tail framing recovery followed by leased complete staged-suffix recovery.
- Outside that narrow recovery, AVA does not destructively rewrite session JSONL during list, export, tree, fork, clone, compact, or migration.
- If a future change needs migration, prefer a copy-forward flow: read the source session, validate it, write a new session or backup file with upgraded entries, and keep the original file untouched unless the user explicitly requests replacement.
- Repair or migration tools must reject symlinked, oversized, malformed, duplicate-key, over-nested, semantic-invalid, or future-version input except for the one quarantined `Invalid` final suffix case above.
- Partial migration output must not replace a usable source session. Write new output atomically or to a fresh session id, then validate the result before reporting success.

## Validation Requirements

Any session format change must include focused coverage in `ava_tests.session` or a narrower suite that is also run from CTest. At minimum, cover:

- current-version round trips,
- legacy missing-version reads,
- future-version rejection,
- payload `schema_version` rejection for unsupported values,
- export/replay behavior for affected entry types,
- branch/tree behavior when metadata or branch summaries are affected,
- failure behavior for corrupt, oversized, symlinked, or traversal-prone paths when migration or repair tooling is involved.

Before marking a release-ready session-format change complete, run the focused session tests, the relevant RPC/headless session smokes, full default CTest when practical, and `git --no-pager diff --check`.

## Branch Summary Ownership

RPC `summarize_branch` stores caller-supplied text and requires explicit
`summary`, `provider`, `model`, and `reason` provenance; it does not generate the
summary itself.

The TUI implements provider-generated summaries of the selected direct parent's
abandoned post-fork context through
[`BranchSummaryCoordinator`](../../src/ava/app/branch_summary_coordinator.h).
Preparation is read-only, and a separate explicit confirmation authorizes the
operation. Confirmation starts one absolute deadline (30 seconds by default);
the provider/credential transport is cancellation-aware and bounded by that
deadline. Generation requires an available selected provider/model, credentials,
and transport authority, and is unavailable offline. Prompt projection and
generated output are bounded; this is not an unlimited background model call.

Consent binds the prepared source bytes and exact branch range. The coordinator
revalidates them after confirmation before recovery or provider work, then
rechecks source/current-session state before appending; changed source or branch
state fails stale rather than silently applying the summary to another range.
Canceling before confirmation is nonmutating. Provider-generated compaction
summaries remain a separate flow: a branch summary preserves abandoned parent
context rather than establishing a compaction boundary.
