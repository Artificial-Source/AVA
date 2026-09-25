# Subagents and background jobs

AVA can delegate a bounded unit of work to a `task` subagent and run it in a separate child session. This page is the canonical user and operator guide for launching that work, controlling live jobs, and understanding what survives process exit.

## Available subagents

AVA always provides two built-ins:

- `general` inherits the parent's visible tools, except that `task`, `job`, and `todowrite` are hidden.
- `explore` is a read-only preset: it exposes `read_file`, `list_directory`, `glob`, and `grep`, and hides mutation, shell, network, LSP, `task`, `job`, and `todowrite`.

Trusted custom subagents are Markdown definitions with frontmatter plus an instruction body. Global definitions come from AVA, Agents, and Claude configuration roots; project definitions are loaded only for a trusted project. See [CONFIG.md](configuration.md#subagents) for exact paths and fields, and [context-resources.md](context-resources.md#subagents) for discovery, prompt visibility, trust, and reload behavior. Those references, rather than this operational page, define the file grammar.

A custom definition is trusted instruction content. Review it before installing it globally or trusting a project's definitions. Custom task definitions cannot replace built-in `general` or `explore`, and every child has `task`, `job`, and `todowrite` removed from its visible tools.

## Selectable primary agents

The same roots may contain `mode: primary` definitions selected at process startup with `ava --agent <name>`; `mode: all` definitions are both primary-selectable and task-visible. `mode: subagent` definitions are not primary-selectable, and primary-only definitions are not included in the task catalog. Built-in `general` and `explore` remain task-only, although a configured primary may independently use either name.

A selected definition's body is appended as explicit system instructions, preserving AVA's base, safety, context, and ambient-extension-free prompt composition. `tools: read-only` intersects the current CLI visibility with AVA's read/search built-ins and does not grant permission. An inherit primary keeps `task`, `job`, and `todowrite` unless CLI visibility removed them.

Selection is invocation-local and available to the TUI, print mode, and CLI RPC; ACP rejects extra startup flags. The resolved definition remains fixed across model switches, prompt/trust reloads, and ordinary turns and is not persisted as a conversation turn. A navigated replacement may resolve the carried name again. Start or replace a session after config/trust changes when selected identity or policy must change; AVA has no live primary-agent picker.

## Foreground and background execution

The model-visible `task` tool takes a short `description`, a complete `prompt`, a listed `subagent_type`, and optional `mode` and `max_tool_iterations` fields:

- **Foreground is the default.** The parent turn blocks until the child finishes, fails, is canceled, or is promoted. The completed result returns directly to the parent model.
- **Background is explicit.** `mode: "background"` returns a `job_id` and `task_id` promptly while the child continues in this AVA process. The legacy `background` boolean is accepted only when it agrees with `mode`.

Each launch creates a durable child-session JSONL file with parent/subagent metadata. The owning parent model can pass the returned child session `task_id` to continue that child in either foreground or background mode. Resume first acquires the child lease and verifies the recorded parent before recovery or writes; each invocation receives a new `job_id` and a fresh tool-round budget. A child cannot recursively dispatch another `task`.

`max_tool_iterations` is an integer from 1 through 1000 and counts provider rounds containing tool calls, not individual calls in one round. A task value overrides the selected agent definition; otherwise the definition's value is used; otherwise the child inherits the parent's configured limit. This is not clamped to six or to the parent's remaining rounds. At the limit, the child executes no more tools and gets one bounded, tool-free request to summarize findings and unfinished work. A noncompliant tool call in that wrap-up is settled in child history without execution, and the child outcome remains `max_turn_requests`.

## Permissions and interaction

Starting `task` is automatically allowed and audited by the default policy unless an exact persisted deny matches the `task` tool and requested `subagent_type`. Launch approval is not blanket authority for the child: each sensitive file, command, network, plugin, MCP, or other operation still passes through the child's inherited visibility and normal permission policy.

A foreground child can route an `Ask` permission decision or the model's `question` tool to the parent interactive UI. A background child has no interactive resolver, so operations or questions that require `Ask` fail closed. Promotion also removes foreground interaction access; a foreground job cannot be promoted while an interaction is outstanding.

See [CONFIG.md](configuration.md#permission-rules) and [security-sandboxing.md](../security/sandboxing.md) for persistent rules, headless behavior, and the external-sandbox boundary.

## Observe and control jobs

Job IDs are exact, opaque, process-local control identifiers bound to the parent session that launched them. A job from another parent is reported as not found; prefixes are not control authority.

Three control surfaces are available:

- The model-visible `job` tool supports `list`, `status`, `wait`, `result`, `cancel`, and `steer`. `steer` accepts an exact owned running `job_id` plus a bounded message, which is delivered FIFO once at the child's next safe provider boundary. It rejects wrong-owner, terminal, canceled, unsteerable, and full-queue jobs. Steering text is persisted to child history only when the child consumes it; the parent `job` tool-call arguments remain in parent history. Steering text is omitted from public snapshots and diagnostics. It does **not** support promotion.
- Interactive `/jobs` lists jobs. `/jobs list` merges live jobs with display-only recorded history (live wins). `/jobs history` shows recorded history only. `/jobs show`, `wait`, `result`, `cancel`, and `promote` accept an exact ID; show/result can print a bounded recorded summary after reopen, while wait/cancel/promote reject historical-only ids. In the TUI, bare `/jobs` opens a searchable **live** selector; Enter opens the child workspace, C cancels, and P promotes when eligible. Recorded history is not a TUI selector or workspace surface.
- RPC provides `list_jobs`, `get_job`, `wait_job`, `get_job_result`, `cancel_job`, and `promote_job`; see [rpc-protocol.md](../rpc-protocol.md#subagent-job-snapshot).

The TUI child workspace is inspection-only. It projects bounded committed child User/Assistant messages and live/final availability, but has no composer or tool controls and does not expose reasoning, paths, session IDs, or full job IDs in the rendered workspace.

While a task belongs to the current AVA process, its task card shows a separate `launch:` row, the `/jobs` selector shows a compact launch suffix, and the child workspace shows a `Launch:` line. This is AVA's configured launch request (the proven display model name when available plus the selected AVA thinking level), not provider-confirmed serving identity. Launch display metadata is presentation-only and process-local: it is excluded from task/job results, public `/jobs` text and JSON, RPC, events, sessions, exports, search, filtering, details queries, and clipboard text. Replayed or historical task cards therefore omit the row after restart.

`status`/`show` can inspect running or terminal state. `wait` waits only for its finite timeout and may return a still-running snapshot; it does not cancel the job. `result` is available only after terminal completion. `cancel` is cooperative: it records a cancellation request and signals the worker, so the snapshot can briefly remain running before reaching `canceled`. Completed, failed, canceled, and interrupted work is terminal; repeat controls return the current terminal state or an invalid-transition/not-found error as appropriate.

## Promote foreground work

Eligible running foreground work can be changed to background work with TUI `/jobs ... promote` (including P in the child workspace) or RPC `promote_job`. Promotion does not restart the worker, create a new child, or change its `task_id`/`job_id`; the blocked foreground `task` call returns a promoted/running result and the same child continues in the background.

Only a running foreground job that has not been canceled can be promoted. Promotion fails while a foreground permission/question interaction is outstanding. Already-background work does not need promotion. The model-visible `job` tool intentionally has no promote action.

## Completion delivery

When background or promoted work reaches a terminal state, AVA schedules a short completion summary for the owning parent. Delivery waits until the parent run controller is idle, then runs as a synthetic parent turn; it never interrupts or appends inside an active ordinary turn. A committed delivery turn is acknowledged, and AVA checks durable parent history before retrying so a completion is not intentionally delivered twice.

Delivery is bounded and best-effort. Transient admission, provider, transport, or acknowledgement failures can be retried; the production defaults are three attempts with a 30-second deadline per attempt. Duplicate notifications are coalesced by delivery identity, and a full advisory queue is rediscovered from still-pending coordinator state while the process remains alive. Exhausted delivery remains available through job controls until ordinary retention removes it, but no further automatic summary is promised.

This mechanism is **process-local**. Closing AVA, a crash, or a restart loses running workers, live job snapshots/results, and pending automatic delivery. AVA does not reconstruct workers, coordinator state, or automatic delivery by scanning sessions.

## Durability and limits

The child-session JSONL is durable session history and remains available through normal session tooling after the process exits. Child automatic compaction uses the launch-time threshold/retention snapshot and the runtime model's exact context window, checks every safe pre-provider boundary, and appends only to that child session. Explicit `auto_threshold_tokens: 0` still disables it. Summary requests use the child's active provider/model (not a parent-global compaction model override), preserve retained tool groups and UTF-8 bounds, and do not reset tool-round counters. The live coordinator record is different: job state, retained final result, cancel state, steering queue, and pending delivery exist only in memory. Parent session JSONL also stores bounded display-only `session_metadata.subagent_job` start and terminal events (schema version 1) with ids, mode, execution, timestamps, and a bounded summary/error. Writers omit credentials, raw launch prompts, raw tool args, path metadata, and launch authority; summary/error text is not a redaction engine and may contain ordinary user content. Those records are excluded from model context, project at most the latest 64 jobs for display, and do not restore live controls, delivery, or launch authority. Forks copy the bytes but do not inherit control: history is parsed only for the same parent session identity. Append-only disk size grows with ordinary session history; there is no fixed disk bound. A durable child session or parent history record therefore does not imply that RPC job controls or automatic delivery can recover the former job after restart. Unmatched start records reopen as interrupted with outcome unknown.

Current production defaults and public caps are:

| Boundary | Current value |
| --- | --- |
| Concurrent running jobs | 8 |
| Retained finished execution records | 64 |
| Retained job description | 8 KiB, truncated when necessary |
| Retained final task text | 64 KiB, truncated when necessary |
| Public `list` result | at most 64 latest entries |
| Display-only recorded jobs | latest 64 unique parent-owned jobs; per-record summary 16 KiB |
| Public `wait` | 1-second default; 30-second maximum |
| Child tool rounds | inherited default 10; definition/task override 1–1000 |
| Model steering queue | 16 messages, 16 KiB each, 64 KiB total |
| Automatic delivery advisory queue | 64 entries by default |
| Retained parent delivery capsules | 64 by default |
| Automatic delivery | 3 attempts by default; 30-second deadline per attempt |

The `task` dispatcher's input bounds can be narrower than retention bounds (for example, its short description is capped before launch). The delivery queue, parent retention, attempt count, and deadline are operational defaults from application options, not persisted format guarantees. Public job snapshots are bounded and redact child paths and internal context.

## Failure and shutdown expectations

Launch can fail before publication because the subagent type, input, permission, child session, credentials/provider setup, or concurrency limit is invalid or unavailable. Once a job is published, worker exceptions and child-run failures become a terminal failed snapshot with a sanitized error. Cancellation and application shutdown request cooperative stop; they cannot guarantee that an external descendant which escapes AVA's verified process-group boundary is contained. See [security-sandboxing.md](../security/sandboxing.md) for that limitation.

If the child session remains, inspect it with normal session commands even when its former process-local job record is gone. Use `result` for a retained terminal result and inspect diagnostics/session history when a launch or delivery error needs investigation.

## Authoritative source and focused tests

The operational contracts are implemented in:

- [`src/ava/agent/agent_turn_subagents.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/agent_turn_subagents.cpp) and [`tool_dispatch_task.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_task.cpp): child creation, foreground/background behavior, continuation, and launch permission;
- [`background_job_registry.h`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/background_job_registry.h), [`subagent_coordinator.h`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/subagent_coordinator.h), and [`job_control.h`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/job_control.h): lifecycle, ownership, promotion, public controls, and limits;
- [`subagent_delivery_manager.h`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/app/subagent_delivery_manager.h): automatic-delivery defaults and process-local boundary;
- [`command_jobs.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/app/command_jobs.cpp) and [`rpc/session_commands.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/app/rpc/session_commands.cpp): interactive and RPC surfaces;
- [`subagent_job_history.h`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/session/subagent_job_history.h): bounded parent-session display history.

Run the focused deterministic suites without a live provider:

```sh
scripts/run-tests.sh --build-dir build --jobs 4 \
  -R '^ava_tests\.(agent_loop|agent_tool_dispatcher|subagent_coordinator|subagent_delivery_manager)$'
```

Related coverage also lives in [`tests/agent_tool_dispatcher_tests.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/tests/agent_tool_dispatcher_tests.cpp), [`tests/app_command_registry_tests.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/tests/app_command_registry_tests.cpp), and [`tests/app_rpc_commands_tests.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/tests/app_rpc_commands_tests.cpp).
