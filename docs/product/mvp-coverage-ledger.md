# AVA MVP Coverage Ledger

This ledger maps the checked Pi-first MVP checklist rows in `docs/product/mvp-baseline.md` to verification evidence. Keep it at suite, smoke, or focused-doc granularity so release reruns stay mechanical and the checklist does not become unreadable.

## Status Taxonomy

Use these terms consistently in product docs:

| Term | Meaning |
| --- | --- |
| Present | Implemented enough for the current AVA MVP target and backed by automated tests, opt-in smokes, or explicit manual evidence. |
| Partial | Useful behavior exists, but the row remains unchecked because important Pi-baseline depth, validation, UX, or release evidence is missing. |
| Deferred | Valid future work, intentionally outside the current MVP slice until a later design or approval gate. |
| Excluded | Not an AVA MVP goal unless the product direction changes. |

Checklist checkboxes mean `Present`. Unchecked rows must say whether they are `Partial`, `Deferred`, or `Excluded` in the row text, the MVP gap ledger, or this file.

## Evidence Types

| Type | Evidence |
| --- | --- |
| Unit/CTest | `ava_tests.<suite>` from `tests/CMakeLists.txt`. |
| CLI/RPC smoke | `ava_cli.<name>` CTest scripts using the built `ava` binary and fake providers/servers. |
| Opt-in PTY smoke | Skipped by default unless the matching environment gate is enabled, for example `AVA_TUI_TMUX_SMOKE=1` or `AVA_TUI_KITTY_IMAGE_SMOKE=1`. |
| Live provider smoke | Skipped by default unless `AVA_LIVE_PROVIDER_SMOKE=1` and provider credentials are present. |
| Docs/manual | Protocol, usage, or release evidence that documents shape or manual verification for behavior that cannot be fully automated yet. |

Latest post-M6/M7 and Carlo merge validation (2026-07-08 local): `cmake --build --preset dev --target ava_tests`, `ctest --preset dev --output-on-failure`, and `git --no-pager diff --check` passed locally. The full default CTest run passed 62/62 with expected skips for `ava_tests.provider_live_smoke`, `ava_tui.tmux_smoke`, `ava_tui.kitty_image_smoke`, and `ava_tui.osc8_smoke`. This default path covers the libcwd-OFF `ava_debug` fallback from Carlo Wood's debug print-members integration; libcwd/ctags-ON codegen validation remains blocked locally because `ctags` and `ccache` are missing.

Historical backend aggregate closure verification (2026-07-04): `cmake --preset dev`, `cmake --build --preset dev`, all three backend-area targeted CTest commands from `docs/goals/pi-mvp-parity/backend-pending-goals-goal.md`, focused safety regression coverage for `ava_tests.(core_json_permission|tools|lsp|mcp)` plus `ava_cli.headless_rpc_tool_failure`, `ctest --preset dev --output-on-failure`, and `git --no-pager diff --check` passed locally. The full default CTest run passed 58/58 with expected skips for the credential-gated provider live smoke and opt-in TUI PTY smokes; earlier opt-in `AVA_TUI_TMUX_SMOKE=1`, `AVA_TUI_KITTY_IMAGE_SMOKE=1`, and `AVA_TUI_OSC8_SMOKE=1` runs remain recorded as local supplemental frontend evidence but are not required for this backend-only closure.

Historical full-goal closure verification (2026-07-04 local): `cmake --preset dev`, `cmake --build --preset dev`, `ctest --preset dev --output-on-failure`, the full-goal focused CTest regex from `docs/goals/pi-mvp-parity/full-parity-all-goals-goal.md`, opt-in `AVA_TUI_TMUX_SMOKE=1`, `AVA_TUI_KITTY_IMAGE_SMOKE=1`, and `AVA_TUI_OSC8_SMOKE=1` smokes, the credential-gated `AVA_LIVE_PROVIDER_SMOKE=1` provider smoke, and `git --no-pager diff --check` passed or skipped with documented prerequisites. The default CTest run passed 58/58 with expected skips; the live-provider opt-in skipped because no supported provider credential variables were present; the final tmux smoke regenerated inspected visual captures under `build/tui-tmux-smoke/evidence/`.

Historical end-to-end tool-smoke verification (2026-07-05 local): `cmake --build --preset dev --target ava ava_fake_provider_server ava_tests`, `ctest --test-dir build -R '^ava_cli\.headless_e2e_model_smoke$' --output-on-failure`, `ctest --preset dev --output-on-failure`, and `git --no-pager diff --check` passed. The full default CTest run passed 59/59 with expected skips for credential-gated provider live smoke and opt-in TUI PTY smokes. The smoke starts the full `ava --rpc` binary against `ava_fake_provider_server`, drives read/search/list/apply_patch/bash in one provider-backed run, answers edit and process permissions through RPC, validates session stats/messages and replay through `--continue`, and asserts provider request logs contain prior tool results before the final assistant answer. `AVA_LIVE_PROVIDER_SMOKE=1 sh scripts/live-model-dogfood.sh` classified as `skipped/no credential` in the local environment.

Internal parallel read/search opt-in evidence (2026-07-09 M1/M2 local): `ava_tests.tool_scheduler` covers the bounded parallel scheduler engine and real builtin `read_file`/`list_directory`/`glob`/`grep` dispatch; `ava_tests.agent_loop` covers default-off AgentLoop integration, provider-order audit/progress/session/replay, Ask fallback, worker-cap clamping, and cancellation; focused sanitizer suites covered scheduler, AgentLoop, session, and tools. This is backend-only evidence for `AgentLoopOptions::parallel_read_search_tools`, not a user-facing/default parallel-tools claim. No full-binary smoke exercises it until a future public/headless CLI/RPC/TUI opt-in exists. Broad/full validation is tracked for M4.

## Product Shell, CLI, And Modes

| Checklist item | Coverage evidence |
| --- | --- |
| Native local terminal coding agent in C++23 | `cmake --build --preset dev`, `ava_tests.core_mode`, `ava_tests.app_runtime`, `README.md` build/run docs. |
| Interactive TUI entry point and non-TTY startup rejection | `ava_tests.tui_composer`, `ava_tests.app_runtime`, `ava_cli.interactive_startup`, `ava_cli.interactive_pty`, `ava_cli.headless_print_*`, opt-in `ava_tui.tmux_smoke`. |
| Print mode, text/JSONL output, piped stdin, and TTY-bound terminal-control sanitization | `ava_tests.app_print`, `ava_cli.headless_print_positional_prompt`, `ava_cli.headless_print_prompt_overrides`, `ava_cli.headless_mode_json_alias`. |
| Stdio JSONL RPC mode | `ava_tests.app_rpc`, `ava_tests.app_rpc_queue`, `ava_tests.app_rpc_resolver`, `ava_cli.headless_rpc_contract`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`; dedicated RPC JSONL import/share remains deferred and documented. |
| Session resume by latest workspace session and id/prefix | `ava_tests.session`, `ava_tests.app_runtime`, `ava_cli.headless_print_session_startup_options`. |
| Direct positional one-shot prompt UX | `ava_cli.headless_print_positional_prompt`, `ava_tests.app_print`. |
| Sessionless/ephemeral `--no-session` mode | `ava_cli.headless_print_no_session`, `ava_tests.session`. |
| Pi-style text file arguments and `@` references | `ava_cli.headless_print_file_args`, `ava_tests.tui_composer`, `ava_tests.tools`. |
| Pi-style session CLI options | `ava_cli.headless_print_session_startup_options`, `ava_tests.session`, `ava_tests.app_runtime`; includes `--session-id` and `--resume`/`-r` aliases. |
| CLI compatibility aliases for headless modes | `ava_cli.headless_mode_json_alias`, `ava_tests.core_mode`. |

## Provider, Models, And Auth

| Checklist item | Coverage evidence |
| --- | --- |
| Provider registry with OpenAI, Anthropic, DeepSeek, Gemini, Kimi, Moonshot, OpenRouter, Z.AI, xAI, Groq, Cerebras, Together, Fireworks, Mistral, and user-defined families | `ava_tests.config_context_auth_oauth`, `ava_tests.provider_openai`, `ava_tests.provider_anthropic`, `ava_tests.provider_gemini`, `docs/goals/pi-mvp-parity/providers-models-auth.md`; live validation remains credential-gated through `ava_tests.provider_live_smoke` and `docs/operations/testing.md`. |
| Model metadata for capabilities, modalities, context windows, pricing, and reasoning controls | `ava_tests.config_context_auth_oauth`, `ava_tests.app_runtime`, `ava_tests.provider_openai`, `ava_tests.provider_anthropic`, `ava_tests.provider_gemini`. |
| Provider/model listing UX comparable to Pi model discovery commands | `ava_tests.app_runtime`, `ava_tests.app_command_registry`, `ava_tests.config_context_auth_oauth`, opt-in `ava_tui.tmux_smoke` model selector/scoped-model checks, `docs/core/usage.md`, `docs/operations/testing.md`. |
| API-key auth from secure local storage and environment variables | `ava_tests.config_context_auth_oauth`, `ava_tests.provider_live_smoke` credential-gated path. |
| OpenAI browser/device/headless OAuth flows | `ava_tests.config_context_auth_oauth`, `ava_tests.app_runtime`, `README.md` auth docs. |
| Anthropic OAuth token resolution and refresh for stored/env tokens | `ava_tests.config_context_auth_oauth`, `ava_tests.provider_anthropic`, `docs/operations/testing.md` live-smoke credential notes. |

## Agent Loop, Events, And Control

| Checklist item | Coverage evidence |
| --- | --- |
| Streaming assistant text, reasoning, tool lifecycle, retry, compaction, cancellation, and terminal turn events | `ava_tests.agent_loop`, `ava_tests.agent_loop_resilience`, `ava_tests.app_event_serialization`, `ava_tests.app_event_bus`, `ava_tests.app_rpc`. |
| Sequential tool-call loop with validation, permissions, audit, and bounded outputs | `ava_tests.agent_tool_dispatcher`, `ava_tests.tools`, `ava_tests.agent_loop`, `ava_cli.headless_rpc_tool_failure`, `ava_cli.headless_e2e_model_smoke`. The default-off internal read/search parallel option is separately covered by `ava_tests.tool_scheduler`, `ava_tests.agent_loop`, and focused sanitizer suites, but remains backend-only with no public flag or full-binary smoke. |
| RPC steering and follow-up queues while a prompt is running | `ava_tests.app_rpc_queue`, `ava_cli.headless_rpc_active_run_rejection`, opt-in `ava_tui.tmux_smoke` active-run queue checks. |
| Cooperative cancellation through RPC and runtime boundaries | `ava_tests.agent_loop_resilience`, `ava_cli.headless_rpc_cancel`, `ava_cli.headless_rpc_bash_process_cleanup`. |
| User-facing interrupt/abort/resume continuation semantics | `ava_tests.tui_composer`, `ava_tests.app_rpc_queue`, `ava_cli.headless_rpc_cancel`, `docs/core/usage.md`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`. |
| Explicit steer-now, queue-next, restore-draft, cancel/interrupt, and resume-later vocabulary | `ava_tests.app_rpc_queue`, `ava_tests.tui_composer`, opt-in `ava_tui.tmux_smoke` active-run queue checks, `docs/core/usage.md`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`. |

## Built-In Tools

| Checklist item | Coverage evidence |
| --- | --- |
| Pi core file/shell/search/list shape | `ava_tests.tools`, `ava_tests.agent_tool_dispatcher`, `ava_cli.headless_tool_visibility`, `ava_cli.headless_e2e_model_smoke`. |
| AVA-native `apply_patch`, web, skill, task, question, and LSP tools | `ava_tests.tools`, `ava_tests.agent_loop`, `ava_tests.app_print`, `ava_tests.lsp`, `ava_cli.headless_rpc_question_reply`, `ava_cli.headless_rpc_question_reply_multi`, `docs/operations/testing.md` headless tool smoke guidance. |
| Bounded tool outputs and side effects through permission/audit paths | `ava_tests.tools`, `ava_tests.permission_rules`, `ava_cli.headless_rpc_permission_reply`, `ava_cli.headless_rpc_tool_failure`, `ava_cli.headless_e2e_model_smoke`. |
| Tool allowlist/exclusion controls | `ava_cli.headless_tool_visibility`, `ava_tests.app_runtime`, `README.md` tool flag docs. |
| Configurable primary agents through `--agent` and foreground/background subagents through `task` | `ava_tests.agent_loop`, `ava_tests.app_runtime`, and `ava_cli.headless_print_positional_prompt`; compatibility coverage for accepted `--allow-tool task`, and opt-in `scripts/live-coding-dogfood.sh` for live coding-tool behavior. |
| Pi-style `find`/`ls` alias mapping | `ava_tests.app_command_registry`, `ava_cli.headless_tool_visibility`, opt-in `ava_tui.tmux_smoke` `/find` and `/ls` checks. |
| Consistent tool cards and plain/headless representations | `ava_tests.tui_composer`, `ava_tests.app_event_serialization`, `ava_tests.tools`, `ava_cli.headless_rpc_tool_failure`, opt-in `ava_tui.tmux_smoke` `/tool`, `/diff`, `/copy tool`, `/copy diff`, and `/copy permission` checks. |

## Permissions, Trust, And Safety

| Checklist item | Coverage evidence |
| --- | --- |
| Backend allow/ask/deny policy with TUI, RPC, and headless resolvers | `ava_tests.core_json_permission`, `ava_tests.permission_rules`, `ava_tests.app_rpc_resolver`, `ava_cli.headless_rpc_permission_reply`. |
| Session grants, durable rules, protected files, and audit entries | `ava_tests.permission_rules`, `ava_tests.session`, `ava_cli.headless_rpc_permission_grant`, `ava_cli.headless_rpc_permission_grant_lifecycle`. |
| Hard-deny paths for unsafe or model-writable policy locations | `ava_tests.permission_rules`, `ava_tests.tools`, `ava_tests.app_runtime`. |
| Project trust boundary for project-local executable/plugin/config resources | `ava_tests.app_runtime`, `ava_tests.app_command_registry`, `ava_tests.app_rpc`, opt-in `ava_tui.tmux_smoke` `/trust` checks. |
| Rule-management UX for list/audit/explain/add/remove/diagnose/export | `ava_tests.permission_rules`, `ava_tests.app_rpc_resolver`, `ava_cli.headless_rpc_permission_reply`, `ava_cli.headless_rpc_permission_grant_lifecycle`, opt-in `ava_tui.tmux_smoke` `/permissions` checks, `docs/core/usage.md`, `docs/core/configuration.md`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`. |
| Clear denial explanations in TUI, RPC, and headless output | `ava_tests.tui_composer`, `ava_tests.tools`, `ava_tests.app_print`, `ava_cli.headless_rpc_permission_reply`, `ava_cli.headless_rpc_tool_failure`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`. |
| Side-effect security review checklist | `docs/development/side-effect-safety-checklist.md`, `docs/operations/testing.md`. |

## Sessions And Conversation History

| Checklist item | Coverage evidence |
| --- | --- |
| Append-only JSONL storage with resume/list | `ava_tests.session`, `ava_cli.headless_rpc_validate_session`. |
| Compaction, export, stats, usage/cost, model/reasoning changes | `ava_tests.session`, `ava_tests.app_compaction`, `ava_cli.headless_rpc_compact_success`, `ava_cli.headless_rpc_context_export`. |
| Backend/RPC tree inspection, fork, clone, names, labels, caller-supplied branch summaries | `ava_tests.session`, `ava_tests.app_rpc`, `docs/rpc-protocol.md` / `docs/headless-protocol.md` session metadata/tree/fork/clone/summarize docs. |
| User-facing session tree workflow | `ava_tests.tui_composer`, `ava_tests.session`, opt-in `ava_tui.tmux_smoke` session selector/tree checks. |
| Pi-style session slash commands | `ava_tests.app_command_registry`, `ava_tests.session`, `ava_tests.app_runtime`, `ava_tests.tui_composer`; includes `/export jsonl` and local `/import <path.jsonl> --confirm`. |
| Markdown, safe self-contained HTML, and raw AVA JSONL export/import | `ava_tests.session`, `ava_tests.app_runtime`, `ava_cli.headless_rpc_context_export`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`, `docs/core/usage.md` export/import docs. |
| Session migration/versioning policy | `ava_tests.session`, `docs/development/session-versioning.md`, `docs/rpc-protocol.md` / `docs/headless-protocol.md` session entry/schema-version notes. |

## Context, Prompts, Skills, And File References

| Checklist item | Coverage evidence |
| --- | --- |
| Project/global instruction loading from Pi-compatible `AGENTS.md`/`CLAUDE.md` files | `ava_tests.config_context_auth_oauth`, `ava_tests.app_runtime`. |
| Prompt commands and skills through unified command/context registry | `ava_tests.app_command_registry`, `ava_tests.config_context_auth_oauth`, `ava_cli.headless_rpc_command_registry`. |
| Plugin prompt/skill resources | `ava_tests.plugin`, `ava_tests.app_runtime`, `ava_tests.app_command_registry`, `ava_tests.app_rpc`, `ava_cli.headless_rpc_plugin_commands`, `ava_cli.headless_rpc_sample_plugin`. |
| Pi-style `SYSTEM.md` and `APPEND_SYSTEM.md` prompt files | `ava_tests.app_runtime`, `ava_tests.config_context_auth_oauth`, `ava_cli.headless_print_prompt_overrides`. |
| CLI overrides for system and appended system prompts | `ava_cli.headless_print_prompt_overrides`, `ava_tests.app_print`. |
| Prompt command templates and interpolation | `ava_tests.app_command_registry`, `ava_cli.headless_rpc_command_registry`. |
| `@` file reference UX and safe expansion | `ava_cli.headless_print_file_args`, `ava_tests.tui_composer`, `ava_tests.tools`. |
| Context freshness diagnostics | `ava_tests.app_runtime`, `ava_cli.headless_rpc_context_export`, opt-in `ava_tui.tmux_smoke` `/context` checks. |

## Config, Settings, And Reload

| Checklist item | Coverage evidence |
| --- | --- |
| Domain-specific XDG JSON files for current config domains | `ava_tests.config_context_auth_oauth`, `ava_tests.app_runtime`, `ava_tests.session`, `ava_tests.lsp`, `ava_tests.plugin`, `ava_tests.mcp`. |
| Settings architecture decision and manual resource layout | `docs/core/configuration.md`, `docs/product/mvp-baseline.md`, `docs/goals/pi-mvp-parity/settings-packages-resources.md`. |
| Safe display/keybinding writes and validation-before-commit | `ava_tests.app_runtime`, `ava_tests.tui_composer`; display and keybinding writes use `ava::core::write_text_file_atomic` and reject symlink targets. |
| Reload diagnostics and last-known-good behavior | `ava_tests.app_runtime`, opt-in `ava_tui.tmux_smoke` settings/reload checks, `docs/core/usage.md`. |
| Project-resource trust gating | `ava_tests.app_runtime`, `ava_tests.app_command_registry`, `ava_tests.plugin`, `ava_tests.mcp`, `ava_tests.lsp`, `docs/core/configuration.md`. |
| Offline/package/resource policy | `ava_cli.offline_print_prompt`, offline coverage in `ava_tests.app_print` and `ava_tests.app_rpc`, local plugin install/remove coverage in `ava_tests.app_runtime` and `ava_tests.app_rpc`, `ava_cli.package_manager_deferred`, `/packages` coverage in `ava_tests.app_runtime`, `docs/core/configuration.md`, `docs/extensions/plugin-system.md`. |
| Keybinding customization UX | `ava_tests.tui_composer`, `ava_tests.app_runtime`, `ava_tests.app_command_registry`, opt-in `ava_tui.tmux_smoke` keybinding init/import/set/reset/validate/reload and custom-key checks, `docs/core/configuration.md`, `docs/core/usage.md`. |

Remaining config/settings work is product polish beyond MVP: a possible future unified settings facade and remote package/resource install only if the safety policy is approved. Future remote/package surfaces must respect the existing `--offline` provider-call guard.

## TUI And UX Maturity

| Checklist item | Coverage evidence |
| --- | --- |
| TUI renders assistant text, thinking blocks, tool lifecycle, and permission prompts | `ava_tests.tui_composer`, `ava_tests.app_event_serialization`, `ava_tests.app_event_bus`. |
| TUI provider/login flows and structured user questions | `ava_tests.tui_composer`, `ava_tests.app_command_registry`, `ava_cli.headless_rpc_question_reply`, `ava_cli.headless_rpc_question_reply_multi`. |
| Mature multiline editor behavior | `ava_tests.tui_composer`, opt-in `ava_tui.tmux_smoke` bracketed paste, history, selection, word movement, external editor, suspend/resume, resize, and active-run follow-up checks. |
| Slash-command palette | `ava_tests.tui_composer`, opt-in `ava_tui.tmux_smoke` slash-palette checks. |
| General editor file/path autocomplete and `@` references | `ava_tests.tui_composer`, `ava_cli.headless_print_file_args`, opt-in `ava_tui.tmux_smoke` file-reference/path-completion checks. |
| Session tree UI | `ava_tests.tui_composer`, `ava_tests.session`, opt-in `ava_tui.tmux_smoke` session selector checks. |
| Permission UX maturity | `ava_tests.tui_composer` OpenCode-aligned prompt wording/request-id/external-directory assertions, `ava_tests.permission_rules`, `ava_tests.app_rpc_resolver`, `ava_cli.headless_rpc_permission_reply`, opt-in `AVA_TUI_TMUX_SMOKE=1` `ava_tui.tmux_smoke` permission prompt/audit/diagnose/copy checks. |
| Tool result UX maturity | `ava_tests.tui_composer`, `ava_tests.tools`, `ava_tests.app_event_serialization`, `ava_cli.headless_rpc_tool_failure`, opt-in `ava_tui.tmux_smoke` `/tool`, `/diff`, `/copy tool`, and `/copy diff` checks. |
| Markdown/code/diff rendering | `ava_tests.tui_composer` markdown/table/code/link/OSC8/large-render assertions, opt-in `ava_tui.osc8_smoke`, `docs/operations/testing.md` TUI evidence strategy. |
| Theme support and visual polish | `ava_tests.app_runtime`, `ava_tests.tui_composer`, opt-in `ava_tui.tmux_smoke` theme/settings/reload/plain-mode checks, `docs/core/configuration.md`. |
| Keyboard shortcut discovery and customization | `ava_tests.tui_composer`, `ava_tests.app_runtime`, `ava_tests.app_command_registry`, opt-in `ava_tui.tmux_smoke` `/hotkeys`, `/keybindings`, and custom-binding checks. |
| Inline image import/preview with safe fallback | `ava_tests.tui_composer`, `ava_tests.session`, opt-in `ava_tui.kitty_image_smoke`, opt-in `ava_tui.tmux_smoke`. |
| First-run onboarding | `ava_tests.tui_composer`, opt-in `ava_tui.tmux_smoke` no-auth onboarding checks. |

The current TUI rows are either checked with deterministic/PTY evidence or explicitly deferred for future product polish, broader accessibility review, or broader performance profiling.

## Extensibility, Packages, And Themes

| Checklist item | Coverage evidence |
| --- | --- |
| Bounded out-of-process local plugin foundation | `ava_tests.plugin`, `ava_tests.app_runtime`, `ava_tests.app_rpc`, `ava_cli.headless_rpc_plugin_commands`, `ava_cli.headless_rpc_sample_plugin`, `tests/golden/ava-080/`. |
| Stdio MCP local slice | `ava_tests.mcp`, `ava_cli.headless_rpc_mcp_commands`, `ava_cli.headless_rpc_mcp_config_errors`, `docs/extensions/mcp.md`, `tests/golden/ava-080/`. |
| Local plugin directory install/remove plus deferred remote package/resource manager | `ava_tests.app_runtime`, `ava_tests.app_rpc`, `ava_cli.package_manager_deferred`, `docs/core/configuration.md`, `docs/extensions/plugin-system.md`; `/plugins install <path>` and `/plugins remove <id>` manage local global disabled plugin directories, while `/packages`, `ava packages ...`, and remote package flows remain deferred. |
| Package trust/signing/source policy requirement before remote install | `docs/core/configuration.md`, `docs/extensions/plugin-system.md`, `ava_cli.package_manager_deferred`, `ava_tests.app_runtime`; remote package installs remain disabled until source allowlists, provenance/signing, compatibility, rollback, and trust UX are approved. |
| Manual custom themes | `ava_tests.app_runtime`, `ava_tests.tui_composer`, opt-in `ava_tui.tmux_smoke`, `docs/core/configuration.md`. |

## Multimodal And Attachments

| Checklist item | Coverage evidence |
| --- | --- |
| Image model metadata, sanitized attachment metadata, managed storage, fork/clone copy, replay validation, provider serialization | `ava_tests.session`, `ava_tests.provider_openai`, `ava_tests.provider_anthropic`, `ava_tests.provider_gemini`, `ava_tests.config_context_auth_oauth`. |
| RPC image input plumbing | `ava_tests.app_rpc`, `ava_tests.session`, `docs/rpc-protocol.md` / `docs/headless-protocol.md` image request docs. |
| TUI/user-facing image import | `ava_tests.tui_composer`, `ava_tests.session`, opt-in `ava_tui.tmux_smoke` clipboard-image fallback checks. |
| Inline preview where terminal capabilities allow plus safe text fallback | `ava_tests.tui_composer`, opt-in `ava_tui.kitty_image_smoke`, opt-in `ava_tui.tmux_smoke`. |

## Testing, Release, And Quality Bar

| Checklist item | Coverage evidence |
| --- | --- |
| CTest, fake providers/servers, plugin/MCP fixtures, sanitizer workflow, live-provider smoke opt-in | `tests/CMakeLists.txt`, `docs/operations/testing.md`, `ava_tests.provider_live_smoke`, CI workflow badge in `README.md`. |
| Headless print/RPC smoke coverage | `ava_cli.headless_print_*`, `ava_cli.headless_rpc_*`, `ava_cli.headless_e2e_model_smoke`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`. |
| Pi-parity checklist mapped to coverage | This file. |
| Provider live-smoke matrix with credential-gated skips | `docs/operations/testing.md` provider live-smoke matrix, `ava_tests.provider_live_smoke`, `tests/provider_live_smoke_tests.cpp`, opt-in full-binary `scripts/live-model-dogfood.sh`. |
| Performance smoke for startup, large transcript render, large tool output, search, and replay | `ava_tests.tui_composer` large-render/large-output/tail-renderer/very-long-transcript budgets, `ava_cli.headless_performance_smoke`, `docs/operations/testing.md` performance release thresholds. |
| TUI regression harness for input/editor/session-tree/permission/tool-card workflows | `ava_tests.tui_composer`, opt-in `ava_tui.tmux_smoke`, opt-in `ava_tui.kitty_image_smoke`, opt-in `ava_tui.osc8_smoke`, `docs/operations/testing.md` terminal-smoke guidance. |
| Documentation consistency before MVP cut | Final docs audit plus corrections in `README.md`, `docs/core/usage.md`, `docs/core/configuration.md`, `docs/operations/testing.md`, `docs/rpc-protocol.md` / `docs/headless-protocol.md`, `docs/product/mvp-baseline.md`, `docs/product/mvp-coverage-ledger.md`, and `docs/goals/pi-mvp-parity/*.md`. |

## Deferred Follow-Up Rows

These unchecked rows are explicitly deferred or excluded from the current MVP closure. They remain useful future work, but they are not unresolved MVP parity blockers.

| Area | Deferred rows | Current reason |
| --- | --- | --- |
| Provider/model/auth validation | Broader provider zoo, Anthropic interactive OAuth, first-class custom provider registration, scoped credential/config overrides, and cross-provider reasoning replay | Core provider paths exist, DeepSeek is implemented through the compatible path with `reasoning_effort` mapping, `/providers` and `/models` expose current metadata and diagnostics, and live-smoke behavior is credential-gated. The deferred provider items need provider-specific auth, metadata, compatibility, and smoke plans before becoming MVP scope. |
| Sessions | Provider-generated branch summaries | Append-only session tree/fork/clone, caller-supplied summaries, and the session-versioning policy exist; provider-generated branch summaries remain deferred until branch UX needs them. |
| Settings/reload/trust | Future unified settings facade, broader offline semantics for future remote surfaces, and remote package/resource install if approved | MVP keeps domain configs with safe display/keybinding writes, reload diagnostics, trust-gated project resources, implemented provider-call `--offline`, and documented package/offline deferrals. |
| TUI/editor/rendering/accessibility/performance | Extra selection polish, richer editor behavior, broader non-tool denial wording, deeper diff navigation, broader screen-reader review, and broader release-workload profiling | MVP TUI/editor/rendering/tool behavior is covered by deterministic tests and opt-in PTY smokes; the deferred items are future product polish or broader audits. |
| Attachments/export | Attachment export/replay behavior for every session/export format | Import, replay, provider serialization, and metadata export exist; raw attachment byte archive/export policy remains outside the current export slice. |
| Release proof | Broader TUI harness expansion | Existing tests, smokes, provider live-smoke matrix, performance thresholds, and final docs consistency are broad; future work is deeper TUI/screen-reader/product polish coverage, not current release-evidence agreement. |

## Deferred Or Excluded MVP Decisions

| Decision | MVP disposition | Rationale / unblocker |
| --- | --- | --- |
| Broader provider zoo beyond current built-ins and custom `providers.json`: Copilot, Bedrock, Vertex, Azure, and similar remaining providers | Deferred | Each provider needs auth semantics, model metadata, compatibility quirks, pricing, and credential-gated live-smoke criteria. The provider validation slice may narrow the next provider decision but should not silently expand the MVP. |
| Anthropic interactive OAuth setup | Deferred | Stored/env Anthropic OAuth token resolution and refresh exist. Official Anthropic docs do not expose a supported third-party authorization/device flow, so AVA should not initiate one unless Anthropic publishes a stable developer OAuth program. |
| Provider-generated branch summaries | Deferred | Caller-supplied branch summaries are implemented through RPC `summarize_branch`; provider-generated compaction summaries remain in `/compact`. Provider-generated branch summaries need a branch-navigation UX, model-call budget, permission policy, and stale-session race design before becoming MVP scope. |
| Parallel or configurable ordinary tool execution | Partial / user-facing deferred | Internal default-off `AgentLoopOptions::parallel_read_search_tools` now runs only preflight-proven builtin `read_file`, `list_directory`, `glob`, and `grep` epochs with provider-order commits, covered by `ava_tests.tool_scheduler`, `ava_tests.agent_loop`, and focused sanitizer suites. There is no CLI/TUI/RPC public flag, no default behavior change, and no full-binary smoke until a public/headless opt-in is added. Native background subagents are covered separately by the `task`/`BackgroundJobRegistry` evidence above. |
| Pi-style automatic `!`/`!!` shell-output injection into provider context | Excluded from MVP | AVA currently runs `!`/`!!` through permissioned `/bash` and keeps output visible/audited. Automatic provider-context injection is intentionally excluded from the MVP because it would require new session/replay entries and data-leak controls. |
| HTTP/server daemon, OpenAPI, generated SDKs, and SSE | Deferred | Stdio JSONL RPC is the MVP automation contract until it proves stable enough to wrap. |
| Plugin/package marketplace, remote install, signing, and package trust | Deferred | Remote code/package flows require a trust, provenance, signing, compatibility, and rollback policy first. Local plugin directory install/remove is the bounded manual plugin path, not broad package-manager parity. |
| Custom provider plugins or dynamic provider package loading | Deferred | Provider auth and model metadata are security-sensitive and should not be loaded dynamically without a strict contract. |
| OS/container sandboxing | Excluded | AVA should not claim sandbox guarantees without real OS/container enforcement and tests. |
| Broader multi-agent orchestration and chained task graphs | Deferred | Configurable task subagents and background job tracking are present; broader orchestration, task graph UX, and plugin-contributed subagent packages remain future work. |
| Advanced MCP transports, OAuth, subscriptions, sampling, templates, binary/blob resources | Deferred | Current MVP keeps a bounded local stdio/read-style resource slice behind explicit permission/audit paths. |
| Desktop/web/cloud surfaces, SaaS sharing, Slack/Discord bots, GPU orchestration, telemetry infrastructure | Excluded | These are OpenCode-style platform surfaces, not AVA local terminal-agent MVP requirements. |
