# Built-in model tools

This is the current user and maintainer reference for AVA's built-in tools exposed to models. It describes the native registry in [`src/ava/agent/tool_metadata.h`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_metadata.h) without duplicating provider JSON schemas. Plugin and MCP tools are external brokered tools and are documented in [plugin-system.md](../extensions/plugin-system.md) and [mcp.md](../extensions/mcp.md).

## Model tools are not slash commands

A **model tool** is a function schema advertised to the active model. The model may call it during a turn. A **slash command** is entered by a person or automation client, such as `/read`, `/bash`, `/jobs`, or `/mcp`; slash commands have their own parsing and behavior in [USAGE.md](usage.md#commands). Similar names do not make the two interfaces interchangeable.

AVA advertises one native name per built-in operation. `--tools` and `--exclude-tools` also accept the Pi-style visibility aliases `read` → `read_file`, `write` → `write_file`, `edit` → `edit_file`, `find` → `glob`, and `ls` → `list_directory`. These aliases select visibility only; they do not add duplicate provider schemas or rename model calls.

## Visibility is not authority

`--tools`, `--exclude-tools`, `--no-builtin-tools`, and `--no-tools` determine which schemas a model can see. Visibility never grants filesystem, process, network, LSP, plugin, or MCP permission. An advertised call still passes through tool validation, workspace/project trust, hard policy, persistent rules, session grants where supported, and the active interactive/headless resolver. Conversely, permission policy does not make a hidden tool visible. See [CONFIG.md](configuration.md#permission-rules) and [security-sandboxing.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/security/sandboxing.md).

Some tools have additional gates: LSP tools appear only with valid LSP capability, project resources require trust, `task` delegates nested operations to child policy, and external plugin/MCP tools have their own brokered permission paths. Metadata permission categories are routing/classification hints, not standalone grants.

## Files, search, and commands

| Native tool | Purpose and key inputs | Bounds and side effects | Metadata category |
| --- | --- | --- | --- |
| `read_file` | Read text at `path`; continue by 1-based `offset` and `limit`. | Defaults to 200 lines and 50 KiB; `limit` ≤ 100,000 and `max_bytes` ≤ 512 KiB. Read-only; outside-workspace paths require applicable authority. | `read` |
| `list_directory` | List one `path` (workspace root by default). | Defaults to 500 entries; `max_entries` ≤ 5,000. Read-only; includes symlink names, while other read-denied entries are omitted. | `search` |
| `glob` | Find readable non-symlink workspace files matching `pattern`; bracket classes are unsupported. | Defaults to 2,000 results; `max_results` ≤ 10,000. Read-only. | `search` |
| `grep` | Search readable non-symlink files for `pattern`, optionally limited by `include`; literal and case-sensitive by default. | Defaults to 2,000 matched lines; `max_matches` ≤ 10,000, with line/output caps. ECMAScript regex is used when `literal=false`. Read-only. | `search` |
| `write_file` | Write complete `content` to `path`; intended for new files or deliberate full rewrites. | Mutates one file through AVA checks; source writes are denied in plan mode. Returns status/byte count rather than file content. | `edit` |
| `edit_file` | Replace one exact, unique nonempty `old_text` span with `new_text` in `path`. | Mutates one file only after uniqueness and permission checks; use `apply_patch` for coordinated edits. | `edit` |
| `apply_patch` | Apply an `edits` array of exact path/old/new replacements. | 1–32 replacements are validated before writes commit. Mutates one or more files through normal edit checks; refuses final symlink targets. | `edit` |
| `bash` | Run a permissioned local `command` for builds/tests. Losslessly representable commands execute as direct argv; shell syntax such as pipes, redirects, expansions, and subshells selects raw-shell intent and requires one-shot Critical approval. | Default timeout 30 s, maximum 120 s; default 200-line tail; `max_lines`/alias `limit` ≤ 100,000 and `max_bytes` ≤ 512 KiB. Starts a contained local process and may cause command-defined side effects after approval. | `bash` |

File and process implementations live under [`src/ava/agent/tool_dispatch_file.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_file.cpp), [`tool_dispatch_search.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_search.cpp), [`tool_dispatch_patch.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_patch.cpp), and [`tool_dispatch_bash.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_bash.cpp). Command containment guarantees and limitations are normative in [security/containment.md](https://github.com/Artificial-Source/AVA/blob/develop/docs/security/containment.md).

### Workspace paths and search results

Across TUI, print, RPC, and ACP, model file tools resolve paths spelled beneath either the logical workspace root or its physical/canonical spelling through the same descriptor-anchored workspace. Symlinks below that root cannot redirect access outside it: lookup fails with a `use the real path` error rather than prompting for the link's target. Absolute symlinks are refused even when they point back inside the workspace. On Linux with `openat2`, contained relative symlinks can be followed; the fallback on systems without it rejects symlink components. `write_file` and `apply_patch` also refuse final symlink targets. FIFOs are rejected as non-regular files without blocking for a writer.

Paths spelled outside the workspace retain normal outside-workspace permission handling in TUI, print, and RPC, including a prompt when required. `read_file` reads through a held descriptor; writes, exact edits, patches, and directory listings retain the existing pathname-based I/O for those outside paths. ACP instead refuses outside-workspace paths entirely; shared path resolution does not expand its launch-approved permission scope.

Workspace `list_directory` results retain symlink names. A contained link can report its descriptor-inspected target's type and size; an escaping, absolute, or dangling link is listed without following its target, with `directory:false` and `size:0`. `glob` and `grep` continue to skip symlink entries.

`glob` and `grep` add a numeric `skipped_paths` result field when entries could not be inspected or read because they were unreadable or vanished; `grep` also skips files larger than 10 MiB instead of failing the search. The field counts these omissions and is omitted when zero. Skipped paths alone do not set `truncated` or trigger a spill file; result, traversal, and output limits still control truncation independently.

## Network, resources, and delegation

| Native tool | Purpose and key inputs | Bounds and side effects | Metadata category |
| --- | --- | --- | --- |
| `webfetch` | Fetch a known HTTP(S) `url`; choose `markdown`, `text`, or `html`, and continue with 1-based `offset`/`limit`. | Network access after approval. Defaults to 200 lines, 1 MiB, and 30 s; `limit` ≤ 100,000, `max_bytes` ≤ 5 MiB, timeout 1–120 s. | `network.fetch` |
| `websearch` | Discover current sources for `query`; `context_max_chars` also accepts `contextMaxCharacters`. | Network access after approval. Defaults to 8 results/10,000 characters/25 s; at most 10 results, 30,000 characters, and 60 s. | `network.search` |
| `skill` | Load a listed local/global skill by exact `name` from `available_skills`. | Adds bounded trusted/approved instruction content and a sampled file list to conversation context; it does not execute the skill's files by loading them. | `skill` |
| `task` | Start or continue a child agent using `description`, complete `prompt`, and an available `subagent_type`; `mode` is `foreground` or `background`, and `task_id` continues foreground work. | Creates child-session work and may start a background job. Description/prompt/runtime output and concurrency are bounded; nested side effects retain their own policy. Legacy `background` is accepted only when consistent with `mode`. | `task` |
| `job` | `list`, inspect `status`, `wait`, retrieve `result`, or `cancel` a parent-owned `job_id`. | `wait` defaults to 1 s and is capped at 30 s; list/result snapshots are redacted and bounded. `cancel` changes child-job state. | `job` |

See [`src/ava/agent/tool_dispatch_web.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_web.cpp), [`tool_dispatch_skill.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_skill.cpp), [`tool_dispatch_task.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_task.cpp), and [`tool_dispatch_job.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_job.cpp). The canonical operational guide for `task`, `job`, promotion, delivery, durability, and limits is [subagents.md](subagents.md). Command syntax is listed in [USAGE.md](usage.md#commands).

## Language-server tools

All five tools require configured LSP capability and use the `lsp.query` metadata category. A first query may separately require approval to launch the selected local server. Paths are at most 4,096 bytes; line and column positions are zero-based. Responses are bounded structured data and hide local server configuration.

| Native tool | Required input | Result / side effect |
| --- | --- | --- |
| `lsp_diagnostics` | `path` | Diagnostics for one workspace file; may launch/query a configured server. |
| `lsp_document_symbols` | `path` | Document symbols, kinds, ranges, and containers. |
| `lsp_workspace_symbols` | `query` (≤ 1,024 bytes) | Symbols across configured servers. |
| `lsp_definition` | `path`, nonnegative `line`, `column` | Definition locations. |
| `lsp_references` | `path`, nonnegative `line`, `column` | Reference locations. |

The complete configuration, process environment, 200-entry/64-KiB result limits, deadlines, and cleanup behavior are in [lsp.md](../extensions/lsp.md). Dispatch lives in [`src/ava/agent/tool_dispatch_lsp.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_lsp.cpp).

## User interaction and conversation state

| Native tool | Purpose and key inputs | Bounds and side effects | Metadata category |
| --- | --- | --- | --- |
| `question` | Ask `question`, optionally with `header`, `options`, multi-select (`multiple`/`allow_multiple`), and custom-answer (`custom`/`allow_custom`) controls. | Pauses for the backend question resolver and returns a bounded answer; it does not grant another tool permission. | `user` |
| `todowrite` | Replace the conversation todo list with the complete `todos` snapshot; an empty list clears it. Each item has bounded `id`, `content`, and `status`. | At most 50 items; mutates conversation coordination state, not workspace files. | `user` |

Implementations are in [`src/ava/agent/tool_dispatch_question.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_question.cpp) and [`tool_dispatch_todo.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_dispatch_todo.cpp).

## Authoritative source and tests

The built-in names, descriptions, input constraints, metadata categories, output summaries, and execution modes are defined in [`src/ava/agent/tool_metadata.h`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_metadata.h). Registration and capability gating are in [`tool_registration.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_registration.cpp); visibility and aliases are in [`tool_registry.cpp`](https://github.com/Artificial-Source/AVA/blob/develop/src/ava/agent/tool_registry.cpp). Dispatchers and lower-level tool implementations remain authoritative for runtime enforcement.

Focused coverage is owned by the corresponding suites under [`tests/`](https://github.com/Artificial-Source/AVA/blob/develop/tests/), especially `agent_loop_tests.cpp`, `agent_loop_tool_execution_tests.cpp`, `agent_tool_dispatcher_tests.cpp`, `tool_scheduler_tests.cpp`, `tools_file_tests.cpp`, `tools_search_tests.cpp`, `tools_process_network_tests.cpp`, `agent_todo_tests.cpp`, `lsp_tests.cpp`, and the deterministic `ava_cli.headless_e2e_model_smoke` registered in [`tests/CMakeLists.txt`](https://github.com/Artificial-Source/AVA/blob/develop/tests/CMakeLists.txt). If metadata, enforcement, or tests disagree with this descriptive page, treat that as a documentation or implementation defect rather than inferring new authority from this summary.
