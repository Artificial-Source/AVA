# AVA Agent Guide

AVA is a native C++23 terminal coding agent. Treat the codebase as a small systems program: keep interfaces narrow, preserve backend safety boundaries, and make local behavior easy to verify with CMake tests.

## Current Scope

- AVA agent work in this repo includes backend, terminal frontend, and TUI runtime work unless the user explicitly narrows the task.
- TUI work is in scope now. Implement real user-facing terminal behavior when the goal calls for it, while preserving backend safety boundaries for permissions, sessions, providers, tools, and process execution.
- TUI changes must stay testable. Prefer renderer/editor/event-state seams that can be exercised with normal CTest tests, then add terminal-backed smoke coverage for behavior that only exists in a real TTY.

## Source Map

- `src/main.cpp`: thin process entry point that initializes the application and delegates to `ava::app::run`; CLI and frontend orchestration live under `src/ava/app/`.
- `src/ava/core/`: shared primitives such as `Result<T>`, errors, JSON helpers, descriptor anchors, IDs, and shared Build/Plan mode.
- `src/ava/config/`: XDG paths, auth storage, model configuration, prompt configuration, and OpenAI OAuth support.
- `src/ava/http/`: neutral HTTP transport, curl, and retry contract.
- `src/ava/event/`: typed RuntimeEvent payload, envelope, and emission ownership.
- `src/ava/provider/`: provider protocol and request/response/stream adapters for OpenAI, Anthropic, Gemini, and OpenAI-compatible services.
- `src/ava/agent/`: agent loop, thin tool dispatch/registration/family adapters, user-question plumbing, configurable task subagents, and process-local background job registry.
- `src/ava/command/`: canonical command planning, classification, policy, environment, and execution metadata.
- `src/ava/containment/`: Linux Landlock/seccomp command-containment planning and enforcement helpers.
- `src/ava/app/`: application entry after `main`, runtime orchestration, CLI/TUI/print/RPC/ACP glue (including `app/acp/`), OpenAI connect flow, command dispatch, project trust, headless policy, and event adapters.
- `src/ava/permissions/`: backend permission policy, persistent rules, prompts, and decisions.
- `src/ava/tools/`: built-in file, search, shell, web, and LSP tools. Keep filesystem and process safety checks here or in clearly permissioned call paths. User-interaction tools such as `question` are registered and dispatched under `src/ava/agent/`.
- `src/ava/session/`: append-only JSONL session storage, leases/authority, compaction, validation, and session lifecycle helpers.
- `src/ava/context/`: project/global instruction and skill loading for provider context.
- `src/ava/mcp/`: stdio MCP config, protocol, client lifecycle, tool/resource/prompt broker, and containment helpers.
- `src/ava/plugin/`: local out-of-process plugin manifest, discovery, enablement, inspected install/remove filesystem lifecycle, runner, diagnostics, tool broker, and event hooks.
- `src/ava/lsp/`: LSP client/process lifecycle and configured provider integration for diagnostics, symbols, definitions, and references.
- `src/ava/diagnostics/`: sanitized runtime diagnostics, records, and bounded diagnostic artifacts.
- `src/ava/observability/`: run observers, trace accounting, and deterministic trace validation/scoring.
- `src/ava/debug/`: optional libcwd-backed debug channels and generated print-member support.
- `src/ava/tui/`: custom terminal UI rendering, input handling, runtime glue, and terminal abstraction. Its live subagent workspace consumes only path-free coordinator snapshots and inspector frames; backend session/source authority remains outside TUI code.
- `src/ava/desktop/`: optional Qt/QML desktop prototype.
- `tests/`: focused `ava_tests` sources and support fakes, plus CMake/Python CLI, RPC, ACP, package/release, PTY, and TUI integration tests. The split tmux harness is `tests/tui_tmux_smoke.py` plus `tests/tui_tmux_scenarios/`.

## Internal Ownership Boundaries

- `AgentLoopOptions` uses focused model-invocation, tool-resource, and tool-execution bundles; credentials stay separate, and `ToolContext` remains execution/safety authority.
- `runtime_prompt` builds `ExtensionResourcePolicy` from the session, then applies `RunOptions` isolation flags while composing the run: ambient plugin/LSP/subagent resources, global/project MCP discovery, and global/plugin-declared skills stay disabled; explicit session MCP can remain unless `disable_session_mcp` is set.
- Runtime prompt ownership splits across `runtime_prompt_state`, `runtime_prompt_file_references`, `runtime_run_outcomes`, and `runtime_prompt` orchestration.
- `/trust` and `/reload` orchestration owners are `command_trust` and `command_reload`; persistence remains `project_trust`.
- The module dependency guard has zero backend exceptions; do not introduce a new module cycle.

## Local Workflow

Use the canonical `dev` preset (`BetaTest` with `EnableDebug=ON`) for normal local development. It keeps Release-style optimization and assertions while compiling AVA's `CWDEBUG`/libcwd instrumentation and debug-dependent tests. Keep runtime debug output off unless a diagnosis specifically needs it. Before configuring any non-Release tree, export a writable `GITACHE_ROOT`; agent-run builds use the AVA-specific `$HOME/.cache/ava/gitache` when it is otherwise unset. Complete debug-enabled configuration also needs Python 3 and JSON-capable Universal Ctags.

```sh
export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
mkdir -p "$GITACHE_ROOT"
cmake -S . -B build -DAVA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=BetaTest -DEnableDebug=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
scripts/build.sh --build-dir build
scripts/run-tests.sh --build-dir build
```

Canonical preset:

```sh
cmake --preset dev
scripts/build.sh
scripts/run-tests.sh
```

When deeper diagnosis requires reliable debugger stepping or full debug output, use a separate Debug tree rather than weakening the normal BetaTest tree:

```sh
cmake -S . -B build-debug -DAVA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug -DEnableDebug=ON
scripts/build.sh --build-dir build-debug
scripts/run-tests.sh --build-dir build-debug
```

Return to `build/` for ordinary work. Use a Release build only as an additional pre-push compilation or packaging check; Release ignores `EnableDebug=ON` and therefore cannot replace the normal debug-enabled test build.

Sanitizers:

```sh
cmake -S . -B build-sanitize -DAVA_ENABLE_SANITIZERS=ON -DAVA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=BetaTest -DEnableDebug=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
scripts/build.sh --build-dir build-sanitize --jobs 2
scripts/run-tests.sh --build-dir build-sanitize --jobs 2
```

The build and test runners use the available logical cores, share a lock that rejects concurrent work in one build tree, and accept `--jobs N`; the test runner also forwards ordinary CTest filters such as `-R`. Sanitizer examples cap both phases at two jobs to limit memory pressure.

Before handing work off, also run:

```sh
git --no-pager diff --check
```

## Engineering Rules

- Follow `docs/development/cpp-safety-rules.md` for C++ work.
- Use C++23 and CMake.
- Prefer small modules, narrow interfaces, and explicit ownership.
- No raw owning pointers, manual `new`, or manual `delete` in application code.
- Use RAII and explicit `Result<T>`/`VoidResult` errors for fallible core APIs.
- Keep filesystem writes behind the approved file/session/config layers.
- Keep command execution behind the permissioned process/tool layer.
- Keep destructive operations behind explicit policy checks.
- Treat model output, terminal input, paths, JSON, session files, auth files, and shell text as untrusted.
- Preserve actionable error context: operation, path/provider/tool name, and underlying cause.
- Every `.cpp` beneath `src/ava/` must use `#include "sys.h"` as its first include.
- Header-defined classes and structs beneath `src/ava/` must end their final public section with an accepted marker: `AVA_DEBUG_PRINT_MEMBERS_ON`, `AVA_DEBUG_PRINT_MEMBERS_ON_BASE(base)`, `AVA_DEBUG_PURE_VIRTUAL_PRINT_MEMBERS`, or deliberate `AVA_DEBUG_PRINT_MEMBERS_OPT_OUT` (none take a trailing semicolon).
- Include `ava/debug/print_members_on.h` in headers that use those markers. Include `debug.h` only when cwds debug APIs such as `Dout`, `Debug`, `DoutEntering`, or `ASSERT` are required.
- Use `ASSERT` only for programmer errors and internal invariants, never for recoverable conditions, untrusted input, or runtime failures; those failures belong on explicit `Result<T>`/`VoidResult` error paths.
- Every `ASSERT` that checks for an API contract violation must have an immediately preceding, actionable comment explaining what the developer did wrong and how to fix it. Write one such comment per `ASSERT`; do not move assertion-specific recovery guidance into public headers merely to avoid repetition.
- `ASSERT`s that check internal invariants should be used sparingly and must not substitute for reasoning about or testing the code. They may be useful in complex code when an invariant violation would otherwise be difficult to detect. Precede each such `ASSERT` with a comment describing the invariant and why it must hold. The comment may begin with `// Paranoia check:` to emphasize that the assertion is believed to be impossible to trigger in correct code.
- Keep `ASSERT` predicates side-effect-free: release builds may omit them, and correctness must never depend on assertion evaluation.
- Ask before adding a new production dependency; pin it exactly and review its license and provenance (see `THIRD_PARTY_NOTICES.md`).
- Keep ordinary test execution credential-free and free of live-provider calls; networked provider checks stay opt-in. A fresh CMake configuration may fetch pinned dependencies as documented in `docs/development/contributing.md`.
- Match completion checks to the change's scope: focused CTest filters for the touched subsystem, clang-format on changed C/C++ files, and only the repository gates from `docs/development/contributing.md` that the change can affect.
- Debug output must never stream prompt/message bodies, credentials, or evolving authority aggregates; log only bounded non-content metadata and opt sensitive aggregates out of generated printing.
- Types in anonymous namespaces must use `AVA_DEBUG_PRINT_MEMBERS_OPT_OUT`; generated print-member definitions cannot support their internal linkage.
- If generated `print_members.cpp` compilation or `*::print_members` linking fails, first build the `generate-print-members` target.

## Change Guidelines

- Prefer the smallest correct change over broad rewrites.
- Keep `main.cpp` from growing further when a change has a clear subsystem home.
- Keep TUI code as presentation/runtime glue; backend modules own permissions, sessions, provider messages, and tool semantics.
- TUI/frontend plans are allowed when they are tied to implementation and verification. Do not stop at UI plans when the user asked for working behavior.
- Keep public headers focused on APIs needed across modules. Move test-only or implementation-only helpers out of production interfaces when practical.
- Add regression tests for safety-sensitive fixes, permission behavior, session persistence, provider parsing, and tool execution.
- Add regression tests for TUI/editor/rendering changes. Terminal-visible behavior should have either deterministic renderer tests, scripted terminal smoke tests, or a documented manual smoke with captured evidence when automation is not yet reliable.
- Format changed C++ with the repo `.clang-format` and keep `.clang-tidy` warnings actionable.

## TUI And Terminal Testing

- The opt-in tmux suite is 23 isolated `ava_tui.tmux_smoke_*` scenarios dispatched by `tests/tui_tmux_smoke.py` into `tests/tui_tmux_scenarios/`. Run the complete wave with `AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 23 -R '^ava_tui\.tmux_smoke_'`.
- Start TUI verification at the smallest deterministic layer: text wrapping, width calculation, editor state, keybinding dispatch, palette/filter state, event reducers, permission/tool-card formatting, and transcript rendering should be covered by CTest unit tests where possible.
- For full terminal behavior, use a pseudo-terminal harness rather than plain pipes. A PTY smoke can set `TERM`, rows, columns, and environment variables, start `ava`, send keystrokes or escape sequences, resize the terminal, and assert on captured screen state and process exit.
- For ncurses-backed behavior, keep `newterm`/RAII lifecycle tests and add real terminal smokes only for behavior that requires a controlling terminal: alternate-screen cleanup, bracketed paste, resize redraw, mouse events, Escape latency, cursor visibility, Unicode cell placement, and terminal-state restoration after cancellation or crash.
- Prefer stable screen assertions over raw escape-sequence snapshots. Capture the terminal through a parser, tmux pane, or equivalent screen model, normalize timing-sensitive output, and assert visible text, cursor position, dimensions, and absence of leaked control sequences.
- Use visual artifacts when they add evidence: tmux captures, asciinema casts, or VHS-style scripted recordings are useful for reviewing complex flows, but they should supplement focused automated checks rather than replace them.
- Every TUI/frontend task should state what was tested for real before handoff. If a terminal smoke cannot run in the current environment, document the blocker and provide the exact command or script that should be run next.

## Reference Code

- Reference repositories are expected under `docs/reference-code/` and can contain their own `.git` directories.
- For Pi parity or comparison work, check `docs/reference-code/` first for the Pi reference repository; do not assume Pi is the only reference repo there.
- Use reference code only for product and behavior comparison. Do not copy architecture or source code into AVA.
- Do not include reference repositories in builds, tests, formatting, or source searches unless the task explicitly asks for reference analysis.
