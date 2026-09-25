# AVA Testing

Supported runtime, smoke, dogfood, and provider-matrix environment controls are cataloged in [environment-variables.md](../core/environment-variables.md). For failure diagnosis before collecting evidence, use [troubleshooting.md](troubleshooting.md).

## Normal Test Run

CMake 3.27+, Python 3, a writable `GITACHE_ROOT`, and JSON-capable Universal Ctags for enabled libcwd/debug print-member generation are required for the complete canonical test registration. Use the preset as the canonical path:

```sh
export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
mkdir -p "$GITACHE_ROOT"
cmake --preset dev
scripts/build.sh
scripts/run-tests.sh
```

A direct configure is noncanonical unless it is cache-equivalent: `CMAKE_BUILD_TYPE=BetaTest`, `EnableDebug=ON`, `EnableAvaBuildTests=ON`, and compile commands enabled. The exact fallback is documented in [contributing](https://github.com/Artificial-Source/AVA/blob/develop/docs/development/contributing.md).

The build and test runners default to the `build` tree, detect the available logical cores, and supply that positive job count to CMake/CTest. Use `--jobs N`, `CMAKE_BUILD_PARALLEL_LEVEL=N`, or `CTEST_PARALLEL_LEVEL=N` to cap concurrency; append build options such as `--target` or CTest options such as `-R`. They share a build-tree safety lock so builds, fixed integration-test roots, and CTest logs cannot collide. An interrupted or untrappably terminated wrapper leaves `.ava-build-tree.lock.d` fail-closed because detached descendants cannot be ruled out; verify that no build/test worker remains before removing that directory manually.

Do not use `cmake --build build --target test --parallel N` to request parallel test execution: it only parallelizes the build tool around one CTest command and does not propagate `N` to CTest. Use `scripts/run-tests.sh --build-dir build --jobs N` for the locked runner, or `ctest --test-dir build --parallel N` when directly diagnosing CTest behavior without the script's build-tree safety lock.

Every registered CTest carries a finite outer timeout: tests that set no explicit `TIMEOUT` receive a 120-second default at configure time, and explicit narrower or longer limits are preserved unchanged. These are only CTest kill-timers; in-test deadlines stay as-authored. For debugger sessions, setting `AVA_DEBUG_NO_TIMEOUT` in the *configure* environment (for example `AVA_DEBUG_NO_TIMEOUT=1` on the configure command) raises every per-test CTest timeout to one hour, or to `AVA_DEBUG_NO_TIMEOUT_SECONDS` when that is a positive integer; rerun configure without it to restore normal timeouts.

The test suite is built as `build/tests/ava_tests` and registered as focused `ava_tests.<suite>` CTests from sources under `tests/`. The LSP and MCP tests also build and use fake servers from `tests/support/`.

The 2026-08-23 tested source-build matrix is Ubuntu 24.04.4 x64 with GCC 13.3: BetaTest/Unix Makefiles and Release/Ninja both passed full CTest. Clang 18 was environment-blocked, not qualified; MSVC, Windows, macOS, and AArch64 have no accepted native candidate evidence. Multi-config is not release-qualified. The audited x64 artifact requires BMI2, `GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.13`, `libncursesw.so.6`, `libtinfo.so.6`, and `curl`, but exact retained bytes still need minimum-host smoke. This matrix does not qualify a dirty working tree or any future artifact; see the [release ledger](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/release-readiness.md).

Plugin/MCP contract changes should also follow [`docs/plugin-compatibility-policy.md`](../plugin-compatibility-policy.md). Keep checked-in golden fixtures small and deterministic under `tests/golden/ava-080/`, and prefer existing `ava_tests` plugin/MCP suites for contract assertions.

## Per-Suite libcwd Debug Logs

In libcwd-enabled builds, set `AVA_DEBUG_OUTPUT_DIR` to an absolute path to capture `ava_tests` libcwd output without contaminating normal stdout or stderr. The final directory is created when absent and otherwise validated as a current-user, non-symlink directory with exact mode 0700. Logs are private mode-0600 files named `ava_tests.<suite>.libcwd.log` (`all` for no argument and `invalid` for invalid arguments); each invocation truncates its deterministic file, while distinct CTest suites can write in parallel without colliding. The setting is ignored by libcwd-disabled builds.

The caller's `LIBCWD_RCFILE_NAME` and `LIBCWD_RCFILE_OVERRIDE_NAME` remain authoritative for channel selection. CTest sets `LIBCWD_NO_STARTUP_MSGS=1` for pre-main silence and `AVA_NO_DEBUG_OUTPUT=1` for later output. A nonempty inherited `AVA_DEBUG_OUTPUT_DIR` explicitly overrides the latter only after `ava_tests` installs its validated private stream. Outside that private test stream, libcwd output stays off by default even in libcwd-enabled builds: `AVA_DEBUG_OUTPUT=1` is the explicit operator opt-in for developer diagnostics, and `AVA_NO_DEBUG_OUTPUT` suppresses it again when both are set. The equivalent direct invocation is:

```sh
LIBCWD_NO_STARTUP_MSGS=1 AVA_NO_DEBUG_OUTPUT=1 \
  AVA_DEBUG_OUTPUT_DIR=/absolute/private/debug-logs ./build/tests/ava_tests session
```

Treat these logs as private diagnostic artifacts because enabled channels may contain process or test details.

## Per-Test Python Harness Timing Traces

Set `AVA_TEST_TIMING_DIR` to an absolute directory to diagnose time spent inside the Python tmux smoke harness without writing timing output to AVA, tmux panes, stdout, or stderr. Unset or empty disables tracing, and a relative path is rejected. The harness creates an absent directory with mode 0700, rejects a final symlink, and opens each trace as a private mode-0600 file. Existing directory permissions are not changed.

Each enabled test invocation writes a separate JSONL file named from CTest's sanitized `AVA_TEST_NAME`, the harness PID, and a random run ID, for example `ava_tui.tmux_smoke_active_run.12345.<run-id>.timing.jsonl`. The PID and run ID prevent parallel or repeated invocations from overwriting one another. If the harness is run outside CTest, its scenario-derived test name is used instead. Records use monotonic elapsed times and include begin/end events, duration, outcome, and polling count for scenario setup and cleanup, tmux state waits, fake-provider request waits, and bounded negative-observation windows. Every record is flushed immediately, so an interrupted run retains its last started or completed phase. Exception messages, terminal captures, regular expressions, provider requests, and filesystem paths are not recorded.

For one scenario:

```sh
mkdir -p build/test-timings
chmod 700 build/test-timings
AVA_TUI_TMUX_SMOKE=1 AVA_TEST_TIMING_DIR="$PWD/build/test-timings" \
  scripts/run-tests.sh --build-dir build -R '^ava_tui\.tmux_smoke_active_run$' --output-on-failure
```

Use a distinct or empty timing directory when comparing runs, or group records by their `run_id`. The trace measures harness-visible phases; it does not enable libcwd output and has no chronological merge contract with files under `AVA_DEBUG_OUTPUT_DIR`. Instrument AVA itself only when these harness records expose unexplained time between an input or provider event and the corresponding observable state.

## Deterministic Fake-Provider Tests

The shared test-only owner in [`tests/fake_provider.py`](https://github.com/Artificial-Source/AVA/blob/develop/tests/fake_provider.py) launches the fake provider with its process-gate descriptor and owns startup and cleanup. Python harnesses import that owner; Node and shell harnesses use its broker, with [`fake_provider_shell.sh`](https://github.com/Artificial-Source/AVA/blob/develop/tests/fake_provider_shell.sh) providing the POSIX shell client. The focused lifecycle test is `ava_tests.fake_provider_owner`. Successful scripted scenarios must use bounded `finish`/`fake_provider_finish` so natural exit 0 is verified; `stop` and broker EOF are reserved for cancellation and error cleanup where a response write may legitimately fail.

For cancellation or active-run assertions, use a gate-delayed scenario, wait for the exact zero-based request with `wait_for_request`, observe the competing operation, and only then release the response or stop the fixture. A numeric `delay_ms` controls streaming cadence, not ordinary response latency. Read request logs after a request gate when checking payloads; retain bounded negative-observation windows and waits for actual RPC or terminal state. A provider gate does not prove that a TUI redraw has completed.

## Release Package and Provenance Checks

Focused release provenance/package tests are offline and deterministic:

```sh
scripts/run-tests.sh -R '^ava_release\.(provenance|install_component|package_linux)$'
scripts/package-linux.sh --require-release-qualified --output-dir /absolute/path/outside/AVA
```

The strict package command creates a fresh private Release build tree with Gitache/libcwd disabled, the pinned in-tree nlohmann JSON source selected, and CMake FetchContent fully disconnected. It rejects supplied binaries, dirty/mismatched source dependencies, missing or unsupported host/binary architecture evidence, disagreement between the canonical packaging-host architecture and detected ELF architecture, unexpected ELF dynamic dependencies, and license-policy failures. The flag `--require-release-qualified` and resulting `release_qualified:true` prove only these implemented static source/gitlink/license/version/native-architecture/dynamic-dependency/package gates. They do **not** prove full CTest, native CI, sanitizer/TSan, terminal gates, retained exact bytes, or publication. Complete candidate qualification requires the [release-readiness ledger](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/release-readiness.md) and [publication runbook](https://github.com/Artificial-Source/AVA/blob/develop/docs/operations/publication.md). Cross-compiled output is not evidence. Temporary, work, and publication directories are classified against the checkout by descriptor ancestry and device/inode identity, so invoking the script through a symlink cannot make a physically in-checkout directory appear external; logical source paths remain unchanged. Publication rechecks the already-opened output descriptor's ancestry before mutation and at namespace revalidation points. Static symlink, directory-identity, ownership, mode, and no-replace guarantees remain mandatory. A malicious process running concurrently under the same effective UID is outside this packaging threat model because it can directly alter the checkout and release inputs; this exclusion does not weaken those static guarantees. Ordinary `--binary` package workflows remain available but their packaged `PROVENANCE.json` is explicitly unqualified; no signing, SBOM, or attestation is implied.

Privacy-safe diagnostics have focused unit and full-binary coverage:

```sh
scripts/run-tests.sh --build-dir build -R '^ava_tests\.(diagnostics|runtime_diagnostics|mcp|plugin)$' --output-on-failure
scripts/run-tests.sh --build-dir build -R '^ava_cli\.(doctor_support|runtime_diagnostics)$' --output-on-failure
```

These suites use canary secrets and isolated XDG roots to verify passive doctor behavior, private descriptor-safe storage, bounded identity-aliased traces, drop/failure counters, typed last-failure records, unique support publication, MCP/plugin public-failure sanitization, and absence of raw values from model/session/RPC/export/support surfaces. ACP trace grammar is covered by `ava_cli.runtime_diagnostics` and the ordinary ACP tests. No live provider or network access is required.

## Headless Tool Smoke

After provider streaming, tool schema, permission, or dispatcher changes, run a live headless smoke with configured provider auth when credentials are available. Keep the workspace isolated under a temporary directory outside the repository so mutating tools do not touch the checkout.

Recommended coverage:

- `ava --print ... --json --allow read-only` for `read_file`, `list_directory`, `glob`, and `grep`. This verifies provider tool-call streaming, read/search permission auto-allow, `.gitignore` behavior, and tool progress events.
- `ava --print ... --json --allow-tool skill` for `skill` when a test skill is available. This verifies the explicit `skill` headless allow path and bounded skill loading behavior.
- `ava --print ... --json` for `task` when a small subagent prompt is available. This verifies prompt-free audited task launch (exact persisted denies still win), child-session creation, recursive-task hiding, foreground nested permission UI for sensitive child actions, fail-closed background nested Ask behavior, and background job metadata when `background:true` is used. `--allow-tool task` remains accepted for compatibility but is not needed.
- `ava --print ... --json --allow-tool webfetch` for `webfetch`. This verifies the explicit `network.fetch` headless allow path and real bounded HTTP fetch behavior.
- `ava --print ... --json --allow-tool websearch` for `websearch`. This verifies the explicit `network.search` headless allow path and bounded search result shaping.
- `ava --print ... --json` without `--allow-tool webfetch` for a prompt that asks for `webfetch`. This verifies `network.fetch` ask prompts fail closed by the default headless resolver; workspace-policy allow decisions may still proceed without prompting.
- `ava --rpc` with a small JSONL harness that answers `permission_requested` with `permission_reply` for `write_file`, `edit_file`, `apply_patch`, and `bash`. The checked-in headless bash cleanup smoke verifies that timed-out shell process groups do not leave a child process behind.
- `scripts/run-tests.sh -R '^ava_cli\.headless_e2e_model_smoke$'` for the full-binary fake-provider coding-agent smoke. This is the default release gate for provider request/response handling, sequential tool dispatch, RPC permission replies, session persistence, provider continuation requests with tool results, replay through `--continue`, and clean exit in one run.
- `ava --rpc` with `question_reply` for the `question` tool.
- `ava --rpc` command-registry smokes for `list_commands` and `invoke_command` across prompt commands, skills, plugin commands, and MCP prompt commands.
- `ava --rpc` plugin/MCP diagnostics smokes for plugin discovery/validation/static resources/enablement/fail-closed execution, MCP server list/inspect/restart, invalid MCP config containment, and fail-closed MCP tool discovery without a TUI resolver. `ava_cli.headless_rpc_sample_plugin` copies `examples/plugins/todo/` into an isolated project plugin directory and verifies the real sample's discovery, resources, enable/disable flow, and fail-closed command execution without starting the sample process.
- `./build/tests/ava_tests plugin` after plugin authoring changes. The plugin suite validates the checked-in sample under `examples/plugins/todo/` through source-path fixture plumbing instead of duplicating the sample manifest or protocol JSON, and remains the coverage for successful sample entrypoint execution.
- `./build/tests/ava_tests mcp` after MCP contract changes. The MCP suite uses the local fake MCP server and golden fixtures for representative tool schema, resource read, and audit shapes; MCP resource behavior must stay behind explicit read-style permission coverage.

LSP model tools are capability-gated in normal headless runtime; see [lsp.md](../extensions/lsp.md) for their current contract. `ava_tests.lsp` uses the stable fake server to cover default-off/global-only exact `clangd` opt-in, rejection of unsupported built-in server ids, executable hardlink/replacement rejection, replacement-sensitive launch permission identity, logical per-root cache deduplication, pull and routed publish diagnostics, full-text versioned `didChange`, cache bounds, malformed/out-of-workspace notifications, absolute deadlines, cancellation, environment filtering, and cleanup without downloads or provider calls. `ava_tests.lsp_real_clangd_smoke` is an explicitly opt-in offline real-server smoke. By default it returns CTest skip code 77 even when `clangd` is installed. When opted in, it discovers an already-installed safe `clangd`, uses no credentials or network access, runs against a private finite fixture, proves initialization plus definition, and cleans up; it also skips when `clangd` is absent or unsafe. It never installs or downloads clangd.

```sh
scripts/run-tests.sh --build-dir build -R '^ava_tests\.lsp$'
AVA_LSP_REAL_CLANGD_SMOKE=1 scripts/run-tests.sh --build-dir build -R '^ava_tests\.lsp_real_clangd_smoke$'
```

## End-To-End AVA Tool Smoke

The deterministic full-tool smoke is part of default CTest:

```sh
scripts/run-tests.sh -R '^ava_cli\.headless_e2e_model_smoke$'
```

The smoke starts the built `ava --rpc` binary against `ava_fake_provider_server`, seeds a temporary workspace under the build tree, and drives `read_file`, `grep`, `list_directory`, `apply_patch`, and `bash` in one provider-backed turn. It uses `--allow read-only` only for read/search prompts; the outside-temp edit target and verification command must be resolved through RPC `permission_reply`. Assertions cover tool lifecycle events, successful tool results, permission events and persisted `permission_decision` session entries, `get_session_stats`, `validate_session`, `get_messages`, provider request-log continuation with prior tool results, and replay through `ava --rpc --continue`. Subagent/task, primary-agent configuration, and background job coverage lives in `ava_tests.agent_loop`, `ava_tests.app_runtime`, and `ava_cli.headless_print_positional_prompt`; run those checks after changing `--agent`, subagent config, `task`, or `BackgroundJobRegistry` behavior.

Opt-in full-binary live dogfood is intentionally separate from the default release gate:

```sh
AVA_LIVE_PROVIDER_SMOKE=1 sh scripts/live-model-dogfood.sh
```

Set `AVA_EXE=/path/to/ava` when the binary is not `./build/ava`. `AVA_LIVE_DOGFOOD_ROOT` names an absolute existing private **parent**, not a disposable workspace: it must be current-euid-owned, non-symlink, and exact mode 0700, and it cannot be `/`, `HOME`, the checkout, or a checkout descendant. The launcher creates an unpredictable mode-0700 child beneath it and never removes the parent or any pre-existing path. Set `AVA_LIVE_DOGFOOD_KEEP=1` to report and retain that child for review; otherwise cleanup removes only the invocation-created child.

The live dogfood script uses the first configured provider credential from the provider live-smoke matrix, writes an isolated `models.json`, starts `ava --rpc --allow read-only`, asks the live model to read `src/live-smoke.txt` with `read_file`, then validates session stats/messages. It prints one classification: `passed`, `skipped/not opted in`, `skipped/no credential`, `credential/auth-blocked`, `provider/rate-limited`, `network-blocked`, `provider-behavior/inconclusive`, or `AVA regression`. Treat provider-behavior/inconclusive as live dogfood evidence that the deterministic fake-provider E2E remains the release gate, not as a release blocker by itself.

Live coding dogfood exercises a broader coding-agent loop with a real provider:

```sh
AVA_LIVE_PROVIDER_SMOKE=1 sh scripts/live-coding-dogfood.sh
```

Set `AVA_EXE=/path/to/ava` when the binary is not `./build/ava`. `AVA_LIVE_CODING_DOGFOOD_ROOT` has the same validated private-parent semantics as `AVA_LIVE_DOGFOOD_ROOT`; the script allocates its own unpredictable mode-0700 evidence child beneath that parent. Set `AVA_LIVE_CODING_DOGFOOD_KEEP=1` to report and retain the child containing RPC output, stderr, workspace, and session files; otherwise cleanup removes only that child.

The live coding dogfood script creates an isolated workspace, trusts a project-local `coding-smoke` skill, asks the live model to load that skill, read a target file, apply an exact edit through `apply_patch` or `edit_file`, and reply with the marker. It validates permission prompts/replies, successful tool results, the mutated file, session JSONL `tool_call`/`tool_result`/`permission_decision` entries, and `validate_session` output. It uses the same classification vocabulary as `live-model-dogfood.sh`; classify model non-compliance separately from AVA-owned regressions.

## Sanitizers

The `sanitize` preset is the canonical ASan/UBSan configuration: it inherits the `BetaTest`, debug, tests, and compile-command settings from `dev`, enables sanitizer instrumentation, and makes UndefinedBehaviorSanitizer non-recovering.

```sh
cmake --preset sanitize
scripts/build.sh --build-dir build-sanitize --jobs 2
scripts/run-tests.sh --build-dir build-sanitize --jobs 2
```

Do not call a build-type-omitting direct sanitizer configure equivalent. A cache-equivalent direct fallback is documented in [contributing](https://github.com/Artificial-Source/AVA/blob/develop/docs/development/contributing.md).

The explicit sanitizer cap avoids multiplying ASan/UBSan memory demand on smaller CI and developer hosts; raise it locally after measuring available memory.

The sanitizer preset enables AddressSanitizer and UndefinedBehaviorSanitizer for supported non-MSVC builds.

## CI Compiler Cache

CI build jobs reuse compiler output through the distro `ccache` package, which is a build-only dependency and never enters the packaged artifact. Only the ccache object store under the runner's temporary directory is cached; no build tree, binary, or secret is ever cached. The correctness boundaries are:

- Cache key prefixes encode OS, architecture, compiler, and configuration (Debug, Release, ASan/UBSan, TSan, SDK, acpx), so incompatible objects never share a cache. The immutable primary key includes the commit SHA; restore keys stay within the same prefix. Pull requests restore read-only; only successful trusted push and `workflow_dispatch` runs save.
- `compiler_check = content` and a 1 GiB size cap are pinned, and all other settings keep ccache's strict defaults: no `sloppiness`, directory hashing enabled, no `base_dir` path rewriting, and no hard links. The policy file is rewritten fresh on every run outside the cached object tree so a restored cache cannot relax it.
- Cacheable CI configure commands pass `-DCMAKE_CXX_SCAN_FOR_MODULES=OFF`: ccache 4.9.1 does not support caching compilations that carry C++ module dependency-scanning flags (`-fmodules-ts`, `-fmodule-mapper=`, `-fdeps-format=`), which GCC 16/Ninja generates otherwise, so those flags are an unsupported cache configuration for CI. [`scripts/guard-no-cxx-modules.sh`](https://github.com/Artificial-Source/AVA/blob/develop/scripts/guard-no-cxx-modules.sh) inspects the effective `ninja -t commands` output before every cached build and fails the job if any module flag survives — including one reintroduced by an explicit `CXX_MODULES` `FILE_SET`, which scans even with the variable `OFF`. Local and default configures are unchanged, and this codebase builds from traditional headers.
- Strict release packaging stays uncached: the package steps invoke `scripts/package-linux.sh` with `CCACHE_DISABLE=1`, so the fresh private configure keeps its default module scanning and qualification inputs. No packaging speedup is claimed.

Each cached build zeroes statistics with `ccache -z` immediately before compiling and prints `ccache -s` afterwards, even on failure, so cold/warm behavior is visible in the job log. Do not record before/after speed percentages without that measured evidence.

## Formatting And Static Checks

Format changed C++ files with the repository `.clang-format`:

```sh
clang-format -i <changed-cpp-or-header-files>
```

Run clang-tidy against changed implementation files after configuring the build:

```sh
clang-tidy <changed-cpp-files> -p build
```

Verify the assertion-comment rule over `src/ava/` directly and through its two
focused CTests (`ava_tests.assert_comments_checker` and
`ava_tests.assert_comments_source`):

```sh
python3 scripts/verify-assert-comments.py .
scripts/run-tests.sh --build-dir build \
  -R '^ava_tests\.assert_comments_(checker|source)$' \
  --output-on-failure
```

For Markdown changes, run both direct repository gates and their four focused
CTest cases:

```sh
python3 scripts/verify-markdown-links.py . --source-tree
python3 scripts/verify-documentation-structure.py .
scripts/run-tests.sh --build-dir build \
  -R '^ava_tests\.(markdown_(link_verifier|links_source)|documentation_structure_(checker|source))$' \
  --output-on-failure
```

When documentation paths or release-artifact payloads change, also run the
offline package, install, and provenance CTests in
[Release Package and Provenance Checks](#release-package-and-provenance-checks).

Before handing work off, check for whitespace and patch-format issues:

```sh
git --no-pager diff --check
```

## Coverage Areas

The historical MVP checklist is mapped to automated suites, CLI/RPC smokes, opt-in terminal smokes, live-provider smokes, or explicit manual/docs evidence in the [MVP coverage ledger](../product/mvp-coverage-ledger.md). Current product and release decisions come from [principles](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/principles.md) and the [release-readiness ledger](https://github.com/Artificial-Source/AVA/blob/develop/docs/product/release-readiness.md), not competitor parity.

### MVP capability release evidence

Pi's reference tree has broad Jest/Vitest coverage across provider protocols, coding-agent sessions, settings/packages, export, and a TypeScript virtual-terminal harness. AVA's MVP evidence uses CTest plus explicit opt-in smokes instead of copying Pi's harness architecture:

| Evidence area | AVA evidence | Release rule |
| --- | --- | --- |
| Provider protocol regressions | `ava_tests.provider_openai`, `ava_tests.provider_anthropic`, `ava_tests.provider_gemini`, `ava_tests.config_context_auth_oauth`, and `ava_tests.provider_live_smoke` | Local protocol tests must pass. Live provider cases are credential-gated and classified in the matrix below. |
| Headless/RPC automation | `ava_cli.headless_print_*`, `ava_cli.headless_rpc_*`, `ava_cli.headless_tool_visibility`, `ava_cli.headless_performance_smoke`, `ava_cli.headless_e2e_model_smoke` | Fake-provider smokes must pass in default CTest; live credentials are not required. |
| Primary agents, subagent/background jobs | `ava_tests.agent_loop`, `ava_tests.app_runtime`, `ava_cli.headless_print_positional_prompt`, `ava_tests.subagent_coordinator`, `ava_tests.subagent_delivery_manager`, and `BackgroundJobRegistry` tests; opt-in live task/coding dogfood when credentials are present | Foreground/background delegation, process locality, hidden-start publication, retention/hard caps, same-process delivery/deduplication, retry exhaustion, exact parent authority, promotion/cancellation races, and shutdown must remain covered by deterministic tests. Restart recovery is intentionally absent. |
| TUI/editor/renderer | `ava_tests.tui_composer`; 23 gated tmux scenarios: `ava_tui.tmux_smoke_suspend_resume`, `ava_tui.tmux_smoke_keybind_conflict`, `ava_tui.tmux_smoke_theme_env`, `ava_tui.tmux_smoke_theme_persisted`, `ava_tui.tmux_smoke_nested_settings_preview`, `ava_tui.tmux_smoke_startup_overview`, `ava_tui.tmux_smoke_mermaid`, `ava_tui.tmux_smoke_active_run`, `ava_tui.tmux_smoke_restore_followup`, `ava_tui.tmux_smoke_streaming_scroll`, `ava_tui.tmux_smoke_transcript_search`, `ava_tui.tmux_smoke_transcript_selection`, `ava_tui.tmux_smoke_subagent_workspace`, `ava_tui.tmux_smoke_branch_summary`, `ava_tui.tmux_smoke_main_startup_trust_keybinds`, `ava_tui.tmux_smoke_main_models_selectors`, `ava_tui.tmux_smoke_main_editor_input`, `ava_tui.tmux_smoke_main_slash_completions`, `ava_tui.tmux_smoke_main_permission_flow`, `ava_tui.tmux_smoke_main_question_flow`, `ava_tui.tmux_smoke_main_session_mgmt`, `ava_tui.tmux_smoke_main_paste_scrollback_attach`, and `ava_tui.tmux_smoke_plugin_ui`; four direct PTY CTests: `ava_tui.kitty_image_smoke`, `ava_tui.iterm2_image_smoke`, `ava_tui.terminal_lifecycle_smoke`, and `ava_tui.osc8_smoke` | Deterministic renderer/editor tests are required; PTY smokes are run when prerequisites exist and otherwise skip with code 77. |
| Virtual-terminal decision | AVA does not add a Pi-style TypeScript virtual terminal for MVP. Renderer tests assert visible rows/widths and tmux/PTY captures assert real terminal behavior. | Revisit a screen-model parser only if tmux/PTY smoke flakes or cannot cover a terminal protocol that renderer tests cannot prove. |
| Performance thresholds | `ava_tests.tui_composer` large-render budgets and `ava_cli.headless_performance_smoke` | Treat budget failures as release regressions unless the threshold is intentionally raised with profiling evidence. |
| Side-effect safety | [`docs/development/side-effect-safety-checklist.md`](../development/side-effect-safety-checklist.md) | New side-effect classes must answer the permission/audit/cancellation/output-bound/test questions before release. |
| Documentation consistency | `README.md`, `docs/core/usage.md`, `docs/core/configuration.md`, `docs/headless-protocol.md`, product docs, and area logs | A broad MVP cut must rerun `git --no-pager diff --check` and reconcile checked rows with the coverage ledger. |

### Provider Live-Smoke Matrix

Run live smokes only when credentials are intentionally present in the environment:

```sh
AVA_LIVE_PROVIDER_SMOKE=1 scripts/run-tests.sh -R provider_live_smoke
AVA_LIVE_PROVIDER_SMOKE=1 sh scripts/live-model-dogfood.sh
AVA_LIVE_PROVIDER_SMOKE=1 sh scripts/live-coding-dogfood.sh
```

`ava_tests.provider_live_smoke` verifies provider transport/connectivity. `scripts/live-model-dogfood.sh` is the opt-in full-binary dogfood path for CLI/RPC startup, agent loop, read-only tool use, permission policy, and session persistence with a live model. `scripts/live-coding-dogfood.sh` extends that coverage to skill loading, file mutation, edit permission prompts, and persisted coding-session evidence. Both dogfood scripts use the shared provider selector in `scripts/live-provider-selection.sh` so the first available OpenAI, Anthropic, DeepSeek, Gemini, Kimi, Moonshot, or OpenRouter credential is handled consistently. `scripts/live-provider-matrix.sh` gives each dogfood case a separate private parent beneath its own private run root, preserving the same child-ownership rules across concurrent matrix invocations.

| Provider | Credential env | Default model / override | Expected CTest behavior without credentials | Result classification |
| --- | --- | --- | --- | --- |
| OpenAI | `OPENAI_API_KEY` | `gpt-4.1-mini` / `AVA_LIVE_OPENAI_MODEL` | Skipped unless gate and key are set | Record `passed`, `skipped/no credential`, `credential/auth-blocked`, `provider/rate-limited`, `network-blocked`, or `AVA regression`. |
| Anthropic API key | `ANTHROPIC_API_KEY` | `claude-sonnet-4-5` / `AVA_LIVE_ANTHROPIC_MODEL` | Skipped unless gate and key are set | Same classification. |
| Anthropic OAuth bearer | `ANTHROPIC_OAUTH_TOKEN` or `ANTHROPIC_AUTH_TOKEN` | `claude-sonnet-4-5` / `AVA_LIVE_ANTHROPIC_MODEL` | Skipped unless gate and token are set | Same classification; interactive Anthropic OAuth remains deferred. |
| DeepSeek | `DEEPSEEK_API_KEY` | `deepseek-v4-flash` / `AVA_LIVE_DEEPSEEK_MODEL` | Skipped unless gate and key are set | Same classification; local tests cover `reasoning_effort=high|max`; live runs classify endpoint/auth/network results. |
| Gemini | `GEMINI_API_KEY` | `gemini-2.5-pro` / `AVA_LIVE_GEMINI_MODEL` | Skipped unless gate and key are set | Same classification; local tests cover native GenerateContent request/response and SSE parsing. |
| Kimi | `KIMI_API_KEY` | `kimi-k2-thinking` / `AVA_LIVE_KIMI_MODEL` | Skipped unless gate and key are set | Same classification. |
| Moonshot | `MOONSHOT_API_KEY` | `kimi-k2.6` / `AVA_LIVE_MOONSHOT_MODEL` | Skipped unless gate and key are set | Same classification. |
| OpenRouter | `OPENROUTER_API_KEY` | `moonshotai/kimi-k2.6` / `AVA_LIVE_OPENROUTER_MODEL` | Skipped unless gate and key are set | Same classification. |

Do not record secret values in release notes. A failed live case is classified as an AVA regression only after credentials, provider availability, model access, and local network reachability are ruled out.

### Performance Release Thresholds

| Path | Current deterministic threshold |
| --- | --- |
| Large TUI transcript redraw | `ava_tests.tui_composer` renders a mixed 900-item transcript at 120-column width within 5 seconds across four redraw passes and preserves requested dimensions/widths. |
| Large tool-output card | `ava_tests.tui_composer` renders collapsed/expanded previews for 20,000 output lines within 2 seconds while using backend total/omitted counts. |
| Very long TUI transcript | `ava_tests.tui_composer` renders a 900+ item transcript frame within 20 seconds and keeps every line width-bounded. |
| Bounded transcript tail parity | `ava_tests.tui_composer` verifies full-render, bounded reverse-tail, and message-start segmentation agree across varied tail budgets in roomy and compact layouts. |
| Completion navigation | `ava_tests.tui_composer` ranks a 2,000-item workspace source once per input/source generation, reuses source indices across repeated navigation/accept/render/hit testing, and formats only visible rows. |
| Transcript drag selection | `ava_tests.tui_composer` covers stripped rendered-row extraction, soft-wrap newlines, grapheme/wide-cell snapping, unowned spacers, headings, duplicate ownership, remap/clamp/clear behavior, 64 KiB+1 rejection, style-preserving reverse-off, `NO_COLOR`, SGR/legacy/ncurses button lifecycles, hover rejection, Shift bypass, and invalid-authority fail-closed behavior. The credential-free gated `transcript_selection` scenario sends real SGR press/drag/release through tmux, copies through `CopySelection`, verifies truthful status and retained highlight, and proves a dragged tool header is not toggled. |
| Detached and streaming transcript | `ava_tests.tui_composer` coalesces routine full/footer requests on a 16ms completion-anchored frame schedule, scrolls the transcript three rendered rows per accepted wheel event, coalesces same-direction wheel bursts at 40ms with immediate opposite-direction acceptance, leaves selectors/questions/drawer/selection-edge autoscroll on one-row steps, reuses a frozen detached layout across draft redraws, synchronizes it once for navigation/resize, preserves anchors across the 1,000-item eviction cap, and bounds append-only updates without cumulative pending-text preparse. The gated `streaming_scroll` scenario condition-polls large active/idle typed and reversing-wheel bursts against the credential-free fake provider. |
| Bounded thinking disclosure | `ava_tests.tui_composer` covers completed long/short thinking previews, live pending full render, legacy `label=thinking` parity, expand/collapse, duplicate independence, index-shift carry across `apply_capped_transcript_snapshot`, header hit-testing, detached anchors, NO_COLOR footers, and search hidden-tail miss/expanded hit. No fake-provider tmux scenario is claimed: the current fake provider does not emit reliable reasoning stream events, and validation must not use paid live providers. |
| Application catalogs | `ava_tests.app_runtime` serializes concurrent catalog refresh/snapshot delivery, avoids duplicate workspace/session enumeration, and consumes current/non-current asynchronous title invalidations exactly once. |
| Headless startup/search/replay | `ava_cli.headless_performance_smoke` has a 30 second RPC/search driver timeout and 15 second `--continue` replay timeout using the fake provider. |

The `ava_tests` binary covers:

- mode parsing
- session JSONL storage, resume, listing, corruption handling, and permissions
- XDG path handling
- OpenAI auth loading/storage and OAuth refresh preflight
- model and prompt configuration
- provider request/SSE parsing, including OpenAI Responses function-call starts from `response.output_item.added`, Anthropic native tools/thinking/cache usage, Gemini GenerateContent request/response and SSE vectors, DeepSeek/Kimi/Moonshot-compatible reasoning-content vectors, and OpenRouter-compatible request/error vectors
- permission audit persistence, file/search/bash/webfetch/LSP tools, bash process-group cleanup, spill files, and atomic file writes
- tool dispatcher and agent loop, including process-local subagent coordinator and automatic-delivery manager coverage
- command registry discovery/invocation for built-ins, prompt commands, skills, plugin commands, and MCP prompts
- plugin manifest/discovery/enablement, out-of-process plugin runner behavior, plugin tools/commands/static resources/events, diagnostics, containment, failure cases, and the checked-in sample plugin workflow
- MCP stdio config, initialize, tool listing/calls, prompt listing/get, tool broker registration, diagnostics, and fake-server success/error/exit cases
- print mode and JSONL RPC success, denial/recovery, malformed input, cancellation, refresh paths, and TTY-bound terminal-control sanitization
- TUI rendering, input, keybindings, palette, permission prompt, markdown, UTF-8, and scroll helpers

Add regression tests for every safety-sensitive bug fix.

## TUI / ncurses Focused Validation

For TUI-only changes, the focused suite is:

```sh
scripts/build.sh --target ava_tests
./build/tests/ava_tests tui_composer
scripts/run-tests.sh -R "ava_tests.tui_composer"
```

The suite includes the CI-safe ncurses baseline currently available in-tree: configured `ESCDELAY`, escape buffering/discard for CSI/OSC/DCS/bracketed-paste markers, mouse wheel/click mapping at the composer layer, resize stress renders, Unicode/CJK/combining/emoji width and cursor placement, `newterm` smokes for xterm/screen terminfo plus tmux/kitty/wezterm/ssh-like environment variables without a real TTY, and large/very-long transcript performance budgets. Current shell assertions additionally prove the adaptive automatic-rail policy: actionable running activity or modified files appears at `144x16`, idle session/context metadata appears at `176x16`, prompts/questions/selectors suppress the rail, and below the relevant threshold ordinary content uses the shared centered 120-column canvas on wider terminals. They retain the single divider and two-cell local inset, omission of branding/placeholders/raw drawer metadata/completed activity, known-zero Context behavior, exact-sentinel filtering that preserves legitimate values containing `unknown`, critical pressure, sanitization, `NO_COLOR`, Unicode cell bounds, cursor and hit testing, complete drawer scrolling/conflicts, subtle reasoning-cycle feedback when the rail is hidden, error-alert admission ahead of queue/attachment budgets without weakening permission-prompt precedence, and a shared dock-aware transcript scroll limit that reaches the oldest transcript with alert, queue, attachment-preview, and 12-line diff-prompt allocations present. F2 assertions add the shared user/assistant-flow/error/system segmentation policy (including System queue/delivery/audit receipts while the active queue dock remains primary), the 44-column roomy threshold versus narrow/short rhythm, no trailing transcript blank, one context-gathering heading, F1 ownership markers, Rich/Compact/Expanded tool presentation, exact adjacent tool-result suppression after raw trailing-whitespace trim without history mutation, renderer-only replacement of proven absolute workspace aliases with known relative changed paths, permission-audit omission from ordinary cards and `/copy tool`, Unicode/`NO_COLOR` bounds, full/tail/message-start parity including leading hidden entries over varied budgets, oldest-scroll reach, frozen detached-output behavior, and existing dock-aware limits.

Real terminal coverage exists as prerequisite-gated CTest smokes. The historical
pre-F1 F0 semantic inventory, four-row shell observations, artifact policy, and
future evidence gaps are recorded in `docs/roadmap/frontend-evidence-baseline.md`.
The current `main_startup_trust_keybinds` scenario generates the plain-text composer and responsive-sidebar captures below its evidence root. The startup matrix is `176x48` (`frontend-f1-roomy-idle-composer.txt`, input/footer rows 46/47), `160x48` (`frontend-f1-wide-idle-composer.txt`, rows 46/47), `120x36` (`frontend-f1-ordinary-idle-composer.txt`, rows 34/35), `80x24` (`frontend-f1-narrow-idle-composer.txt`, rows 22/23), `100x12` (`frontend-f1-short-idle-composer.txt`, rows 10/11), and `160x12` (`frontend-f1-short-wide-auto-sidebar-hidden.txt`, rows 10/11). Rows are zero-based. Only the `176x48` idle capture asserts the exact two-cell-inset `Session` rail, one divider, compact mode/provider/model metadata, unchanged footer, and absence of AVA/live branding, Activity/Modified placeholders, idle/no-change/unknown text, and raw session/path/workspace/version metadata. The `160x48` and `160x12` captures prove the exact 20-column inset around the centered 120-column transcript/composer canvas; widths through 120 remain zero-origin and full width.

Drawer evidence remains the initial and scrolled `80x24` captures `frontend-f1-narrow-sidebar-drawer.txt` and `frontend-f1-narrow-sidebar-drawer-scrolled.txt`, plus the End-scrolled `100x12` capture `frontend-f1-short-sidebar-drawer.txt`. These retain the same bottom input/footer rows and prove that complete session/path/workspace/context/version detail remains reachable with focus, scrolling, resize, and short-terminal behavior intact. Every saved frame is checked for dimensions, ESC absence, unexpected C0-byte absence, and final LF.

Running Activity remains deterministic-renderer evidence only. In the fake-provider active-run scenario, the provider request log is not exposed until the pending response can already have settled, so a condition-synchronized `Running` row is not reliable; no `frontend-f1-active-rail.txt` artifact is claimed. The existing `active_run` scenario still proves draft, four-cell signal meter, queue, scroll, fake-provider behavior, and the preserved running-shell cancellation hint, while the startup scenario supplies the targeted real-terminal red/green rail-curation proof.

F2 real-terminal evidence is produced by `main_permission_flow` after its deterministic `/write` settles. `frontend-f2-tool-shell-expanded-ordinary.txt` captures the Expanded ordinary `120x36` shell, changed path and diff, and exactly one preserved `wrote 27 bytes` assistant result. Rich is the current default: the card shows the human call and useful bounded output on separate rows while omitting routine `permission checked` text, audit IDs, and diagnostic commands. Expanded visible rows project a known absolute workspace path to `src/main.cpp` only when that relative alias is proven by the same tool item; explicit `/copy permission` and `/permissions` commands remain the audit-detail surfaces. The same scenario saves `frontend-f2-transcript-wide.txt` at `160x48`, `frontend-f2-transcript-ordinary.txt` at `120x36`, `frontend-f2-transcript-narrow.txt` at `80x24`, and `frontend-f2-transcript-short.txt` at `100x12`. Every frame asserts exact dimensions, terminal line bounds, quiet bottom composer/footer, no separate duplicate `result:` row, ESC/C0 hygiene, the centered 120-column canvas at `160x48`, and zero-origin full-width presentation through 120 columns; the short frame additionally asserts compact transcript rhythm. The driver uses screen predicates and resize redraw conditions rather than fixed sleeps, then restores `120x32` with synchronized dimensions and changed-screen evidence.

F3 composer-discovery evidence is produced without live providers by `main_slash_completions` and `active_run`: `frontend-f3-slash-ordinary-120x36.txt`, `frontend-f3-slash-centered-160x36.txt`, `frontend-f3-slash-narrow-80x24.txt`, `frontend-f3-slash-cancel-focus.txt`, `frontend-f3-active-empty-hint.txt`, and `frontend-f3-active-forced-path.txt` under their scenario evidence roots. The scenarios assert exact pane dimensions, bounded capture rows/columns, final-LF evidence writes, canonical insertion after human-facing display, active forced-Tab and slash/`@`/normal-path mouse completion, contextual-row ownership with the shared `│  ` gutter, cancel/focus-preserving resize reflow, an exact 20-column wide-canvas inset with a real SGR click sent to coordinates derived from the rendered candidate, automatic-rail click rejection at the `176x36` idle disclosure boundary, and terminal-control hygiene. They use only the fake provider and condition-based request accounting.

F4 selector evidence is produced by `main_models_selectors`, `main_session_mgmt`, `main_slash_completions`, `main_permission_flow`, and `main_startup_trust_keybinds`. Representative captures are `model-selector-arrow-scroll.txt`, `model-selector-quiet-100x12.txt`, `session-selector.txt`, `session-selector-named-default-path-hidden.txt`, `frontend-f3-file-reference-quiet.txt`, `frontend-f2-transcript-wide.txt`, and `startup-ready-composer.txt`. Assertions cover fixed quiet rows, display-name-first model groups, title-first sessions, hidden default paths/IDs/capabilities, opt-in path disclosure, keyboard/mouse parity, pending authority repaint, one actionable startup warning, the completed write target, exact dimensions, ESC/C0 hygiene, final LF, and process cleanup. The full opt-in 23-scenario wave, including `mermaid`, remains the closure command.

F5 tool, permission, and question evidence is rooted under `build/tui-tmux-smoke/main_permission_flow/evidence/` and `build/tui-tmux-smoke/main_question_flow/evidence/`. `frontend-f5-permission-prompt-roomy.txt` and `frontend-f5-denied-tool-card.txt` retain the quiet `! Permission required` surface, human action, sanitized command, risk/reason, truthful choices, one settled denial card, and no raw decision ID or stale Running duplicate. `frontend-f5-tool-card-mouse-expanded.txt` proves that an SGR click expands the original write card without appending a duplicate. The lifecycle captures cover `160x48`, `120x36`, `80x24`, and `100x12`; Rich previews cap shell/search output at 20 rows and Expanded previews at 200 rows, select the actual tail for shell output, and report exact hidden logical-line counts when authoritative totals exist or a conservative `more output hidden` otherwise. Tests include a 1,000-line tail and a single near-512-KiB line to prove bounded materialization.

The isolated fake-provider `main_question_flow` scenario saves `frontend-f5-question-single.txt`, `frontend-f5-question-multi-narrow.txt`, and `frontend-f5-question-short.txt`. It checks the distinct configurable question surface, wrapped questions, SGR mouse, keyboard/mouse multi-select behavior, one-row same-direction wheel-burst coalescing with prompt confirmation ordering, direction reversal, and reflow at `80x24` and `100x12`. `streaming_scroll` and `active_run` additionally exercise idle/streaming key and three-row transcript wheel floods (40ms same-direction coalescing, immediate reverse), active `/details` and `/tool` changes, preserved drafts, and bounded observed response latency. Deterministic composer coverage remains authoritative for modal/search/custom/copy/secret boundaries, exact versus truncated payload suppression, presentation/query round trips, card hit testing, frame scheduling, frozen detached layout, and the 20/28/40/80/120-column plus 1–12-row permission/question matrix. Every real-terminal frame checks exact dimensions, line bounds, final LF, ESC/unexpected-C0 absence, and process/socket cleanup. No scenario uses live credentials.

F6 has four direct-PTY tests. `ava_tui.terminal_lifecycle_smoke` runs clean
Ctrl+D and SIGTERM exits, replies to the keyboard-protocol device-attributes
query, and requires `modifyOtherKeys` disable, Kitty keyboard pop,
bracketed-paste disable, explicit cursor show, paired alternate-screen exit,
exact normalized termios restoration, exit `0`/`130`, and no surviving process
group. The shared parameterized image driver backs
`ava_tui.kitty_image_smoke` (Kitty transmit and delete-before-keyboard-pop)
and `ava_tui.iterm2_image_smoke` (iTerm2 OSC 1337 with BEL termination);
text-only fallback remains tmux evidence. `ava_tui.osc8_smoke` retains three
OSC 8 link assertions and parses a complete ST-terminated OSC 52 sequence,
validates its base64, and compares the decoded bytes with the fake assistant
response. Drivers use private roots, allowlisted credential-free environments,
bounded captures, and finite cleanup; the OSC case adds only a dummy Moonshot
key and loopback endpoint. Verbose closure output can be saved to
`build/tui-f6-evidence/protocol-closure.log`.

Deterministic F6 performance regressions run through `ava_tests.tui_composer`: four `120x36` redraws of 900 mixed items must remain under five seconds; compact plus expanded rendering of 20,000 output lines stays under two seconds; three `96x30` scroll positions over 1,320 mixed items stay under twenty seconds; 86 mixed items are bounded across 40 resize geometries; and streaming tests enforce exact tail parity plus viewport/carry work limits for 800–900-line sources, an 18,000-byte token, 180 incremental appends, and a 90,000-byte tool argument. These generous CI ceilings catch pathological regressions and are not benchmark claims.

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 23 -R '^ava_tui\.tmux_smoke_'
AVA_TUI_KITTY_IMAGE_SMOKE=1 scripts/run-tests.sh -R '^ava_tui\.kitty_image_smoke$'
AVA_TUI_ITERM2_IMAGE_SMOKE=1 scripts/run-tests.sh -R '^ava_tui\.iterm2_image_smoke$'
AVA_TUI_TERMINAL_LIFECYCLE_SMOKE=1 scripts/run-tests.sh -R '^ava_tui\.terminal_lifecycle_smoke$'
AVA_TUI_OSC8_SMOKE=1 scripts/run-tests.sh -R '^ava_tui\.osc8_smoke$'
```

## JUnit Test Result Summaries And Required Terminal Gates

CI test steps write CTest JUnit XML with `--output-junit` and summarize it through [`scripts/summarize-test-results.py`](https://github.com/Artificial-Source/AVA/blob/develop/scripts/summarize-test-results.py). The summary prints only test counts (passed/failed/skipped), the elapsed suite time when the XML supplies it, and a bounded list of the slowest test names and times. It never prints stdout, stderr, or failure body content, and it escapes and length-limits test names. CTest exit statuses are unchanged; the summary step runs with `if: always()` and itself fails clearly on a missing, malformed, or empty JUnit file, so an absent summary can never masquerade as a successful test run.

For required terminal gates the summary becomes strict: `--require NAME` demands that a test executed exactly once and passed, `--require-count N` pins the exact total, and `--require-no-skips` forbids skips. Under any of these options, failed tests also fail the gate, so a harness skip (CTest skip return code 77) or failure cannot look green the way a bare `--no-tests=error` run could.

The routine CI Release leg runs four required real-terminal tests with tmux installed — `ava_tui.terminal_lifecycle_smoke` plus the tmux scenarios `main_editor_input`, `main_permission_flow`, and `active_run` — capped at two jobs. The equivalent local gate is:

```sh
AVA_TUI_TMUX_SMOKE=1 AVA_TUI_TERMINAL_LIFECYCLE_SMOKE=1 \
  scripts/run-tests.sh --build-dir build --jobs 2 --no-tests=error \
  --output-junit /tmp/ava-junit-terminal.xml \
  -R '^(ava_tui[.]terminal_lifecycle_smoke|ava_tui[.]tmux_smoke_(main_editor_input|main_permission_flow|active_run))$'
python3 scripts/summarize-test-results.py /tmp/ava-junit-terminal.xml \
  --require-count 4 --require-no-skips \
  --require ava_tui.terminal_lifecycle_smoke \
  --require ava_tui.tmux_smoke_main_editor_input \
  --require ava_tui.tmux_smoke_main_permission_flow \
  --require ava_tui.tmux_smoke_active_run
```

The full release-candidate terminal gate runs for `workflow_dispatch` runs with `full_terminal` enabled and for `v*` version tag pushes. It executes all 23 tmux scenarios (`--jobs 4`) plus the four direct-PTY tests `terminal_lifecycle_smoke`, `kitty_image_smoke`, `iterm2_image_smoke`, and `osc8_smoke` (`--jobs 2`) — 27 tests total — with `AVA_TUI_TMUX_SMOKE`, `AVA_TUI_TERMINAL_LIFECYCLE_SMOKE`, `AVA_TUI_KITTY_IMAGE_SMOKE`, `AVA_TUI_ITERM2_IMAGE_SMOKE`, and `AVA_TUI_OSC8_SMOKE` set, and the strict summary requires all 27 to have executed and passed with no skips. Routine runs execute either the small selection or the full wave, never both. The full wave is also the local release-candidate closure command:

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 4 --no-tests=error \
  --output-junit /tmp/ava-junit-terminal-tmux.xml -R '^ava_tui[.]tmux_smoke_'
AVA_TUI_TERMINAL_LIFECYCLE_SMOKE=1 AVA_TUI_KITTY_IMAGE_SMOKE=1 \
AVA_TUI_ITERM2_IMAGE_SMOKE=1 AVA_TUI_OSC8_SMOKE=1 \
  scripts/run-tests.sh --build-dir build --jobs 2 --no-tests=error \
  --output-junit /tmp/ava-junit-terminal-pty.xml \
  -R '^ava_tui[.](terminal_lifecycle_smoke|kitty_image_smoke|iterm2_image_smoke|osc8_smoke)$'
python3 scripts/summarize-test-results.py \
  /tmp/ava-junit-terminal-tmux.xml /tmp/ava-junit-terminal-pty.xml \
  --require-count 27 --require-no-skips
```

The tmux family dispatches the 23 independent scenarios listed in the [MVP capability release evidence](#mvp-capability-release-evidence) table, including `nested_settings_preview`, `startup_overview`, `branch_summary`, `plugin_ui`, and `mermaid`. Each gets a guarded leaf under `build/tui-tmux-smoke/<scenario>/`, its own HOME/XDG/workspace, private config-free tmux socket, and separate evidence directory at `build/tui-tmux-smoke/<scenario>/evidence/`. Drivers enforce a 50-second internal deadline, clean private tmux/provider process groups on SIGINT or SIGTERM, and receive a 10-second graceful-cleanup window before CTest's 60-second outer timeout. The fake-provider request logs remain under each scenario root, including active/restore and the two question-tool runs. Where scenarios assert delivered conversation turns, local request-log accounting excludes only requests whose system prompt exactly matches the stable title-generation prompt; the complete raw log remains available for content checks and diagnostics report both normal-turn and total-request counts.

The `mermaid` scenario verifies disabled/pending/failure/stale original-fence fallback and the bounded successful projection path without a live provider. Mermaid execution is application-owned; no renderer subprocess or semantic transcript mutation is claimed.

Interactive startup has offline CLI/PTY coverage that the TUI requires a terminal on both stdin and stdout, unknown `--line-shell` is rejected, and `--print`/`--rpc` remain available for non-TTY runs. This is not a screen-reader certification test.

The MVP strategy is renderer/editor reducers first, then PTY/tmux assertions for terminal protocols and cleanup. AVA intentionally does not require a separate virtual-terminal parser for MVP; add one only if focused renderer tests plus the existing PTY smokes stop providing stable evidence.

## 0.90 Release-Candidate Checklist

Before treating a 0.90 release-candidate audit as complete, run and record:

```sh
scripts/build.sh --target ava ava_tests
scripts/run-tests.sh
scripts/run-tests.sh -R "ava_tests\.(session|agent_loop|agent_loop_resilience|app_print|app_runtime|app_command_classification|config_context_auth_oauth|provider_openai|provider_anthropic|provider_gemini|app_command_registry|app_compaction|core_json_permission|plugin|mcp)|ava_cli\.headless_rpc_"
git --no-pager diff --check
```

When C++ or CTest behavior changed, also run a sanitizer build/test, using the preset when it works or the direct build directory fallback:

```sh
scripts/build.sh --build-dir build-sanitize --jobs 2 --target ava_tests
scripts/run-tests.sh --build-dir build-sanitize --jobs 2
```

Run `clang-format` on changed C++ files and `clang-tidy <changed-cpp-files> -p build` when `clang-tidy` is available in PATH. If it is unavailable, record that environment limitation in the release journal.

Use `docs/versions/0.90.md` as the release-candidate test evidence map. It maps 1.0 capability areas to `ava_tests` suite names, `ava_cli.headless_rpc_*` scripts, golden fixtures, and live/manual smoke expectations.

For provider live smokes, use environment credentials only; the smoke suite intentionally does not read or print auth files. The CTest suite `ava_tests.provider_live_smoke` is skipped by default; opt in with `AVA_LIVE_PROVIDER_SMOKE=1` plus one or more provider credentials such as `OPENAI_API_KEY`, `ANTHROPIC_API_KEY`, `ANTHROPIC_OAUTH_TOKEN`, `ANTHROPIC_AUTH_TOKEN`, `DEEPSEEK_API_KEY`, `GEMINI_API_KEY`, `KIMI_API_KEY`, `MOONSHOT_API_KEY`, or `OPENROUTER_API_KEY`. Optional model overrides are `AVA_LIVE_OPENAI_MODEL`, `AVA_LIVE_ANTHROPIC_MODEL`, `AVA_LIVE_DEEPSEEK_MODEL`, `AVA_LIVE_GEMINI_MODEL`, `AVA_LIVE_KIMI_MODEL`, `AVA_LIVE_MOONSHOT_MODEL`, and `AVA_LIVE_OPENROUTER_MODEL`.

Record each provider case as one of: passed, skipped/no credential, credential/auth-blocked, provider/rate-limited, network-blocked, or AVA regression. The full-binary live dogfood script may also report provider-behavior/inconclusive when the model returns a valid response but does not perform the requested read-only tool call. A provider-breadth release claim needs the focused provider suites (`ava_tests.config_context_auth_oauth`, `ava_tests.provider_openai`, `ava_tests.provider_anthropic`, `ava_tests.provider_gemini`, `ava_tests.provider_live_smoke`) plus a dated matrix of the enabled live cases, model ids, result classifications, and whether any failure is credentials/provider/network/AVA-owned. Anthropic interactive OAuth is not a live-smoke requirement because Anthropic does not document a third-party authorization or device flow; use `ANTHROPIC_API_KEY`, `ANTHROPIC_OAUTH_TOKEN`, or `ANTHROPIC_AUTH_TOKEN` only when the credential is already available. Historical 0.90 evidence had OpenAI and Kimi-for-coding live-smoked, with Kimi-for-coding accepted as the additional production-quality provider path at that release-candidate cut; current provider-breadth claims should use the matrix runner and classify every enabled live case separately.
