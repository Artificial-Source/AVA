# AVA Codebase Guide

This guide is the practical source map for maintainers changing AVA. Start with
[`../architecture.md`](architecture.md) for runtime flow and authority
boundaries, then use this catalog to find the owning module and focused tests.
The catalog covers every top-level production module under `src/ava/`; vendored
or experimental trees are not architectural authority.

## Build and source shape

- `src/main.cpp` is the production `ava` executable entry point.
- `src/ava/CMakeLists.txt` declares backend and frontend library groups.
- Each `src/ava/<module>/CMakeLists.txt` lists that module's production files and
  actual target links. Update it when adding or removing C++ files.
- `src/ava/app/app.cpp` is the CLI composition entry point.
- `tests/CMakeLists.txt` assembles focused C++ suites, support executables, and
  whole-process CTest cases.
- `tests/module_dependency_rules_test.py` enforces the most important include
  directions. `tests/fixtures/module_dependency_rules.json` contains only
  reviewed temporary exceptions and is currently empty.

Every `.cpp` below `src/ava/` includes `sys.h` first. Use canonical
`ava/<module>/...` includes across modules. CMake target visibility describes
build propagation; it does not make headers a stable public SDK.

CMake 3.27 is the minimum. The canonical `dev`/`sanitize` presets require a writable `GITACHE_ROOT`, Python 3 for complete test registration, and JSON-capable Universal Ctags when debug/libcwd print-member generation is enabled. The tested 2026-08-23 source matrix is Ubuntu 24.04.4 x64 with GCC 13 under BetaTest/Make and Release/Ninja. Clang was environment-blocked; MSVC/Windows/macOS and AArch64 were not qualified, multi-config is not release-qualified, and dirty trees are never artifact candidates. Current release authority is the [release-readiness ledger](../product/release-readiness.md).

## Module catalog

“Nearest tests” names the first regression suites to inspect, not an exhaustive
list. CTest names generally use `ava_tests.<suite>`; suites can also be run
through the locked runner or directly as `./build/tests/ava_tests <suite>`.

### `agent/`

**Owns:** `AgentLoop`, provider-message projection, assistant-turn assembly,
model-visible tool metadata/registry/dispatch, scheduling, tool timeline/results,
todos/questions, subagent coordination and background jobs.

**Key entry points:** `agent_loop.h`, `agent_loop.cpp`,
`agent_loop_session.cpp`, `message_builder.h`, `history_projection.h`,
`tool_registry.h`, `tool_registration.h`, `tool_dispatcher.h`,
`tool_scheduler.h`, `subagent_coordinator.h`, and
`background_job_registry.h`.

**Belongs here:** orchestration that connects provider-neutral events to tool
calls and subsequent provider iterations; adapters from model-visible calls to
lower-level tool services.

**Does not belong here:** frontend/protocol behavior, provider-native HTTP JSON,
raw filesystem/process mechanics, or session pathname/lease acquisition.

**Nearest tests:** `agent_loop_*_tests.cpp`, `agent_tool_dispatcher_tests.cpp`,
`tool_scheduler_tests.cpp`, `agent_todo_tests.cpp`,
`subagent_coordinator_tests.cpp`, and `agent_loop_tests.cpp`.

### `app/`

**Owns:** the composition root and user-facing application runtime: CLI mode
selection, runtime session opening, prompt execution, run admission/persistence
routing, commands/catalogs, trust/onboarding/auth connection, TUI integration,
print mode, and protocol adapters.

**Key entry points:** `app.cpp`, `runtime.h`, `runtime/Session.h`,
`runtime/OpenOptions.h`, `runtime/RunOptions.h`, `session_run_controller.h`,
`commands.h`, `command_registry.h`, `print_mode.h`, `rpc_mode.h`, `acp_mode.h`,
`interactive_tui.cpp`, and `interactive.h`. Protocol implementations live under
`app/rpc/` and `app/acp/`. `Application.h` currently constructs the
application-scoped memory page pool and node resource, but no production
allocation path uses that pool yet.

**Belongs here:** composition and lifecycle policy involving several lower
modules; translation between shared runtime behavior and a frontend contract.

**Does not belong here:** reusable low-level I/O, provider parsers, session
format rules, or TUI cell/render algorithms.

**Nearest tests:** `app_runtime_*_tests.cpp`, `app_rpc_*_tests.cpp`,
`app_command_*_tests.cpp`, `app_print_tests.cpp`,
`session_run_controller_tests.cpp`, `app_event_serialization_tests.cpp`, and the
`cli_headless_*.cmake` whole-process tests.

### `command/`

**Owns:** canonical command intent and immutable `CommandPlan` classification,
executable discovery/identity, child environment construction, and private
process-group primitives.

**Key entry points:** `command.h`, `command_plan.cpp`, `intent.cpp`, `policy.h`,
`discovery.h`, `environment.h`, and `private_group.h`.

**Belongs here:** parsing/classification facts needed before approval and
execution; secret-free sealed plan identity.

**Does not belong here:** permission resolution, containment policy application,
command output capture, or shell UI.

**Nearest tests:** `command_tests.cpp`,
`app_command_classification_tests.cpp`, and command cases in
`agent_loop_command_safety_tests.cpp`.

### `config/`

**Owns:** XDG locations, model/provider/reasoning/prompt profiles, auth records
and secure auth-file storage, OpenAI OAuth helpers, and session-title settings.

**Key entry points:** `xdg_paths.h`, `model_config.h`, `model_profiles.h`,
`provider_profiles.h`, `prompt_config.h`, `auth.h`, `auth_file_store.h`, and
`openai_oauth.h`.

**Belongs here:** typed loading/validation of owner configuration and built-in
catalog defaults.

**Does not belong here:** active runtime selection, UI, provider request
serialization, or workspace side effects.

**Nearest tests:** `config_context_auth_oauth_tests.cpp`,
`app_runtime_model_tests.cpp`, and `provider_openai_limits_tests.cpp`.

### `containment/`

**Owns:** verified command containment plans and Linux Landlock/seccomp setup.

**Key entry points:** `containment.h`, `containment_plan.cpp`, `landlock.cpp`,
and `seccomp_network.cpp`.

**Belongs here:** kernel capability probing, descriptor-identified filesystem
rules, pre-exec restriction installation, and truthful availability state.

**Does not belong here:** deciding whether a user should approve a command,
launching arbitrary plugin/MCP/LSP processes, or claiming whole-process
sandboxing.

**Nearest tests:** `containment_tests.cpp` and containment cases in
`tools_process_network_tests.cpp`.

### `context/`

**Owns:** bounded discovery/loading of markdown context resources and skills.

**Key entry points:** `context_loader.h`, `markdown_resource.h`, and
`skill_loader.h`.

**Belongs here:** resource parsing, limits, and source metadata below runtime
prompt assembly.

**Does not belong here:** project-trust decisions, command catalogs, active
prompt lifecycle, or tool dispatch.

**Nearest tests:** context/skill cases in
`config_context_auth_oauth_tests.cpp`, `app_runtime_prompt_tests.cpp`, and
`app_runtime_command_catalog_tests.cpp`.

### `core/`

**Owns:** dependency-light primitives shared across AVA: `Result`/`Error`, IDs,
strict JSON helpers, modes/outcomes, paths, atomic files, trusted-home checks,
`open_beneath`, `AnchorOpen`/`AnchorSet`, process arguments, and generated
version metadata.

**Key entry points:** `result.h`, `error.h`, `strict_json.h`, `ids.h`,
`atomic_file.h`, `open_beneath.h`, `AnchorSet.h`, and `Application.h`.

**Belongs here:** broadly reusable mechanisms with no product-layer dependency.

**Does not belong here:** model/tool/session policy, frontend state, or
convenience code used by only one high-level subsystem.

**Nearest tests:** `core_tests.cpp`, `core_mode_json_permissions_tests.cpp`, and
`test_harness_tests.cpp`.

### `debug/`

**Owns:** optional libcwd formatting and generated-debug support, including
bounded member printers.

**Key entry points:** `debug_ostream_operators.h`, `maxlen.h`,
`print_members_on.h`, and `print_reference.h`.

**Belongs here:** developer-only debug representation support that compiles only
under the configured debug path.

**Does not belong here:** production diagnostics, telemetry, public errors, or
behavior required by release builds.

**Nearest tests:** `debug_tests.cpp`, `libcwd_debug_output_test.py`, and
`print_members_coverage_test.py`.

### `desktop/`

**Owns:** the optional experimental Qt Quick/QML desktop prototype.

**Key entry points:** `main.cpp`, `desktop_controller.h`, and `qml/Main.qml`.
It is enabled with `AVA_BUILD_DESKTOP_QML` and builds `ava-desktop` separately.

**Belongs here:** prototype Qt application/controller and QML presentation.

**Does not belong here:** assumptions that the production runtime is integrated,
or duplicated provider/tool/session behavior.

**Nearest tests:** no dedicated automated desktop suite currently exists; the
nearest gate is configuration/build with the option enabled, followed by the
manual checks in [`../desktop-qml.md`](../interfaces/desktop-qml.md).

### `diagnostics/`

**Owns:** bounded private runtime traces, artifact storage, safe typed failures,
and support-export records.

**Key entry points:** `runtime_diagnostics.h`, `artifact_store.h`, `records.h`,
and `safe_failure.h`.

**Belongs here:** privacy-filtered diagnostic metadata and private descriptor-safe
artifact lifecycle.

**Does not belong here:** public runtime events, session records, raw provider
bodies, credentials, or ordinary debug logging.

**Nearest tests:** `diagnostics_tests.cpp`,
`runtime_diagnostics_tests.cpp`, `runtime_diagnostics_cli_test.py`, and
`doctor_support_cli_test.py`.

### `event/`

**Owns:** immutable frontend-neutral runtime event alternatives, envelope
metadata/payload types, mapping to envelope classes, and JSONL serialization.

**Key entry points:** `RuntimeEvent.h`, `RuntimeEventType.h`, `EventEnvelope.h`,
and `events.h`.

**Belongs here:** neutral, typed, safe-to-publish event data with no knowledge of
its producer or consumer.

**Does not belong here:** provider/agent source types, frontend formatting,
protocol request handling, or runtime callbacks. Source-to-event adapters belong
in `app/runtime_event_adapters.*`.

**Nearest tests:** `events_tests.cpp`, `app_event_serialization_tests.cpp`, and
`tests/golden/runtime-events-v1/`.

### `http/`

**Owns:** HTTP request/response and transport interfaces, curl transport,
streaming callbacks, retries, cancellation, and optional transport observation.

**Key entry points:** `transport.h` and `curl_transport.h`.

**Belongs here:** provider-agnostic HTTP mechanics and decorators.

**Does not belong here:** provider endpoints/payloads, user permission policy,
or interpretation of model errors.

**Nearest tests:** fake transport coverage in `tests/support/fake_transport.*`,
provider contract HTTP/SSE tests, and retry/cancel cases in agent/provider
suites.

### `lsp/`

**Owns:** configured and built-in LSP server recipes, bounded project/document
acquisition, subprocess lifecycle, transport/protocol, and cached client
operations.

**Key entry points:** `lsp_client.h`, `configured_provider.h`,
`builtin_recipes.h`, and `bounded_file_reader.h`.

**Belongs here:** safe LSP process/protocol mechanics behind the tool boundary.

**Does not belong here:** model-visible schemas, generic file tools, frontend
rendering, or silent server installation/download.

**Nearest tests:** `lsp_tests.cpp`, `lsp_real_clangd_smoke.cpp`, and
`tests/support/fake_lsp_server.cpp`.

### `mcp/`

**Owns:** MCP config, protocol, bounded stdio client lifecycle, and permissioned
tool/resource/prompt brokering.

**Key entry points:** `config.h`, `protocol.h`, `stdio_client.h`, and
`tool_broker.h`.

**Belongs here:** MCP-specific discovery, wire behavior, lifecycle, validation,
and descriptors offered upward to the agent.

**Does not belong here:** the agent tool registry, generic tool policy,
frontend protocol, or trust-by-protocol assumptions.

**Nearest tests:** `mcp_tests.cpp`, `tests/support/fake_mcp_server.cpp`,
`cli_headless_rpc_mcp_*.cmake`, and MCP golden fixtures.

### `observability/`

**Owns:** optional private `RunObservation` and trace context/records.

**Key entry point:** `run_observer.h`.

**Belongs here:** callback-safe, bounded observation data that is explicitly
separate from runtime correctness.

**Does not belong here:** public events, persistence authority, credentials, or
logic whose failure changes a successful run.

**Nearest tests:** `run_observer_tests.cpp` and observation cases in
`runtime_diagnostics_tests.cpp` and `session_run_controller_tests.cpp`.

### `permissions/`

**Owns:** operation/risk/action policy, permission prompt and resolution types,
command permission metadata, durable exact-match rule codecs/storage/matching,
and grant scope constraints.

**Key entry points:** `permission.h` and `permission_rules.h`.

**Belongs here:** backend decisions that precede frontend resolution, and
validated durable permission rules.

**Does not belong here:** UI prompts, raw command planning, actual file/process
execution, or treating project trust as permission.

**Nearest tests:** `permission_rules_tests.cpp`,
`core_mode_json_permissions_tests.cpp`, `tui_permission_tests.cpp`, and
`acp_permission_gateway_tests.cpp`.

### `plugin/`

**Owns:** plugin manifests/discovery/enablement/install, bounded out-of-process
runner protocol, static resources, diagnostics, event hooks, and brokered tool
descriptors.

**Key entry points:** `manifest.h`, `discovery.h`, `enablement.h`, `runner.h`,
`static_resources.h`, `tool_broker.h`, and `diagnostics.h`.

**Belongs here:** plugin-specific lifecycle and validation around local child
processes.

**Does not belong here:** loading third-party native code in-process, direct
agent registration, bypassing permissions, or generic MCP behavior.

**Nearest tests:** `plugin_tests.cpp`, `app_runtime_plugin_tests.cpp`,
`cli_headless_rpc_plugin_commands.cmake`, and
`cli_headless_rpc_sample_plugin.cmake`.

### `provider/`

**Owns:** provider-neutral request/content/stream types; OpenAI, Anthropic,
Gemini, and compatible request serializers/parsers; error normalization; and the
built-in provider factory registry.

**Key entry points:** `provider.h`, `registry.h`, the `*_provider.*` and
`*_request.*` pairs, and OpenAI stream parser files.

**Belongs here:** exact native wire adaptation and bounded normalization into
provider-neutral events.

**Does not belong here:** HTTP retry ownership, model catalog/auth storage,
tool execution, session mutation, or public frontend serialization.

**Nearest tests:** `provider_openai_*_tests.cpp`,
`provider_anthropic_tests.cpp`, `provider_gemini_tests.cpp`,
`provider_openai_compatible_tests.cpp`, and credential-gated
`provider_live_smoke_tests.cpp`.

### `session/`

**Owns:** JSONL record serialization/validation, stores, exact leases,
append/read authority, recovery/rollback, v4 assistant-output transactions,
logical/public/request projections, compaction, attachments, metadata, trees,
branches, stats, transcript, import/export, and portable sanitization.

**Key entry points:** `session_store.h`, `record.h`, `validation.h`,
`assistant_output.h`, `logical_projection.h`, `attachments.h`,
`session_branch.h`, and `session_tree.h`.

**Belongs here:** persistence format and authority-safe operations independent
of a frontend or running model loop.

**Does not belong here:** provider invocation, prompt commands, automatic
pathname reacquisition for current-runtime reads, or UI transcript layout.

**Nearest tests:** `session_store_tests.cpp`, `session_authority_tests.cpp`,
`session_recovery_tests.cpp`, `session_assistant_output_tests.cpp`,
`session_projection_tests.cpp`, `session_attachment_tests.cpp`,
`session_compaction_tests.cpp`, `session_tree_tests.cpp`, and
`session_read_authority_inventory_test.py`.

### `tools/`

**Owns:** reusable bounded file/read/write/edit/search, patch/diff, shell,
webfetch/websearch, LSP-tool, spill-file, secure-workspace, ignore-rule, and
mutation-queue mechanics plus `ToolContext`.

**Key entry points:** `file_tools.h`, `file_io.h`, `bash_tool.h`,
`search_tools.h`, `lsp_tools.h`, `webfetch_tool.h`, `websearch_tool.h`,
`secure_workspace.h`, `mutation_queue.h`, and `tool_io.h`.

**Belongs here:** side-effect implementation after policy inputs have been
supplied, with bounds, descriptors, cancellation, cleanup, and audit facts.

**Does not belong here:** model-visible schemas and loop orchestration,
frontend approval UI, or provider behavior.

**Nearest tests:** `tools_file_tests.cpp`, `tools_search_tests.cpp`,
`tools_process_network_tests.cpp`, `tools_tests.cpp`, and whole-process
`cli_headless_e2e_model_smoke.cmake`.

### `tui/`

**Owns:** ncurses TUI semantic state, composer/editor/input, transcript and
selection/search, keybindings, themes/text wrapping, tool cards, views,
runtime actions, and rendering/hit testing.

**Key entry points:** `composer.h`, `runtime.h`, `event_state.h`, `keybindings.h`,
`theme.h`, `text.h`, and `tool_cards.h`.

**Belongs here:** terminal presentation and interaction derived from neutral
runtime events/state.

**Does not belong here:** provider/session/tool implementations, permission
policy, protocol wire contracts, or business state that another frontend needs.

**Nearest tests:** `tui_composer_*_tests.cpp`, `tui_runtime_state_tests.cpp`,
`tui_keybinding_tests.cpp`, `tui_modal_tests.cpp`, `tui_selector_tests.cpp`,
`tui_transcript_*_tests.cpp`, `tui_tool_card_tests.cpp`, and opt-in
`tui_tmux_scenarios/`.

#### `tui/terminal/`

**Owns:** the lower ncurses terminal abstraction: `Session`, `Window`, cells,
grapheme clusters, attributes/colors/borders, dimensions/positions, and terminal
lifecycle primitives.

**Key entry points:** `Session.h`, `Window.h`, `GraphemeCluster.h`, and the small
value-type headers in the directory.

**Belongs here:** terminal/cell mechanisms reusable by the AVA TUI without app
semantics.

**Does not belong here:** composer state, tool cards, protocol events, or agent
runtime behavior.

**Nearest tests:** `tui_terminal_input_tests.cpp`,
`tui_terminal_lifecycle_smoke.py`, `tui_kitty_image_smoke.py`,
`tui_osc8_smoke.py`, and terminal cases in `tui_composer_tests.cpp`.

## Practical change recipes

### Add or modify a provider

1. Put native request/auth and parser code in `provider/`; keep the public seam
   as `ProviderRequest`, `HttpRequest`, and bounded `StreamEvent` values.
2. Register a factory in `provider/registry.cpp`.
3. Put built-in model/profile and credential-location changes in `config/`.
4. Add request, response, SSE, terminal/error, limit, and private-field leakage
   tests in the matching provider suite. Prefer fake transport/loopback tests;
   live-provider tests remain opt-in.
5. Update [`../providers.md`](../core/providers.md) and configuration docs when user
   behavior changes; do not duplicate endpoint schemas in architecture docs.

### Add a built-in tool

1. Decide the owning low-level module (`tools/`, `lsp/`, etc.) and implement a
   bounded, cancellation-aware primitive there.
2. Add the model-visible metadata/schema and handler adapter under `agent/`, then
   register it in `tool_registration.cpp`.
3. Add or reuse an `Operation` and backend permission policy. Ensure Ask/Deny,
   persistent-rule, headless, and audit behavior are explicit.
4. Emit bounded timeline/result data and persist the correct session records.
5. Add focused primitive and dispatcher/loop tests plus a fake-provider
   whole-process smoke when the end-to-end contract changes.
6. Complete the questions in
   [`side-effect-safety-checklist.md`](side-effect-safety-checklist.md).

### Change session persistence

1. Start at `session/record.*`, `validation*`, and `session_store*`; identify
   physical format, logical projection, public projection, and provider-request
   projection separately.
2. Preserve exact lease/read/append authority and explicit open-time recovery.
   Never make current-runtime code reopen by pathname.
3. For a format change, follow
   [`session-versioning.md`](session-versioning.md), update import/export and all
   projections, and add old/new/corrupt/truncated/bounded tests.
4. Update the normative [`../session-format.md`](../session-format.md). If RPC or
   ACP changes, update their separate contracts and goldens too.
5. Run the session, authority, recovery, projection, controller, and relevant
   whole-process replay suites.

### Add or change an app command

1. Put shared command behavior/registration under `app/command_*.cpp` and
   `command_registry.*`; keep TUI-only formatting/input in `tui/` or the TUI
   adapter.
2. Define whether the command runs during an active prompt and how it interacts
   with `SessionRunController`.
3. Route session writes through `Session::append_owned`/controller routes and
   reads through `Session::read_authority`.
4. Add registry/classification tests and adapter tests for every exposed
   frontend. Update [`../USAGE.md`](../core/usage.md) or the relevant protocol
   contract.

### Change RPC or ACP

1. Keep framing/lifecycle/resolvers in `app/rpc/` or `app/acp/`; reuse shared
   runtime commands rather than importing one protocol into the other.
2. Maintain cancellation, output serialization, bounded records, safe error
   projection, and one authoritative terminal response/commit.
3. Update the normative [`../rpc-protocol.md`](../rpc-protocol.md) or
   [`../acp.md`](../acp.md), machine-readable/golden fixtures, and compatibility
   policy where applicable.
4. Run focused direct/unit suites and the corresponding `cli_headless_*` or ACP
   subprocess/official-SDK tests.

### Change the TUI or terminal layer

1. Keep semantic event state distinct from rendered rows and terminal mechanics.
2. Update render and hit-test paths from the same layout/window calculation.
3. Add deterministic composer/input/selection tests first. Use the isolated tmux
   or direct-PTY smoke only when real terminal behavior is required.
4. Run the focused commands documented in [`../TESTING.md`](../operations/testing.md) and
   update [`../terminal-setup.md`](../operations/terminal-setup.md) or
   [`../themes-keybindings.md`](../interfaces/themes-keybindings.md) for user-visible
   controls/capabilities.

### Change command safety or containment

1. Classify and seal in `command/`; compute approval policy in `permissions/`;
   prepare/apply kernel restrictions in `containment/`; execute/capture/clean up
   in `tools/`.
2. Do not collapse these stages or derive authority from display text.
3. Add adversarial identity/path/environment/process cleanup tests, including
   unavailable-platform behavior.
4. Reconcile [`../security-sandboxing.md`](../security/sandboxing.md) and
   [`../security/containment.md`](../security/containment.md). Do not broaden
   containment claims beyond tested behavior.

### Add a plugin or MCP capability

1. Keep native protocol/lifecycle code in `plugin/` or `mcp/` and expose a
   validated broker descriptor upward.
2. Register only through the agent's composed registry; preserve collisions,
   permission, event, audit, timeout, cancellation, and output-bound rules.
3. Add fake child-process tests and deterministic golden fixtures for a stable
   contract change.
4. Update [`../plugin-system.md`](../extensions/plugin-system.md),
   [`../mcp.md`](../extensions/mcp.md), and
   [`../plugin-compatibility-policy.md`](../plugin-compatibility-policy.md) as
   applicable.

## Focused validation workflow

Configure once with the canonical preset, after exporting a writable `GITACHE_ROOT`, then use the repository wrappers so build and tests do not race in one tree. The debug-enabled preset also requires Python 3 and JSON-capable Universal Ctags when libcwd print-member generation is active:

```sh
export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
mkdir -p "$GITACHE_ROOT"
cmake --preset dev
scripts/build.sh --build-dir build --target ava_tests
scripts/run-tests.sh --build-dir build -R '^ava_tests\.<suite>$' --output-on-failure
```

For direct diagnosis after the target is built, use `./build/tests/ava_tests <suite>`. Direct CMake configuration is noncanonical unless it supplies the cache-equivalent `BetaTest`, debug, test, and compile-command values documented in [contributing](contributing.md).

For whole-process changes, build `ava` and the necessary fake support target,
then run the narrow CTest expression named in [`../TESTING.md`](../operations/testing.md).
Broaden to the default suite when a shared boundary changes. Finish every patch
with:

```sh
git --no-pager diff --check
```
