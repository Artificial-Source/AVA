# AVA Tooling Plan

AVA tools are the model's hands. They must be small, predictable, permissioned, and easy to test.

## Tool Contract

Every tool has:

- Stable name.
- JSON-schema-like input definition.
- Short model-facing description.
- Central metadata for permission category, output bounds, execution mode, and event rendering hints.
- Permission requirements.
- Deterministic validation before side effects.
- Structured result with human text plus metadata.
- Output truncation rules.
- Cancellation support where applicable.

Tool metadata should stay generic by default, matching the lean backend maturity baseline. The metadata shape should still allow later provider/model-specific descriptions or availability choices, once AVA has the Phase 5 provider/model capability catalog.

For the backend MVP, the tool contract also becomes the foundation for plugin and MCP tools. Built-in tools, plugin tools, and MCP tools must share one registry path for schema validation, permission checks, source identity, event emission, audit records, cancellation, and output bounds. This registry work must preserve current built-in tool behavior before external tools are allowed through it.

Image reading remains deferred from the core text `read` tool. Multimodal image input now flows through session attachment metadata and provider serializers instead: AVA validates/copies stored attachments, reconstructs provider-neutral image content parts, and serializes verified bytes only for image-capable providers/models.

Every tool result should include:

- `ok`: boolean.
- `summary`: short human-readable result.
- `content`: bounded text output when useful.
- `metadata`: tool-specific structured details.
- `truncated`: boolean.

## MVP Tool Set

### read

Reads a file with line numbers.

Inputs:

- `path`
- `offset`
- `limit`

Rules:

- Normalize path through the safe file layer.
- Deny directories.
- Enforce max bytes and max lines.
- Mark long lines as truncated.

### list_directory

Lists readable entries in one directory without reading file contents.

Inputs:

- `path` optional.
- `max_entries` optional.

Rules:

- Normalize the directory path through the safe file layer.
- Return names, type, and size only.
- Omit entries denied by read/search policy.
- Bound entry count and output bytes.

### write

Creates or overwrites one file.

Inputs:

- `path`
- `content`

Rules:

- Permission check before mutation.
- Parent directory must exist unless explicitly allowed later.
- Writes must be atomic where practical.
- No partial write on failure.

### edit

Replaces exact text in one file.

Inputs:

- `path`
- `old_text`
- `new_text`

Rules:

- Fail if `old_text` is missing.
- Fail if `old_text` appears multiple times.
- Preserve raw bytes; exact matching is still the default behavior.
- Preserve line endings and UTF-8 BOM bytes during replacement.
- When an exact match fails, include actionable diagnostics for common CRLF/LF and BOM-at-start mismatches.
- Return a bounded unified diff preview with `diff_truncated` metadata.
- Serialize same-path mutations through the in-process mutation queue.
- Preserve original file if validation fails.

### apply_patch

Applies a structured multi-file patch.

Rules:

- Validate all affected paths first.
- Validate all hunks before applying any mutation.
- Apply atomically as a transaction where practical.
- Acquire affected path mutation locks in deterministic order before validation/staging/commit.
- Preserve existing all-validation-before-write behavior; cross-file commit can still fail partway because filesystem renames are per-file.
- Return per-file summary plus bounded unified diff preview and `changed_files` metadata.

### glob

Discovers files by pattern.

Inputs:

- `pattern`
- `path` optional.

Rules:

- Use project root or explicit safe directory.
- Respect gitignore by default.
- Skip symlinked file matches by default so search results do not advertise paths that `read_file` will reject; local/internal callers may explicitly opt in only when they own the follow-through behavior.
- Provider-visible glob calls cannot disable ignore filtering. Local/internal callers may use `no_ignore` only under explicit local control; permission checks, secret-path denial, workspace boundaries, `.git` exclusion, and output caps still apply.
- Limit result count.

### grep

Searches file contents.

Inputs:

- `pattern`
- `path` optional.
- `include` optional.

Rules:

- Use ripgrep-compatible semantics if `rg` is available, or native fallback later.
- Respect gitignore by default.
- Skip symlinked files by default so grep does not read through paths that `read_file` would reject; local/internal callers may explicitly opt in only when they own the follow-through behavior.
- Provider-visible grep calls cannot disable ignore filtering. Local/internal callers may use `no_ignore` only under explicit local control; permission checks, secret-path denial, workspace boundaries, `.git` exclusion, binary-file safeguards, and output caps still apply.
- Return file, line, and matched preview.
- Limit matches and output bytes.

Current native ignore support intentionally implements a small `.gitignore` subset without new dependencies: root and nested `.gitignore` files, blank/comment lines, `!` negation, leading `/` anchoring relative to the ignore-file directory, trailing `/` directory-only rules, unescaped trailing-space trimming, backslash escapes for literal characters, `*`, `?`, and common `**` wildcard behavior. Last matching rule wins. Bracket character classes are not modeled yet. Ignored directories are pruned during search; hardcoded generated-directory pruning remains as a default safety/performance fallback while `.git` stays excluded even for local/internal `no_ignore` callers.

### bash

Runs permissioned local commands through sealed command plans.

Inputs:

- `command`: required command string.
- `timeout_ms`: optional.
- `max_lines`: optional trailing-output line bound; `limit` is an alias.
- `max_bytes`: optional retained-output byte bound.

Rules:

- Use the runtime-selected `ToolContext::current_dir`, falling back to the workspace directory; the public tool does not accept a working-directory override.
- Enforce timeout.
- Enforce output byte limit.
- Kill process tree on cancellation or timeout.
- Never run detached by default.
- Permission scan before execution.
- Prefer argv-style execution for internal commands; shell string execution is isolated behind this single executor.

The current schema and dispatcher are `src/ava/agent/tool_metadata.h` and `src/ava/agent/tool_dispatch_bash.cpp`; runtime directory selection is in `src/ava/agent/agent_turn_executor.cpp`. Neither `workdir` nor `description` is a current input. A public structured `argv`/`cwd` lane remains unimplemented.

### webfetch

Fetches bounded HTTP(S) text content after explicit network permission.

Inputs:

- `url`
- `max_bytes` optional, capped by backend policy
- `timeout_ms` optional, clamped by backend policy

Rules:

- Validate the URL before permission or network access.
- Allow only `http://` and `https://` URLs.
- Deny userinfo, control bytes, empty hosts, IP literal hosts, numeric IP aliases, localhost names, `.localhost`, and `.local` names.
- For default network execution, resolve the host before fetching and reject private, loopback, link-local, multicast, reserved, documentation, shared, or otherwise non-global addresses.
- Request `network.fetch` permission for the validated URL; read-only file/search permission modes do not imply network access.
- Use a bounded GET with timeout and response byte cap. Redirects are disabled for `webfetch` until each redirected target can be revalidated and re-authorized.
- Return only 2xx responses as successful tool results.
- Return text-like bodies only; reject binary content types when known and reject bodies containing NUL bytes.
- Return bounded `content`, `content_type` when available, `status_code`, `truncated`, `total_bytes`, and `output_bytes` metadata.
- Do not perform web search, browser automation, credential handling, image attachment conversion, or provider-specific multimodal payloads in this tool.

### websearch

Searches the web for current sources after explicit network-search permission.

Inputs:

- `query`
- `num_results` optional, capped by backend policy.
- `context_max_chars` optional, capped by backend policy.
- `timeout_ms` optional, clamped by backend policy.

Rules:

- Validate query size and control bytes before network access.
- Request `network.search` permission for the query.
- Return bounded search results suitable for source discovery, not long-form reading.
- Prefer following with `webfetch` on a specific result URL when the model needs source detail.
- Do not perform browser automation, credential handling, or workspace mutation.

### question

Asks the user for structured input.

Inputs:

- `question`
- `choices` optional.
- `allow_custom` default true.

Rules:

- Availability requires a question resolver, not an interactive-mode flag. The TUI and bidirectional headless RPC install resolvers and can answer questions.
- Modes without a resolver, including one-shot print mode, return the `question resolver is unavailable` tool error rather than asking.
- The resolver gate is in `src/ava/agent/tool_dispatch_question.cpp`; RPC installs its resolver in `src/ava/app/rpc/prompt_worker.cpp`.

### skill

Loads a listed local or global `SKILL.md` instruction file into the conversation.

Inputs:

- `name`

Rules:

- Only load skills that the context loader has discovered and listed.
- Request `skill` permission before loading the selected skill content.
- Bound loaded skill content and sampled file lists.
- Treat skill text as untrusted instructions that augment context; it must not bypass tool permissions.

### task

Runs a foreground or background subagent in a child session.

Inputs:

- `description`: short label for the delegated work.
- `prompt`: full instruction for the child agent.
- `subagent_type`: `general`, `explore`, or a configured custom subagent from the available subagents list.
- `task_id` optional: resume an existing foreground child session.
- `command` optional: local command label from the caller.
- `background` optional: when true, start a tracked background child session and return immediately with `task_id` and `job_id`.

Rules:

- Automatically allow and audit task launch before starting the child session. `--allow-tool task` remains accepted for compatibility but is not needed for launch. Foreground child operations that independently Ask use the normal parent permission UI; background child operations that Ask fail closed.
- Create or reopen a child session under the same session root and link it to the parent through session metadata.
- Hide recursive `task` from child tool schemas. The built-in `explore` and any `tools: read-only` custom subagent expose only read/search/list tools.
- Background children require a runtime-owned background job registry plus fresh provider and transport instances. They do not inherit parent UI callbacks, LSP providers, or nested background factories.
- Child sessions are capped to a small bounded tool-iteration budget so runaway delegated work cannot monopolize a turn.
- Return bounded structured metadata including `task_id`, `session_path`, `state`, `stop_reason`, and `job_id` for background tasks. The child session remains the source of truth for full transcript details.

### LSP code intelligence

Queries diagnostics, document symbols, workspace symbols, definitions, and references from a locally configured language server.

Inputs:

- `lsp_diagnostics`: `path`
- `lsp_document_symbols`: `path`
- `lsp_workspace_symbols`: `query`
- `lsp_definition`: `path`, zero-based `line`, zero-based `column`
- `lsp_references`: `path`, zero-based `line`, zero-based `column`

Rules:

- Request `lsp.query` permission for the target path before any server query. Starting a configured LSP subprocess also requests explicit high-risk `lsp.server.launch` permission for the selected argv vector.
- Treat permission as read-like: deny secret paths, ask outside the workspace, and allow workspace files by default.
- Provider-visible input includes only the file path. Server command argv is local configuration/test harness state and is not model-controlled.
- Advertise provider schemas only when explicit servers or the globally opted-in built-in recipe produce a configured provider; otherwise LSP tools are unavailable to model calls.
- Load LSP server config from AVA-owned `lsp.json` files (`$AVA_CONFIG_DIR/lsp.json` and workspace `.ava/lsp.json`). The schema is `version:1` plus bounded explicit `servers[]`; owner-controlled global config may additionally use exact `builtin_servers:["clangd"]`. Project config cannot enable built-ins.
- Validate unique server ids, argv strings, extension filters, language ids, config size/ownership, strict integer fields, and timeout bounds before exposing schemas. Shared `AnchorSet` acquisition permits only symlink targets contained in the selected writable anchor; mixed arrays, wrong known-field types, duplicate ids, and control-byte arguments are rejected.
- Start the selected server lazily after `lsp.query` and identity-bound `lsp.server.launch` permission, using an explicit argv vector and logical root. Do not use a shell or launch servers during schema discovery/status reporting.
- Use JSON-RPC `Content-Length` framing, real advertised capabilities, and bounded pull or centrally routed publish diagnostics plus symbols/definitions/references.
- Synchronize bounded on-disk text with full-text `didOpen` and versioned `didChange`. Unsaved-buffer sync remains deferred.
- Percent-encode file URI path bytes while preserving real path separators, so literal encoded separators in filenames cannot cross the permission boundary.
- Bound startup and request timeouts independently, contain each LSP subprocess in a parent-verified process group before exec, and tear down the verified group on timeout, cancellation, leader exit, or client destruction.
- Return bounded structured diagnostics, symbols, or locations. Symbol, definition, and reference results normalize in-workspace file URIs to workspace-relative paths and include stable LSP ranges.
- Redact local server command and workspace details from provider-visible LSP failures; keep detailed process context for local diagnostics only.

Current non-goals: automatic language-server installation, automatic recipes beyond exact global installed-only `clangd`, workspace-wide diagnostics, unsaved-buffer sync, TUI rendering, plugin/MCP integration, provider registry changes, and new external dependencies.

## Permission Integration

Tools must not decide safety internally in an ad hoc way. They should produce permission requests and rely on the permission service.

Examples:

- `read` requests `file.read` for the normalized path.
- `write`, `edit`, and `apply_patch` request `file.write` for every affected path.
- File deletion requests `file.delete` and is not part of MVP write/edit behavior.
- `bash` requests `shell.run` and may request `shell.destructive` based on command scan.
- `webfetch` requests `network.fetch`.
- `websearch` requests `network.search`.
- `skill` requests `skill`.
- `task` requests `task` with the selected `subagent_type` as command context.
- LSP diagnostics/symbols/definitions/references request `lsp.query` for the target file path; workspace symbol search uses the workspace root as the permission target and the query as command context. Configured server startup separately requests `lsp.server.launch` with the exact JSON-array encoded argv vector in `command`.

## Output Truncation

Every tool output must be bounded.

Defaults to decide during implementation:

- Max output bytes per tool call.
- Max lines per read.
- Max grep matches.
- Max bash stdout bytes.
- Max bash stderr bytes.

If output is truncated, the result must say so and include enough metadata to re-run a narrower command.

Backend tools may also create a session-local spill file for truncated high-volume output. Spill files are additive local metadata: provider-visible `bash`, `glob`, and `grep` results include only the safe spill filename (`spill_file`) when one is written, plus `spill_truncated` when the spill itself hit the fixed per-file cap; absolute spill paths are not replayed to the provider. Spill directories live under the session file directory (for example `<session-dir>/spill`), are created lazily, and must never be placed in the workspace. Current stable spill formats are raw combined bash output, one glob path per line, and grep lines as `path:line:content`.

Long-running or high-output tools can emit coarse `tool_progress` runtime events with existing event fields: `text`, `call_id`, `tool`, and `status`. Progress is best-effort and additive; consumers that ignore it continue to observe the same tool start/result flow.

## Testing Priority

Tool tests are more important than UI tests early.

Required regression areas:

- Path traversal and external directory behavior.
- Symlink behavior.
- Exact edit ambiguity.
- Patch validation before mutation.
- Bash timeout and cancellation.
- Bash output truncation.
- Search `.gitignore` pruning, provider-inaccessible `no_ignore`, spill files, and `tool_progress` events.
- Webfetch URL validation, DNS pinning, redirect-disabled behavior, content-type filtering, and headless `network.fetch` permission.
- Websearch query validation, result bounding, provider failure shaping, and headless `network.search` permission.
- Skill discovery, bounded skill loading, permission behavior, and rejection of unavailable/unknown skills.
- LSP diagnostics/symbols/definitions/references permission, bounded `didOpen` sync, file URI encoding/decoding, timeout/size caps, provider error redaction, malformed response handling, and provider JSON bounds.
- OpenAI Responses tool-call event parsing, including `response.output_item.added` function-call items.
- Live headless smoke for every model-visible tool class: read/search/webfetch in print mode; write/edit/apply_patch/bash/question through RPC resolver replies; LSP through fake-server tests unless a local diagnostics provider is configured.
- Permission allow/ask/deny behavior.
- Nonexistent files and directories.
- Unicode and long-line handling.

## Tooling Rule

AVA should have fewer tools with excellent semantics rather than many tools with unclear behavior.
