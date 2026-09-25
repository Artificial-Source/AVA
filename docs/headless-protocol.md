# AVA Headless Protocol

This document defines the current backend contract for AVA headless modes. AVA supports one-shot print mode and a JSONL RPC MVP over stdin/stdout; provider streaming is exposed through runtime event envelopes. Server mode remains deferred.

## Modes

- `interactive`: current TUI behavior. Human prompts are allowed. Both stdin and stdout must be terminals; piped or redirected interactive startup is rejected. Use print or RPC for non-interactive runs.
- `print`: one prompt in, text or JSONL events out, process exits after the turn. Enable with `ava --print "prompt"`, `ava -p "prompt"`, `ava --mode text "prompt"`, `ava --mode json "prompt"`, or `printf 'prompt' | ava --print`.
- `rpc`: JSONL request/response envelopes over stdio for long-lived automation. Enable with `ava --rpc`, `ava --output rpc`, or `ava --mode rpc`.
- `server` (deferred): no contract yet. Server mode is explicitly out of scope until the stdio RPC contract is proven.

## Session Selection

AVA creates a resumable append-only session by default for interactive, print, and RPC modes. `--continue` resumes the newest persisted session for the current workspace, and Pi-compatible `--resume`/`-r` are aliases. `--session <id-or-prefix>` resumes an exact session id or unique prefix, and Pi-compatible `--session-id <id-or-prefix>` is an alias. `--fork <id-or-prefix>` creates a new persisted branch from an exact session id or unique prefix, copies the selected source history, records fork metadata, and can be combined with `--name <name>` to set the new branch display name. `--name <name>` on a non-fork startup records session metadata for the opened session. `--session-dir <dir>` selects the session storage directory for this process before resume, fork, continue, or new-session creation.

`--session`/`--session-id`, `--continue`/`--resume`/`-r`, and `--fork` are mutually exclusive. `--no-session` starts an ephemeral session for the current process. Runtime entries remain available to in-process commands such as `get_messages`, `/stats`, or `/export`, but AVA does not write a resumable JSONL history file under the configured sessions directory and the session is not returned by session listing. Temporary attachment and spill files may exist only while the process/session store is alive. `--no-session` cannot be combined with `--session`/`--session-id`, `--continue`/`--resume`/`-r`, or `--fork`. RPC `get_state` includes `sessionless:true` when this mode is active.

## Startup Reasoning

`--thinking off|<level>` is a Pi-compatible startup alias for AVA's existing reasoning control. `off` clears the active explicit reasoning selection. Any other value is validated against the active model's resolved reasoning policy (`reasoning_level_map`/`thinking_level_map` plus `reasoning_levels`) and provider/API-family rules during startup; for the default GPT-5.5 profile this means `low`, `medium`, `high`, or `xhigh`. Unsupported levels fail with an error that includes `option: --thinking` and the supported levels. When a model map rewrites the user-facing level, AVA persists the user-facing level and sends the resolved provider level in provider requests.

## Offline Mode

`--offline` can be combined with interactive, print, or RPC startup. It sets the session offline flag and disables provider model calls for prompt turns and provider-backed compaction before credential resolution. Print prompt turns fail closed with `permission_denied`, action `prompt` or `compact`, message `offline mode is enabled; provider model calls are disabled`, and hint `rerun without --offline to send prompts to the provider`. RPC provider-backed requests return in-band `success:false` while local non-provider commands remain available. `--offline` does not grant tool permissions and is not an OS/network sandbox; network-capable tools remain controlled by tool visibility plus permission policy.

## Print Input And Output

Print mode accepts an optional prompt argument after `--print`/`-p`. If no prompt argument is supplied and stdin is not a terminal, stdin is used as the prompt. If both a prompt argument and piped stdin are supplied, AVA merges them deterministically as:

```text
<explicit prompt>

<stdin content>
```

Text output is the default. In text output, stdout contains the final assistant text only; diagnostics, tool progress, and errors go to stderr. When stdout or stderr is a TTY, AVA sanitizes terminal control bytes before writing model/tool text to that stream; non-TTY pipes keep raw text for automation. JSONL output is selected with `--json`, `--output json`, or Pi-compatible `--mode json`; stdout contains serialized event envelopes and ends with `done`, `canceled`, or `error` for turns that reach the runtime. Credentials and startup failures are reported on stderr.

## Tool Visibility Flags

Tool visibility flags filter which native model-visible tool names are exported to the provider and accepted by the dispatcher for the current process. They apply to interactive, print, and RPC sessions:

- `--tools name[,name...]` or `-t name[,name...]`: allowlist tool names.
- `--exclude-tools name[,name...]` or `-xt name[,name...]`: remove tool names. Exclusion overrides allowlisting.
- `--no-builtin-tools` or `-nbt`: hide AVA built-ins while leaving enabled plugin and MCP tools available unless other filters remove them.
- `--no-tools` or `-nt`: hide all tools unless `--tools` explicitly re-enables exact names.

These flags are not permission grants. Headless permission flags such as `--allow-tool read_file` only answer compatible backend permission prompts for tools that remain visible and callable.

AVA accepts Pi built-in names as input aliases for visibility filters: `read` maps to `read_file`, `write` to `write_file`, `edit` to `edit_file`, `find` to `glob`, and `ls` to `list_directory`. Provider schemas remain AVA-native, so `--tools read,grep,find,ls` advertises `read_file`, `grep`, `glob`, and `list_directory`.

## Headless Permission Flags

Headless modes are fail-closed by default for backend permission decisions whose action is `ask`. Print mode installs an explicit deny resolver when no policy flag is provided. RPC mode emits a `permission_requested` event for ask prompts unless a supplied headless policy auto-allows the request. Permission decisions whose action is already `allow` proceed without a resolver, and decisions whose action is already `deny` are never upgraded by these flags.

Supported policy flags:

- `--allow read-only`: allows read/search-style permission prompts (`read_file`, `list_directory`, `glob`, and `grep` shapes) when the backend asks. Network, write, edit, patch, bash, and question prompts remain denied.
- `--allow-tool glob,grep,list_directory,read_file,skill,task,webfetch,websearch,mcp,plugin`: accepts only the listed tool families. The flag auto-allows compatible Ask prompts for `glob`, `grep`, `list_directory`, `read_file`, `skill`, `webfetch`, `websearch`, `mcp`, and `plugin`; unsupported values such as `bash`, `write_file`, `edit_file`, `apply_patch`, `question`, or arbitrary strings are rejected as usage errors. `task` remains accepted for compatibility, but task launch is already automatically allowed and audited, so it has no launch prompt to approve. Foreground child operations that independently Ask use the normal parent permission UI; background child operations that Ask fail closed. `skill` only auto-allows exact `skill` prompts from the `skill` tool, `webfetch` only auto-allows exact `network.fetch` prompts produced by the `webfetch` tool, `websearch` only auto-allows exact `network.search` prompts produced by the `websearch` tool, `mcp` auto-allows MCP server launch/connect plus `mcp.tool.call` and `mcp.resource.read` prompts for MCP-prefixed tools, and `plugin` auto-allows exact plugin model tool launch/call prompts plus exact plugin command launch/run prompts. `plugin` does not auto-allow plugin proxy file/search/shell/network prompts or passive plugin event hook launch/observe prompts. `--allow read-only` never allows skill, task, network, MCP, or plugin prompts.

Examples:

```sh
ava --print "summarize the repo" --allow read-only
ava --print "inspect this file" --allow-tool read_file
ava --print "find symbols" --allow-tool glob,grep,list_directory
ava --print "load the relevant skill" --allow-tool skill
ava --print "delegate focused exploration"
ava --print "fetch release notes" --allow-tool webfetch
ava --print "search current release notes" --allow-tool websearch
ava --print "use configured MCP context" --allow-tool mcp
ava --print "use configured plugin command" --allow-tool plugin
ava --rpc --allow read-only
```

Invalid permission flag values exit with code `2` and write a usage error to stderr before provider/auth startup. In RPC mode, matching read/search, exact `skill`, exact `webfetch`/`websearch` network prompts, MCP prompts covered by `--allow-tool mcp`, or supported plugin prompts covered by `--allow-tool plugin` are auto-allowed before `permission_requested`; non-matching ask prompts still require an explicit `permission_reply` unless a persistent permission rule matches. RPC clients may reply with `allow_session` to create an in-memory exact-match grant for the current RPC process. Those grants are inspectable, revocable, and clearable through RPC commands; they are not persisted across AVA restarts. Persistent rules are managed only by `permission_rule_add`/`permission_rule_remove` and are stored outside the model-writable workspace path.

## Stdout / Stderr Contract

- In headless `print` and `rpc` modes, stdout is protocol output only.
- Human-readable diagnostics, startup warnings, and fatal errors go to stderr.
- Protocol events are newline-delimited JSON objects. Writers must not emit partial JSON records.
- Consumers should treat unknown envelope fields as forward-compatible metadata and unknown event names as non-fatal unless the enclosing request fails.

## Exit Codes

- `0`: success; in RPC mode, the protocol loop exited cleanly, even if individual requests returned in-band `success:false` responses.
- `1`: print-mode runtime/provider/tool/permission failure, unavailable headless interaction, RPC startup failure, or unrecoverable RPC stdio read/write failure.
- `2`: command-line usage error. Recoverable malformed RPC request records produce `success:false` responses and do not change the process exit code by themselves.

## Provider Credentials

Runtime provider calls resolve credentials for the active session provider. OpenAI keeps its existing stored OAuth/API-key behavior and falls back to `OPENAI_API_KEY` when no stored OpenAI credential exists. Non-OpenAI connect setup stores provider-scoped API keys in `auth.json`, for example `{"anthropic":{"type":"api_key","api_key":"..."}}`, and can also read provider API-key environment variables such as `ANTHROPIC_API_KEY`, `KIMI_API_KEY`, `MOONSHOT_API_KEY`, and `OPENROUTER_API_KEY`. Anthropic can additionally read Claude OAuth bearer tokens from `ANTHROPIC_OAUTH_TOKEN` or the Anthropic SDK-compatible `ANTHROPIC_AUTH_TOKEN`; when no stored Anthropic credential exists, precedence is `ANTHROPIC_OAUTH_TOKEN`, then `ANTHROPIC_AUTH_TOKEN`, then `ANTHROPIC_API_KEY`. Stored Anthropic Claude OAuth entries use `{"anthropic":{"type":"oauth","access_token":"...","refresh_token":"...","expires_at":1893456000}}` with optional `account_id`/`source` metadata. AVA writes canonical `refresh_token`/`expires_at` fields and accepts `refresh`/`expires` aliases when reading manually-created files. Interactive setup is available with `ava login [provider]`, `ava auth login [provider]`, `ava connect [provider]`, and TUI `/connect`; omitting the provider opens a searchable provider picker, and TUI `/connect` uses a modal. OpenAI interactive setup offers browser OAuth, headless OAuth, and API key methods. Secrets are read with terminal echo disabled where possible and masked in TUI prompts. Headless setup can store credentials without a browser or TTY, for example `ava connect openai --headless-oauth`, `printf '%s\n' "$ANTHROPIC_API_KEY" | ava connect anthropic --api-key-stdin`, or `ava connect moonshot --api-key-env MOONSHOT_API_KEY`. AVA only reads credential files when they are owned by the current user and not readable by group/other users; manually-created files should use owner-only permissions such as `chmod 600 ~/.config/ava/auth.json`. OpenAI and Anthropic OAuth credentials refresh automatically before use when a refresh token is present; expired/near-expiry stored Anthropic OAuth without a refresh token fails closed and requires updating or removing the stored Anthropic entry before environment credentials are considered.

The direct runtime set also recognizes `DEEPSEEK_API_KEY` and `GEMINI_API_KEY`, and headless API-key setup can store Gemini credentials with `ava connect gemini --api-key-env GEMINI_API_KEY`.

## Provider Retry And Idempotency

Provider transport retries are bounded and provider-neutral. AVA retries transport I/O failures, HTTP rate limits, and transient HTTP responses before any streaming body chunks are delivered. It parses `Retry-After` when present, emits `retry` and `retry_tick` events for visible backoff, and checks cancellation during retry waits.

Retry policy by request class:

- OpenAI, Anthropic, DeepSeek, Gemini, Kimi, Moonshot, and OpenRouter prompt requests are best-effort retries. AVA does not currently attach provider-specific idempotency keys, so a provider could process a request even if the client later sees a transport failure.
- Streaming prompt retries stop after the first response chunk is delivered. Once text, reasoning, tool-call, or provider events have begun, AVA treats the stream as non-replayable and surfaces later failures instead of silently retrying a partial turn.
- Non-streaming prompt retries may retry transient/rate-limited failures because no response body has been accepted yet. They are still best-effort because provider-side deduplication is not guaranteed.
- Context-overflow repair is separate from transport retry. AVA may perform one bounded compaction/retry path when the provider error is classified as context overflow.

Headless clients should treat repeated prompt attempts as possible duplicate provider requests unless a future protocol version exposes provider-specific idempotency metadata.

## Event Envelope

Headless JSON event records are newline-delimited envelopes emitted by the shared runtime event bus:

```json
{"schema_version":1,"event_id":"event_...","timestamp":"2026-04-30T00:00:00Z","session_id":"session_...","name":"assistant_message","type":"assistant_message","payload":{"text":"hello"},"text":"hello"}
```

AVA's backend/frontend content boundary is semantic. Backend producers emit event names, lifecycle/status metadata, tool/permission/question payload fields, and Markdown or plain text. They do not emit terminal layout instructions, cards, panels, borders, columns, ncurses attributes, or ANSI escape codes as the internal styling model. Frontend adapters convert Markdown/plain text into `ava::tui::Text`, a layout-free sequence of `TextRun` values with explicit `NewLine` runs and styled `TextSpan` runs. The TUI renderer owns wrapping, scrolling, layout, terminal sanitization, SGR conversion, and ncurses drawing.

Envelope fields:

- `schema_version`: integer schema version. The current value is `1`.
- `event_id`: AVA-owned unique id for this emitted event record.
- `timestamp`: event timestamp when available.
- `session_id`: active runtime session id when available.
- `run_id`, `turn_id`, `message_id`, `request_id`, `correlation_id`: optional correlation metadata. RPC prompt and command events include `request_id` for the client request that caused the event.
- `name`: event name.
- `payload_type`: optional stable payload family for typed consumers. Runtime events use values such as `session`, `message`, `reasoning`, `provider`, `tool`, `compaction`, `retry`, `cancellation`, `error`, and `completion`; resolver/queue events may use `permission`, `question`, or `queue`.
- `payload`: event-specific object. Payloads are intentionally minimal and must not require clients to parse session JSONL internals.
- `type` and documented runtime fields such as `text`, `tool`, `status`, `category`, `message`, `stop_reason`, `trigger`, `reason`, and small counters: compatibility aliases for the pre-envelope flat event shape. New clients should prefer `name` and `payload`; unknown future payload fields are not automatically promoted to the top level.

Current event names:

- `session_start`: runtime session metadata with `mode`, `provider`, and `model`. Persisted JSONL `session_start` entries record prompt override and loaded context-source metadata; detailed base prompt source path, byte count, and fingerprint are exposed by `/context`, not this event.
- `user_message`: accepted user input for a turn.
- `message_update`: live assistant text delta emitted while a streaming provider response is in progress; includes `text` and `status`.
- `message_end`: live provider stream completion marker; includes `status`.
- `provider_event`: live non-text provider stream event such as tool-call argument deltas or provider stream errors; includes `status`, and may include `call_id`, `tool`, `text`, or `message` depending on the provider event.
- `reasoning_start`, `reasoning_delta`, `reasoning_end`: provider-neutral reasoning lifecycle events for models that expose visible thinking/reasoning. Payloads may include bounded `text` deltas, `reasoning_format`, `reasoning_redacted`, and `reasoning_signature_present`; they never expose raw provider verification signatures or opaque redacted-thinking payloads.
- `assistant_message`: final assistant text for a completed turn.
- `tool_start`: tool call began; includes `call_id`, `tool`, a safe argument summary when available, and may include structured `args` or fallback `args_json`.
- `tool_progress`: tool progress or partial result update; includes `call_id`, `tool`, `status`, bounded `text` when available, and may include partial `result` or fallback `result_json`.
- `tool_result`: tool call completed; includes `call_id`, `tool`, `status`, a safe result summary when available, and may include `args`, `result`, `structured_result`, `diff`, `changed_paths`, `permission_request_ids`, `diff_truncated`, output truncation/spill fields, and match/byte/line counters. `structured_result` is the canonical semantic tool result when present. It has `schema_version:1`, `call_id`, `tool`, `status` (`success`, `error`, or `canceled`), `ok`, `content_type`, optional `content`, optional `error`, optional `permission_request_ids`, and optional diff/path/truncation/spill counters. Flat `status` remains a compatibility field and mirrors the tool timeline lifecycle.
- `compaction_start`, `compaction_end`: provider-backed compaction lifecycle events. Payloads include `trigger`, `attempt`, `max_attempts`, and known token/summary byte counters when available.
- `retry`: bounded backend retry lifecycle event. Payloads include `attempt`, `max_attempts`, and `delay_ms` when the backend retry path has those values. Provider transport retries use `trigger:"provider_transport"` with provider-neutral reasons such as `rate_limited`, `transient`, or `transport_io`; context-overflow retries use `reason:"context_overflow"`; stale compaction snapshot retries use `reason:"stale_compaction_snapshot"`.
- `retry_tick`: backend retry countdown tick emitted while a bounded retry delay is sleeping. Payloads include `remaining_ms` plus the same retry correlation metadata where available. Clients should treat it as status/progress, not a final failure.
- `cancel_requested`: RPC cancel request was accepted. Payload includes `active_run`, `cleared_steer`, `cleared_follow_up`, and the active prompt request id when one exists.
- `permission_grant_revoked`, `permission_grants_cleared`: RPC session-grant lifecycle events. Payloads include the revoked grant or the number of grants cleared.
- `permission_rule_added`, `permission_rule_removed`: persistent permission rule lifecycle events emitted after successful rule changes. Payloads include the affected `rule` and, for removal, `removed:true`.
- `canceled`: terminal event for a cooperatively canceled runtime turn.
- `error`: runtime, provider, or tool boundary failure; includes `category`, `message`, and optional `details`.
- `done`: terminal event for a successful turn; includes `stop_reason` and small counters when available.
- `steer_queued`, `steer_applied`, `steer_skipped`: RPC steering queue lifecycle events. Payloads include `message` and skipped events include `reason`.
- `follow_up_queued`, `follow_up_started`, `follow_up_skipped`: RPC follow-up queue lifecycle events. Payloads include `message` and skipped events include `reason`.

Enabled plugin event hooks observe the same runtime event envelopes internally using canonical event names such as `tool_result`. Hook execution is best-effort and does not add new RPC envelope fields or make the originating prompt/command fail when a hook fails.

## RPC Mode

The normative AVA RPC v1 client-author contract is [`rpc-protocol.md`](rpc-protocol.md). It defines UTF-8/LF JSONL framing, versioning, request/response/error/event envelopes, correlation, command and event catalogs, active-run admission, resolver replies, compatibility corrections, examples, and troubleshooting. Do not infer AVA behavior from Pi RPC, JSON-RPC 2.0, ACP, or older examples in release ledgers.

Shared headless behavior remains in this document: startup/session selection, offline and tool-visibility flags, permission policy, stdout/stderr separation, exit codes, provider credentials/retries, and the runtime event envelope used by both print JSONL and RPC. RPC-specific clients should begin with `get_protocol` and follow the normative contract.

## Permission And Question Behavior

Headless operation is fail-closed by default:

- Permission decisions that require user approval fail unless headless policy supplies `--allow read-only`/`--allow-tool` for a supported read/search tool, `--allow-tool skill` for exact skill loads, `--allow-tool webfetch` for exact `network.fetch` webfetch prompts, `--allow-tool websearch` for exact `network.search` websearch prompts, `--allow-tool mcp` for MCP launch/connect/tool/resource prompts, or RPC mode receives an explicit `permission_reply` for the active resolver request.
- Permission-denied tool results include the backend reason, risk label, generated `permission_request_id`, and follow-up `/permissions audit show <permission_request_id>` plus `/permissions diagnose <permission_request_id>` commands in `structured_result.error.details`; non-command operations may also include a resolver reason. `bash`/`RunCommand` diagnostics use only a safe recipe display or `<redacted one-shot command>`, never raw argv, shell text, request payloads, or resolver failure text. Text print mode writes those details to stderr for human operators.
- The `question` tool fails with an unavailable interaction error unless RPC mode receives an explicit `question_reply` for the active resolver request.
- Destructive operations remain behind existing backend permission policy checks.

## Server Mode Deferral

No socket, daemon, HTTP server, or background service semantics are defined in this phase. Server mode must not be implemented by inferring behavior from `print` or `rpc`; it requires a separate contract review.
