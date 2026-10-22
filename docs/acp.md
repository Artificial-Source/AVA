# Stable ACP v1 Session Endpoint and Interoperability

AVA exposes the pinned stable ACP v1 protocol over stdio:

```sh
cd /path/to/approved/workspace
ava --acp
```

`--acp` is standalone. ACP JSON-RPC 2.0 records are the only stdout bytes; bounded diagnostics use stderr. The verified launch-directory descriptor and its absolute normalized logical path define the maximum filesystem scope for the connection. The stable v1 implementation can route exact file operations and command execution through capabilities negotiated with the ACP client; interoperability hardening adds independent automated evidence and truthful client configuration guidance without widening that profile.

## Interface selection

These interfaces have different contracts and must not be treated as aliases:

- `ava --acp` is AVA's standards-based stable ACP v1 editor/IDE endpoint over JSON-RPC 2.0 stdio.
- `ava --rpc` is AVA's proprietary versioned JSONL stdio interface for automation and custom clients. It is not JSON-RPC 2.0 or ACP; see [`rpc-protocol.md`](rpc-protocol.md).
- JSON-RPC 2.0 is ACP's message envelope, not a claim that arbitrary JSON-RPC clients implement ACP.
- Pi RPC is reference behavior for subprocess embedding, custom UI, automation, and testing. AVA RPC is not Pi RPC, and Pi RPC is not AVA's editor-interoperability interface.

## Current interoperability evidence

The labels below describe evidence, not a generic supported-editor list. A setup example does not establish runtime compatibility.

| Client or harness | Version | Evidence/status | Evidence date | Scope |
|---|---:|---|---|---|
| Official `@agentclientprotocol/sdk` | 1.2.1 | `automated` | 2026-07-13 | The official SDK drives the real `ava --acp` subprocess through initialization, lifecycle, streamed updates, permission plus client filesystem, client terminal, cancellation, and clean exit against the loopback fake provider. |
| `acpx` | 0.12.0 | `automated, opt-in alpha compatibility smoke, not official conformance` | 2026-07-13 | The published alpha CLI drives one raw-agent workflow. It is absent from default CTest and is neither the official conformance authority nor a production dependency. |
| `Zed` | 1.9.0 (`ced90fc636c4ede05402befc38a63bae7fd741bd`) | `manual verified` | 2026-07-14 | The [confined real-client report](interop/evidence/zed-1.9.0-2026-07-14.md) captures initialization/new session, tool and permission cards, client filesystem read/write, client terminal execution, final `end_turn`, in-flight cancellation, clean client close, and zero tagged process residue. The claim is limited to that exact version and those observed flows. |
| `JetBrains` | 2026.1+ | `configuration documented but not executed` | not executed; docs checked 2026-07-13 | The client was not installed or run in this environment. |
| `CodeCompanion.nvim` | not executed | `configuration documented but not executed` | not executed; docs checked 2026-07-13 | The client was not installed or run in this environment; its documented ACP client capability has `terminal=false`. |

The machine-checked status is in [`acp-support.json`](acp-support.json). Evidence policy and the deliberately incomplete Zed report template are in [`interop/evidence/README.md`](interop/evidence/README.md). There is no broad Zed, JetBrains, Neovim, or generic-editor compatibility claim.

## Client configuration

Use the exact AVA executable intended for the integration. AVA owns its provider/model/auth configuration; do not place credentials in a client example or commit them with editor settings.

## Zed custom agent

Zed's documented custom-agent location is `$XDG_CONFIG_HOME/zed/settings.json` (normally `~/.config/zed/settings.json`). Merge this entry with existing settings and replace the command with an absolute path:

```json
{
  "agent_servers": {
    "AVA": {
      "type": "custom",
      "command": "/absolute/path/to/ava",
      "args": ["--acp"],
      "env": {}
    }
  }
}
```

Open the Agent settings, select the AVA external agent, and start the thread from the intended project directory. Zed documents interactive ACP logs through `dev: open acp logs`, but no headless ACP runner. Logs can contain source, paths, protocol content, or credentials and are not repository evidence until manually reviewed and sanitized.

The bounded dogfood script never writes normal Zed settings. It generates this shape under a unique temporary `$XDG_CONFIG_HOME`, records the exact AVA command, and requires a dedicated display plus either reviewed OS confinement evidence or an explicit already-provisioned disposable credential-free graphical account/VM acknowledgement. HOME/XDG isolation alone is **not** a sandbox.

## JetBrains 2026.1+ custom agent

JetBrains AI Assistant 2026.1+ documents custom agents in `~/.jetbrains/acp.json`. Replace the command with an absolute path. Disabling default MCP forwarding in this minimal example prevents client configuration from silently widening the tested AVA composition:

```json
{
  "default_mcp_settings": {
    "use_custom_mcp": false,
    "use_idea_mcp": false
  },
  "agent_servers": {
    "AVA": {
      "command": "/absolute/path/to/ava",
      "args": ["--acp"],
      "env": {}
    }
  }
}
```

Select **Add Custom Agent** in AI Chat, save the file, and select AVA. JetBrains was not installed or executed here, so this is `configuration documented but not executed`, not compatibility evidence. Its documentation also says ACP agents are currently unavailable through WSL.

## CodeCompanion.nvim custom ACP adapter

CodeCompanion.nvim documents custom ACP adapters in its Neovim setup. This complete adapter starts AVA directly and advertises only the client filesystem capabilities used by the adapter:

```lua
require("codecompanion").setup({
  adapters = {
    acp = {
      ava = function()
        local helpers = require("codecompanion.adapters.acp.helpers")
        return {
          name = "ava",
          formatted_name = "AVA",
          type = "acp",
          roles = { llm = "assistant", user = "user" },
          commands = {
            default = { "/absolute/path/to/ava", "--acp" },
          },
          defaults = {
            mcpServers = {},
            timeout = 20000,
          },
          parameters = {
            protocolVersion = 1,
            clientCapabilities = {
              fs = { readTextFile = true, writeTextFile = true },
              terminal = false,
            },
            clientInfo = {
              name = "CodeCompanion.nvim",
              version = "1.0.0",
            },
          },
          handlers = {
            setup = function(self) return true end,
            auth = function(self) return true end,
            form_messages = function(self, messages, capabilities)
              return helpers.form_messages(self, messages, capabilities)
            end,
            on_exit = function(self, code) end,
          },
        }
      end,
    },
  },
})
```

CodeCompanion's ACP documentation says terminal operations are not implemented and it advertises `terminal=false`. AVA therefore omits `bash`; it never falls back to a local shell for that ACP connection. CodeCompanion's `/resume` flow uses `session/load`, while AVA deliberately supports exact-context `session/resume` and defers replaying `session/load`, so that UI flow is not claimed. CodeCompanion.nvim was not installed or run here; the example is `configuration documented but not executed`.

## Reproducing automated interoperability

The Node packages are test-only, installed in separate test trees, absent from production artifacts, and unnecessary for normal AVA use. Installation is a reviewed setup step; test execution performs no package download. Use committed lockfiles, never `npx`, and disable lifecycle scripts:

```sh
# Official SDK 1.2.1 plus its exact zod 4.4.3 peer dependency.
cd tests/acp-sdk
npm ci --ignore-scripts --no-audit --no-fund
cd ../..
cmake -S . -B build-acp-sdk \
  -DAVA_BUILD_TESTS=ON \
  -DAVA_REQUIRE_ACP_SDK_INTEROP=ON
cmake --build build-acp-sdk --target ava ava_fake_provider_server
ctest --test-dir build-acp-sdk --output-on-failure \
  -R '^ava_cli[.]acp_sdk_interop$'
scripts/live-acp-dogfood.sh sdk --build-dir "$(pwd)/build-acp-sdk"
```

A normal configure leaves `AVA_REQUIRE_ACP_SDK_INTEROP=OFF`; the ordinary test returns CTest skip code 77 when Node or the installed package tree is absent. Required mode makes a missing runtime/package a failure. The dedicated CI job uses exact Node 24.13.1.

`acpx` is separately locked and remains opt-in alpha tooling. Reproduction requires exact Node 24.13.1 (`node --version` must print `v24.13.1`):

```sh
cd tests/acp-acpx
npm ci --ignore-scripts --no-audit --no-fund
cd ../..
cmake -S . -B build-acpx \
  -DAVA_BUILD_TESTS=ON \
  -DAVA_ENABLE_ACPX_INTEROP=ON
cmake --build build-acpx --target ava ava_fake_provider_server
ctest --test-dir build-acpx --output-on-failure \
  -R '^ava_cli[.]acpx_interop$'
scripts/live-acp-dogfood.sh acpx --build-dir "$(pwd)/build-acpx"
```

A default configure leaves `AVA_ENABLE_ACPX_INTEROP=OFF`, so `ava_cli.acpx_interop` is absent from default CTest. The harness invokes `tests/acp-acpx/node_modules/.bin/acpx` directly, verifies exact versions, uses the raw `--agent` path, and blocks adapter/package download fallbacks.

### Diagnostics and ownership

- SDK skip 77 in an ordinary build means install the locked `tests/acp-sdk` tree or use required mode to diagnose the missing dependency. Required-mode configuration and the focused test must not skip.
- An absent `ava_cli.acpx_interop` is expected unless `AVA_ENABLE_ACPX_INTEROP=ON`. Version failures require exact Node 24.13.1 and `acpx` 0.12.0; do not substitute a network-installed adapter.
- Each harness creates unique HOME/XDG/session/workspace roots, constructs an allowlisted environment, uses only a conspicuous fake key and loopback fake provider, owns its process groups, applies finite deadlines, and escalates TERM to KILL. A surviving process or non-empty successful stderr is a test failure.
- The official SDK harness is the mandatory independent automated gate. The alpha `acpx` result is advisory compatibility breadth and never upgrades the official evidence claim.

## Real-client evidence and maintenance

Run `scripts/live-acp-dogfood.sh --help` for exact usage; the Zed actions are `zed run` and `zed sanitize-copy`. `zed run` requires explicit absolute AVA, fake-provider, and Zed paths, a dedicated display declaration, and a valid confinement declaration. It stages separate lifecycle/tool/permission/client-filesystem/terminal and cancellation scenarios, restarts the loopback fake provider between phases, and leaves every evidence field incomplete until an operator records and reviews observations. It must never be run as an ordinary-account profile-only GUI smoke.

Raw logs/screenshots stay under the script's mode-0700 temporary root. The copy action accepts only a completed, bounded, manually reviewed textual report and rejects fake keys, credential-looking values, user-home/private-temp paths, unchecked fields, embedded images, and oversized artifacts. It does not create a `manual verified` artifact automatically. Zed 1.9.0 is `manual verified` only for the flows preserved in the [2026-07-14 report](interop/evidence/zed-1.9.0-2026-07-14.md); new versions or broader flows require new evidence.

Maintainers must apply these rules:

1. Changes to the `schema-v1.19.0` fixture release, source commit, or SHA-256 require schema/manifest review and regeneration of all affected protocol evidence. Stable v1 is the only target; unstable schema and draft v2 remain excluded.
2. A capability claim requires an implemented handler, deterministic lower-level coverage, and independent-client evidence for the claimed flow. Configuration alone is not execution.
3. Real-client claims require a versioned preserved report under [`interop/evidence/`](interop/evidence/) with confinement, commands, observed-versus-inferred outcomes, cleanup, and redaction records. Never silently carry evidence to a newer client version.
4. SDK, `zod`, or `acpx` upgrades require separate package manifest/lock integrity, git head/shasum where published, license, transitive dependency, API, and harness review. Keep dependencies test-only and installations script-disabled.
5. Keep `docs/acp-support.json`, package locks, CMake/CI gates, setup examples, and captured evidence labels in one reviewed change. Do not turn SDK/acpx into production dependencies or describe unexecuted clients as supported.

## Protocol profile

AVA negotiates protocol version 1 and implements:

- `initialize`
- `session/new`, `session/list`, `session/resume`, `session/close`
- `session/prompt`, `session/cancel`, and `$/cancel_request`
- outbound `session/update`
- outbound `session/request_permission`

Initialization advertises list/resume/close session capabilities and always reports `loadSession:false`. Before successful initialization, AVA resolves the effective default provider/model exactly once, requires its provider to exist in the immutable startup provider catalog (built-ins plus valid configured user-defined providers), pins the complete model metadata snapshot, and derives `promptCapabilities.image` from that same object. Explicit embedding/test provider-bundle factories may supply a synthetic declared provider; a service without an injected catalog falls back to built-ins only. New and resumed hosts use this immutable provider/model snapshot; editing `models.json` after startup cannot drift a session or its advertised capabilities. The successfully decoded client capabilities are likewise retained as one immutable per-connection snapshot before any session can be created. An explicit runtime override wins and is pinned. Startup fails actionably if the configured default does not resolve to a declared model or its provider is unavailable. Audio, embedded context, additional directories, session delete, modes, configuration options, HTTP/SSE MCP, and authentication/logout are not advertised.

`session/load` is deliberately rejected with `-32601`. AVA cannot replay images, reasoning, and complete tool lifecycles exactly, so it never returns a partial text-only history. Stable ACP v1 defines `session/resume` as continuing exact session context without replaying previous messages; that method remains supported.

[`acp-support.json`](acp-support.json) is the exhaustive method, capability, content, update, tool, permission, transport, error, and stop-reason disposition for this profile.

## Prompt content

All prompts support:

- `text`;
- `resource_link`, projected as a reference only and never read or fetched;
- when `promptCapabilities.image` is true, `image` with canonical base64 and an exact `image/png`, `image/jpeg`, `image/gif`, or `image/webp` MIME type.

An image block sent to a text-only pinned session model returns `-32602` before provider setup or attachment import. For an image-capable model, bytes are signature-checked by AVA's attachment layer before being stored as a session attachment and sent through provider-neutral image content. Limits are 8 images, 192 KiB per image (the decoded maximum under ACP's 256 KiB JSON-string cap), and 720 KiB total image bytes, with the 1 MiB record cap applying first. Text is bounded to 512 KiB in aggregate. Unknown discriminators, malformed recognized fields, MIME mismatches, non-canonical base64, audio, and embedded resources return `-32602`; unsupported blocks are never silently ignored.

ACP text disables CLI/TUI `@file` expansion. Resource links and image `uri` metadata therefore cannot trigger implicit local or remote reads.

## Ordered session updates

A typed ACP mapper projects protocol-neutral runtime events into the pinned `SessionUpdate` union. This profile emits:

- `agent_message_chunk` for assistant text;
- `agent_thought_chunk` for non-redacted reasoning text;
- `tool_call` and `tool_call_update` with the provider's stable `toolCallId`, bounded title, kind, status, text content, and safe in-workspace locations;
Tool kinds emitted by the stable v1 registry are `read`, `edit`, `search`, `execute`, and `other`. The exact authorization order is an initial `tool_call` with `pending`, the matching `session/request_permission.toolCall` also with `pending`, one `tool_call_update` with `in_progress` only after backend/client authorization succeeds and immediately before execution, then `completed` or `failed`. A denied or canceled call never reports `in_progress`. AVA does not misrepresent unified patch text as ACP's full old/new-text diff type, and does not emit terminal tool content. Unknown internal events stay internal. Redacted provider reasoning stays internal.

Live updates are FIFO-enqueued before the terminal prompt response. Adjacent provider text or reasoning fragments are coalesced into UTF-8-safe chunks of at most 1 KiB before accounting, so provider chunk fragmentation cannot exhaust the budget while updates remain incremental. A final assistant message is omitted when the same text was already streamed. Each prompt is bounded to 16,384 updates and 16 MiB of encoded update payload, enough to represent the provider transport's bounded 8 MiB response plus JSON-RPC envelopes, in addition to peer queue/record limits. Mapping, queue, or write failure aborts prompt work and produces one request-terminal or connection-terminal outcome.

Plan, usage, mode, configuration, command, and session-info updates remain deferred because AVA does not have exact protocol-neutral semantics for them.

## Explicit ACP tools

The stable v1 model registry is composed explicitly from:

- `read_file`, `list_directory`, `glob`, `grep`;
- `write_file`, `edit_file`, `apply_patch`;
- `bash` only when the immutable client capability snapshot has `terminal:true`;
- tools discovered from the session's immutable stdio MCP configuration after exact persistent operator authorization.

Without terminal negotiation, `bash` is neither exposed nor dispatchable and cannot fall back to local fork/exec. With terminal negotiation, AVA parses the safe command string locally, sends `argv[0]` as `terminal/create.command`, the remaining argv as `args`, an explicit empty environment, the preserved logical session cwd, and a bounded output limit. Non-ACP RPC/TUI runs retain the local permissioned executor.

AVA installs an immutable exact-file adapter when either `fs.readTextFile` or `fs.writeTextFile` is true and preserves the two flags independently. `read_file` uses `fs/read_text_file` only when read support is true; `write_file` uses `fs/write_text_file` only when write support is true. An unsupported individual operation remains descriptor-secure and local. A supported client operation never falls back to local bytes or mutation after a client error. When neither flag is true, both operations are local. List/search/context/session storage always stay local. The permission layer and mutation queues remain authoritative. Every outbound file call receives a freshly descriptor-validated, in-root absolute path. Symlink escapes, absolute symlinks below the workspace root, and protected paths fail before a client request. `write_file` omits its pre-permission local diff preview only when the client owns writes; a locally owned write previews and mutates local descriptor-anchored bytes even if client reads are available.

`edit_file` and `apply_patch` require coherent ownership. With neither filesystem flag they remain visible and fully local; with both flags they remain visible and read/write through the client. When exactly one flag is true, both tools are omitted from the model's exact ACP registry and a direct dispatch fails closed before permission resolution or file I/O. Client-owned `apply_patch` calls are limited to one distinct target path per call, although multiple non-overlapping edits to that target remain supported. ACP exposes no client filesystem transaction primitive, so AVA rejects multi-target client patches before execution starts or any client write is requested. Descriptor-secure local patches and ordinary local RPC/TUI patches stage every target before committing any target. Their per-file commits are atomic replacements, but the final sequence across multiple files is inherently non-atomic; an error after a commit reports the paths already changed rather than claiming rollback.

Bounded `read_file` calls forward the normalized one-based tool offset as ACP `line` and request `max_lines + 1` through `limit` when representable. The extra line is a sentinel used to enforce `line_limited` and `next_offset` accurately while AVA still reapplies `max_bytes` locally. An ACP window contains no authoritative full-file size or line count, so `total_bytes` and `total_lines` are omitted from tool, command, timeline, event, and plugin payloads while retained `output_bytes`, `output_lines`, requested line range, and sentinel-derived continuation metadata remain exact. This includes empty windows beyond EOF, which never fabricate a total from the requested offset. ACP uint32 overflow is rejected. Exact full-file reads for `edit_file` and `apply_patch` omit `line` and `limit`, so offsets are never applied twice and edits never operate on a window.

Outbound `fs/write_text_file` uses a fail-stop delivery policy. Cancellation or a deadline can still remove a queued write before delivery. Once the writer has delivered the request, however, AVA cannot safely distinguish a completed client mutation from an unobserved response: it returns the explicit ambiguous-delivery error and aborts the ACP connection before admitting another client-owned mutation. A cancellation racing a writer-claimed write retains the same transport-abort arbitration and reports cancellation only when non-delivery is established; otherwise it reports ambiguous delivery. Reads, permission requests, terminal calls, and control notifications retain normal cancellation behavior.

Terminal execution uses `terminal/create` → `terminal/wait_for_exit` → `terminal/output` → `terminal/release`, with one fresh terminal ID per command and exactly one release attempt after ID acquisition. `terminal/create` is fail-stop resource acquisition: queued cancellation establishes non-delivery, while delivered timeout/cancellation ambiguity or a delivered malformed result without a usable terminal ID aborts the connection because AVA cannot otherwise recover terminal ownership. Pending futures are polled at 10 ms. Prompt cancellation or the local Bash timeout sends `$/cancel_request` for the active wait before locally retiring it, then attempts kill, bounded final output, and release. Cleanup calls have finite deadlines and ignore prompt cancellation so release receives a final bounded opportunity. A response and cancellation race through one pending-call owner. Terminal exit codes accept the complete ACP uint32 range and are represented protocol-neutrally as signed 64-bit values; only zero is success. Signal-only or unavailable status maps to unsuccessful `exit_code=-1`. AVA reapplies byte and line bounds locally even when the client reports truncation. When `terminal/output.truncated` is true, the response is only a retained tail and supplies no authoritative original counts; AVA therefore omits `total_bytes`, `total_lines`, and `omitted_lines` while preserving exact retained output counts, flags, text, and terminal status. Local command execution still reports exact totals. Primary RPC/DTO errors retain precedence and cleanup failures are attached as context; an unconfirmed release is an error. A delivered `terminal/create` that times out before returning a terminal ID is inherently ambiguous, is reported as such, and synchronously poisons the ACP connection because AVA has no ID to kill or release.

Required client result objects and fields remain strict, including filesystem content, terminal ID, and terminal output/truncation. In accordance with the pinned schema's `x-deserialize-default-on-error`, malformed optional response `_meta`, `exitStatus`, `exitCode`, and `signal` fields default absent without discarding valid siblings. A status containing both a valid code and valid signal remains contradictory and is rejected; a status with neither after defaulting is unavailable rather than a DTO failure. Initialize decoding applies the same field-local rule to optional capability objects, booleans, `clientInfo.title`, and `_meta`, while required `clientInfo.name`/`version` determine whether `clientInfo` survives and `protocolVersion` remains strict. ACP envelope, nesting, collection, record, and 256 KiB string bounds are unchanged. Additive unknown object members are accepted. This behavior has deterministic in-memory, fake-client subprocess, official-SDK, and opt-in `acpx` coverage. The evidence table above is exhaustive; it does not imply interoperability with unexecuted clients.

It never merges plugins, global/project MCP, LSP, web tools, skills, `question`, `task`, or subagents. ACP v1 has no general elicitation primitive, so `question` remains hidden; task/subagent execution is also hidden in this profile. Built-in and MCP executions use the same runtime tool-event path and ACP tool updates.

Built-in file tools in ACP, TUI, print, and RPC share a protocol-neutral descriptor-anchored workspace layer. Paths spelled beneath either its logical root or its physical/canonical spelling resolve through the held workspace descriptor. On Linux, `openat2` uses `RESOLVE_BENEATH`: contained relative symlinks can be followed, while escaping links and absolute symlinks (even those pointing back inside) fail with a `use the real path` error. The component-wise `openat`/`O_NOFOLLOW` fallback is stricter and rejects symlink components. Local reads and metadata use opened descriptors; in-workspace writes securely create/walk parents, stage through the verified parent descriptor, sync, and `renameat`. `apply_patch` refuses final symlink targets. ACP still refuses outside-workspace paths entirely, unlike the normal outside-workspace permission handling in other modes. Directory-listing symlink visibility, nonblocking FIFO rejection, and the additive search `skipped_paths` field are described in [the built-in file-tool guide](core/tools.md#workspace-paths-and-search-results).

Session MCP entries use the pinned implicit stdio shape (`name`, absolute `command`, string `args`, and `{name,value}` `env`) and communicate through standards-compliant newline-delimited JSON-RPC. Names, commands, arguments, environment entries, counts, bytes, and duplicates are bounded. HTTP, SSE, and explicit transport discriminators are rejected. The configuration is accepted and retained only as session-local composition; supplying it is not authorization. Discovery and normalized-name collisions fail the prompt rather than shadowing tools. No process starts unless protected persistent operator Allows exactly authorize both launch and server connect; MCP tool calls and resource reads require their own exact persistent Allows. ACP client permission responses and in-memory session grants cannot authorize any of these session MCP operations. Children use the persisted logical session cwd, a clean environment containing only AVA's trusted default `PATH` (or the client's explicit bounded `PATH`) plus explicit variables, exact MCP `2024-11-05` version negotiation, bounded messages/results/list pagination/deadlines, cancellation, process-group cleanup, and RAII shutdown.

For immutable session MCP only, the persistent `mcp.server.launch` rule command is canonical compact JSON with fields in this order: `argv` (the absolute command followed by exact argument strings), `env` (all explicit `{name,value}` pairs sorted by name), `cwd` (the persisted logical child cwd), and `clean_environment:true`. Argument boundaries and every explicit environment value are therefore part of the authorization identity. The identity is never truncated; if it exceeds the persistent-rule command bound, no exact launch Allow can be installed or matched and launch fails closed. Ordinary global/project RPC and TUI MCP permission commands retain their existing space-joined form.

## Permissions

AVA's normal tool policy remains authoritative. Resolution order for an `Ask` decision is:

1. ACP launch-root hard scope and persistent operator rules;
2. for session-supplied MCP, an authoritative `session_config` denial when no exact persistent operator Allow matched;
3. for all other operations, an exact in-memory grant or deny for this host session;
4. for all other operations, outbound `session/request_permission` to the client.

Matching persistent denies and exact Allows remain authoritative. A session MCP persistent Allow must include the exact non-empty command identity for launch, connect ID, `server:tool` call, or `server:resource-uri` read as applicable; a broader rule does not authorize it. An ACP client can never self-authorize its supplied MCP executable through its permission response or an in-memory session grant. It also cannot upgrade a built-in hard deny or expand the launch-approved filesystem root/tool registry. Permission requests for non-session-MCP operations carry the session ID, stable tool-call ID, title, kind, pending status, safe locations, and bounded control-safe action details. Commands show their complete operation identity when it fits the bounded display; an oversized display is visibly truncated and permits only one-shot decisions. File mutations show a bounded proposed diff and deliberately offer only one-shot decisions, because a path/tool grant would not bind later content.

| optionId | kind | Effect | Availability |
|---|---|---|---|
| `allow_once` | `allow_once` | one request | every prompted operation |
| `allow_always` | `allow_always` | exact allow in this host session | non-mutation operations with a complete bounded action display; commands additionally require backend-approved reusable scope and exact recipe authority |
| `reject_once` | `reject_once` | one request | every prompted operation |
| `reject_always` | `reject_always` | exact deny in this host session | non-mutation operations with a complete bounded action display; commands additionally require backend-approved reusable scope and exact recipe authority |

For operations that offer them, always decisions match the complete operation, mode, workspace, normalized logical root-relative target, and tool identity, plus the exact command or backend workspace recipe key for a reusable sealed command. Existing path components are validated through the same descriptor-anchored layer before the permission request or grant key is formed. Commands with an unverified executor, critical/one-shot backend scope, or missing exact recipe authority cannot offer always decisions, even when their display fits. Delegated ACP terminal commands have an unverified executor and backend maximum scope `once`, so they are always one-shot. Grants are bounded process memory only, are never wildcarded or persisted, and are cleared on close/shutdown. A path retargeted through a symlink after approval fails the actual descriptor-relative operation and cannot reuse a grant. File mutations never receive an in-memory session grant; an unoffered always response fails closed. Permission responses accept only the pinned `selected` shape with one offered option ID or `cancelled`. Invalid, failed, or timed-out responses fail closed with an actionable tool error. Prompt/client-request cancellation yields a Cancel resolution and terminal `cancelled` stop reason; late responses are ignored and cannot create grants.

Permission audits retain request/tool/operation identity and use protocol-neutral resolver sources: `policy`, `persistent_rule`, `persistent_rule_error`, `client`, `session_grant`, `hard_scope`, `session_config`, or `client_cancel`. ACP command arguments are redacted from durable permission records. Legacy persisted aliases `acp_client`, `acp_session_grant`, `acp_hard_policy`, `acp_session_mcp`, `acp_client_cancel`, and `acp_client_error` are accepted only when reading older sessions and are never emitted by current writers.

RPC and TUI permission behavior is unchanged.

## Session and connection safety

A connection owns at most 32 independent hosts. IDs are exact opaque persistent IDs; prefix lookup is disabled. Each host holds an exclusive CLOEXEC advisory session lease. Different sessions may run concurrently, but one session admits one prompt. Generation-aware cancellation cannot leak into a later prompt and cannot overwrite a committed completion.

Every lifecycle `cwd` must be absolute, existing, normalized, traversal-free, and lexically equal to or below the approved logical launch root. AVA preserves a logical `$PWD` spelling, including symlink aliases in that root, when it identifies the same directory as the physical launch cwd. The retained launch-root descriptor anchors filesystem authority; descendants are opened relative to it with beneath-root containment checks and a no-follow final component for lifecycle `cwd`. A different physical/canonical spelling can therefore fail this lifecycle lexical root check even when it identifies the same directory. The original logical cwd is persisted and cannot be rebound on load/resume. In accordance with the pinned schema's field-local defaults, a malformed `additionalDirectories` field defaults empty and invalid array items are skipped; any normalized valid non-empty directory still fails because AVA does not advertise additional roots. A missing or malformed `mcpServers` field defaults empty and malformed server entries are skipped; valid HTTP/SSE or explicit transport entries remain unsupported and fail. Malformed optional prompt-content annotations, `_meta`, and optional resource/image fields default absent while required content fields and unsupported discriminators remain strict.

Exact session resume reads are bounded by 8 MiB / 1 MiB line / 16,384-entry session limits but emit no replay, as required by ACP v1 resume semantics. Before inserting a resumed host, AVA validates the replayable history against the immutable startup model and rejects image, tool, or provider-native reasoning history that model cannot safely consume with `-32602`; it never switches back to the persisted model. Before each provider run, AVA streams that bounded session history to seed the finalized provider tool-call ID set. IDs are unique across the complete persistent session, so reuse is rejected before ACP updates, persistence, or dispatch. Provider fallback IDs include a fresh secure response/parser component while retaining same-stream fragment merging. `session/list` accepts omitted parameters, `params:null`, or an object; omitted/null values normalize to the empty object. List pages contain at most 50 records and use at most 16 bounded connection-local snapshots. Full `session/load` replay remains deferred rather than dropping rich history.

ACP framing and peer limits remain:

| Resource | Limit |
|---|---:|
| Input or encoded output record | 1 MiB |
| JSON nesting / collection items | 64 / 4,096 |
| Any JSON string/key | 256 KiB |
| Outbound queue | 256 records / 4 MiB |
| Pending outbound calls | 64 |
| Concurrent inbound requests / worker queue | 8 / 32 |
| Service workers | 10 |
| Default outbound permission deadline | 30 seconds |
| File/create/output client call deadline | 5 seconds |
| Terminal kill/release cleanup deadline | 2 seconds |
| Command wait deadline | requested Bash timeout, at most 120 seconds |
| Client-call poll interval | 10 ms |
| Stalled fd write deadline / shutdown grace | 2 seconds / 2 seconds |

The peer has one writer. Outbound request IDs combine a connection nonce with a monotonic counter. A response for a queued ID is ignored; a response received while the writer has claimed the request is staged and can fulfill the future only after successful write acknowledgement. Write failure discards staged data and closes the connection. Canceling a claimed request first marks cancellation requested, then aborts the transport so the writer acknowledgement and cancellation have one owner; queued requests can be invalidated locally. Delivered normal requests may be retired without closing the connection, while delivered fail-stop file writes poison and abort it. Thus a guessed or backpressured response cannot create a permission grant before the request is visible, and an ambiguously completed client file mutation cannot be followed by another reported-successful mutation on the same connection.

No session/registry mutex is held across provider, tool, outbound request, or future waits. Gateways are weak from hosts and unbound before peer teardown. EOF, output failure, shutdown, prompt cancellation, and deadlines release waiters; response/deadline races have one completion owner. For inbound prompts, the JSON-RPC request and host run controller share the real `Completing` commit boundary: `$/cancel_request` before it wins with `-32800`, while cancellation after it is a no-op and the successful `PromptResponse` is retained. Mutating lifecycle requests use the same peer committer: initialize commits after result validation and before publishing initialized state; new/resume/close commit immediately before registry mutation. Cancellation that wins first performs no mutation, while cancellation after commit is a no-op and the actual lifecycle success/error is retained.

### Error matrix

| Condition | JSON-RPC outcome |
|---|---|
| Missing configured provider credentials | `-32000` with setup guidance |
| Unadvertised/deferred method, including `session/load` | `-32601` |
| Invalid prompt block or image sent to a text-only session model | `-32602` |
| Permission/policy or non-authentication provider setup failure | `-32603`; never mislabeled as authentication required |
| `$/cancel_request` before prompt terminal commit | `-32800` |
| `$/cancel_request` after prompt terminal commit | Existing success/error response; cancellation loses |
| Outbound permission canceled while queued | Future `-32800`; record is skipped |
| Outbound permission canceled/timeout while writer-claimed | Connection abort plus one cancellation-or-ambiguous-delivery terminal |
| Outbound file write canceled/timeout while queued | Future `-32800`; record is skipped and the connection remains usable |
| Outbound file write canceled/timeout after delivery | Explicit ambiguous-delivery error; connection is synchronously poisoned and aborted |
| Response before outbound delivery acknowledgement | Ignored if queued; staged if claimed, never fulfilled early |
| Terminal create timeout after possible delivery | Error explicitly reports ambiguous creation; connection is synchronously poisoned because no terminal ID is available for cleanup |
| Terminal cleanup failure, including unconfirmed release | Primary error with cleanup context; never clean success |

## Authentication and stop reasons

This stable v1 profile does not advertise in-protocol authentication. Configure credentials before launch, for example with `ava connect openai` or the provider environment variable. Missing credentials return `-32000` with setup guidance.

Runtime terminal outcomes map exhaustively to `end_turn`, `max_tokens`, `max_turn_requests`, `refusal`, and `cancelled`; abnormal runtime error is a JSON-RPC request error.

## Normative pin

AVA targets only `tests/fixtures/acp-v1/schema.json`:

- release `schema-v1.19.0`;
- source commit `a213df5240048f96d2b23f644984bb20c188a234`;
- SHA-256 `92c1dfcda10dd47e99127500a3763da2b471f9ac61e12b9bf0430c32cf953796`.

Unstable schema and draft v2 are not used.
