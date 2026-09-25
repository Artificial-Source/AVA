# AVA Plugin And MCP Foundation

This document defines AVA's current 1.0 plugin and MCP foundation. It is grounded in external extension-system lessons, but adapted for AVA's constraints: native C++23, one binary, explicit permission boundaries, inspectable local files, and a core that keeps working when plugins fail.

Advanced extension features remain 1.1+ roadmap work.

## External Reference Lessons

The external reference systems use trusted TypeScript extensions loaded in-process. The relevant behavior areas are:

- Extension user-facing documentation.
- Extension type definitions.
- Extension loading and runner lifecycle code.
- Extension process/wrapper code.
- Package and skill resource documentation.

Useful external-baseline ideas:

- Extensions are easy to author: a small module gets an API object and calls `registerTool`, `registerCommand`, or `on` for events.
- Extension resources are discovered from global, project, package, and explicit CLI/config paths.
- Packages can bundle extensions, skills, prompts, and themes through package metadata.
- Tools have names, descriptions, schemas, execution modes, optional prompt snippets, and optional UI rendering.
- Runtime events cover session, input, agent, turn, provider request/response, message streaming, tool call/result, compaction, and model selection.
- Tool hooks can block calls or transform results, which lets extensions implement custom permission gates.
- Skills and prompt templates are filesystem resources, not compiled code.
- RPC mode streams events and can bridge extension UI prompts to non-TTY clients.

External-baseline limits that AVA should not copy directly:

- In-process extensions run with full user permissions. That trust model is clear, but it means a bad extension can crash or compromise the process.
- Native MCP support is not always part of the reference extension model, which pushes MCP through extension code instead.
- Broad extension APIs can expose UI customization, providers, and provider request interception. AVA should start narrower until safety, events, permissions, and sessions are stable.

## AVA Direction

AVA ships a small, stable, local plugin foundation for 1.0.

The default plugin shape is an out-of-process executable that speaks a versioned JSONL protocol over stdin/stdout. AVA owns discovery, enablement, validation, permission checks, event emission, audit/session records, cancellation, timeouts, output bounds, diagnostics, and lifecycle cleanup.

AVA does not load third-party native shared libraries in-process for 1.0. A native plugin ABI would make crashes, memory corruption, and C++ ABI compatibility part of the public support burden.

The compatibility rules for this surface live in [`docs/plugin-compatibility-policy.md`](../plugin-compatibility-policy.md). That policy defines compatible `ava.plugin.v1` additions, breaking-change handling, golden contract expectations, and MCP resource containment requirements.

## Goals

- A developer or AI assistant can create a useful plugin without reading AVA C++ internals.
- Built-in tools and plugin tools use the same registry, validation, permission, event, audit, and cancellation paths.
- Plugin crashes, hangs, malformed JSONL, invalid schemas, or unsupported API versions produce contained plugin failures.
- Project plugins do not execute until explicitly enabled by the user.
- Plugin permissions are inspectable, revocable, and visible in session audit records.
- MCP servers work through the same plugin/runtime safety model rather than as a bypass around AVA permissions.

## Non-Goals For 1.0

- Extension marketplace, remote install flow, or broad package manager.
- In-process native plugin ABI.
- Arbitrary TUI slots, theme plugins, or custom renderer plugins.
- Provider/message interception that can silently rewrite prompts or provider requests.
- Hard cross-platform sandbox guarantees for arbitrary executables.
- Trusting MCP servers as safe just because they speak MCP.

These are non-goals for the 1.0 plugin/MCP foundation, not discarded product ideas. Marketplace and remote install flows, richer UI bridges, and advanced remote MCP transports stay on the 1.1+ roadmap after the out-of-process safety model is proven. Native AVA `task` subagents are now a core agent feature, separate from the plugin foundation; plugin-contributed subagent packages remain future work until package provenance, trust, and compatibility policy exist.

## Plugin Discovery

Supported locations are explicit and inspectable:

- Global plugins: `$XDG_CONFIG_HOME/ava/plugins/<plugin-id>/plugin.json`, falling back to `~/.config/ava/plugins/<plugin-id>/plugin.json`.
- Project plugins: `.ava/plugins/<plugin-id>/plugin.json`.
- Explicit CLI/config paths can be added later for development, but should not be the primary stable interface.

Discovered executable plugins are disabled by default. AVA can inspect their manifests without starting a process, but it does not run entrypoints until the user enables the plugin. Enablement is machine-local state stored under `$XDG_STATE_HOME/ava/plugin-enablement.json`, falling back to `~/.local/state/ava/plugin-enablement.json`, keyed by canonical workspace path, plugin id, and scope.

`/plugins install <path>` imports a local plugin directory into the global plugin directory. The path must be a directory containing `plugin.json`, or a direct `plugin.json` path. Install validates the manifest, copies only directories and regular files, rejects symlinks and special files, leaves the plugin disabled, and never starts the entrypoint. `/plugins remove <id>` removes only disabled global plugins; project plugins remain repository files managed outside AVA.

## Plugin Authoring Guide

The stable 1.0 authoring surface is a local directory with a `plugin.json` manifest and an out-of-process entrypoint. Start from the checked-in sample at `examples/plugins/todo/`; it is intentionally small, uses POSIX shell, and is exercised by regression tests.

Recommended layout:

```text
.ava/plugins/com.example.todo/
  plugin.json
  plugin.sh
  prompts/todo-review.md
  skills/todo-triage.md
  README.md
```

Use the same layout under `$XDG_CONFIG_HOME/ava/plugins/<plugin-id>/` for a global plugin. Project plugins under `.ava/plugins/<plugin-id>/` are discovered from the active workspace but remain disabled until explicitly enabled on the local machine.

Authoring checklist:

- Choose a stable lowercase `id` such as reverse-DNS (`com.example.todo`) or an owner-prefixed id. It may contain letters, digits, `.`, `_`, and `-`, but cannot start empty, use uppercase, repeat separators, or end with a separator.
- Set `schema_version` to `1` and `api_version` to `ava.plugin.v1`.
- Declare an `entrypoint` with a command and optional string `args`. For portable shell samples, prefer `"command": "/bin/sh", "args": ["plugin.sh"]` so the plugin does not depend on executable file mode.
- List static `contributes.tools`, `contributes.commands`, `contributes.prompts`, `contributes.skills`, and `contributes.event_hooks` that AVA can inspect before running the plugin. Prompt and skill paths must be safe relative paths inside the plugin directory.
- Keep stdout reserved for LF-delimited JSON protocol records. Write diagnostics to stderr; AVA captures a bounded stderr tail for failures.
- Use a real JSON parser in non-trivial plugins. The shell sample only parses AVA's compact demo records and is not a general JSON parser.

Entrypoint behavior:

- AVA launches the plugin process with the plugin directory as the child working directory.
- AVA sends one `initialize` record before any tool, command, or event request.
- The plugin must answer every request with one LF-delimited JSON object and echo the request `id` exactly.
- `list`, `inspect`, `validate`, `prompts`, `prompt`, `skills`, and `skill` read metadata or static resources only; they do not start the plugin process.
- `dynamic-prompts`, `dynamic-prompt`, `dynamic-skills`, and `dynamic-skill` are intentionally separate because they start enabled plugin code to ask for runtime prompt/skill resources.
- `enable` and `disable` update local enablement state only; they do not start or stop a resident daemon.
- `/plugin run` and model-dispatched plugin tools start a fresh plugin process for the call path today, subject to permission checks and timeouts.

Minimum JSONL records:

```json
{"id":"ava_1","type":"initialize","api_version":"ava.plugin.v1","plugin_id":"com.example.todo","workspace":"/repo"}
{"id":"ava_1","type":"initialized","api_version":"ava.plugin.v1","plugin_version":"0.1.0","contributions":{"tools":[],"commands":[],"prompts":[],"skills":[],"event_hooks":[]}}
{"id":"ava_command_cmd_1","type":"command.call","command":"status","arguments":{},"context":{"call_id":"cmd_1","workspace":"/repo"}}
{"id":"ava_command_cmd_1","type":"command.result","ok":true,"content":"Todo sample plugin is ready.","metadata":{"open_items":0}}
{"id":"ava_tool_call_1","type":"tool.call","tool":"todo_add","arguments":{"text":"write tests"},"context":{"call_id":"call_1","workspace":"/repo"}}
{"id":"ava_tool_call_1","type":"tool.result","ok":true,"content":"Todo item accepted by the sample plugin.","metadata":{"items":1}}
{"id":"ava_event_event_1","type":"event.observe","event":"tool_result","payload":{"tool":"read_file","status":"success"},"context":{"call_id":"event_1","workspace":"/repo"}}
{"id":"ava_event_event_1","type":"event.observed","ok":true,"content":"Todo sample observed the event.","metadata":{"events":1}}
```

Static prompts and skills are plain markdown resources. AVA reads them through `/plugins prompt <id> <name>` and `/plugins skill <id> <name>` after manifest validation. The host opens each resource beneath one descriptor for the plugin directory, rejects escaping intermediate symlinks, final symlinks, non-regular files, and content over 64 KiB, then parses only the bytes read from that exact descriptor; the logical manifest path remains presentation metadata rather than read authority. Runtime context and `/context` retain this bounded snapshot instead of reopening the pathname. Static resources remain useful when the plugin entrypoint is disabled or failing. When a plugin is enabled, AVA autoloads its static prompts into runtime context and lists its static skills in `available_skills`; this path honors project trust for project plugins and never launches the plugin process or asks the dynamic resource protocol for content.

Dynamic prompts and skills are generated by an enabled plugin process over the same JSONL runner. They are opt-in: the manifest must declare `dynamic.prompts` before AVA sends prompt resource requests, and `dynamic.skills` before AVA sends skill resource requests. AVA skips enabled plugins without the matching capability during list commands, and direct reads fail before process launch when the capability is absent. Dynamic resources are never mixed into the static `/plugins prompt` or `/plugins skill` paths, so static resources remain inspectable manifest files and cannot accidentally execute code. Use dynamic resources only when a prompt or skill must be computed from runtime state.

Dynamic resource records:

```json
{"id":"ava_resource_...","type":"resource.list","kind":"prompt","context":{"workspace":"/repo"}}
{"id":"ava_resource_...","type":"resource.list.result","ok":true,"kind":"prompt","resources":[{"name":"review","description":"Generate a review prompt"}]}
{"id":"ava_resource_...","type":"resource.read","kind":"skill","name":"triage","context":{"workspace":"/repo"}}
{"id":"ava_resource_...","type":"resource.read.result","ok":true,"kind":"skill","name":"triage","content":"# Triage\n..."}
{"id":"ava_resource_...","type":"resource.read.result","ok":false,"kind":"skill","name":"triage","content":"resource unavailable"}
```

The only stable `kind` values are `prompt` and `skill`. List results return bounded metadata objects with valid contribution-style `name` values and optional descriptions. Direct read names use the same contribution-name contract: non-empty, at most 96 bytes, and only ASCII letters, digits, `_`, `-`, or `.`. Read results return text content and may include optional object `metadata`. Failure results (`ok:false`) must still include bounded `content` so AVA can surface plugin-reported errors instead of treating the record as malformed. Dynamic resource content is capped at the same 64 KiB ceiling used by static plugin prompt/skill files, and all records remain subject to plugin JSONL record size, depth, timeout, stderr, and cancellation limits.

Permissions:

- Plugin process launch requires `plugin.execute`.
- Dynamic prompt/skill list and read commands require `plugin.execute` because they launch plugin code, but they do not require `plugin.tool.call` or `plugin.command.run`.
- Dynamic prompt/skill resource requests can use the existing core-service proxy when the manifest also declares `proxy.read`, `proxy.search`, or `proxy.session`; those underlying proxy operations keep their own capability, permission, audit, timeout, and cancellation rules.
- Tool calls require `plugin.tool.call` after launch permission.
- Command calls require `plugin.command.run` after launch permission.
- Event hooks require `plugin.event.observe` after launch permission.
- A manifest `permissions` block is useful documentation for reviewers, but runtime enforcement is the permission prompt around process launch and contributed operations. The current core-service proxy slice additionally gates AVA-mediated read/search/status access behind explicit `proxy.read`, `proxy.search`, or `proxy.session` manifest capabilities.

### Host-rendered foreground TUI UI

`ava.plugin.v1` has one additive, bounded UI contract. A manifest opts into each surface independently with the exact capability string `ui.status`, `ui.widget`, `ui.select`, or `ui.confirm`; there is no generic `ui` capability. These capabilities authorize protocol negotiation only. They do not grant terminal access, a render callback, or permission to publish a surface.

Authority is available only to the exact canonical `/plugin run <plugin-id> <command> [arguments_json]` invocation entered directly as the foreground command in the interactive TUI. The opaque authority is bound to that complete command, plugin id, command name, invocation id, live TUI runtime, and one absolute deadline no more than 120 seconds after minting. RPC, ACP, print, non-TTY/headless, model-dispatched plugin-tool, event-hook, background, queued follow-up, synthetic, and plugin-to-plugin paths receive no UI authority. A UI record on any such path fails generically and terminates that plugin call; UI bytes are not copied into the response or event stream.

For a UI-capable direct TUI command, AVA evaluates `plugin.execute` and `plugin.command.run` first. It then preflights the high-risk, default-Ask `plugin.ui.present` permission **before starting the child**, even if the child would never emit a UI record. Denial, cancellation, expiry, a missing exact enablement record, or a malformed/failed enablement read starts no process. While the command remains active, AVA re-reads the exact enablement file/workspace/plugin/scope tuple through a fail-closed predicate at a finite 25 ms cadence. Each snapshot is a no-follow, nonblocking regular-file read capped at 1 MiB; a missing, non-regular, oversized, malformed, or failed snapshot revokes authority. Revocation drives both process-runner and presenter cancellation, so an atomic external disable during command output, status/widget exchange, or a blocking modal closes the surface and terminates the child promptly. Revocation is latched for that invocation; re-enabling applies only to a later invocation. Because a foreground command owns the interactive dispatch loop, `/plugins disable` in the same AVA process cannot race that command; use another AVA process or another atomic writer to revoke it while active. A normal same-process disable after the command updates future-invocation state only.

Every published surface has host-owned attribution on two separate fixed lines: `Plugin ID · <complete canonical id>` and `Command · <complete command name>`. AVA never hashes, ellipsizes, or prefix-truncates either identity. The coordinator and renderer share one display-cell fit predicate and the same dock/modal row geometry. Before applying a request, the coordinator proves that both complete identity lines and all mandatory host controls fit. If they do not, it returns `cancel` and publishes no plugin-controlled surface; because status/widget requests require `ack`, their call then fails as invalid or unauthorized. Titles, descriptions, labels, status text, and body lines may be display-cell truncated. Modal choice hit testing uses the same rendered row window. Fixed controls are `Ctrl+C stop · 120s max` for docks, plus `Enter select · Esc cancel` for selects and `Enter confirm · Esc cancel` for confirms. Confirmation is host-rendered and always opens with **Cancel** selected; plugins cannot change that default, and the attributed surface is never treated as an AVA permission/auth prompt.

The plugin-to-host request records are LF-delimited JSON objects with these exact fields (unknown or duplicate fields are invalid):

- `ui.status`: required string `id`, exact `type:"ui.status"`, and string `text` (empty is allowed). The only valid response is `ack`.
- `ui.widget`: required string `id`, exact `type:"ui.widget"`, non-empty string `title`, and non-empty string array `lines`. The only valid response is `ack`.
- `ui.select`: required string `id`, exact `type:"ui.select"`, non-empty string `title`, string `description`, and non-empty object array `choices`. Every choice has exact required string fields `id` and non-empty `label`, plus optional string `description`; choice ids must be unique. Valid responses are `select` with one declared choice id or `cancel`.
- `ui.confirm`: required string `id`, exact `type:"ui.confirm"`, non-empty string `title`, and string `description`. Valid responses are `confirm` or `cancel`.

AVA replies with an exact `ui.action` object. It always has string `id` matching the request, `type:"ui.action"`, and `action:"ack"|"select"|"confirm"|"cancel"`; `option_id` is required only for `select` and forbidden for every other action. Request ids and choice ids are 1–96 decoded UTF-8 bytes but in practice use the stricter ASCII id grammar: first character alphanumeric, remaining characters alphanumeric, `.`, `_`, or `-`.

All sizes below count decoded UTF-8 bytes unless stated otherwise:

- One UI JSONL record is at most 65,536 bytes and at most 128 JSON object/array levels deep.
- One invocation accepts at most 64 plugin-to-host UI request records with unique request ids: at most one status, two widgets, and eight modal records total across selects and confirms.
- Status text is at most 256 bytes.
- Every title, description, label, optional choice description, and widget line is at most 256 bytes.
- A widget has 1–8 lines and at most 2,048 aggregate bytes across its title and lines.
- A select has 1–32 choices. A select or confirm has at most 8,192 aggregate payload bytes across its request id, title, description, and (for select) all choice ids, labels, and descriptions.
- Host geometry further caps a modal at 160 display cells by 22 rows and a dock at 12 rows; the current terminal/composer budget can make either smaller and therefore fail the mandatory-chrome fit check.
- The authority deadline is absolute and at most 120 seconds. It covers permission preflight, startup, and every UI exchange rather than resetting per record; expiry immediately starts bounded presenter/process cleanup, whose teardown grace does not extend UI authority.

Text must be shortest-form, well-formed UTF-8 representing Unicode scalar values; malformed sequences, overlong encodings, surrogates, and values above U+10FFFF are rejected. To keep terminal control out of host rendering, AVA rejects U+0000–U+001F, U+007F–U+009F (including ESC and C1), U+061C, U+200E–U+200F, U+2028–U+2029, U+202A–U+202E, and U+2066–U+206F, whether supplied as raw UTF-8 or JSON escapes. This contains escaped CSI/OSC/control payloads and bidi reordering controls before presentation. Errors are generic and do not echo the raw record, raw UI text, enablement path, or underlying enablement parse/I/O detail.

Status and up to two widgets exist only for the owning foreground invocation. One modal may be active at a time. Enter, Esc, Ctrl+C, permission denial, authority expiry, external disable, child exit, protocol failure, runtime shutdown, or invocation completion deterministically resolves/cancels pending interaction, terminates/reaps the child when needed, and invokes idempotent presenter close. A close racing coordinator polling is revalidated after queue transfer, so it cannot receive an acknowledgement or transiently publish UI after invalidation.

UI state is ephemeral and is not written to sessions, RPC/ACP responses or events, exports, provider/model context, plugin diagnostics, or other persistence. The UI protocol grants no browser, network, auth, form, file, clipboard, secret, editor, shell, or arbitrary input access. Marketplace delivery, arbitrary markup/styles/geometry, native/in-process renderer code, forms or secret entry, browser/auth flows, and file pickers remain deferred. (The plugin executable itself remains local code under the separate `plugin.execute` trust model; UI authority does not sandbox or expand it.) Current evidence is deterministic protocol/coordinator/composer coverage plus the credential-free tmux smoke; this contract does not claim behavior on an untested real-terminal matrix.

Local workflow:

```sh
ava --continue
```

Inside AVA:

```text
/plugins validate examples/plugins/todo/plugin.json
/plugins install examples/plugins/todo
/plugins list
/plugins inspect com.example.todo
/plugins prompts com.example.todo
/plugins prompt com.example.todo todo-review
/plugins skills com.example.todo
/plugins skill com.example.todo todo-triage
/plugins enable com.example.todo
/plugin run com.example.todo status {}
/plugins disable com.example.todo
/plugins remove com.example.todo
```

Dynamic resource commands require a plugin that explicitly declares `dynamic.prompts` or `dynamic.skills`; the static todo sample above intentionally does not execute for dynamic resource discovery.

Troubleshooting:

- `plugin manifest must be a valid JSON object`: fix `plugin.json`; `validate` and `install` never start the entrypoint.
- `api_version is unsupported`: use `ava.plugin.v1`.
- `plugin is disabled`: run `/plugins enable <id>` in the workspace that discovered the plugin.
- `permission_denied`: approve the permission prompt in an interactive session or wire an RPC permission reply; headless modes fail closed by default.
- `plugin initialize response is malformed`: stdout contained non-protocol text, missing fields, the wrong `id`, or unsupported `api_version`.
- `timed out waiting`: the entrypoint did not answer within the startup or request timeout.
- Missing prompt or skill content: check that the resource path is relative, inside the plugin directory, a regular file, and below the resource size cap.
- Missing dynamic prompt or skill content: check that the plugin declares `dynamic.prompts` or `dynamic.skills`, is enabled, approves `plugin.execute`, and returns a `resource.read.result` with matching `kind`, `name`, and bounded `content`.
- Duplicate ids are disabled and reported through `/plugins failures`.
- Install fails on symlinks or special files: copy the plugin into a normal local directory with regular files only before running `/plugins install`.

Current core-service proxy slice: plugins that explicitly declare `proxy.read`, `proxy.search`, or `proxy.session` can ask AVA over the plugin protocol to perform bounded read/search operations or read a small session status snapshot. File/search work routes through AVA's existing file/search permission, audit, and cancellation paths. Edit, shell, network, and session mutation proxy operations are still unavailable. If a plugin performs those side effects directly inside its own process, AVA only mediates the plugin launch/call permission and cannot provide built-in file/shell/network policy for the internal operation. Keep early plugins narrow and prefer static resources or explicit AVA built-in tools for unsupported effects.

Proxy contract for the 1.1 hardening slice:

- Capability gate: proxy access is opt-in through manifest capabilities. The current slice recognizes `proxy.read`, `proxy.search`, and `proxy.session`; shell, network, edit, and session mutation are intentionally not implied.
- Protocol records: plugins send `proxy.request` JSONL records with required `type:"proxy.request"`, `id`, `operation`, and `arguments` fields, for example `{"type":"proxy.request","id":"px1","operation":"file.read","arguments":{"path":"README.md"}}`. AVA replies with `proxy.response` using the same `id`, `ok`, `content`, and optional `metadata`/`error`. Successful `content` is a JSON string containing presentation JSON for the proxied operation, not a nested protocol record. Error responses use `error.category`, `error.message`, and optional `error.details` strings. Proxy responses are bounded by the same plugin record limits as tool results.
- Initial operations: `file.read` accepts a workspace-relative or absolute `path` plus optional positive integer `max_bytes`, `limit`, and `offset` bounds, resolves relative paths inside the workspace, rejects non-regular resolved files, and routes through AVA's existing read-file permission checks. `file.search` accepts either grep-style `query` plus optional `include`, `literal` (must be `true` if present), `case_insensitive`, `max_matches`, and `max_line_length`, or glob-style `pattern` plus optional `max_results`. Search skips symlinks, does not evaluate repository ignore files in the proxy path, and routes through AVA's existing search policy; `root` and regex mode (`literal:false`) are rejected until scoped roots and interruptible regex execution are implemented. `session.status` requires empty `{}` arguments and returns read-only session metadata: `session_id`, `mode`, `provider_id`, `model_id`, `workspace`, and `current_dir`; it does not expose transcript content, session entries, credentials, or mutation APIs. Results are presentation JSON strings, not raw model messages.
- Permission model: a proxy request never inherits blanket plugin trust. AVA evaluates underlying file/search operations (`ReadFile` or `SearchFiles`) with `permission_tool_name`/audit context identifying the plugin and contributed tool/command. `session.status` is capability-gated and read-only, and does not trigger a separate file/search/command permission prompt. Persistent rules, session grants, and deny policies apply exactly as they do for built-in tools when an underlying permissioned service is used.
- Audit model: current session/audit entries identify proxy activity through existing fields: `actor="plugin:<plugin_id>:<kind>:<name>"`, `tool_name="<model_operation>:proxy:<operation>"`, and the normal tool call/request IDs where available. Dedicated `plugin_id`, `proxy_operation`, and proxy call-id fields remain a future additive audit schema change. A denied proxy request is returned to the plugin as a structured error and is not retried internally.
- Cancellation and timeouts: the parent plugin request cancellation token and plugin request deadline are shared with proxied operations. A canceled or timed-out parent call cancels pending proxy work, and a proxy timeout must not leave partial writes or unbounded output.
- Recursion guard: proxy dispatch cannot invoke plugin-contributed tools or commands. The current read/search/status slice may call only built-in file/search service functions or the in-memory session status snapshot, preventing plugin A -> proxy -> plugin A cycles.
- Output limits: proxied read/search output must carry truncation/spill metadata when large; plugins receive bounded content and cannot request arbitrary unbounded file or search dumps.
- Threat model: project plugins are untrusted local code. The proxy exists to bring their requested file/search effects back under AVA policy/audit, not to make plugin processes safe. Plugins that bypass AVA and read files directly remain outside AVA mediation; users should only enable plugins they trust to run local code.

## Plugin Manifest

The manifest is the stable authoring surface. JSON is preferred because AVA already has JSON infrastructure and plugin protocol records are JSONL.

Example:

```json
{
  "schema_version": 1,
  "id": "com.example.todo",
  "name": "Todo Sample Plugin",
  "version": "0.1.0",
  "api_version": "ava.plugin.v1",
  "description": "Minimal local plugin that demonstrates a command, a tool, static resources, and an event hook.",
  "entrypoint": {
    "command": "/bin/sh",
    "args": ["plugin.sh"]
  },
  "capabilities": ["tools", "commands", "prompts", "skills", "event_hooks"],
  "permissions": {
    "file": [],
    "shell": [],
    "network": [],
    "session": []
  },
  "contributes": {
    "tools": [
      {
        "name": "todo_add",
        "description": "Add one item to the session todo list.",
        "input_schema": {
          "type": "object",
          "properties": {
            "text": { "type": "string" }
          },
          "required": ["text"],
          "additionalProperties": false
        }
      }
    ],
    "commands": [
      {
        "name": "status",
        "description": "Report that the sample plugin is ready."
      }
    ],
    "prompts": [
      {
        "name": "todo-review",
        "description": "Review todo state for follow-up work.",
        "path": "prompts/todo-review.md"
      }
    ],
    "skills": [
      {
        "name": "todo-triage",
        "description": "Triage todo items before implementation.",
        "path": "skills/todo-triage.md"
      }
    ],
    "event_hooks": [
      { "event": "tool.result" }
    ]
  }
}
```

Manifest rules:

- `id` is stable, lowercase, and globally unique by convention, such as reverse-DNS or GitHub-owner style.
- `api_version` must match a supported AVA plugin API version.
- The entrypoint runs with the plugin directory as its working directory. Use `/bin/sh plugin.sh` for scripts that should not depend on executable mode, or a relative path with a slash such as `./plugin` for executable files inside the plugin directory.
- `contributes` may declare static contributions that AVA can inspect before execution.
- Runtime registration may add dynamic contributions only after handshake and core-side validation.
- Unknown manifest fields are ignored unless they appear under a schema-controlled contribution object.
- Invalid contributions are disabled individually when possible; invalid manifests disable the plugin.

## Plugin Protocol

The plugin runner uses strict LF-delimited JSON records. Stderr is diagnostics only and is bounded. Stdout is protocol only.

Minimum handshake:

```json
{"id":"ava_1","type":"initialize","api_version":"ava.plugin.v1","plugin_id":"com.example.todo","workspace":"/repo"}
{"id":"ava_1","type":"initialized","api_version":"ava.plugin.v1","plugin_version":"0.1.0","contributions":{"tools":[],"commands":[],"prompts":[],"skills":[],"event_hooks":[]}}
```

Tool call:

```json
{"id":"ava_tool_call_...","type":"tool.call","tool":"todo_add","arguments":{"text":"write tests"},"context":{"call_id":"call_...","workspace":"/repo"}}
{"id":"ava_tool_call_...","type":"tool.result","ok":true,"content":"Added todo: write tests","metadata":{"count":1}}
```

Command call:

```json
{"id":"ava_command_cmd_...","type":"command.call","command":"status","arguments":{},"context":{"call_id":"cmd_...","workspace":"/repo"}}
{"id":"ava_command_cmd_...","type":"command.result","ok":true,"content":"Todo sample plugin is ready.","metadata":{"open_items":0}}
```

Event observation:

```json
{"id":"ava_event_event_...","type":"event.observe","event":"tool_result","payload":{"tool":"read_file","status":"success"},"context":{"call_id":"event_...","workspace":"/repo"}}
{"id":"ava_event_event_...","type":"event.observed","ok":true,"content":"optional diagnostic","metadata":{"count":1}}
```

Cancellation:

```json
// Future extension; not sent by the current runner.
{"id":"ava_2","type":"cancel","reason":"user_cancelled"}
```

Protocol rules:

- Every request has a string `id`; every response echoes it exactly.
- Requests time out independently from plugin process startup.
- Current AVA cancellation is cooperative through the request cancel callback and process termination path; the `cancel` protocol record above is reserved for a future plugin-visible cancellation extension and is not part of the stable v1 runtime today.
- Malformed records, unknown response ids, oversized records, or invalid result schemas are plugin errors.
- Plugin logs use explicit protocol records or bounded stderr; they are never mixed into tool results unless requested.
- AVA records plugin errors as runtime events and session audit entries.
- Event hook manifests may declare dotted names such as `tool.result`; AVA normalizes dots to underscores and sends canonical runtime event names such as `tool_result` in `event.observe` requests.
- Event hook failures are best-effort diagnostics. A failed hook does not fail the user turn, slash command, or MCP command that emitted the observed runtime event.

## Contribution Types

1.0 supports these contribution types:

- Tools: model-callable operations with JSON-schema-like input and structured bounded results.
- Slash/backend commands: user-invoked commands routed through the backend command dispatcher.
- Prompt templates and skills: static markdown resources discovered from plugin directories.
- Dynamic prompt/skill resources: explicit `/plugins dynamic-*` commands that execute enabled plugins through the out-of-process runner and `plugin.execute` permission boundary.
- Non-mutating event hooks: observe lifecycle events and optionally add diagnostics, but not rewrite provider requests in 1.0.
- MCP servers: configured endpoints that AVA can launch and adapt into AVA tools. MCP server declarations in plugin manifests are deferred.

Provider plugins, custom UI renderers, and prompt/provider interception remain deferred. The implemented additive host-rendered status/widget/select/confirm contract is the narrow slice approved by the [historical Pi-inspired TUI feature expansion record](https://github.com/Artificial-Source/AVA/blob/develop/docs/history/tui-pi-feature-expansion.md); it does not authorize plugin terminal output, arbitrary geometry, styles, themes, editors, key handlers, or render callbacks.

### Pi Extension Capability Disposition

| Pi extension capability | AVA MVP disposition |
| --- | --- |
| Tools | Implemented as out-of-process plugin tools with explicit `plugin.execute`/`plugin.tool.call` permission and bounded structured results. |
| Commands | Implemented as backend/slash/RPC plugin commands; execution is permissioned and audited. |
| Prompts | Implemented as static plugin prompt resources and command-registry entries, not automatic provider-request rewrites. |
| Skills | Implemented as static plugin skill resources plus the built-in bounded `skill` tool. |
| Events | Implemented as non-mutating event hooks; hooks cannot rewrite prompts, provider requests, or tool results in v1. |
| UI/render slots | Bounded, attributed, host-rendered foreground-TUI status/widget/select/confirm records are implemented under the exact capabilities and authority rules above. Arbitrary TUI slots, markup, renderers, geometry, styles, and input capture remain deferred. |
| Keybindings | Core AVA keybinding config exists; plugin-contributed keybindings are deferred to avoid hidden input capture and conflict complexity. |
| Themes | Core built-in/custom theme files exist; plugin theme packages are deferred to the package/trust design. |
| Custom providers/request interception | Deferred; provider auth, model metadata, and request mutation are security-sensitive and stay in core/config for MVP. |
| Packages/remote install | Local plugin directory install/remove is implemented; remote code/resource install remains deferred pending provenance, signing, compatibility, rollback, and trust policy. |

## Permissions And Audit

Plugin-contributed operations must not get side-effect authority merely by registering a tool. AVA enforces launch and call permissions today; the current read/search/status core-service proxy slice reuses existing file/search permission categories for file/search work and exposes only capability-gated read-only status metadata for `session.status`. Future side-effecting proxy operations must reuse the relevant shell, network, file mutation, or session permission categories.

Permission categories:

- `plugin.execute`: run a plugin entrypoint.
- `plugin.tool.call`: call a plugin tool.
- `plugin.command.run`: run a plugin command.
- `plugin.event.observe`: subscribe to runtime events.
- `plugin.ui.present`: high-risk Ask preflight for one exact UI-capable direct foreground TUI command; it grants no authority on any other route.
- `mcp.server.launch`: launch a local MCP server process.
- `mcp.server.connect`: connect to a configured MCP server.
- `mcp.tool.call`: call an MCP tool.
- `mcp.resource.read`: read a configured MCP resource through an MCP server.
- Existing categories such as `file.read`, `file.write`, `shell.run`, `network.fetch`, and `external.directory` still apply when a plugin asks AVA to perform those operations through a core service proxy.

Audit records emitted today include:

- Permission request id.
- Operation, such as `plugin.execute`, `plugin.tool.call`, `plugin.command.run`, `plugin.ui.present`, `plugin.event.observe`, `mcp.server.launch`, `mcp.server.connect`, `mcp.tool.call`, or `mcp.resource.read`.
- Agent mode.
- Model-facing tool or command name when applicable.
- Policy action, reason, and risk.
- Target path or command when applicable.
- Resolver resolution, source, and reason when applicable.

Dedicated plugin id/version, contribution id/type, requested capability, core operation, MCP server id, and MCP tool-name audit fields are deferred compatibility-preserving metadata. Today, those identities are carried indirectly through operation, tool name, command, path, and error context.

## Deferred Core Service Proxy

AVA exposes an initial safe-operation proxy through protocol requests instead of encouraging plugins to perform side effects directly. The current slice keeps plugin execution permissioned and audited and additionally provides read/search proxy operations plus a read-only `session.status` metadata operation; broader shell/network/edit/session operations remain future work.

Initial proxy operations:

- Implemented now: read/search files through AVA file/search tools when the manifest declares `proxy.read` or `proxy.search`.
- Implemented now: read-only `session.status` metadata when the manifest declares `proxy.session`.
- Deferred: file mutations through AVA write/edit/apply-patch paths.
- Deferred: shell commands through AVA process policy.
- Deferred: network fetch/search through AVA network policy.
- Deferred: transcript reads, richer session statistics, session mutation, and any session operation requiring explicit session permissions.

This does not sandbox arbitrary plugin code. It makes well-behaved plugins easy to write safely and gives AVA one auditable path for operations that go through AVA.

## MCP Integration

MCP is a first-class extension surface that shares AVA's tool registry, permission, runtime event, and audit paths. Current 1.0 support uses explicit global/project MCP config files rather than plugin manifest contributions. A shorter user-facing MCP summary lives in [`docs/extensions/mcp.md`](mcp.md).

Config locations:

- Global: `$XDG_CONFIG_HOME/ava/mcp.json`, falling back to `~/.config/ava/mcp.json`.
- Project: `.ava/mcp.json` under the active workspace.

Config shape:

```json
{
  "servers": [
    {
      "id": "demo",
      "name": "Demo MCP",
      "command": "demo-mcp-server",
      "args": ["--stdio"],
      "enabled": true
    }
  ]
}
```

Global servers default to enabled when `enabled` is omitted. Project servers default to disabled when `enabled` is omitted, so repositories cannot silently opt users into running project-local MCP server commands. Global MCP config must not reference workspace-relative executable or script paths; entries such as `./server`, `.ava/server.js`, or `node_modules/.bin/server` belong in project MCP config and require `/trust project` before they can run.

Current 1.0 MCP scope:

- Stdio MCP server transport.
- Server definitions from global and project config.
- Explicit `enabled:true` for project MCP servers before command execution.
- MCP `initialize` lifecycle with server capability capture.
- `tools/list` and `tools/call`, adapted into AVA's tool registry.
- `resources/list` and `resources/read`, adapted into opaque no-argument read-style AVA tools with explicit `mcp.resource.read` permission. Resource listing follows bounded pagination and does not expose server-controlled resource names, URIs, MIME types, or descriptions to provider tool schemas before read approval.
- `prompts/list` and `prompts/get`, surfaced through the command registry as dynamic `/mcp:<server_id>:<prompt_name>` prompt commands.
- Per-server startup timeout, initialize timeout, request timeout, cancellation, and process-tree cleanup.
- Health status and diagnostics visible through plugin/MCP inspect commands.
- Schema conversion from MCP tool input schemas to AVA/provider-compatible tool schemas, with unsupported schemas disabled and explained.
- Tool naming that avoids collisions, such as `mcp_<server_id>_<tool_name>` or another deterministic sanitized prefix.
- Session audit entries for server launch, tool calls, resource reads, errors, and permission decisions.

Deferred MCP scope:

- Plugin manifests cannot contribute MCP server definitions yet; MCP servers are discovered through explicit MCP config files.
- Streamable HTTP transport for remote MCP servers.
- Progress/log notifications surfaced as runtime events.
- Resource subscriptions if they can be made bounded and cancellable.
- MCP marketplace/discovery beyond explicit configured servers.
- Automatic trust of server-declared side-effect safety.
- Complex OAuth flows for remote MCP servers.
- MCP sampling callbacks that let servers ask AVA's model to complete arbitrary requests.
- Cross-platform OS sandbox guarantees for local MCP server processes.

MCP safety rules:

- MCP servers are programs chosen by the user. Treat them as untrusted at the AVA boundary, but do not claim they are OS-sandboxed unless a real sandbox exists.
- Calling an MCP tool is a permissioned operation even if its schema looks read-only.
- Reading an MCP resource is exposed as a no-argument read-style tool and requires `mcp.resource.read` approval; resources are never silently injected into prompt context. Resource read output is text-only for this slice: AVA accepts MCP `contents[].text`, preserves the first text item's URI/MIME metadata in the tool result, and rejects missing `contents`, blob-only, or otherwise non-text resource responses instead of reporting false success.
- Launching a local MCP server is a permissioned process execution event.
- Connecting to an MCP server session is permissioned; future remote transports also need explicit network permission.
- MCP tool and resource results are bounded before they enter the model context.
- MCP server stderr is diagnostics only and is kept as a bounded tail.
- MCP errors are surfaced as MCP/plugin failures, not as core AVA crashes.

## Commands And Diagnostics

AVA provides backend commands that work in TUI, print/RPC where applicable, and tests:

- `/plugins list`
- `/plugins install <path>`
- `/plugins remove <id>`
- `/plugins inspect <id>`
- `/plugins enable <id>`
- `/plugins disable <id>`
- `/plugins validate <path>`
- `/plugins failures`
- `/plugins prompts <id>`
- `/plugins prompt <id> <name>`
- `/plugins skills <id>`
- `/plugins skill <id> <name>`
- `/plugins dynamic-prompts`
- `/plugins dynamic-prompt <id> <name>`
- `/plugins dynamic-skills`
- `/plugins dynamic-skill <id> <name>`
- `/plugin run <id> <command> [arguments_json]`
- `/mcp list`
- `/mcp inspect <server>`
- `/mcp tools <server>`
- `/mcp restart <server>`

Most static plugin and MCP operations have RPC command forms for external editor integrations, including local plugin install/remove. Dynamic plugin prompt/skill resource commands are slash/backend-command operations in this slice and do not yet have dedicated RPC command types such as `list_dynamic_plugin_prompts`. MCP prompts are exposed through `list_commands`/`invoke_command` as dynamic command-registry entries, not as direct `/mcp prompts` slash commands. `/mcp tools` launches a fresh stdio process for discovery, emits tool start/result events, and requires `mcp.server.launch` and `mcp.server.connect` permission approval. `/mcp restart` is informational because current MCP stdio servers are per-discovery/per-tool-call processes rather than resident daemons.

## Testing Requirements

Minimum regression coverage:

- Manifest parse/validation for valid, invalid, and unknown-field cases.
- Project plugin discovery without execution until enablement.
- Plugin initialization success, unsupported API version, startup timeout, malformed handshake, oversized record, and clean shutdown.
- Plugin tool registration, successful call, invalid arguments, invalid result, timeout, cancellation, and crash.
- Permission denial for plugin execution and plugin tool calls.
- Audit/session records for plugin execution and tool calls.
- Fake MCP server initialize/list-tools/call-tool success.
- Fake MCP server `resources/list`/`resources/read` success, bounded pagination, no-argument resource tool dispatch, opaque pre-approval schema metadata, text-only read enforcement, cancellation, and headless `--allow-tool mcp` approval.
- Fake MCP server prompt list/get success through command-registry discovery and invocation.
- Fake MCP server tool error, malformed initialize response, early startup/discovery exit, timeout, cancellation,
  stderr bounding, and process cleanup.
- Direct `ava --rpc` headless smoke coverage for plugin list/failures/inspect/validate/resource/enable/disable
  commands, real-sample plugin discovery/resource/enable/disable flow, fail-closed plugin command execution, MCP
  list/inspect/restart commands, invalid-config containment, and fail-closed `list_mcp_tools` behavior without a TUI
  resolver.
- Tool name collision behavior between built-in, plugin, and MCP tools.

## Implemented 1.0 Foundation

- Internal tool registry with built-in, plugin, and MCP tool registration.
- Plugin manifest parsing, diagnostics, discovery, local directory install/remove, local enable/disable state, and validation commands.
- Out-of-process plugin runner with initialize, tool call, command call, event observation, cancellation, bounded stderr, timeouts, and shutdown.
- Plugin tool contributions, plugin command contributions, static prompt/skill resources, opt-in explicit dynamic prompt/skill resources, and non-mutating event hooks.
- Direct headless RPC plugin command smokes for discovery, diagnostics, static resources, real-sample project plugin
  coverage, enablement, and fail-closed execution.
- Stdio MCP config loading, initialize, `tools/list`, `tools/call`, `resources/list`, `resources/read`, `prompts/list`, `prompts/get`, bounded stderr
  diagnostics, tool broker registration, command-registry prompt exposure, slash/RPC diagnostics, direct headless
  command smokes, and fake-server success/error/exit regression coverage.

## 1.0 Decisions

- Plugin manifests are JSON only for 1.0. TOML can be reconsidered later if AVA's broader config format settles on TOML.
- Project plugin enablement is stored outside the repository by default under `$XDG_STATE_HOME/ava/plugin-enablement.json`, falling back to `~/.local/state/ava/plugin-enablement.json`. The state file is keyed by canonical workspace path and plugin id so a repository cannot enable executable plugin code for other users by committing `.ava` files.
- Global plugin enablement uses the same state file with a global scope key. Explicit config may discover global plugins, but execution still requires enablement unless the plugin ships as a trusted built-in later.
- Built-in tool names are reserved. Plugin tool model names use `plugin_<sanitized_plugin_id>_<tool_name>` by default. MCP tool model names use `mcp_<sanitized_server_id>_<tool_name>` by default. MCP resource tool names use `mcp_<sanitized_server_id>_resource_<16_hex_server_id_hash>_<16_hex_uri_hash>` so provider-visible names are bounded, stable by server id and resource URI, and do not reveal raw resource inventory before `mcp.resource.read` approval. Name collisions disable the later contribution and produce diagnostics instead of overriding a tool silently.
- Stdio MCP transport is required for 1.0. Streamable HTTP MCP remains strongly desired, not required.
- Plugin process launch uses a separate permission category from shell commands. If a plugin asks AVA to run a shell command through the core service proxy, that proxied command still goes through the normal shell policy.
- Plugin and MCP stderr capture is bounded. The initial target is an in-memory tail of the last 64 KiB per process, with larger log spill files deferred until diagnostics prove the need.
- Plugin restart is manual for 1.0. Current plugin and MCP stdio processes are launched per call or discovery; `/mcp restart` reports that the next discovery or tool call will launch a fresh process.
- Plugins do not get a plugin-to-plugin event bus in 1.0. Event hooks observe AVA runtime events only, and inter-plugin communication is deferred.
- `ava.plugin.v1` compatibility is governed by the plugin/MCP compatibility policy; additive optional fields are preferred, while breaking manifest/protocol/schema changes require an explicit versioned transition.

## Deferred/Future Implementation Choices

- Exact JSON schema subset accepted for plugin and MCP tool inputs.
- Broader timeout configurability beyond the current bounded plugin startup/request and MCP call defaults.
- Whether plugin enablement should support a separate machine-local project nickname for moved worktrees.
- Plugin-manifest MCP server contributions.
- MCP resource subscriptions, resource templates, binary/blob resource surfacing, Streamable HTTP, and progress/log notification surfacing.
