# AVA Architecture

This document is the maintainer-level map of the architecture implemented under
`src/ava/`. It describes ownership and data flow; it does not redefine wire,
configuration, persistence, or security contracts. Follow the linked normative
documents when exact fields or behavior matter.

## System context

AVA is a C++23 local coding agent. The normal `ava` process owns configuration,
model-provider communication, prompt assembly, the agent/tool loop, permissions,
session persistence, and one selected frontend. It can start local child
processes for commands, plugins, MCP servers, and LSP servers. It can contact
model and explicitly approved web endpoints through the HTTP layer.

External actors are:

- a person using the ncurses TUI or one-shot print mode;
- an automation client using proprietary JSONL RPC;
- an editor using ACP v1 over JSON-RPC 2.0 stdio;
- configured model and web services;
- the workspace and XDG-owned configuration/state trees; and
- approved command, plugin, MCP, and LSP child processes.

AVA is not a network daemon. RPC and ACP are stdio protocols. The optional Qt
Quick desktop target is an experimental, separately built prototype and does
not currently embed the production agent runtime. See
[`desktop-qml.md`](../interfaces/desktop-qml.md).

## Processes and frontends

`src/main.cpp` initializes the application and calls `ava::app::run` in
`src/ava/app/app.cpp`. Argument and terminal detection select one of these
adapters:

| Adapter | Main implementation | Boundary |
| --- | --- | --- |
| Interactive terminal | `app/interactive.cpp`, `app/interactive_tui.cpp`, `tui/` | ncurses input/rendering around the shared runtime |
| Print | `app/print_mode.cpp` | one prompt; text or versioned event output; noninteractive permission policy |
| RPC | `app/rpc_mode.cpp`, `app/rpc/` | proprietary JSONL protocol, worker/resolver and session commands |
| ACP | `app/acp_mode.cpp`, `app/acp/` | ACP v1 JSON-RPC 2.0 peer, sessions, permissions, and client capabilities |
| Desktop prototype | `desktop/main.cpp`, `desktop/qml/` | optional `ava-desktop` Qt process; prototype controller only |

The first four adapters converge on `app/runtime.*`, an open
`app::runtime::Session`, and `run_prompt`. Frontends own presentation,
protocol framing, and how an `Ask` permission or question reaches the user.
They must not reimplement provider requests, tool policy, or session mutation.
The exact automation contracts are in
[`headless-protocol.md`](../headless-protocol.md),
[`rpc-protocol.md`](../rpc-protocol.md), and [`acp.md`](../acp.md).

## End-to-end run flow

The same logical pipeline serves TUI, print, RPC, and ACP:

```text
user/editor/client input
  -> frontend parses input and supplies permission/question/event adapters
  -> app runtime opens or resumes Session and admits one correlated run
  -> prompt state combines the base prompt, trusted configuration and context
  -> SessionReadAuthority projects bounded history into provider messages
  -> AgentLoop builds a provider-neutral ProviderRequest and tool schemas
  -> Provider serializes it; HTTP Transport sends it and streams response bytes
  -> provider StreamParser emits bounded provider-neutral StreamEvents
  -> AgentLoop accumulates text/reasoning/tool calls
       -> ToolDispatcher resolves each registered built-in/plugin/MCP tool
       -> permission policy/resolver decides Allow, Ask, or Deny
       -> tools perform bounded work; process tools honor cancellation/cleanup
       -> tool results are persisted and returned in the next provider request
  -> committed assistant output is appended as one guarded v4 transaction
  -> app adapters convert internal progress to neutral RuntimeEvents
  -> frontend renders or serializes events and the terminal result
```

More detail by stage:

1. **Open.** `app/runtime_session.cpp` and `app/runtime_sessions.cpp` resolve the
   workspace, XDG paths, model, credentials, trust, context, anchors, and a
   persistent or ephemeral session. A persistent runtime retains the exact
   `SessionLease` for its lifetime. Torn-tail and incomplete assistant-output
   recovery occur only at the explicit open/resume boundary.
2. **Admit.** `SessionRunController` allows one active correlated run, rejects a
   different concurrent request, exposes steering/wake/stop controls, and owns
   serialized append routes. A persistence failure is latched rather than
   hidden by a retry.
3. **Assemble.** `app/runtime_prompt*.cpp`, `context/`, and `config/` produce the
   active system prompt. Runtime history reads use `SessionReadAuthority`, which
   carries the exact leased inode (or shared ephemeral state) and the selected
   read limits. `agent/history_projection.*` and message builders turn history
   into provider-neutral messages.
4. **Invoke.** `AgentLoop` coordinates provider iterations. A `Provider`
   converts `ProviderRequest` to `HttpRequest`; `Transport` performs buffered or
   streaming I/O. Provider parsers normalize native SSE/JSON into bounded
   `StreamEvent` values. Retry and observation are decorators around transport,
   not provider-wire behavior.
5. **Dispatch.** `ToolRegistry` is composed from built-ins and permitted plugin
   and MCP brokers. `ToolDispatcher` validates a call and invokes the owning
   handler. `tools/` implements low-level file/search/process/web/LSP operations;
   `agent/tool_dispatch_*.cpp` adapts them to model-visible schemas, results,
   services, timeline records, and session entries. Tool results feed the next
   provider iteration until a terminal assistant response or bounded stop.
6. **Commit and publish.** Active append routes write through the controller's
   immutable `SessionAppendTarget`. Version 4 assistant output is staged and
   committed as one batch before it becomes visible. `app/runtime_event_adapters.*`
   removes internal/provider-private detail and creates neutral events from
   provider and tool state. Frontend sinks render or serialize those events.

The public session format and event/wire projections deliberately differ from
provider-native replay state. Do not expose opaque provider replay fields when
adding an event or protocol field. See [`session-format.md`](../session-format.md).

## Module and dependency boundaries

`src/ava/CMakeLists.txt` separates backend libraries from frontend libraries.
The table below describes the intended ownership direction; concrete target
links in each module's `CMakeLists.txt` and the enforced include policy in
`tests/module_dependency_rules_test.py` are the mechanical authority.

| Layer/module | Owns | Must not own |
| --- | --- | --- |
| `core` | errors/results, IDs, strict JSON helpers, paths, descriptor-safe opens, anchors, process arguments | product policy, UI, provider or protocol behavior |
| `observability` | optional private run observation records | public runtime events, session truth, wire output |
| `event` | immutable frontend-neutral runtime events, envelopes, serialization | agent/provider/tool behavior or frontend rendering |
| `http` | transport interfaces, curl implementation, retry/observation decorators | provider-specific payloads or credentials policy |
| `command` | canonical command discovery, intent, sealed plans, environment and process-group primitives | user permission decisions or UI prompts |
| `config`, `context` | XDG/auth/model/prompt profiles; bounded markdown/skill loading | active-run orchestration or tool execution |
| `containment`, `permissions` | command containment plans; operation policy, rules, prompts and decisions | shell execution, frontend prompting, protocol framing |
| `provider` | provider-neutral request/events plus native request serialization and response parsing | retries, tool execution, session mutation, UI |
| `session` | JSONL records, validation/projections, lease/read/append authority, recovery, attachments, branches/exports | prompt execution or frontend state |
| `tools`, `lsp` | bounded side-effect primitives and LSP subprocess/protocol management | model-visible tool-loop policy or presentation |
| `plugin`, `mcp` | discovery/config, bounded child protocols, permissioned tool/resource brokers | in-process third-party execution or agent-loop ownership |
| `diagnostics` | private bounded artifacts, safe failures and support records | raw secrets/provider payload publication |
| `agent` | provider/tool iteration, message projection, tool registry/dispatch, scheduling, subagents/jobs | CLI/protocol/TUI policy or direct session-path reacquisition |
| `app` | composition root, runtime session, commands, run lifecycle, frontend adapters, RPC/ACP services | low-level reusable primitives |
| `tui`, `tui/terminal` | semantic terminal state, composer/input/rendering; ncurses terminal lifecycle and cells | provider/session/tool implementations |
| `desktop` | experimental Qt/QML shell | production runtime behavior until explicitly integrated |
| `debug` | libcwd-only formatting/debug support | product behavior or required release functionality |

Important dependency rules:

- `event` is the neutral publication boundary and may depend only on lower-level
  `core`/debug support, never `app`, `agent`, or a frontend.
- `provider` depends on `http` and configuration types, not on `agent` or
  `session`; the agent owns the provider/tool loop.
- `session` does not call the agent. The runtime injects append/read authority
  into the loop.
- `tools`, plugin brokers, and MCP brokers remain below `agent`; brokers do not
  register by reaching upward. `agent/tool_registration.cpp` pulls descriptors
  into its registry.
- `app` is intentionally the widest dependency/composition layer. New reusable
  policy should move downward only when its ownership and dependency direction
  are clear.
- Public target links are transitive compile interfaces, not a promise of a
  stable external C++ SDK. Internal headers are repository implementation
  details; see [`documentation.md`](documentation-policy.md).

## Authority and trust boundaries

AVA distinguishes possession of a path string from authority to mutate or read
an object:

- **Workspace filesystem authority.** `core::AnchorSet` retains pre-opened
  directory descriptors. File tools resolve model-supplied paths beneath the
  matching anchor and reject escapes rather than relying on canonicalize-then-open.
- **Session mutation authority.** A persistent `SessionLease` pins and locks the
  exact session inode. `SessionAppendTarget` combines the store with a duplicated
  lease; `SessionRunController` serializes immutable-generation append routes.
  Sessionless runs use explicitly named in-memory paths.
- **Session read authority.** `SessionReadAuthority` is copyable but policy-bound:
  it carries an exact lease duplicate or shared ephemeral state and fixed read
  limits. Current-runtime code must not reopen the session pathname as a
  fallback.
- **Project trust.** Trust controls loading project-owned prompts, skills,
  commands, plugins, MCP, and LSP configuration. It does not grant runtime tool
  permission and is not a sandbox.
- **Tool permission.** Backend policy classifies every operation before a
  frontend resolver can approve an `Ask`. Persistent rules cannot upgrade hard
  denies. Command approval binds a sealed plan/recipe identity, not untrusted
  display text.
- **Child processes.** Commands, plugins, MCP servers, and LSP servers are
  separate processes with bounded protocols, environments, deadlines, and
  cleanup. Process separation protects AVA protocol/runtime integrity; it is
  not general OS isolation. Exact containment claims and limits are normative in
  [`security-sandboxing.md`](../security/sandboxing.md) and
  [`security/containment.md`](../security/containment.md).
- **Credentials and diagnostics.** Credentials remain runtime inputs and must
  not enter debug aggregates, sessions, public events, RPC, or exports.
  Provider failures are normalized at the provider boundary. Observability is
  opt-in, private, bounded, and separate from event/session truth; see
  [`diagnostics.md`](../operations/diagnostics.md).

## Concurrency and cancellation

Concurrency is deliberately bounded and owned at explicit seams:

- `SessionRunController` admits at most one active run per runtime session. It
  uses a `std::stop_token`, correlated command queues, finite queue limits, and
  generation-bound append routes. Shutdown rejects new work, drains or fails
  accepted appends, then releases persistence authority.
- RPC owns an input loop, prompt worker, resolver state, and synchronized output.
  ACP's peer owns request workers and stop tokens. Protocol adapters convert a
  client cancel into the same runtime cancellation path.
- `AgentLoop` polls the injected cancellation callback across transport,
  provider iterations, tools, compaction, steering, and subagents. Cancellation
  is cooperative; each blocking boundary must have a finite timeout or an
  unblocking mechanism.
- Read/search tool parallelism is optional and bounded. Only calls proven
  noninteractive by permission preflight may enter a parallel read epoch;
  barriers and mutations remain ordered. Results preserve provider order.
- Background task subagents are process-local and application-scoped through
  `SubagentCoordinator`; the registry has bounded concurrency/retention.
  Completion delivery is synchronized at safe parent-turn boundaries.
- Child command and protocol processes have finite deadlines and explicit
  TERM-to-KILL/process-group cleanup where supported. Do not infer containment
  of descendants that create another session.
- Event bus subscribers run synchronously in registration order and publishing
  stops at the first error. User observer callbacks are not a place for
  unbounded or reentrant work.

When changing locking, preserve documented local lock orders and never call an
unknown callback while holding an internal state lock unless that API explicitly
requires it. The repository-wide C++ rules are in
[`development/cpp-safety-rules.md`](cpp-safety-rules.md).

## Persistence model

Persistent sessions are append-only JSONL plus sibling attachment storage.
`SessionStore` owns format and validation; it does not by itself confer runtime
authority. Persistent mutation requires the exact active lease, and normal
runtime mutation flows through an append target/controller. Ephemeral sessions
share bounded in-memory state and are never silently converted to pathname I/O.

The read path strictly validates records, versions, parent relationships,
transaction state, line/file/entry bounds, and cancellation. Opening/resuming is
the only mutation boundary for torn final records and incomplete v4 assistant
transactions. Listing is non-mutating. Branch/import handoffs retain acquired
store/lease ownership rather than reopening by name.

Physical v4 assistant-output staging records support atomic logical visibility:
ordered items are appended with a matching terminal commit; public projections
ignore an incomplete valid suffix and never expose private replay metadata.
Compaction appends a summary boundary rather than rewriting old records.
Attachments are descriptor-validated separately from JSONL metadata.

Exact envelopes, compatibility, recovery semantics, and attachment caveats are
specified in [`session-format.md`](../session-format.md) and
[`development/session-versioning.md`](session-versioning.md).

## Extension and provider seams

### Providers

Implement `provider::Provider` for request/auth behavior and a bounded
`StreamParser` for native response streams, then register a factory in
`builtin_provider_registry()`. Keep provider-native serialization/parsing in
`provider/`; use `http::Transport` rather than issuing I/O directly. Normalize
unknown errors/finish reasons, enforce parser limits before creating event
vectors, and strip private replay fields before public event adaptation. Model
catalog/auth defaults belong in `config/`, not the provider parser. Protocol
and live-smoke status is in [`providers.md`](../core/providers.md).

### Built-in tools

Add metadata/schema and registration in `agent`, a dispatch adapter in the
matching `agent/tool_dispatch_*.cpp`, and reusable bounded mechanics in
`tools/`, `lsp/`, or another owning lower layer. Every side effect needs a
backend operation, permission/audit path, cancellation/deadline behavior,
output bounds, session/tool events, and tests. Follow
[`development/side-effect-safety-checklist.md`](side-effect-safety-checklist.md).

### Plugins and MCP

Plugins and MCP servers are out-of-process integrations. Their modules own
manifest/config discovery, protocol bounds, lifecycle, and brokers. The agent
pulls brokered descriptors into one per-run registry so external tools use the
same permission, event, result, and cancellation path as built-ins. Do not add
an in-process native extension ABI or let a broker bypass tool policy. Normative
authoring/compatibility details live in [`plugin-system.md`](../extensions/plugin-system.md),
[`plugin-compatibility-policy.md`](../plugin-compatibility-policy.md), and
[`mcp.md`](../extensions/mcp.md).

### Frontends

A new frontend should adapt input, event output, permission/question resolution,
and cancellation around `app::runtime::Session` and `run_prompt`. It should not
parse session JSONL, construct provider wire payloads, or execute tools itself.
A stable external contract needs its own versioned normative document and
golden/whole-process tests.
