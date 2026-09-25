# AVA Backend Current State

Milestone M0.1 of the [backend modernization ledger](backend-modernization-progress.md).
This is a **current-state map**, not a target design. Nothing here proposes changes;
sections describe the code as it exists at the starting commit.

## Scope and method

- **Starting commit:** `c94ac8631419` (full: `c94ac863141975806bbab52e950a2f2499108b65`).
  The inventory below was taken from the committed tree at that commit plus the
  ledger-only commit `78ca9a06` (`docs: start backend modernization ledger`), which
  touches no source files. The branch was created while unrelated working-tree changes
  existed under `docs/core/`, `src/ava/agent/`, and `src/ava/app/`; those changes were
  preserved separately and are **not** reflected here. This document maps the clean
  committed tree only.
- **Method:** static source reading of the prompt path, every process-spawning
  subsystem, the session storage stack, and the cancellation/shutdown wiring. No
  benchmarks, RSS measurements, or runtime tracing were performed; every claim is
  backed by a linked source file and named symbol. Line numbers are deliberately
  not cited because they drift; symbols and file paths are the stable references.
- **Platform note:** all process-spawning code paths inventoried here are POSIX
  (`fork`, `execve`/`fexecve`, `setpgid`, `kill`, `waitpid`). `_WIN32` conditionals
  exist only in diagnostics/session record formatting, not in process management.

## Ownership diagram

Ownership is expressed with `std::shared_ptr` for application-scoped services and
with stack-scoped construction for per-run objects. There is no central registry of
live child processes or threads; ownership is distributed across the objects below.

```
Application (src/ava/app/app.cpp, src/ava/core/Application.cpp)
│
├─ runtime::Session  (src/ava/app/runtime/Session.h, session_ts.h; thread-safe wrapper)
│   ├─ SessionStore                       value member; storage identity only, no entry cache
│   ├─ SessionRunController               shared_ptr; run admission, phases, stop, append queues
│   │   └─ SessionAppendTarget            shared_ptr; sole runtime append authority (duplicated lease)
│   ├─ SessionReadAuthority               optional; lease-bound read policy for this session
│   ├─ SubagentCoordinator                shared_ptr, application-scoped
│   │   └─ BackgroundJobRegistry          value member; owned JoinThread per background job
│   ├─ SubagentDeliveryManager            shared_ptr; owns "SA_delivery" JoinThread worker
│   ├─ SessionTitleCoordinator            shared_ptr; owns "session_title" JoinThread worker(s)
│   ├─ BranchSummaryCoordinator           owns "branch_summary" JoinThread worker
│   ├─ RuntimeDiagnostics                 shared_ptr
│   ├─ McpConfig                          shared_ptr<const>
│   └─ ProviderCatalog                    shared_ptr<const>; built once, pinned for process life
│
├─ per prompt run (stack of run_admitted_prompt, src/ava/app/runtime_prompt.cpp)
│   ├─ ActiveRunGuard                     admission token from SessionRunController
│   ├─ RetryTransport / ObservedTransport optional wrappers around the caller's Transport
│   ├─ ConfiguredLspProvider              shared_ptr; caches LSP clients for this run only
│   └─ AgentLoop (src/ava/agent/agent_loop.cpp)
│       └─ detail::AgentTurnExecutor (src/ava/agent/agent_turn_executor_internal.h)
│           ├─ AgentTurnSession           per-turn session append/read adapter
│           ├─ ToolContext + ToolDispatcher   per run (src/ava/agent/tool_dispatcher.cpp)
│           │   └─ ToolRegistry           per-run composition of static builtins + plugin + MCP
│           ├─ ProviderEventAccumulator   per provider turn
│           └─ PendingCommittedToolResults  per committed assistant turn
│
└─ per frontend (TUI: src/ava/tui/runtime.h, event_state.h; RPC/ACP workers)
    ├─ TuiEventState                      transcript/pending/queue vectors (unbounded-by-design UI state)
    ├─ ApplicationCatalogCoordinator      cached session tree + summaries
    ├─ MermaidRenderCoordinator           owns "mermaid_render" JoinThread worker
    └─ RunObservation (observability)     bounded queue + owned writer JoinThread (opt-in)
```

Key ownership facts:

- Per-run state (`AgentLoop`, `AgentTurnExecutor`, `ToolDispatcher`, LSP provider) is
  constructed on the `run_admitted_prompt` stack in
  [src/ava/app/runtime_prompt.cpp](../../src/ava/app/runtime_prompt.cpp) and destroyed
  when the run returns. Nothing about a run is registered globally.
- `BackgroundJobRegistry` workers are `ava::core::JoinThread` (a checked `std::jthread`
  wrapper in [src/ava/core/thread.h](../../src/ava/core/thread.h)) created with a
  `"background_job"` label in
  [src/ava/agent/background_job_registry.cpp](../../src/ava/agent/background_job_registry.cpp);
  destruction requests stop and joins.
- The same `JoinThread` pattern owns the coordinator workers: `SubagentDeliveryManager`
  ([src/ava/app/subagent_delivery_manager.cpp](../../src/ava/app/subagent_delivery_manager.cpp)),
  `SessionTitleCoordinator`
  ([src/ava/app/session_title_coordinator.cpp](../../src/ava/app/session_title_coordinator.cpp)),
  `BranchSummaryCoordinator`
  ([src/ava/app/branch_summary_coordinator.cpp](../../src/ava/app/branch_summary_coordinator.cpp)),
  `MermaidRenderCoordinator`
  ([src/ava/app/mermaid_render_coordinator.cpp](../../src/ava/app/mermaid_render_coordinator.cpp)),
  and the trace writer in
  [src/ava/observability/run_observer.h](../../src/ava/observability/run_observer.h).

## Process lifecycle diagram

There is **no unified process table**. Each subsystem spawns, watches, cancels, and
reaps its own children with local RAII. The common shape:

```
spawn site                parent setup              child setup (post-fork, pre-exec)
────────────────────────  ────────────────────────  ─────────────────────────────────
curl (provider I/O)       pipes; config via stdin   dup2 stdio; PATH reset; execlp curl
bash leader               4 pipes (output, status,  setpgid(0,0) behind gate; waits for
                          control, sentinel gate)   release byte; optional containment;
                                                    fexecve of approved descriptor
bash sentinel             forked by AVA as sibling  joins verified PGID; pause() loop;
                                                    never execs; holds PGID identity
plugin worker             3 pipes (stdin/out/err)   setpgid(0,0); chdir; PATH reset; execvp
MCP server                3 pipes                   setpgid(0,0); chdir; env built; execve
LSP server                2 pipes + gate pipe       setpgid(0,0) behind gate; /dev/null
                                                    stderr; fexecve or trusted-path execve
mermaid helper            2 pipes + exec pipe       setpgid(0,0); SIGSTOP handshake;
                                                    fexecve of O_NOFOLLOW descriptor
browser opener            none (detached)           double-fork; setsid; stdio→/dev/null
clipboard helper          1 pipe                    dup2; stderr→/dev/null; execvp
external editor           none                      std::system("exec $EDITOR file")

teardown (all distributed RAII, per owner):
  curl:      cancel → SIGKILL pid + waitpid (kill_and_wait); success → waitpid
  bash:      SIGTERM group (300 ms grace) → SIGKILL group → wait for group exit (500 ms);
             leader and sentinel reaped individually
  plugin:    normal EOF shutdown reaps the leader only; timeout/cancel may signal its group
             while the leader is tracked; no post-leader process-group verification
  MCP:       same leader-only normal-shutdown gap; forced termination targets the group only
             while the direct child is still considered live
  LSP:       process-group kill on client destruction; cached clients die with the run
  mermaid:   SIGCONT after handshake; deadline/cancel → group kill; reaped by coordinator
  browser:   grandchild deliberately orphaned (re-parented to init); no reaping possible
  clipboard: bounded wait; deadline → SIGKILL + waitpid
  editor:    std::system blocks until exit; temp file RAII cleanup
```

## Model turn and tool-call lifecycle

Entry points into `ava::app::run_prompt`
([src/ava/app/runtime_prompt.cpp](../../src/ava/app/runtime_prompt.cpp),
declared in [src/ava/app/runtime.h](../../src/ava/app/runtime.h)):

- interactive TUI submit path: `handle_interactive_submission` in
  [src/ava/app/interactive.cpp](../../src/ava/app/interactive.cpp) (two call sites:
  direct prompt and command-expanded prompt)
- headless print mode: [src/ava/app/print_mode.cpp](../../src/ava/app/print_mode.cpp)
- RPC prompt worker: [src/ava/app/rpc/prompt_worker.cpp](../../src/ava/app/rpc/prompt_worker.cpp)

Sequence inside one prompt run:

1. **Admission.** `run_prompt` consults `SessionRunController::inspect_admission` /
   `admit` ([src/ava/app/session_run_controller.h](../../src/ava/app/session_run_controller.h)).
   A second concurrent request either joins the existing outcome or is rejected;
   there is no backend prompt queue (frontends keep their own visible queues).
2. **Run setup.** `run_admitted_prompt` snapshots session state under the session
   lock, refreshes the subagent delivery parent capsule, builds the plugin event
   observer sink, optionally wraps the transport in `RetryTransport`, obtains a
   lease-bound `SessionReadAuthority`, and takes the guard's append routes.
3. **Loop construction.** An `AgentLoop` is stack-constructed with `AgentLoopOptions`
   ([src/ava/agent/agent_loop.h](../../src/ava/agent/agent_loop.h)): model invocation
   options, tool resources (plugin dirs, MCP config files, session MCP config, skill
   dirs, LSP provider), tool execution policy, subagent definitions, and callbacks.
   Per-turn bounds live here: `max_tool_iterations` (10), `max_provider_events` (4096),
   `max_assistant_text_bytes` (256 KiB), `max_tool_argument_bytes` (256 KiB),
   `max_tool_result_context_bytes` (8 KiB).
4. **Turn execution.** `AgentLoop::run_turn` attaches an optional `RunObservation`
   trace scope, then delegates to `detail::AgentTurnExecutor::run`
   ([src/ava/agent/agent_turn_executor.cpp](../../src/ava/agent/agent_turn_executor.cpp)):
   - load persisted provider tool-call ids; cancellation checkpoint
     (`AgentTurnSession::check_canceled` in
     [src/ava/agent/agent_loop_session.cpp](../../src/ava/agent/agent_loop_session.cpp));
   - optional pre-turn auto-compaction via the `compact_context` callback into
     [src/ava/app/runtime_compaction.cpp](../../src/ava/app/runtime_compaction.cpp);
   - append the active-turn user message through the run's append route;
   - build subagents (built-in defaults when none configured) and **initialize tools**:
     `AgentTurnExecutor::initialize_tools` constructs the `ToolContext` and a per-run
     `ToolDispatcher`, whose registry is composed per run by `compose_tool_registry` in
     [src/ava/agent/tool_registration.cpp](../../src/ava/agent/tool_registration.cpp):
     the static built-in table (`builtin_tool_registry()`, a function-local static fed
     from `builtin_tool_metadata()`), plus plugin-brokered tools
     (`ava::plugin::visit_enabled_plugin_tools`) and MCP-brokered tools
     (`ava::mcp::visit_enabled_mcp_tools`);
   - **provider loop** (`while (true)`):
     `request_provider_turn` → `build_messages` (a full history materialization, see
     session section) → provider request build → transport send (streaming or unary)
     with a `ProviderEventAccumulator` → parsed `ParsedAssistantTurn`;
     `persist_assistant_turn` stages the v4 assistant-output batch and its
     `AssistantTurnCommit` (visible to readers only on commit);
     if no tool calls remain, map `finish_reason` to a terminal outcome and return;
     otherwise `execute_tools` dispatches calls — sequentially, or in a parallel epoch
     scheduled by `detail::ToolScheduler`
     ([src/ava/agent/tool_scheduler.cpp](../../src/ava/agent/tool_scheduler.cpp)), which
     runs slots on per-epoch `JoinThread` workers — and commits each
     `ToolDispatchResult` through `PendingCommittedToolResults`
     ([src/ava/agent/agent_turn_executor_internal.h](../../src/ava/agent/agent_turn_executor_internal.h))
     so every committed provider tool call is settled with a durable result;
     loop until `max_tool_iterations` (terminal `MaxTurnRequests`) or a terminal
     disposition.
5. **Run completion.** `run_admitted_prompt` emits terminal events, transitions the
   guard to `Completing`, and calls `ActiveRunGuard::complete`, which drains queued
   appends; a persistence failure there overrides the proposed outcome to
   `PersistenceError`. Session-title scheduling happens strictly after guard
   completion.

Provider I/O for every provider call goes through the run's
`ava::http::Transport`, in production always `CurlCliTransport`
([src/ava/http/curl_transport.h](../../src/ava/http/curl_transport.h)), which forks a
`curl` child per request (see the spawn table). Background/scheduled work (session
titles, branch summaries, subagent children) constructs its own provider and
`CurlCliTransport` instances via factories passed in `AgentLoopOptions`
(`background_provider_factory`, `background_transport_factory`) and in the app-level
coordinators.

## Session write / read / compaction / branch lifecycle

Format references: [docs/session-format.md](../session-format.md),
[docs/development/session-architecture.md](../development/session-architecture.md),
[docs/development/session-versioning.md](../development/session-versioning.md), and
[docs/development/internals/session-run-controller.md](../development/internals/session-run-controller.md).

**Write path.** Sessions are append-only JSONL; `kCurrentSessionEntryVersion` is 4 in
[src/ava/session/session_store.h](../../src/ava/session/session_store.h). All runtime
persistent writes flow through `SessionAppendTarget`
([src/ava/session/session_store.h](../../src/ava/session/session_store.h),
implementation in `session_store_append.cpp` / `session_store_authority.cpp`):

- The target is created from an active exclusive `SessionLease`
  ([src/ava/session/session_lease.cpp](../../src/ava/session/session_lease.cpp));
  it duplicates the locked open-file description and revalidates the exact published
  inode before accepting appends.
- Run-scoped appends are routed through `SessionRunController` queues (bounded:
  `kMaxSessionAppendQueueEntries` 256 / `kMaxSessionAppendQueueBytes` 4 MiB; commands
  64 / 64 KiB; retained outcomes 64) so frontends never write the store directly.
- v4 assistant output is a staged transaction: `append_batch` reserves and writes zero
  or more `AssistantOutputItem` records followed by exactly one `AssistantTurnCommit`;
  the staged items become visible to readers **only on commit**. Schema and bounds
  (`kMaxAssistantOutputItemsPerTurn` 4096) live in
  [src/ava/session/assistant_output.h](../../src/ava/session/assistant_output.h).
- After a partial batch write failure the target latches and rejects all mutations
  until `recover()` rebuilds state from storage; torn-tail and incomplete-suffix
  recovery are explicit open/resume boundaries with quarantine for persistent
  sessions. Ephemeral sessions keep their whole transcript in memory
  (`SessionStore::EphemeralState::entries` in
  [src/ava/session/session_store_internal.h](../../src/ava/session/session_store_internal.h))
  and have no quarantine path.

**Read path.** `SessionStore` itself does **not** cache entries; every read re-scans:

- `SessionReadAuthority::load()` / `load_bounded()`
  ([src/ava/session/session_store_read.cpp](../../src/ava/session/session_store_read.cpp))
  perform a full lease-bound snapshot (`pread` on the owned lease duplicate; persistent
  reads never reopen by pathname) and materialize a `std::vector<SessionEntry>`.
  `SessionReadLimits` supplies bounded defaults (8 MiB file, 1 MiB line, 16384
  entries), but normal CLI/TUI/RPC `OpenContext` leaves the override unset and
  `SessionReadAuthority` therefore uses `legacy_unbounded_session_read_limits()` for
  file and entry counts while retaining hard line and JSON-depth bounds. Strict
  adapters can opt into the bounded policy explicitly.
- Full materializations happen on every provider turn (`build_messages` in
  [src/ava/agent/message_builder.cpp](../../src/ava/agent/message_builder.cpp) calls
  `read_authority.load()`), twice per runtime compaction
  ([src/ava/app/runtime_compaction.cpp](../../src/ava/app/runtime_compaction.cpp)),
  and for catalog/session-tree refreshes
  (`ApplicationCatalogCoordinator::refresh_current_session_during_operation` in
  [src/ava/app/command_palette.cpp](../../src/ava/app/command_palette.cpp)).
- Session listing/resume scans every session file of the workspace
  (`SessionStore::list_sessions` / `list_sessions_bounded` in
  [src/ava/session/session_store_catalog.cpp](../../src/ava/session/session_store_catalog.cpp);
  `SessionListLimits`: 4096 sessions, 32 MiB aggregate). The RPC `list_sessions`
  serializer and the TUI session selector both go through these scans; the session
  tree view additionally builds `session_tree` nodes
  ([src/ava/session/session_tree.cpp](../../src/ava/session/session_tree.cpp)) with
  per-session metadata loads. There is no on-disk index; catalog, tree, resume, and
  context assembly all pay full scans.

**Compaction.** Compaction is **additive**: a `Compaction` entry is appended and later
reads project "active context" from the latest checkpoint forward
(`project_active_compaction_context`, `estimate_session_tokens`,
`should_auto_compact` in
[src/ava/session/compaction.h](../../src/ava/session/compaction.h) /
[compaction.cpp](../../src/ava/session/compaction.cpp)). The runtime path
(`compact_runtime_context` in
[src/ava/app/runtime_compaction.cpp](../../src/ava/app/runtime_compaction.cpp)) loads
the authoritative history, summarizes via the provider, and commits with an exact
compare-and-swap: `SessionAppendTarget::append_compaction_if_snapshot_matches`
revalidates a fresh bounded snapshot against the expected entries
(`compaction_snapshot_matches`), so a concurrent append yields a non-mutating
`SnapshotMismatch`. Only context-neutral automatic-title metadata is tolerated as a
suffix. One compaction ticket may be pending per run controller.

**Branch / fork / clone.** `session_branch`
([src/ava/session/session_branch.cpp](../../src/ava/session/session_branch.cpp))
materializes the source history, selects the fork point, and bulk-copies validated
entries into a newly created destination via `SessionStore::append_validated_copy`
(the only v4 bypass, used because the copy is prevalidated as a whole). Branch
metadata (`branch_origin` "fork"/"clone") and an additive `BranchSummary` entry
(`append_branch_summary_if_absent`, coordinated by `BranchSummaryCoordinator`)
complete the flow. `/fork`, `/fork-from`, and the RPC equivalents build on this; the
TUI caches the resulting tree in `ApplicationCatalogCoordinator`.

## Cancellation and shutdown path

Cancellation is a **cooperative polling** model; there is no centralized cancellation
registry.

- **Origins.** TUI: `RuntimeActiveRunController::request_stop` sets the
  `run_cancel_requested` atomic
  ([src/ava/tui/runtime_active_run_internal.cpp](../../src/ava/tui/runtime_active_run_internal.cpp));
  Escape inside permission/question prompts also routes to `request_stop`
  ([src/ava/tui/runtime_prompts_internal.cpp](../../src/ava/tui/runtime_prompts_internal.cpp)).
  RPC/ACP/print frontends supply their own `cancel_requested` callbacks.
  `SessionRunController::request_stop` records the stop against the active run and
  wins arbitration only before `Completing`.
- **Propagation.** `run_admitted_prompt` ORs the caller callback with the
  `ActiveRunGuard` stop token into `RunOptions::cancel_requested`; that single
  predicate is copied into `AgentLoopOptions`, `ToolContext`, transport calls, plugin
  and MCP client calls, LSP requests, and session bounded scans. The agent loop checks
  it at named boundaries (`check_canceled`), the curl transport polls it every 100 ms
  and kills the child, the bash tool terminates the verified process group, and
  plugin/MCP clients abort their request loops.
- **Signal handling** is minimal and local: OAuth connect installs temporary
  SIGINT/SIGTERM/SIGHUP handlers
  ([src/ava/app/connect_openai.cpp](../../src/ava/app/connect_openai.cpp)); ACP and RPC
  modes ignore selected signals
  ([src/ava/app/acp_mode.cpp](../../src/ava/app/acp_mode.cpp),
  [src/ava/app/rpc_mode.cpp](../../src/ava/app/rpc_mode.cpp)). There is no global
  "kill all children on exit" path.
- **Shutdown** is distributed RAII: `SessionRunController::shutdown()` rejects new
  work, finishes or fails accepted appends, and releases the append target;
  `JoinThread` destructors request-stop-and-join coordinator/registry workers;
  plugin, MCP, and LSP teardown reaps each tracked direct child and may signal its
  process group; `~SessionLease` closes the locked descriptor. Plugin and MCP normal
  shutdown stop tracking as soon as the leader exits and do not verify that its process
  group is empty, so a descendant can survive the direct child. More generally, leak
  safety depends on each owner running its destructor — a killed or hung owner has no
  supervisor to reclaim its process group (the detached browser grandchild is
  deliberately unreapable; see the spawn table).

## Process spawn sites

All sites are POSIX. "Owner" is the object whose lifetime bounds the direct child;
"cancel" is the in-flight abort mechanism; "cleanup" records the implemented teardown,
including any gap rather than implying descendant cleanup. Every row's exception/risk
is current behavior, not a proposal.

| # | Site (file · symbol) | Owner | Lifetime | Cancel | Cleanup | Exception / risk |
|---|---|---|---|---|---|---|
| 1 | [src/ava/http/curl_transport.cpp](../../src/ava/http/curl_transport.cpp) · `CurlCliTransport::send` / `send_streaming` | The calling provider/tool request (run stack, coordinator worker, or tool) | One HTTP request | `cancel_requested` polled every 100 ms → `kill_and_wait` (SIGKILL + `waitpid`) | `waitpid` on success; `kill_and_wait` on every error path | Kills the pid only, **no process group**; a `curl` that itself spawned helpers is not covered (current curl usage does not). Response cap `kMaxCurlResponseBytes` 8 MiB. |
| 2 | [src/ava/tools/bash_tool.cpp](../../src/ava/tools/bash_tool.cpp) · `run_bash` (leader + sentinel) | The `bash` tool invocation (one tool call) | One command | `is_canceled` checks pre-spawn; in-flight timeout/cancel → `terminate_verified_group` | SIGTERM to verified PGID (300 ms `kProcessGroupGrace`) → SIGKILL → `wait_for_group_exit` (500 ms); leader and sentinel reaped individually; `gate_failure_cleanup` before group verification | Strongest lifecycle in the tree: parent/child-acknowledged PGID, sentinel holds PGID identity against recycling, optional development containment handshake before `fexecve`. If AVA dies between fork and reap, the group outlives it (no supervisor). Delegated `command_executor` path spawns nothing locally — execution and containment reporting move to the frontend. |
| 3 | [src/ava/plugin/runner.cpp](../../src/ava/plugin/runner.cpp) · `PluginProcess::start`/`launch` | One brokered plugin tool call ([src/ava/plugin/tool_broker.cpp](../../src/ava/plugin/tool_broker.cpp) · `dispatch_plugin_tool`) | **One-shot**: start → initialize → `call_tool` → `shutdown` per call | `cancel_requested` aborts startup/request waits | Normal `shutdown` closes stdin and reaps the leader; if it remains live, `terminate_child` sends SIGTERM (group if verified), polls, then SIGKILL and reaps the leader; pipes close | Fresh fork+exec per tool call (no persistent worker); startup cost paid per call. If the leader exits after forking a same-group descendant, shutdown marks the leader reaped and never checks or kills the remaining group. Discovery ([src/ava/plugin/discovery.cpp](../../src/ava/plugin/discovery.cpp) · `discover_plugins`) only scans directories and parses `plugin.json` — no spawn. Protocol: `kPluginApiVersion` = `ava.plugin.v1` ([src/ava/plugin/manifest.h](../../src/ava/plugin/manifest.h)). |
| 4 | [src/ava/mcp/stdio_client.cpp](../../src/ava/mcp/stdio_client.cpp) · `McpStdioClient::start`/`launch` | One MCP tool/resource call **or** one tool-discovery pass ([src/ava/mcp/tool_broker.cpp](../../src/ava/mcp/tool_broker.cpp) · `visit_enabled_mcp_tools`, `dispatch_*`) | One-shot per call; one-shot per discovery | `cancel_requested` aborts waits | Normal `shutdown` closes stdin and reaps the leader; only a still-live leader reaches group SIGTERM→SIGKILL termination; pipes close | **Tool discovery spawns every configured server** at registry composition (per run) to `tools/list`; each tool call re-spawns the server. As with plugins, a leader that exits before a same-group descendant is not followed by group-empty verification, so the descendant can survive. Protocol pinned to `2024-11-05`. Server env may inherit the parent environment unless `clean_environment`. |
| 5 | [src/ava/lsp/lsp_process.cpp](../../src/ava/lsp/lsp_process.cpp) · `LspProcess` launch (via `SubprocessLspClient::start`) | `ConfiguredLspProvider::clients_` map ([src/ava/lsp/configured_provider.cpp](../../src/ava/lsp/configured_provider.cpp)) — created per `run_admitted_prompt`, so the cache lives **one prompt run** | First use of a server/root pair → end of run | `cancel_requested` passed to requests | Gate-verified PGID; client teardown kills the group; stale/dead clients evicted from `clients_` on next use | Cache key `server.id + root` is run-local; the next prompt re-forks every LSP server. Executable identity revalidation (`ExecutableIdentity` fstat checks) for built-in recipes. |
| 6 | [src/ava/app/mermaid_render_coordinator.cpp](../../src/ava/app/mermaid_render_coordinator.cpp) · render worker | `MermaidRenderCoordinator` (TUI-scoped; `"mermaid_render"` JoinThread) | One diagram render | Deadline `kMermaidRenderDeadline` or coordinator cancel → group kill | SIGSTOP handshake before exec; exec-failure pipe; child reaped by worker on all outcomes | Helper resolved to an `O_NOFOLLOW` descriptor and `fexecve`d; missing helper is a first-class outcome (`MissingHelper`). |
| 7 | [src/ava/app/browser_open.cpp](../../src/ava/app/browser_open.cpp) · `open_url_in_browser` / `spawn_detached` | **None (deliberately detached)** — used by OAuth connect flows ([src/ava/app/command_connect.cpp](../../src/ava/app/command_connect.cpp), [src/ava/app/connect_openai.cpp](../../src/ava/app/connect_openai.cpp)) | Until opener exits | None | Double-fork + `setsid` + stdio→`/dev/null`; intermediate child reaped; grandchild orphaned to init | Grandchild is unreapable and unkillable by AVA by design; `AVA_DISABLE_BROWSER_OPEN` disables the site. URL scheme allowlist (`http(s)`), `$BROWSER` without spaces, then `xdg-open`/`gio`/`open`/`wslview`. |
| 8 | [src/ava/app/clipboard_image.cpp](../../src/ava/app/clipboard_image.cpp) · `capture_command_stdout` | The clipboard-image read (composer paste path) | One helper invocation | None (bounded by `timeout` only) | Non-blocking read with deadline → SIGKILL + `waitpid`; byte cap | No cancellation callback; relies on the caller-supplied timeout. Helper taken from environment-configured commands. |
| 9 | [src/ava/app/external_editor.cpp](../../src/ava/app/external_editor.cpp) · `edit_text_with_external_editor` | The TUI external-editor action | One editor session | None (blocks the prompt thread) | `std::system("exec $EDITOR …")` reaped by the call; temp file RAII (`mkstemp`, 0600, unlink) | Goes through the shell by construction; editor string comes from `VISUAL`/`EDITOR` verbatim (user-controlled environment). Draft size cap `kExternalEditorMaxBytes`. |

Not process spawns but adjacent: `detail::ToolScheduler`
([src/ava/agent/tool_scheduler.cpp](../../src/ava/agent/tool_scheduler.cpp)) runs
parallel tool epochs on `JoinThread` workers; background subagent jobs run child
`AgentLoop::run_turn` invocations on `BackgroundJobRegistry` threads
([src/ava/agent/agent_turn_subagents.cpp](../../src/ava/agent/agent_turn_subagents.cpp)
— inline `task` subagents run on the executor thread, coordinated `job` subagents run
on registry threads), and each background run forks its own curl children via
`background_transport_factory`.

## In-memory retention inventory

Everything below is heap state retained beyond a single call. "Bounded" values cite
the enforcing constant.

**Provider / transport**

- `ParsedAssistantTurn` / `ProviderTurn` per iteration, including transient tool
  arguments and provider-native reasoning (`AVA_DEBUG_PRINT_MEMBERS_OPT_OUT`), in
  [src/ava/agent/agent_turn_executor_internal.h](../../src/ava/agent/agent_turn_executor_internal.h).
- `ProviderEventAccumulator` per provider turn (stream events up to
  `max_provider_events` = 4096; assistant text capped at 256 KiB).
- Full rebuilt provider message vector per turn: `build_messages`
  ([src/ava/agent/message_builder.cpp](../../src/ava/agent/message_builder.cpp))
  materializes the entire leased history and projects it; retained only for the call.
- `ProviderCatalog` is built once and pinned as `shared_ptr<const>` for process life
  ([src/ava/provider/catalog.h](../../src/ava/provider/catalog.h)); provider instances
  themselves are per-runtime (`with_provider_runtime`) or per-background-run factory
  products. `ProviderRegistry` factories are static registrations
  ([src/ava/provider/registry.cpp](../../src/ava/provider/registry.cpp)).

**Tools**

- Per-run `ToolDispatcher` owning a `ToolContext` copy and the composed `ToolRegistry`
  ([src/ava/agent/tool_dispatcher.cpp](../../src/ava/agent/tool_dispatcher.cpp));
  destroyed at run end.
- The built-in tool table is a process-lifetime function-local static
  (`builtin_tool_registry()`); immutable after first use.
- `ToolTimelineEntry` results accumulate in `AgentLoopResult::tool_timeline` for the
  run; large tool output spills to the session `spill/` directory
  ([src/ava/tools/spill_files.h](../../src/ava/tools/spill_files.h)) rather than memory.
- `BufferedToolCallbacks` (permission audits, progress) per parallel epoch in the
  executor.

**Plugin**

- Discovered manifests: `std::vector<DiscoveredPlugin>` rebuilt at each registry
  composition (per run); no persistent plugin worker or manifest cache.
- Per-call `PluginProcess` state: pipes, bounded stderr tail, initialization record —
  freed at call end.

**MCP**

- `McpConfig` held as `shared_ptr<const>` per session; brokered tool descriptors
  (`McpToolBinding`/`McpResourceBinding`) rebuilt per registry composition.
- Per-call/per-discovery `McpStdioClient` (initialization record, capabilities JSON)
  freed at call end.

**LSP**

- `ConfiguredLspProvider::clients_` (`unordered_map` of live `SubprocessLspClient`)
  plus cached diagnostics/symbol responses inside clients — retained for exactly one
  prompt run, then destroyed with the provider.

**Subagents / background jobs**

- `SubagentCoordinator::jobs_` map of `JobState` shared_ptrs
  ([src/ava/agent/subagent_coordinator.h](../../src/ava/agent/subagent_coordinator.h)).
- `BackgroundJobRegistry` records, bounded: `max_running_jobs` 8,
  `max_retained_finished_jobs` 64, description ≤ 8 KiB, final text ≤ 64 KiB
  (`BackgroundJobRegistryOptions`).
- `SubagentDeliveryManager` parent capsules and pending deliveries (generation-bound).

**Session**

- Persistent `SessionStore`: no entry cache; retention is whatever each full `load()`
  materializes (see the read path list). Ephemeral `SessionStore::EphemeralState`
  retains the **complete transcript** in memory by design.
- `SessionRunController`: bounded command/append queues and ≤ 64 retained outcomes.
- Compaction CAS carries the full expected snapshot vector for the comparison.
- `ApplicationCatalogCoordinator::cache_` retains the session tree and per-session
  summaries/metadata for the TUI palettes.

**TUI**

- `TuiEventState` ([src/ava/tui/event_state.h](../../src/ava/tui/event_state.h)):
  `transcript`, `pending_tools`, `permission_audits`, `queued_messages`, `activity`,
  `modified_files`, `todos` — the completed transcript grows for the lifetime of the
  visible session.
- `TuiRuntimeStateSnapshot` and conversation buffers
  ([src/ava/tui/runtime.h](../../src/ava/tui/runtime.h)): `output`, `tool_timeline`,
  `conversation_output`, `conversation_tool_timeline`, request-id lists.
- `ApplicationCatalogCoordinator` cache (above) and mermaid render completions.

**Observability (opt-in)**

- `RunObservation` keeps a bounded record queue with drop counters and an owned writer
  thread spooling to the trace directory
  ([src/ava/observability/run_observer.h](../../src/ava/observability/run_observer.h));
  drops are accounted, never silently unbounded.

## Current invariants and compatibility boundaries

These are the constraints the current code enforces and external behavior depends on;
they define what later milestones must preserve or explicitly migrate.

1. **Session format v4** (`kCurrentSessionEntryVersion`): append-only JSONL, one
   `SessionEntry` per line; staged assistant output is visible only after
   `AssistantTurnCommit`; compaction and branch summaries are additive entries.
   Public contract: [docs/session-format.md](../session-format.md),
   [docs/development/session-versioning.md](../development/session-versioning.md).
2. **Single writer authority**: persistent sessions require the exclusive
   `SessionLease`; all runtime writes go through `SessionAppendTarget` routes issued
   by the admitting `SessionRunController`; legacy raw `SessionStore::append` rejects
   v4 assistant-output records. Recovery after a latched persistence failure is
   explicit (`recover()`, `reset_persistence_failure()`), never implicit.
3. **Run admission**: one active run per session; same-request joins, different-request
   rejects; no backend queue. Frontend-visible behavior (TUI follow-up queue, RPC)
   depends on this contract
   ([docs/development/internals/session-run-controller.md](../development/internals/session-run-controller.md)).
4. **Built-in tool names and schemas are a static table** (`builtin_tool_metadata()`
   via [src/ava/agent/tool_registration.cpp](../../src/ava/agent/tool_registration.cpp));
   providers, prompts, permission rules, and TUI rendering depend on these names.
   `exact_builtin_tool_names` composition requires an immutable session MCP config.
5. **Plugin protocol** `ava.plugin.v1` over stdio JSONL, one process per call;
   discovery is manifest-only. Compatibility policy:
   [docs/plugin-compatibility-policy.md](../plugin-compatibility-policy.md).
6. **MCP**: stdio JSON-RPC pinned to protocol version `2024-11-05`; one-shot clients
   for both discovery and calls; model tool names derived with collision rules
   (strict mode errors on duplicates).
7. **Wire protocols**: [docs/rpc-protocol.md](../rpc-protocol.md),
   [docs/headless-protocol.md](../headless-protocol.md), [docs/acp.md](../acp.md) are
   frozen external interfaces layered over the same `run_prompt` path.
8. **Sealed command execution**: bash commands execute only from a fresh prepared
   plan, through approved descriptors (`fexecve`), in a parent/child-verified process
   group, with optional development containment; the delegated `command_executor`
   contract moves that boundary to the frontend without local spawn.
9. **Transport boundary**: provider I/O is abstracted behind `ava::http::Transport`
   ([src/ava/http/transport.h](../../src/ava/http/transport.h)); production injects
   `CurlCliTransport` (forking curl), tests inject fakes. Streaming is optional per
   transport (`supports_streaming`).
10. **Trusted exec PATH**: curl, plugin, MCP (unless overridden), and LSP children
    exec with a reset/trusted `PATH` (`kTrustedExecPath`); the external editor and
    browser opener intentionally use the user environment.

## Measured and unknown caveats

- **No measurements were taken for M0.1.** Startup cost, RSS, full-scan latency, and
  per-call plugin/MCP spawn overhead are asserted qualitatively from code structure
  only; M0.2/M0.3 own the reproducible harness and honest baselines. Nothing in this
  document should be quoted as a performance measurement.
- **Unknown: actual frequency** of MCP server re-spawns in production sessions
  (depends on tool-call mix); the code guarantees at least one spawn per registry
  composition per configured server, plus one per call.
- **Known plugin/MCP descendant gap:** normal shutdown proves only that the direct
  leader was reaped. If that leader forks a same-process-group descendant and exits on
  stdin EOF, plugin and MCP shutdown do not verify or terminate the remaining group;
  the descendant can survive even while immediate-child checks report success.
- **Unknown: orphan behavior under SIGKILL of AVA itself.** Every child-cleanup path
  is destructor- or poll-loop-driven; no site registers children in a supervisor or
  uses subreaper semantics, so a hard-killed AVA can leave bash process groups,
  plugin/MCP/LSP servers, and curl children behind. The exact residual set per
  workload is unmeasured.
- **Unknown: Windows viability of the process layer** — all spawn sites are POSIX;
  there is no Windows process implementation in the mapped tree.
- **Excluded concurrent work**: the initial checkout contained unrelated changes under
  `docs/core/`, `src/ava/agent/`, and `src/ava/app/`; they were preserved separately
  and are not part of this branch. This map reflects the committed tree at
  `c94ac8631419`; it must be re-verified if that separate work is later integrated.
- **Not exhaustively traced:** ACP peer threads
  ([src/ava/app/acp/peer.cpp](../../src/ava/app/acp/peer.cpp) owns raw `std::thread`
  writer/deadline threads) and plugin UI protocol forwarding were identified but not
  deep-mapped; they are frontend-side and do not spawn processes.
- Line numbers are intentionally absent; all references are file + symbol and were
  verified against the mapped tree on 2026-08-30.
