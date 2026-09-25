# AVA RPC v1 Protocol

This is the normative client-author specification for AVA RPC protocol version 1. Where another AVA document disagrees about RPC wire behavior, this document wins.

## Purpose And Non-Purpose

AVA RPC is a local, long-lived subprocess protocol for automation clients that need to drive one AVA process, consume semantic runtime events, answer permission/question requests, and operate AVA sessions. Start it with `ava --rpc`, `ava --mode rpc`, or `ava --output rpc`.

AVA RPC is **not Pi RPC**, JSON-RPC 2.0, or the Agent Client Protocol (ACP). It has no JSON-RPC `jsonrpc`/`method`/`params` members, no ACP transport or capability negotiation, and no claim of editor interoperability. Standards-based editor integration uses the separate `ava --acp` JSON-RPC 2.0 endpoint documented in [`acp.md`](acp.md); neither protocol replaces or tunnels the other.

Pi RPC is intended for subprocess embedding in non-Node applications, IDEs/custom UIs, automation, and testing (Node users can use Pi's in-process session API). AVA shares the broad subprocess use case but has a different wire contract: AVA uses client `type` commands, `success` responses, AVA event envelopes, explicit permission/question resolver commands, AVA session/version semantics, stable AVA error codes, direct permissioned command execution, and AVA-specific model/plugin/MCP operations. A Pi RPC client must not be pointed at AVA without an adapter.

RPC is not a network server, remote trust boundary, shell sandbox, provider-side exactly-once mechanism, or session-file API. Clients should use RPC results rather than parsing AVA's persisted JSONL.

## Transport, UTF-8, And JSONL Framing

- stdin: client-to-AVA request records only.
- stdout: AVA protocol records only (responses and events).
- stderr: startup diagnostics, warnings, and unrecoverable stdio failures; never parse it as RPC.
- Encoding is strict UTF-8. AVA validates a complete record before JSON parsing. Invalid UTF-8 produces a recoverable `invalid_request` response with `id:""`; AVA never reflects invalid bytes into stdout JSON.
- LF byte `0A` is the sole record delimiter. Every emitted stdout record ends in LF.
- For CRLF input, AVA strips exactly one CR immediately before the LF before parsing.
- A non-empty, unterminated final stdin record is accepted and processed at EOF. Empty EOF ends the loop cleanly.
- A request record is one JSON object and is limited to 32 MiB. JSON nesting is limited to 64 before recursive extraction. Duplicate member names at any object depth are rejected after escape decoding (`"type"` and `"t\u0079pe"` collide). AVA drains an oversized record through its LF and then recovers with an error.
- Writers never intentionally emit partial JSON records. A stdout write/flush failure is fatal and cancels active work and resolver waits.

Do not use a line API that also splits on Unicode separators. Buffer bytes, split only on LF, decode each complete record as UTF-8, then parse JSON.

## Versioning

A request may contain integer `protocol_version:1`. Omission means legacy v1 and is equivalent to `1`. Other values or types are recoverable `invalid_request` errors.

`get_protocol` returns:

```json
{"protocol_version":1,"supported_protocol_versions":[1],"event_schema_version":1,"supported_event_schema_versions":[1],"session_entry_version":4,"supported_session_entry_versions":[0,1,2,3,4],"capabilities":["direct_bash_rpc","job_controls"],"direct_command_types":["run_bash","run_command","list_jobs","get_job","wait_job","get_job_result","cancel_job","promote_job"]}
```

Protocol, event-envelope, and session-entry versions are independent. The supported session-version array is generated from the reader's accepted contiguous range `0..kCurrentSessionEntryVersion`; clients must use that field rather than hard-code a historical list. Unknown additive object fields and unknown future event names must be ignored. A client must not infer support for another protocol version from an event or session version.

Session entry v4 adds private physical ordered-output records (`assistant_output_item` and `assistant_turn_commit`). RPC public callbacks and `get_messages` use the compatibility projection: existing assistant `text`, `tool_calls`, and usage fields remain, while a committed v4 assistant message may add a private-free `ordered_output` array. Its text elements carry additive `assistant_phase`; reasoning elements carry only visible text/format/redaction and omission-presence booleans; function elements carry call id/name/arguments. Legacy-compatible entry selection and the established 8 KiB per-entry/1 MiB response caps are applied before this additive detail. If ordered detail is omitted, the response adds `ordered_output_truncated:true` and `ordered_output_omitted_count`; its legacy `messages` and `message_count` remain unchanged. It never exposes staging fields, provider item IDs/output indexes, native reasoning replay payloads, signatures, redacted payloads, or exact tool-result bindings. A valid final incomplete staging suffix is ignored by public reads; every other v4 classifier diagnostic is returned as an error. Portable JSONL is separately import-valid and preserves committed v4 physical order/bindings after sanitization.

## Message Categories And Envelopes

Every stdout record is either a response (`type:"response"`) or an event (`schema_version`, `name`, and compatibility `type`). Every stdin record is a request.

### Request

```json
{"id":"client-1","type":"get_state","protocol_version":1}
```

`id` and `type` are required non-empty strings. Command payload members are top-level. Unknown/additive members, including members belonging to another command, are ignored when their names are unique. Duplicate names are always invalid, including inside additive objects. A member recognized by the selected command is different: if present it must have the documented JSON type and valid value; wrong-typed optional members are errors, not defaults.

### Success

```json
{"id":"client-1","type":"response","success":true,"result":{}}
```

`result` is command-specific and is normally an object. Existing result fields are preserved; clients must tolerate additive fields.

### Error

```json
{"id":"client-1","type":"response","success":false,"error":{"category":"invalid_argument","code":"invalid_request","message":"...","details":"..."}}
```

`error.code` is the stable machine branch. `category` remains the AVA error category, `message` is short human text, and `details` is diagnostic and may grow context lines. Do not branch on `message` or `details`.

Stable v1 codes are `invalid_request`, `active_run`, `canceled`, `follow_up_skipped`, `job_not_ready`, `io_error`, `not_found`, `permission_denied`, `provider_error`, `session_error`, `tool_error`, and `internal_error`. More codes may be added. A client must preserve an unknown code and fall back to `category`/generic handling.

Malformed records without a valid parseable ID use `id:""`. Unsupported commands are `invalid_request`. Request-level errors are recoverable; the process continues unless stdin/stdout itself fails.

### Event

```json
{"schema_version":1,"event_id":"event_...","timestamp":"2026-07-12T00:00:00Z","session_id":"session_...","request_id":"prompt-1","correlation_id":"prompt-1","name":"message_update","payload_type":"message","payload":{"text":"hi","status":"streaming"},"type":"message_update","text":"hi","status":"streaming"}
```

Required event fields are `schema_version`, `event_id`, `name`, `type`, and object `payload`; runtime context fields may be empty/omitted where unavailable. `type` equals `name` in v1. Prefer `name` and `payload`; top-level payload aliases are compatibility fields.

## Common Fields, IDs, Correlation, Errors, And Results

Client request IDs and resolver `request_id`/`correlation_id` values are non-empty UTF-8 strings, at most 256 bytes. They reject ASCII control/space and command-metacharacter bytes. Client IDs must be unique while work is outstanding across active prompts, direct commands, and accepted queued follow-ups. A duplicate receives `invalid_request`; it does not replace, cancel, or resolve the original request. An ID may be reused only after its response has been emitted.

- Response `id`: the originating client request ID.
- Event `request_id`: the client command responsible for the event. Prompt runtime events use the active prompt/follow-up ID.
- Event `correlation_id`: the logical run being affected. A queued steer/follow-up has its own `request_id` and the active prompt ID as `correlation_id`.
- Resolver `payload.resolver_request_id`: AVA-owned ephemeral resolver ID used in `permission_reply`/`question_reply`.
- Permission `payload.permission_request_id`: durable audit identity; it is not the resolver ID.
- Session-grant `grant_id`: a new process-local revocation handle generated when `allow_session` is accepted. It is neither the resolver ID nor the durable `permission_request_id`; the grant object carries that originating `permission_request_id` as a separate field.
- Automatic subagent delivery uses an AVA-owned `automatic_delivery_...` run request identity distinct from both the coordinator's `delivery_attempt_...` identity and every client prompt request ID.
- `event_id`, `session_id`, `run_id`, `turn_id`, `message_id`, and tool `call_id` are AVA-owned.

Prompt success contains `session_id`, `final_text`, `stop_reason`, `provider_iterations`, `tool_calls`, and optional `tool_timeline`. Command-dispatch success contains `handled`, `quit`, `output`, `text`, and optional `tool_timeline`. State results contain protocol/session/workspace/mode/model/cancel/reasoning/context metadata. Lists and session/model results are bounded and additive.

Session title queries use one effective-title rule: a manual `name` wins whenever any manual name has been persisted, including explicit empty suppression; otherwise `generated_title` is used. `list_sessions.sessions[].title` and each `session_tree.sessions[].title` serialize that effective title. Tree nodes and `session_metadata` additionally serialize `name`, `generated_title`, and `has_manual_name`, allowing clients to distinguish untitled sessions from manual-empty suppression. Automatic fallback/refinement metadata is observable through these existing query commands; protocol v1 defines no live automatic-title event.

## Shared Payload Shapes

### Tool timeline/event entry

Tool records use `status` (`running`, `success`, `error`, or `canceled` as applicable), `call_id`, `tool`, optional safe summaries, optional object `args`/`result` (or `args_json`/`result_json` fallback), and optional `structured_result`. Additive fields include `content_type`, error metadata, `diff`, `changed_paths`, `permission_request_ids`, truncation/spill flags and paths, byte/line/match counters. `structured_result` v1 has `schema_version:1`, identity, status, `ok`, content type/content, optional error, and related metadata.

### Subagent job snapshot

Model-tool and RPC job controls share one bounded public `schema_version:1` JSON snapshot. It exposes `job_id`, `task_id`, `parent_session_id`, `child_session_id`, `delivery_id`, mode, execution/delivery states, start/update and optional terminal/promotion/cancel/delivery timestamps, cancellation/promotion flags, delivery attempt/accounting counters, and truncation metadata. Wait responses add `timed_out`; list responses add bounded list truncation/count fields. The public snapshot does not include process-local interactive display fields such as display title or subagent type. Job snapshots and pending automatic deliveries are process-local and are not recovered or delivered after AVA closes or crashes. Display-only recorded job history in parent session JSONL is a slash-command surface, not an RPC recovery or delivery mechanism.

Interactive slash `/jobs` is a separate human-readable surface over the same owner-bound live controls and optional display-only recorded history. It prints human state/mode labels, display-only list ordinals, and optional process-local sanitized title/type text; ordinals are never control authority, and exact `job_id` values remain required for show/wait/result/cancel/promote. `/jobs list` merges live jobs with recorded history (live wins); `/jobs history` is recorded history only. Interactive list/show/wait/cancel/promote receipts omit terminal result content except that historical show/result may include a bounded recorded summary; only `/jobs result <job_id>` includes the bounded terminal summary/message under the same content policy as RPC `get_job_result`. wait/cancel/promote reject historical-only ids.

List and status omit terminal `summary`, `error`, and `message`. Result requests include a bounded completed `summary`; failed results include only stable `status`, sanitized `message`, and `error_category`; canceled/interrupted results include only stable status and a sanitized message. Snapshots never expose filesystem paths, coordinator state/errors, provider bodies, commands/arguments, credentials, or formatted error context. An unfinished result returns stable `job_not_ready`.

### Permission request/reply

`permission_requested.payload` includes `resolver_request_id`, optional durable `permission_request_id`, `operation`, `mode`, `target_path`, `command`, `tool_name`, `reason`, `risk`, and optional `diff_preview`/`diff_truncated`. Reply with `decision` equal to `allow`, `allow_session`, or `deny`; optional `reason` is at most 1024 bytes. `allow_session` creates an in-memory exact-match grant for this RPC process. Sealed commands bind that grant to the stable workspace recipe key supplied by backend metadata rather than raw command text; commands whose backend scope is one-shot cannot create a reusable grant. Persistent allow/deny rules are separate commands and never override built-in hard denies.

When `operation` is `bash` and the request originates from a sealed command plan, `permission_requested.payload` includes an optional `command_metadata` object with the following additive fields:

| Field | Type | Description |
|-------|------|-------------|
| `level` | string | Command risk level: `standard`, `sensitive`, or `critical`. |
| `family` | string | Command family (e.g. `inspection`, `cmake_build`, `interpreter_inline`, `raw_shell`). |
| `fingerprint` | string | Instantaneous integrity binding for the sealed plan (`sha256:ava-command-plan-v5:...`); never a durable grant identity. |
| `execution_domain` | string | `direct_argv` or `raw_shell`. |
| `resolved_executable` | string | Preserved logical spelling of the resolved executable; physical identity remains descriptor-bound internally. |
| `origin` | string | Executable provenance: `system`, `user`, or `workspace`. |
| `cwd` | string | Preserved logical working-directory identity. |
| `containment_available` | boolean | Whether verified local process containment is available for this plan. |
| `containment_status` | string | `not_required`, `available`, `active`, `unavailable`, or `unverified_delegated_executor` as applicable. |
| `backend_maximum_scope` | string | Maximum reusable scope: `once`, `session`, or `workspace`. Critical/raw/unverified commands are always `once`. |
| `recipe_payload_version` | string | Version of the typed durable recipe payload. |
| `global_recipe_key` / `workspace_recipe_key` | string | Stable typed recipe keys; empty when the plan is not reusable. |
| `recipe_display` | string | Secret-screened display summary; never matching authority. |
| `effective_allowed_scopes` | string array | Scopes actually available after backend/policy intersection. |
| `containment_profile_id` | string | Bound containment policy profile. |
| `containment_network_allowed` | boolean | Whether the sealed plan permits network access. |
| `environment_profile_id` | string | Synthetic environment profile identifier. |
| `environment_digest` | string | Synthetic environment digest (no environment values). |
| `executor_identity_verified` | boolean | `true` for AVA's descriptor-bound local executor; `false` for delegated ACP execution. |

Session grant serialization (`permission_grants` result) includes additive `command_recipe_key` and `recipe_display` fields for reusable sealed commands. Grant matching uses the workspace recipe key, not raw command text or the instantaneous fingerprint. If policy/backend scope is `once`, an `allow_session` reply is downgraded to one-shot `allow`. Legacy v1 persistent command Allows are ignored; authoritative Denies remain active.

### Question request/reply

`question_requested.payload` includes `resolver_request_id`, `header`, `question`, option objects (`value`, `label`), and `multiple`, `allow_custom`, `secret`, `modal`, `searchable`. A single-select reply uses one of string `answer` or string `selected`. A multi-select reply may use string-array `selected_options` and, when allowed, string `answer`. Values must match current options; wrong types, duplicates, or invalid combinations do not resolve the pending question.

### Images

`prompt.attachments` is an array of non-empty local image paths. `prompt.images` is an array of `{"type":"image","data":"<standard-base64>","mimeType":"image/png"}` (snake-case `mime_type` is accepted when aliases agree). Combined input is at most 16 PNG/JPEG/WebP/GIF images. AVA verifies regular files/bytes, MIME/signature, limits, hash, and session-owned storage before provider use; responses/session messages expose metadata, never uploaded raw bytes.

## Lifecycle, Admission, And Interleaving

Structural validity above is independent of timing. At most one active run exists. Run kinds are prompt/follow-up, direct bash command, and compaction. Read-only state/catalog/grant queries documented as active-safe may interleave; session/history/policy/model mutations and a second run return `active_run`.

`steer` and `follow_up` are admitted only for an active prompt/follow-up run, never for `run_bash`, `run_command`, or `compact`. `steer` responds immediately after queueing and is applied only at a safe provider boundary. `follow_up` deliberately delays its response until it runs or is skipped. Queues are independently capped at 64 entries and 64 KiB of message bytes.

For a follow-up transition, AVA emits the parent response first. It does not change the active request ID while selecting the child. Immediately before `follow_up_started`, AVA activates the child; that event and all child runtime events use the child ID. Events and unrelated responses can interleave, so clients must correlate by IDs rather than assume response order.

`cancel` sets cooperative cancellation, releases permission/question waits fail-closed, clears queues, emits skipped events, responds to `cancel`, and emits terminal responses/events as workers observe cancellation. EOF and stdout write failure perform the same resolver/queue release without an arbitrary user-response timeout. Provider requests already sent are not guaranteed to be aborted or deduplicated.

## Command Catalog

The payload column defines recognized fields. Unless marked required, listed fields are optional. Admission is defined once in the lifecycle section rather than repeated per command.

<!-- command-catalog:start -->
| `type` | Recognized payload and result |
| --- | --- |
| `get_protocol` | No payload. Returns protocol/event/session versions and capabilities. |
| `get_state` | No payload. Returns current session, workspace, mode, provider/model, cancel, reasoning, and context state. |
| `prompt` | Required string `message`; optional `attachments`, `images`. Streams a turn and returns prompt result. |
| `steer` | Required string `message`. Queues safe-boundary guidance; returns `queued` and `correlation_id`. |
| `follow_up` | Required string `message`. Queues the next prompt; response is deferred. |
| `cancel` | No payload. Returns cancellation flag, active-run boolean, and cleared queue counts. |
| `permission_reply` | Required strings `request_id`, `correlation_id`, `decision`; optional string `reason`. Resolves a permission request. |
| `question_reply` | Required strings `request_id`, `correlation_id`; reply uses `answer`, `selected`, or string-array `selected_options`. |
| `permission_grants` | No payload. Lists process-local exact-match session grants. |
| `permission_grant_revoke` | Required string `grant_id`. Returns and emits the revoked grant. |
| `permission_grants_clear` | No payload. Returns/emits cleared count. |
| `permission_rules` | No payload. Lists persistent global/workspace rules and files. |
| `permission_rule_add` | Required strings `action`, `operation`, `reason`; optional `scope`, `mode`, `tool_name`, `target_path`/`path`, `command`, `command_recipe_key`, `recipe_display`, and boolean `critical_acknowledged`. Adds/emits a rule under backend validation. |
| `permission_rule_remove` | Required string `rule_id`. Removes/emits a persistent rule. |
| `get_messages` | No payload. Returns bounded visible message-like entries; hides internal replay and secrets/signatures. |
| `get_session_stats` | No payload. Returns bounded entry/usage/cost/type statistics. |
| `validate_session` | No payload. Returns replay validation issues and counts. |
| `list_sessions` | No payload. Lists workspace sessions. |
| `list_jobs` | No payload. Returns owner-scoped bounded public job snapshots without terminal content. |
| `get_job` | Required string `job_id`. Returns one owner-scoped public status snapshot without terminal content. |
| `wait_job` | Required string `job_id`; optional positive integer `timeout_ms`, capped at 30000 (default 1000). Returns a snapshot with `timed_out:true` when still running. |
| `get_job_result` | Required string `job_id`. Returns bounded safe terminal content, or stable `job_not_ready`. |
| `cancel_job` | Required string `job_id`. Requests cooperative cancellation and returns status. |
| `promote_job` | Required string `job_id`. Promotes a running foreground child and returns status. |
| `session_tree` | No payload. Returns roots, leaves, current path, and session nodes. |
| `session_metadata` | No payload. Returns current append-only metadata view. |
| `set_session_name` | Required string `session_name` (max 256 bytes). Appends metadata. |
| `set_session_labels` | Required string-array `labels` (max 32; unique; 64 bytes each). Appends metadata. |
| `new_session` | No payload. Creates and activates a session; returns state with `created:true`. |
| `open_session` | Required string `session_id`. Compatibility alias for `switch_session`. |
| `switch_session` | Required string `session_id` or unambiguous prefix. Activates and returns state. |
| `fork_session` | Optional strings `session_id`, `branch_from_entry_id`, `session_name`; optional `labels`. Copies through a source entry and activates the fork. |
| `clone_session` | Optional strings `session_id`, `session_name`; optional `labels`. Full-copy clone; `branch_from_entry_id` is invalid. |
| `summarize_branch` | Required strings `branch_root_entry_id`, `branch_tip_entry_id`, `summary`, `provider`, `model`, `reason`; optional `session_id`. Appends caller-supplied summary. |
| `list_models` | No payload. Returns effective selectable catalog and capability/reasoning metadata. |
| `set_model` | Required string `model`; optional string `provider`. Switches after replay compatibility checks. |
| `cycle_model` | No payload. Selects next configured/scoped model. |
| `set_reasoning` | Required string `reasoning_level`; optional integer `reasoning_budget_tokens`, string `reasoning_display`. Validates against active model/API family. |
| `clear_reasoning` | No payload. Clears explicit reasoning controls and persists change when needed. |
| `run_bash` | Required non-empty string `command`. Runs direct permissioned `/bash`, emits tool/resolver events, returns command result. |
| `run_command` | Same as `run_bash`; v1 compatibility alias, not arbitrary slash dispatch. It remains supported for the lifetime of protocol v1; new clients should prefer `run_bash`. |
| `compact` | Optional string `instructions`. Runs provider-backed `/compact`, emits lifecycle/retry events, returns command result. |
| `export` | No payload. Returns Markdown export through command result text/output. |
| `export_html` | Optional string `outputPath` or `output_path` (aliases must agree). Returns HTML or permissioned write summary. |
| `context` | No payload. Returns loaded prompt/context source diagnostics through command result. |
| `list_commands` | No payload. Returns unified built-in/prompt/skill/plugin/MCP command registry. |
| `invoke_command` | Required string `name`; optional string `command_arguments`. Dispatches a named registry command; prompt commands become prompt runs. |
| `list_plugins` | No payload. Lists local discovered plugin state. |
| `plugin_failures` | No payload. Lists plugin diagnostics. |
| `inspect_plugin` | Required string `plugin_id`. Returns plugin inspection output. |
| `install_plugin` | Required string `path`. Installs a local plugin directory through permissioned plugin command handling. |
| `remove_plugin` | Required string `plugin_id`. Removes a managed local plugin. |
| `enable_plugin` | Required string `plugin_id`. Records enablement. |
| `disable_plugin` | Required string `plugin_id`. Records disablement. |
| `validate_plugin` | Required string `path`. Validates a local manifest/path. |
| `list_plugin_prompts` | Required string `plugin_id`. Lists static prompt resources. |
| `get_plugin_prompt` | Required strings `plugin_id`, `name`. Returns one prompt resource. |
| `list_plugin_skills` | Required string `plugin_id`. Lists static skill resources. |
| `get_plugin_skill` | Required strings `plugin_id`, `name`. Returns one skill resource. |
| `run_plugin_command` | Required strings `plugin_id`, `name`; optional object `arguments` (legacy stringified object accepted). Runs through plugin permissions/events. |
| `list_mcp_servers` | No payload. Lists configured stdio MCP servers. |
| `inspect_mcp_server` | Required string `server_id`. Returns server diagnostics/config summary. |
| `list_mcp_tools` | Required string `server_id`. Starts bounded discovery and returns tools. |
| `restart_mcp_server` | Required string `server_id`. Informational for current per-operation stdio processes. |
<!-- command-catalog:end -->

### Active-safe commands

While a run is active, `get_protocol`, `get_state`, `list_models`, `list_sessions`, `list_jobs`, `get_job`, `wait_job`, `get_job_result`, `cancel_job`, `promote_job`, `session_tree`, `session_metadata`, `permission_grants`, `permission_grant_revoke`, and `permission_grants_clear` remain available. The grant revoke/clear exceptions are genuinely concurrent-safe: they mutate only the separately mutex-protected process-local grant collection and do not mutate the active session/run controller. Persistent permission-rule commands are not active-safe. Other materializing/mutating commands are rejected. Resolver replies and `cancel` remain available because they settle active work.

## Event Catalog

All events below are ordinary records on the **same stdout JSONL stream** as responses. “Resolver” describes lifecycle semantics, not a second control channel; resolver replies are client requests on stdin.

Runtime events:

- Session/message: `session_start`, `user_message`, `message_update`, `message_end`, `assistant_message`. `session_start` has `payload_type:"session"`; its payload contains string `mode`, `provider`, and `model` (plus any additive common fields emitted by a future runtime).
- Provider/reasoning: `provider_event`, `reasoning_start`, `reasoning_delta`, `reasoning_end`. `provider_event` has `payload_type:"provider"`; its currently populated generic payload fields may include string `text`, `status`, `trigger`, `reason`, error metadata, and retry/counter metadata. Clients must accept an empty payload and additive provider-specific fields.
- Tools: `tool_start`, `tool_progress`, `tool_result`.
- Compaction/retry: `compaction_start`, `compaction_end`, `retry`, `retry_tick`. Compaction payloads add privacy-safe `trigger` plus normalized `reason` (`manual`, `automatic`, or `overflow`), selected `provider`/`model`, active `estimated_tokens`, effective `threshold_tokens`, `retained_tokens`, and `post_compaction_tokens` when known. Overflow retry remains bounded to one replay. The immutable checkpoint marks that replay as `scheduled`; the subsequent terminal runtime event reports whether the retried run completed or failed, rather than rewriting the checkpoint. Raw provider payloads and private reasoning metadata are never included.
- Terminal: `canceled`, `error`, `done`.

RPC control/resolver events:

- `cancel_requested` reports active request and cleared queue counts.
- `permission_requested`, `permission_replied`, `permission_grant_revoked`, `permission_grants_cleared`, `permission_rule_added`, `permission_rule_removed`.
- `question_requested`, `question_replied`.
- `steer_queued`, `steer_applied`, `steer_skipped`, `follow_up_queued`, `follow_up_started`, `follow_up_skipped`. Queue payloads contain bounded `message`; skipped payloads contain `reason`; truncation adds `message_truncated` and `message_bytes`.

Terminal `done` contains stop/counter metadata; `canceled` is cooperative cancellation; runtime `error` is an event inside a run and does not replace the command's exactly-one response.

## Examples

Protocol discovery:

```text
> {"id":"protocol","type":"get_protocol","protocol_version":1}\n
< {"id":"protocol","type":"response","success":true,"result":{"protocol_version":1,"supported_protocol_versions":[1],"event_schema_version":1,"supported_event_schema_versions":[1],"session_entry_version":4,"supported_session_entry_versions":[0,1,2,3,4],"capabilities":["direct_bash_rpc","job_controls"],"direct_command_types":["run_bash","run_command","list_jobs","get_job","wait_job","get_job_result","cancel_job","promote_job"]}}\n
```

Prompt success with tool summary/timeline (events for this run may appear before the response):

```json
{"id":"p1","type":"response","success":true,"result":{"session_id":"session_1","final_text":"Updated a.txt","stop_reason":"completed","provider_iterations":2,"tool_calls":1,"tool_timeline":[{"status":"success","call_id":"call_1","tool":"write_file","argument_summary":"a.txt","result_summary":"wrote file","changed_paths":["a.txt"],"permission_request_ids":["permreq_1"]}]}}
```

Command success (`context`, `export`, `run_bash`, and other command-dispatch results use this common shape):

```json
{"id":"cmd1","type":"response","success":true,"result":{"handled":true,"quit":false,"output":["line one","line two"],"text":"line one\nline two"}}
```

`tool_timeline` is omitted when empty by current production serializers, so clients must treat it as optional.

State and malformed recovery:

```text
> {"id":"state","type":"get_state"}\n
< {"id":"state","type":"response","success":true,"result":{"protocol_version":1,"session_id":"session_..."}}\n
> not-json\n
< {"id":"","type":"response","success":false,"error":{"category":"invalid_argument","code":"invalid_request","message":"malformed RPC JSON object","details":"..."}}\n
```

Permission flow (events may be interleaved with other responses):

```json
{"id":"p1","type":"prompt","message":"edit the file"}
{"schema_version":1,"name":"permission_requested","request_id":"p1","correlation_id":"p1","payload":{"resolver_request_id":"permission_1","permission_request_id":"permreq_1","operation":"edit","mode":"build","target_path":"/work/a","command":"","tool_name":"edit_file","reason":"approval required","risk":"high"}}
{"id":"reply1","type":"permission_reply","request_id":"permission_1","correlation_id":"p1","decision":"deny","reason":"not in this run"}
```

Follow-up ordering:

```text
< follow_up_queued(request_id=fu1, correlation_id=p1)
< response(id=p1)
< follow_up_started(request_id=fu1, correlation_id=fu1)
< ...child events...
< response(id=fu1)
```

A source checkout also contains a standard-library-only client under `examples/rpc-client/`; examples and source trees are intentionally not installed in the host artifact.

## RPC v1 Contract-Delta Ledger

These are compatibility corrections, not a protocol-version fork. No coherent-client break without a compatibility path was found.

| Correction | Prior behavior | Intended v1 | Why prior traffic is malformed/contradictory | Compatibility/release note |
| --- | --- | --- | --- | --- |
| UTF-8 | Raw invalid bytes could reach JSON extraction/escaping. | Validate before parse; empty-ID recoverable error; valid UTF-8 stdout only. | JSON text is Unicode and invalid bytes cannot form coherent JSON traffic. | Valid UTF-8 clients unchanged. |
| Recognized optional types | Some wrong-typed fields collapsed to absent/default. | Present recognized fields have exact types; unrelated/additive fields remain ignored. | A present enum/string with another JSON type contradicts its command schema. | Omission/default behavior is unchanged; malformed senders get `invalid_request`. |
| Run kind | One boolean allowed steer/follow-up against direct/compact work. | Queue controls require prompt/follow-up kind. | Direct bash/compact have no provider continuation boundary. | Prompt clients unchanged; invalid queue requests fail immediately. |
| Follow-up activation | Selecting a child changed `active_request_id` before parent response. | Parent response first; child activation immediately before `follow_up_started`. | Events/responses could contradict parent/child correlation. | Documented coherent ordering is now enforced. |
| Outstanding IDs | Duplicate active/deferred IDs were not rejected centrally. | IDs unique until response; duplicate leaves original untouched. | Two outstanding meanings for one correlation key are ambiguous. | Sequential ID reuse after response remains supported. |
| Error codes | Errors exposed category/message/details only. | Add stable `error.code`; retain all old fields. | Text is diagnostic, not a reliable branch contract. | Purely additive. |
| Protocol discovery | Event schema support was implicit. | Add current/supported event schema versions; omitted request protocol is v1. | Clients could not distinguish protocol and event evolution. | Purely additive; omission remains accepted. |
| Framing | LF/CRLF/final-record behavior lacked a frozen contract. | LF delimiter, strip one CR, accept non-empty unterminated final record. | Different line-reader assumptions split coherent records differently. | Matches existing AVA behavior and is now golden-tested. |
| Resolver release | Some shutdown/write paths relied on indirect wake-up ordering. | Cancel/EOF/write failure settle and notify all resolver waits, without short timeout. | An orphaned resolver can deadlock clean shutdown. | Successful reply timing is unchanged. |

Release note: clients that previously sent wrong-typed recognized optional fields, duplicate outstanding IDs, or prompt queue commands during direct/compact runs must correct those malformed requests. Unknown fields, omitted optional fields, CRLF, final unterminated records, legacy v1 omission, and all coherent response/result fields remain compatible.

## Compatibility And Troubleshooting

- Ignore unknown object members and event names; require only fields this document marks required.
- Correlate every response by `id` and every run event by `request_id`/`correlation_id`; never assume a cancel response precedes the canceled prompt response.
- On `active_run`, wait for/settle the active response rather than retrying with a new ID in a tight loop.
- On `invalid_request`, log `message`/`details`, but branch on `code`; verify exact JSON types and outstanding-ID reuse.
- If parsing fails, verify stdout was not merged with stderr, split only on LF, decode strict UTF-8, and retain partial bytes until LF or EOF.
- If a prompt stalls, keep reading events and answer `permission_requested`/`question_requested`; do not impose an arbitrary short resolver timeout. Use explicit `cancel` for operator policy.
- EOF means disconnect: AVA cancels active work and does not start queued follow-ups. Keep stdin open for long-lived clients.
- Exit `0` means the RPC loop ended cleanly even if individual commands failed in-band. Exit `1` means startup or unrecoverable stdio/runtime failure; exit `2` is CLI usage error.
