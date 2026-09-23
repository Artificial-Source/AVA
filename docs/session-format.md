# AVA Session Format Reference

This document describes AVA's public, append-only session JSONL format. It is intended for tools that need to inspect, archive, validate, or copy AVA sessions without depending on UI output. The packaged protocol contract is [`headless-protocol.md`](headless-protocol.md). A source checkout additionally contains the implementation under `src/ava/session/` and regression coverage in `tests/session_tests.cpp`; source and test trees are intentionally not installed in the host artifact.

AVA session JSONL is **not Pi-compatible**. AVA accepts some Pi-compatible CLI/RPC aliases, but persisted session files use AVA-native envelope fields, entry type names, payload schemas, attachment storage, and replay rules. Do not import Pi logs as AVA sessions, and do not assume AVA JSONL can be replayed by Pi.

## Storage

- Persistent sessions are stored under AVA's session root, which defaults to `$XDG_STATE_HOME/ava/sessions` or `~/.local/state/ava/sessions` when `XDG_STATE_HOME` is unset. `--session-dir <dir>` overrides the root for the current process.
- Session files are grouped by a stable hash of the absolute, normalized workspace path: `<session-root>/<workspace-key>/<session_id>.jsonl`.
- Session directories are owner-only and session files are owner read/write. AVA rejects session files that are symlinks or not regular files.
- `--no-session` uses an ephemeral in-memory session store. Runtime entries can still be used by in-process commands, but no resumable JSONL file is written and temporary attachment/spill storage may disappear when the process exits.
- Image bytes live outside the JSONL file under a sibling attachment directory, `<session-root>/<workspace-key>/<session_id>.attachments/...`. See [Attachment caveats](#attachment-caveats).

## JSONL Envelope

Each line is one JSON object. New AVA writers emit these top-level fields in this order:

```json
{"version":4,"id":"entry_...","parent_id":"","type":"user_message","timestamp":"2026-04-30T00:00:00Z","data":{"text":"hello"}}
```

Envelope fields:

- `version`: integer session entry format version. New records must write the current version, `4`.
- `id`: entry id. AVA-generated ids use `entry_...`. Replay validation requires ids to be unique.
- `parent_id`: optional parent entry id. Empty means no parent. When present, replay validation expects it to reference an earlier entry; unsafe path-like values are rejected.
- `type`: stable entry type string. Current readers reject unknown type strings when opening/importing a session.
- `timestamp`: UTC timestamp string written as `YYYY-MM-DDTHH:MM:SSZ` by current AVA writers.
- `data`: entry-specific JSON object. It must be an object, not an array/string/null.

AVA's shared strict JSON boundary permits at most 64 simultaneously open object/array containers, counting the root as depth one. Session envelopes and nested JSON-string payloads such as function arguments, structured results, usage metadata, and provider-native reasoning are rejected above that boundary before recursive validation.

Records are newline-delimited. AVA rejects raw `\n` or `\r` bytes inside `data` and rejects serialized lines at or above the 1 MiB line cap. Use normal JSON escaping for text newlines.

## Versions

- Current writer version: `4` (`kCurrentSessionEntryVersion`).
- Accepted on-disk versions: explicit `1`, `2`, `3`, and `4`, plus missing `version` as legacy in-memory version `0`.
- Explicit `version:0` should not be written. `0` in RPC responses means the source JSONL record omitted the field.
- Future versions are rejected with a session error rather than best-effort replay.
- Payloads with independent compatibility use `data.schema_version`. `session_metadata`, `branch_summary`, `assistant_output_item`, and `assistant_turn_commit` use `schema_version:1`; `tool_result.data.structured_result` also uses `schema_version:1`. `compaction` predates that convention and currently has no `data.schema_version`.

More versioning policy is in [`docs/development/session-versioning.md`](development/session-versioning.md).

## Entry Types

Current AVA entry type strings are:

| Type | Purpose |
| --- | --- |
| `session_start` | Runtime/model metadata captured when a session starts. |
| `session_metadata` | Append-only display/tree metadata such as name, labels, archive state, branch provenance, and optional nested display-only `subagent_job` history. |
| `user_message` | User text and optional image attachment metadata. |
| `assistant_message` | Final assistant text plus usage/cost metadata when known. |
| `tool_call` | Provider-requested tool call and arguments. |
| `tool_result` | Tool completion result, including canonical `structured_result` when present. |
| `permission_decision` | Durable permission audit record for allow/ask/deny and resolver outcomes. |
| `mode_change` | Build/plan mode transition. |
| `model_change` | Provider/model transition and model capability snapshot. |
| `reasoning_block` | Provider-native reasoning/thinking content or redaction metadata. |
| `reasoning_change` | Durable reasoning setting change for the active model. |
| `assistant_output_item` | Private physical v4 staging record for one ordered assistant output item. |
| `assistant_turn_commit` | Private physical v4 terminal record that makes a staged assistant turn visible. |
| `compaction` | Context compaction boundary and summary. |
| `branch_summary` | Caller-supplied summary of a source session range. |
| `error` | Runtime/provider/tool error record. |
| `cancel` | Cooperative cancellation marker. |

External readers that only need transcript data should prefer RPC `get_messages`, which returns sanitized `user_message`, `assistant_message`, `reasoning_block`, `tool_call`, and `tool_result` entries and hides internal replay messages.

## Important Payloads

### `session_start`

Current writers store small session/model metadata such as:

- `mode`: `build` or `plan`.
- `provider` and `model`: active provider/model ids.
- `prompt_override`: boolean.
- `context_sources`: count of loaded context sources.
- Model capability metadata when known: `input_modalities`, `output_modalities`, `reasoning_levels`, `compatibility_quirks`, `supports_tools`, `supports_streaming`, `supports_reasoning`, `reports_usage`, `display_name`, `family`, `api_family`, `context_window_tokens`, `max_output_tokens`, and `reasoning_format`.

`session_start` is intentionally not a complete context archive. It does not persist the base prompt text, detailed context source paths, prompt fingerprints, or loaded file contents; use `/context` or RPC `context` for freshness diagnostics. Token fields are numeric metadata, booleans must be booleans, and the overall entry remains subject to the JSONL line cap.

### Messages

- `user_message.data.text` contains user text.
- `user_message.data.attachments[]` may contain image metadata only. Attachments are user-message-only.
- Internal replay user messages may include `internal_replay:true`, `replay_of`, and `reason`; they are hidden from normal exports/RPC transcript reads.
- `assistant_message.data.text` contains final assistant text. Current writers also include `tool_calls` and a `usage` object when usage/cost data is known or estimated.

### Ordered assistant output transaction (v4)

`assistant_output_item` and `assistant_turn_commit` are physical records for one ordered assistant response. They are private session-file records, not a public transcript or RPC callback contract. AgentLoop writes them additively and provider replay reconstructs their ordered native items. The RPC compatibility projection emits safe legacy-shaped reasoning, one assistant message, and function tool calls; its assistant message keeps legacy `text`, `tool_calls`, and usage and adds private-free sequence-ordered `ordered_output` (with text `assistant_phase`). The separate ordered public projection emits each v4 text/reasoning/function item in exact sequence for human renderers and transcript/export surfaces without exposing provider-private replay fields. Provider requests and compaction instead use the stricter request-owned projection described below.

Each item is version `4` and has strict `data.schema_version:1`, a bounded `assistant_turn_id`, `sequence` in `0..4095`, and one closed `kind` variant:

- `text`: `text` and `assistant_phase` (`commentary`, `final_answer`, or legacy-compatible `unknown`);
- `reasoning`: `format`, `redacted`, and at least one non-empty `text`, `signature`, `redacted_data`, or `native_item_json`; the latter three are provider-private replay metadata. Portable physical v4 copies set `private_replay_metadata_omitted:true` and retain no private fields. Request-time portable projection drops the reasoning item entirely; it never promotes reasoning text into ordinary assistant text;
- `function_call`: `call_id`, `name`, and an object-valued JSON `arguments` string.

Optional `provider_item_id` and `provider_output_index` identify native provider output; an index is also in `0..4095`. Integer spellings are strict (for example, `00` is rejected). Unknown, duplicate, and variant-incompatible fields are rejected. During OpenAI Responses replay, a text item with a valid provider item id remains a native message when its phase is `unknown`; AVA omits `phase` rather than collapsing it into an ordinary role message. OpenAI message ids at most 64 bytes are preserved exactly; longer valid opaque ids use a deterministic bounded compatibility id derived only from the opaque id, never message content.

A contiguous item group becomes visible only when its immediately following `assistant_turn_commit` has the same `assistant_turn_id`, dense sequences, matching `item_count`, and strict `schema_version:1`. The commit is the sole owner of `provider`, `model`, `finish_reason`, and the optional established `usage`/cost object; current writers also add `api_family` and `reasoning_format` when known. Item records do not carry response usage or model/provider metadata. A zero-item response is one commit with `item_count:0`.

Provider request history is a request-owned projection. Native reasoning, provider item identities, assistant phases, and source tool ids survive only when provider, model, API family, reasoning format (when reasoning exists), tool support, and bindings are proven exact-compatible with the request target. Missing, incomplete, unknown, or contradictory target/source provenance forces portable projection: visible user and assistant answer text remains, reasoning and provider-private metadata are dropped, complete tool pairs receive deterministic request-local ids or bounded labelled text, and incompatible historical images become MIME-and-size-only placeholders. Projection never mutates the persisted session.

A final uncommitted item group is an incomplete-turn **warning** and is ignored by the shared logical projection, replay, stats, compaction, Markdown/HTML/JSONL export, transcript, and RPC public history. Opening/resuming a persistent session is the explicit recovery boundary: after torn-tail repair, AVA holds the exact active lease, quarantines the raw complete-line staged suffix in a unique owner-only sibling, syncs, and truncates only that proven suffix. Ephemeral recovery removes only the same proven in-memory tail. No recovery removes a committed turn, an interior group, malformed v4 records, or unrelated entries; those fail closed unchanged. A staged group followed by an unrelated record, an invalid item/commit, duplicate identity, sparse sequence, or bad count is malformed and is a validation error. Import rejects an incomplete final group with recovery guidance. Fork/clone prefixes are classified before copying and reject any v4 diagnostic, so a target cannot end inside a staged or committed group; an explicit earlier target before a later staged suffix remains valid.

Mixed v0–v3/v4 histories are supported. Stats project a valid committed v4 turn as one logical `assistant_message`, plus one logical `reasoning_block` per reasoning item and one logical `tool_call` per function item; usage, cost, and timestamp are read once from its commit. Uncommitted staging is excluded.

### `tool_call` and `tool_result`

- `tool_call.data` includes `call_id`, `name`, and `arguments`. `arguments` is stored as a JSON string containing the provider arguments JSON.
- `call_id` must be non-empty and unique until resolved.
- `tool_result.data` includes `call_id`, `name`, `success`, `status`, `result`, and usually `structured_result`.
- `status` values are `success`, `error`, or `canceled`.
- `structured_result` is the canonical semantic result when present. It uses `schema_version:1`, repeats `call_id` and `tool`, includes `status`, `ok`, `content_type`, optional `content`, optional `error`, optional `permission_request_ids`, and optional diff/path/truncation/spill counters.
- Replay validation pairs each `tool_result` with an earlier `tool_call`, rejects duplicate results, rejects mismatched tool names, and can require/validate `structured_result` when callers enable that stricter mode.
- A result for a committed v4 `function_call` must additionally contain the exact `assistant_output_entry_id` of that function item. Its only valid immediate result window starts after that turn's commit and ends before the next `user_message`, `assistant_message`, `assistant_output_item`, `assistant_turn_commit`, or `compaction`; audit/bookkeeping records may appear within the window. This exact binding remains mandatory even when legacy v3 tool-result pairing is configured as lenient. If a committed turn reaches the provider token limit with function calls, AVA executes and authorizes none of them, appends one ordered failed result per function with `retryable:false` and stable error code `provider_output_truncated`, counts the batch against the normal tool-iteration limit, and may ask the provider to reissue complete calls. On open/resume, a committed v4 function without an exact later bound result receives one synthetic failed/interrupted result saying its execution outcome is unknown and must not be retried automatically; AVA never re-executes it during recovery.

### `permission_decision`

Permission audit entries record backend policy decisions and resolver outcomes. Important fields include:

- Required semantic fields: `permission_request_id`, `operation`, `mode`, `tool_name`, `action`, `reason`, and `risk`.
- Common optional fields: `target_path`, `command`, `resolution`, `resolution_source`, `resolution_reason`, `actor`, and `rule_id`. For `bash`/`RunCommand`, `command` is only a safe `recipe_display` or `<redacted one-shot command>`; raw argv, shell text, request payloads, and resolver failure text are not durable audit data.
- `action` is `allow`, `ask`, or `deny`; `resolution` is `allow`, `deny`, or `cancel` when a prompt has been resolved.
- Current `resolution_source` values include `policy`, `resolver`, `session_grant`, `no_resolver`, `resolver_failed`, `persistent_rule`, `persistent_rule_error`, `client_cancel`, `hard_scope`, `session_config`, and `client`.
- Legacy `acp_hard_policy`, `acp_session_mcp`, `acp_client`, `acp_session_grant`, `acp_client_cancel`, and `acp_client_error` sources are accepted only for read compatibility with older sessions; current writers do not emit them.
- `risk` values are `low`, `medium`, `high`, and `critical`.

Validation checks that policy `allow`/`deny` entries resolve consistently, that `ask` prompts have matching resolver outcomes when resolved, and that unresolved permission prompts are not crossed by compaction boundaries.

### Reasoning

- `reasoning_block.data` includes `provider`, `model`, `format`, `redacted`, and at least one of `text`, `signature`, or `redacted_data`.
- OpenAI Responses native replay metadata, when explicitly present on a current-version `openai_responses` block, is a bounded strict JSON object with `type:"reasoning"`, a non-empty opaque `id`, and a present array-valued `summary` (which may be empty). Unknown additive provider fields are retained only in the private session record.
- RPC `get_messages` sanitizes reasoning blocks: non-redacted text may be returned, but raw provider signatures and opaque redacted-thinking payloads are replaced with safe fields such as `signature_present` and `redacted`.
- `reasoning_change.data` includes `provider`, `model`, `enabled`, `format`, and when enabled a `level`; it may include `budget_tokens` and `display` where the selected model supports those controls.
- Validation checks reasoning entries against the active provider/model boundary established by `session_start` and `model_change`. Legacy malformed optional native OpenAI metadata may fall back to readable synthetic history; current-version explicitly-present malformed metadata is rejected.
- Legacy v3 reasoning remains readable on local human/RPC surfaces. Version 4 records ordered OpenAI Responses output through transactional records; RPC receives the compatibility reasoning/message/tool view with additive ordered output, human renderers use the per-item ordered public view, request/compaction projection drops non-exact reasoning, and physical records retain private native replay metadata.

### `compaction`

Compaction records a context boundary and summary. Current fields include `trigger`, `status:"recorded"`, `summary_unavailable`, `summary`, `instructions`, `model`, `threshold_tokens`, `estimated_tokens`, `keep_recent_tokens`, `keep_recent_messages`, `max_summary_bytes`, `recent_context`, and `history_projection:"portable-v1"`. Current compaction summaries and retained tails are generated only from forced-portable request messages.

Validation requires a non-empty `summary`, boolean `summary_unavailable` when present, `status:"recorded"` when present, numeric token/retention metadata when present, the exact `portable-v1` value when `history_projection` is present, and no unresolved tool calls or permission prompts at the compaction boundary. Request replay always accepts marked portable compactions. An older unmarked compaction retains its summary/recent replay material only when its complete represented source range and boundary are proven exact-compatible with the request target; otherwise AVA emits a neutral omission notice.

### `session_metadata`

`session_metadata.data.schema_version` is `1`. Metadata entries are append-only; the latest value for each field wins. Current fields and limits:

- `name`: manual title string, at most 256 bytes, no control bytes. Presence is durable even when the value is empty.
- `generated_title`: automatic title string, non-empty, at most 160 bytes, valid UTF-8, and no control bytes.
- `labels`: unique non-empty strings, at most 32 labels, at most 64 bytes per label, no control bytes.
- `archived`: boolean.
- `parent_session_id`, `source_session_id`: AVA session ids.
- `branch_from_entry_id`: entry id in the source session.
- `branch_origin`: empty or one of `root`, `fork`, `clone`, `manual`, `import`.
- `actor`: optional string, at most 64 bytes.
- `subagent_job`: optional nested object, `schema_version:1`. Display-only background/subagent job history. It is not tree metadata and does not affect titles, labels, or model context. Older sessions without the field remain valid.

When present, `subagent_job` is strictly typed:

- `phase`: `start` or `terminal`.
- `job_id`, `task_id`, `parent_session_id`, `child_session_id`, `delivery_id`: non-empty identifiers, at most 96 bytes, `[A-Za-z0-9_.:-]`.
- `mode`: `foreground` or `background`.
- `execution`: `starting` for start records; terminal records use `completed`, `failed`, `canceled`, or `interrupted`.
- `started_at`, `updated_at`: required timestamps; `terminal_at` is required on terminal records.
- Optional bounded `summary` (16 KiB), `error` (4 KiB), `stop_reason` (1 KiB), truncation booleans, `error_category`, and non-negative accounting counters.
- Records never store credentials, prompts, raw tool args, paths, or launch authority.

Readers project at most the latest 64 unique jobs whose `parent_session_id` matches the current session. Forks must not inherit control. Unmatched start records display as interrupted with outcome unknown. Append-only disk size grows with ordinary session history; this is not a fixed disk bound.

The effective display title is the latest manual `name` whenever any `name` field has appeared; otherwise it is the latest `generated_title`. Consequently `name:""` is an explicit durable suppression of generated display titles, even if a later record contains `generated_title`. Sessions written before `generated_title` remain valid and keep their existing manual/untitled behavior; readers that do not know the additive field may ignore it.

Forks and clones copy source entries into a new session and append `session_metadata` provenance to the new session. Title metadata is folded from the complete source independently of an earlier fork prefix: an explicit branch name wins, otherwise the latest source manual name (including empty suppression) is persisted, and relevant generated-title metadata is retained. Branch ancestry fields are unchanged. The source session file is left untouched.

### `branch_summary`

`branch_summary.data.schema_version` is `1`. Required fields:

- `summary`: non-empty text, at most 8192 bytes; newlines and tabs are allowed, other control bytes are not.
- `source_session_id`: source session id.
- `branch_root_entry_id` and `branch_tip_entry_id`: earlier entries in the source session, with root not after tip.
- `provider` and `model`: provenance strings, each at most 256 bytes.
- `reason`: provenance string, at most 1024 bytes.
- `actor`: optional string, at most 64 bytes.

Branch summaries are caller-supplied in the current MVP. Provider-generated branch summaries are deferred; provider-generated compaction summaries are a separate `compaction` behavior.

## Attachment Caveats

Persisted image attachments are metadata references, not inline uploads:

- Only `user_message.data.attachments[]` supports attachments.
- Each attachment object may contain `id`, `type:"image"`, `mime_type`, `byte_size`, `sha256`, `storage_path`, and optional `redacted`.
- Supported MIME types are `image/png`, `image/jpeg`, `image/webp`, and `image/gif`.
- `byte_size` must be an integer from 1 through 20 MiB, and `sha256` must be a 64-character hex digest.
- `storage_path` must be a relative `attachments/...` path without absolute roots, `..`, empty segments, backslashes, drive prefixes, or control bytes.
- AVA imports local RPC `attachments` paths and inline RPC `images` uploads into the session attachment store before writing message metadata. Raw base64 image data is invalid in persisted session JSONL.
- Replay loads bytes only from the active session's attachment directory and verifies path containment, symlinks, byte size, and SHA-256 before sending image content to providers.
- Portable JSONL export does not bundle attachment bytes. It converts every exported image attachment to the accepted `redacted:true` metadata form, preserves safe audit fields such as id/MIME/size/digest, and replaces the source storage reference with a deterministic portable placeholder. The export therefore re-imports without attachment bytes, but it cannot replay the original image content.
- Branch/fork/clone code copies non-redacted image attachment bytes for copied entries. Slash `/import <path.jsonl>` accepts portable redacted attachment metadata without reconstructing attachment bytes; directly imported non-redacted attachment references still require an attachment-aware archive because image replay would otherwise be dangling.

Provider replay also has request-level caps, including at most 16 images and aggregate byte limits before base64 expansion; some providers have lower per-image limits.

## Import and Export Guidance

- `/export` writes a Markdown transcript by default.
- `/export html [path]` writes or returns an HTML rendering.
- `/export jsonl [path]` or `/export raw [path]` emits a portable sanitized AVA JSONL archive for v0–v4 histories. It preserves every committed v4 `assistant_output_item` and `assistant_turn_commit` in exact order plus exact `assistant_output_entry_id` tool-result bindings, while stripping provider item IDs/output indexes and all native reasoning/signature/redacted payload values. Portable v4 reasoning sets `private_replay_metadata_omitted:true`; request-time replay drops that reasoning item entirely. Text phase/order, functions, commit provider/model/finish/usage, replay-valid parent chains, and redacted attachment metadata remain. The result is validated before return and is importable, but intentionally cannot losslessly replay provider-native data.
- RPC `export` and `export_html` return Markdown/HTML command output. Dedicated RPC portable JSONL export/import/share commands are deferred; use slash/line-shell `/export jsonl` for a portable archive.
- `/import <path.jsonl> --confirm` is slash/line-shell only for the current MVP. It opens one local regular non-symlink descriptor with nonblocking final-component checks, then reads only that descriptor. Imports are capped at 8 MiB per file, less than 1 MiB per JSONL record, and 16,384 entries before replay validation; confirmed valid imports create and switch to a new session.
- Import treats missing-version legacy entries as version `0` while reading and preserves their canonical wire form (the top-level `version` member remains absent) when appending to the new session.
- Markdown and HTML export, transcript, compaction prompts, and token estimation use the ordered public projection. RPC `get_messages` uses the compatibility projection with additive `ordered_output`; its legacy-compatible message selection and caps run before ordered detail, and an omitted-detail response reports additive `ordered_output_truncated` and `ordered_output_omitted_count`. Portable JSONL uses its physical-order archive projection. Normal AVA startup, resume, list, tree, fork, clone, compact, and export do not rewrite existing session files. Prefer copy-forward import/migration into a new session over editing JSONL in place.
- Automation should prefer RPC `get_messages`, `get_session_stats`, `validate_session`, `session_metadata`, and `session_tree` for stable views. Direct JSONL readers should tolerate additive fields and ignore non-message bookkeeping entries they do not need, but AVA itself rejects unknown entry type strings when opening/importing a session.

## Validation Checklist

AVA's parser and replay validator currently enforce these important constraints:

- Each JSONL line must be below the line-size cap and parse as an entry envelope with non-empty `id`, `type`, `timestamp`, and object-shaped `data`.
- Missing top-level `version` is legacy version `0`; explicit supported versions are `1..4`; future versions are rejected.
- `session_id` values must be non-empty safe path segments. Non-empty `parent_id` values must also be safe path segments, with no reserved `.`/`..`, path separators, or control bytes.
- Entry ids must be unique for replay; `parent_id` should reference an earlier entry.
- Message payloads must be valid objects; attachment metadata is user-message-only and must match the shape in [Attachment caveats](#attachment-caveats).
- Tool calls and results must pair by `call_id`; duplicate call ids, result-without-call, mismatched tool names, duplicate results, and unresolved calls are validation errors.
- Permission `ask` prompts must be resolved consistently or remain pending only until the end of validation; compaction cannot cross unresolved permission prompts.
- `session_metadata`, `branch_summary`, `assistant_output_item`, `assistant_turn_commit`, and `structured_result` schema versions must be supported; v4 output records require `schema_version:1` and keep usage only on the commit.
- Durable model and reasoning entries must be consistent with the active provider/model boundary.
- `validate_session` over RPC returns `{session_id, session_path, ok, error_count, warning_count, issues}` with stable issue `kind` strings, severity, entry index, entry id, optional tool call id, and diagnostic message.

For format changes, update the versioning policy, replay validation, export behavior if affected, and focused session/RPC tests before release.
