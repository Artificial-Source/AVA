# Settings, Packages, Resources, Startup, And Theming

## Goal Objective

Close Pi parity for settings, global/project configuration, resource loading, packages, themes, startup UI, and reload diagnostics, or document the AVA product decision for each deferred capability.

Suggested Codex command:

```text
/goal Bring AVA settings/packages/resources parity to documented closure using docs/goals/pi-mvp-parity/settings-packages-resources.md. First inspect Pi settings/package/startup docs and AVA config modules, then implement or defer every 100 percent criterion with tests and docs. Stop for approval before enabling remote package install, model-writable project settings, telemetry, or self-update behavior.
```

## Pi References To Inspect First

| Topic | Pi paths |
| --- | --- |
| Settings manager | `docs/reference-code/pi/packages/coding-agent/src/core/settings-manager.ts` |
| Package manager | `docs/reference-code/pi/packages/coding-agent/src/core/package-manager.ts`, `docs/reference-code/pi/packages/coding-agent/src/package-manager-cli.ts` |
| Resource loader | `docs/reference-code/pi/packages/coding-agent/src/core/resource-loader.ts` |
| Startup UI | `docs/reference-code/pi/packages/coding-agent/src/cli/startup-ui.ts` |
| Themes | `docs/reference-code/pi/packages/coding-agent/src/modes/interactive/theme/`, `docs/reference-code/pi/packages/coding-agent/docs/themes.md` |
| Trust | `docs/reference-code/pi/packages/coding-agent/src/core/trust-manager.ts`, `project-trust.ts` |
| Docs | `docs/reference-code/pi/packages/coding-agent/docs/settings.md`, `packages.md`, `themes.md`, `security.md`, `quickstart.md` |
| Tests | `docs/reference-code/pi/packages/coding-agent/test/settings-manager.test.ts`, `package-manager.test.ts`, theme/startup tests |

## AVA References To Inspect First

| Topic | AVA paths |
| --- | --- |
| Config paths | `src/ava/config/xdg_paths.cpp`, `src/ava/config/xdg_paths.h` |
| Auth/model/prompt config | `src/ava/config/auth*.cpp`, `src/ava/config/model_config.cpp`, `src/ava/config/prompt_config.cpp` |
| Display/keybindings | `src/ava/app/tui/tui_display_settings.cpp`, `src/ava/tui/keybindings.cpp` |
| Project trust | `src/ava/app/project_trust.cpp`, command trust handlers |
| Plugins/MCP/LSP config | `src/ava/plugin/discovery.cpp`, `src/ava/mcp/config.cpp`, `src/ava/lsp/configured_provider.cpp` |
| Settings UI | `src/ava/tui/runtime.cpp`, `src/ava/app/command_palette.cpp`, `src/ava/app/commands.cpp` |
| Docs | `docs/core/configuration.md`, `docs/core/usage.md`, `docs/extensions/plugin-system.md`, `docs/plugin-compatibility-policy.md` |
| Tests | `tests/config_context_auth_oauth_tests.cpp`, `tests/app_runtime_tests.cpp`, `tests/plugin_tests.cpp`, `tests/mcp_tests.cpp`, `tests/tui_composer_tests.cpp` |

## Current Gap Summary

AVA has domain-specific XDG JSON configs, a TUI `/settings` select-list, theme/keybinding reload, project-resource trust gating, local/offline plugin directory install/remove, and an implemented provider-call `--offline` guard. Pi has a unified global/project settings model, safe external-edit preservation, broad package/resource install flows, first-run theme/analytics setup, and resource filtering. AVA now closes MVP parity by documenting the domain-config product decision, tightening keybinding writes through the atomic config helper, recognizing broad package-manager entry points as deferred, and explicitly deferring remote package/self-update/telemetry surfaces unless approved.

## 100 Percent Criteria

| Criterion | Required AVA State |
| --- | --- |
| Unified settings decision | AVA either implements a global/project settings model with merge order and schema validation, or documents why domain-specific config remains the product choice. |
| Safe writes | Config writes use lock or atomic replacement where mutation exists, preserve external edits where applicable, and surface actionable parse errors. |
| Project settings trust | Any project-local setting that influences execution, model context, tools, plugins, MCP/LSP, prompt, or auth is gated by outside-workspace trust and cannot be silently model-written into authority. |
| Reload diagnostics | `/reload` and automatic reloads report success/failure for settings, keybindings, display, prompts, plugins, MCP, LSP, skills, and provider/model config without losing last-known-good state. |
| Package/resource decision | Pi-style `install/remove/update/list/config` is implemented for approved local-only resource sources or explicitly deferred with trust/signing/source policy requirements. |
| Resource filters | If packages exist, extensions/skills/prompts/themes can be enabled/disabled per source with project/global precedence. If deferred, document how users install resources manually today. |
| Startup setup | First-run experience is implemented or explicitly mapped to AVA's auth onboarding. Theme detection/analytics opt-in are accepted or rejected. |
| Theme story | Built-in/custom themes, reload, NO_COLOR, terminal background inference, docs, and tests are complete for MVP. |
| Offline mode | `--offline` or equivalent is implemented or deferred, with clear effects on web tools, provider catalog updates, version checks, package updates, and remote MCP. |
| Tests | Settings parse/write/reload, external edit preservation, trust gating, package decisions, and TUI settings rows have tests. |

## Implementation Slices

| Slice | Work |
| --- | --- |
| S1. Settings architecture decision | Decide whether to introduce a unified `settings.json` facade over existing domain configs or keep domain configs with a settings index. Document in this file and `docs/core/configuration.md`. |
| S2. Safe config write utility | Reuse or create a narrow config write helper for owner-only, atomic writes, validation-before-commit, and external-edit preservation where needed. |
| S3. Reload diagnostics | Add structured reload result objects and TUI/headless display for each reloadable domain. Keep last good config on failure. |
| S4. Project settings trust | Define allowed project-local settings, deny model-writable policy escalation, and test trust decisions. |
| S5. Package/resource policy | Write a package manager product decision. Keep the approved local plugin directory install/remove slice separate from future remote npm/git/marketplace package flows. |
| S6. Startup UI | Decide whether first-run theme selection is MVP. Keep analytics excluded unless explicitly approved. |
| S7. Theme polish | Finish docs/tests for custom themes, terminal background inference, and NO_COLOR/plain behavior. |

## Non-Goals Unless Approved

| Item | Reason |
| --- | --- |
| Remote package install from npm/git | Requires trust, provenance, update, rollback, and compatibility policy. |
| Self-update | Distribution-specific and riskier than feature parity. |
| Analytics/telemetry | AVA local-first posture should not add telemetry without explicit product approval. |
| Model-writable project settings authority | Violates AVA's safety boundary. |

## Verification

Targeted commands:

```sh
ctest --test-dir build -R 'ava_tests\.(config_context_auth_oauth|app_runtime|plugin|mcp|lsp|tui_composer)$' --output-on-failure
```

For real settings TUI behavior:

```sh
AVA_TUI_TMUX_SMOKE=1 ctest --test-dir build -R ava_tui.tmux_smoke --output-on-failure
```

Before area completion:

```sh
cmake --build --preset dev
ctest --preset dev --output-on-failure
git --no-pager diff --check
```

## Progress Log

- 2026-07-03: Initial goal file created. Initial status: domain configs strong; unified settings/package/startup decisions remained open.
- 2026-07-04: Area checkpoint plan: S1 inspect Pi settings/package/resource/startup/theme/trust references and AVA config/reload/theme/trust code; S2 preserve AVA domain config architecture and document why a unified merged `settings.json` is deferred; S3 improve safe config writes where the active settings UI mutates files; S4 document project-resource trust and reload diagnostics; S5 recognize package-manager entry points while deferring remote/update/list flows until safety policy exists; S6 document startup/theme/offline/telemetry decisions; S7 update product ledgers and run targeted/full validation plus material review.
- 2026-07-04: Inspected Pi references listed above. Summary: Pi deep-merges global/project settings with project settings gated by trust, lock-protects settings writes while preserving external edits, supports npm/git/local package install/list/remove/update/config plus resource filters, auto-discovers package/project/user resources, runs optional first-time theme/analytics setup, supports large JSON theme schemas and auto theme detection, and gates trust-requiring project resources.
- 2026-07-04: Inspected AVA references listed above. Summary: AVA already has XDG domain configs, owner-only auth storage, atomic model scoped-cycle writes, atomic display theme writes, project trust stored outside the workspace, `/reload` rows for hot/restart-required domains, manual plugin/MCP/LSP/resource directories, `/settings` theme/model/trust/keybinding rows, auth-only onboarding, NO_COLOR/AVA_TUI_THEME/COLORFGBG theme precedence, and broad deterministic/TUI smoke coverage.
- 2026-07-04: Implemented minimal settings/package/resource closure.
  - Changed `src/ava/app/commands.cpp`: keybinding `init`, `import`, `set`, and `reset` now use `ava::core::write_text_file_atomic`, so active settings writes use owner-only atomic replacement and reject symlink targets after validating candidates.
  - Changed `src/ava/app/command_catalog.cpp`: added disabled `/packages`/`/package` catalog entry with install/update deferral rationale so package-manager requests are not sent to the model.
  - Changed `src/ava/app/app.cpp`: added top-level `ava packages ...` / `ava package ...` placeholder that exits successfully with the package deferral and manual resource-install guidance.
  - Added `tests/cli_package_manager_deferred.cmake` and registered `ava_cli.package_manager_deferred` in `tests/CMakeLists.txt`.
  - Changed `tests/app_runtime_tests.cpp`: covers `/packages` disabled handling, keybinding symlink-target rejection through atomic writes, and restoration of a normal keybinding config afterward.
  - Changed docs: `docs/core/configuration.md`, `docs/core/usage.md`, `docs/product/mvp-baseline.md`, and `docs/product/mvp-coverage-ledger.md` now record the domain-config decision, manual resource layout, package/offline/telemetry/self-update deferrals, and updated evidence.

## Settings And Resource Disposition Matrix

| Criterion | Disposition | Evidence / rationale |
| --- | --- | --- |
| Unified settings decision | Domain configs accepted for MVP; unified facade deferred | `docs/core/configuration.md` documents why AVA keeps separate auth/model/prompt/display/keybinding/plugin/MCP/LSP/trust/permission files to preserve safety boundaries and avoid model-writable authority. |
| Safe writes | Implemented for active user-mutated settings surfaces | Auth has owner-only locked atomic storage; model scoped-cycle and display theme writes already use atomic replacement; this area moved keybinding init/import/set/reset onto `write_text_file_atomic` and added symlink-target coverage. |
| Project settings trust | Implemented | Trust decisions live in `$XDG_STATE_HOME/ava/project-trust.json`; project commands, skills, plugins, MCP/LSP config, and project system prompt resources are skipped until `/trust project`; context files remain visible. |
| Reload diagnostics | Implemented for MVP | `/reload` reports display/models/trust/prompts/compaction/keybindings and restart-required auth/permissions/LSP/MCP/plugins, with targeted app-runtime coverage and opt-in TUI smoke coverage. |
| Package/resource workflow | Local plugin directory install/remove implemented; broad package manager deferred | `/plugins install <path>` and `/plugins remove <id>` manage local global disabled plugin directories without entrypoint launch. `/packages` and `ava packages ...` return deferral text; no npm/git/marketplace/self-update side effects are added. |
| Resource filters | Deferred with manual install docs | Because packages are deferred, package resource filters are also deferred. Manual global/project resource locations are documented in `docs/core/configuration.md`; plugin enable/disable remains available for local plugins. |
| Startup setup | AVA-native auth onboarding retained; local setup wizard removed | Fresh TUI startup opens the normal composer and retains auth-first `/connect` guidance. The former Wave 4 wizard was removed by product decision; no onboarding marker is read or written. Analytics/telemetry remain excluded. |
| Theme story | Implemented for MVP | Built-in/custom themes, display.json, auto reload, `NO_COLOR`, `AVA_TUI_THEME`, `COLORFGBG`, docs, unit tests, and opt-in TUI smoke coverage exist. Package-delivered themes are deferred. |
| Offline mode | Implemented as a provider-call guard | `--offline` fails closed before provider prompt turns or provider-backed compaction resolve credentials or send requests. Local slash/RPC inspection commands and local plugin directory install/remove remain available; the flag is not an operating-system network sandbox for tools or future remote MCP/package surfaces. |
| Tests | Implemented for changed surfaces | Targeted tests listed below cover config/reload/theme/trust, packages deferral, and keybinding safe writes. |

## Validation Evidence

```sh
clang-format -i src/ava/app/app.cpp src/ava/app/command_catalog.cpp src/ava/app/commands.cpp tests/app_runtime_tests.cpp
cmake --preset dev
cmake --build --preset dev --target ava_tests ava
ctest --test-dir build -R 'ava_tests\.(app_runtime|config_context_auth_oauth|plugin|mcp|lsp|tui_composer)$|ava_cli\.package_manager_deferred' --output-on-failure
# Passed: 7/7 targeted tests.
ctest --preset dev --output-on-failure
# Passed: 58/58, with expected skips for provider live smoke and opt-in TUI smokes.
git --no-pager diff --check
# Passed with no whitespace errors.
```

Manual/headless package placeholder smoke is covered by `ava_cli.package_manager_deferred`: `ava packages list` exits 0 and prints the package-manager deferral with provenance/manual-resource documentation pointers.

## Decisions, Deferrals, And Residual Risks

- Decision: AVA will not introduce a single global/project `settings.json` for MVP. Domain configs remain the product shape because auth, provider/model metadata, model context, executable project resources, permissions, and display preferences need different validators and trust boundaries.
- Decision: Project-local executable/stronger model-context resources stay gated by `/trust project`; model-writable project files cannot silently grant package/plugin/MCP/LSP/prompt authority.
- Decision: Active settings writes that AVA performs for display and keybindings use atomic replacement with symlink-target rejection. Auth/model/trust paths already have their own atomic or locked write flows.
- Deferral: Pi-style npm/git/marketplace package install/list/update/config is not implemented. Local plugin directory install/remove is implemented only for global plugin directories and leaves installed plugins disabled; remote install, self-update, and package update checks require local-source allowlists, provenance/signing, compatibility policy, rollback, and explicit trust UX.
- Deferral: Package resource filters for extensions/skills/prompts/themes are not implemented because package install is deferred. Manual resource installation remains the supported MVP path.
- Current product decision: Pi's combined theme-wizard/analytics first-run setup is not adopted. The former local-only Wave 4 wizard was removed and superseded; AVA keeps auth-first `/connect` guidance and local-first no-telemetry behavior.
- Decision: `--offline` is implemented as a provider-call guard, not an OS network sandbox. Future remote package catalogs, remote MCP transports, version checks, and network sandboxing need separate policy before they can rely on it.
- Residual risk: Domain-specific config means users do not have one Pi-style settings file to inspect; `docs/core/configuration.md`, `/settings`, `/reload`, and command-specific validation are the mitigation.
- Pending questions: none blocking safe progress for this area.

## Review Findings

- Main reviewer result: no material findings. Reviewer verified atomic keybinding writes, `/reload` per-domain behavior/last-known-good handling, `/packages` deferral behavior, docs alignment, targeted tests, and `git diff --check` cleanliness. Residual note only: `ava packages ...` exits 0 for the deferred placeholder by documented choice.
- Security-auditor result: no material exploitable/safety issues. Reviewed keybinding validation/atomic writes/symlink rejection, disabled package entry points, package/offline/telemetry/self-update deferrals, and project-resource trust gating.

## 2026-07-04 Backend Aggregate Closure Audit

- Re-audited this final backend area against the aggregate goal. Unified settings, safe writes, project trust, reload diagnostics, package/resource disposition, startup setup, theme story, offline mode, and test criteria are all implemented/accepted for MVP or explicitly deferred/excluded with rationale; no frontend/TUI/editor implementation was added for this backend closure.
- Current aggregate verification rerun: `cmake --preset dev`, `cmake --build --preset dev`, the settings/package targeted CTest command, `ctest --preset dev --output-on-failure`, and `git --no-pager diff --check` all passed locally. Default-gated live-provider and opt-in TUI smokes remained skipped, which is acceptable for this backend-only area.

## 2026-07-08 M6/M7 And Carlo Follow-Up

- Local/offline plugin directory install/remove is now implemented through `/plugins install <path>` and `/plugins remove <id>` plus RPC `install_plugin`/`remove_plugin`, scoped to global plugin directories with installed plugins left disabled. This does not change the broad package-manager deferral for `/packages`, remote marketplaces, git/npm installs, self-update, source signing, or provenance.
- Enabled/trust-gated static plugin prompt resources now autoload into runtime context, static plugin skill resources appear in `<available_skills>` and load via `/skill:<name>` or provider `skill`, and failed enabled plugin static resources show in `/context` freshness.
- Post-merge validation passed `cmake --build --preset dev --target ava_tests`, `ctest --preset dev --output-on-failure` (62/62 with expected skips for `ava_tests.provider_live_smoke`, `ava_tui.tmux_smoke`, `ava_tui.kitty_image_smoke`, and `ava_tui.osc8_smoke`), and `git --no-pager diff --check`. Carlo's libcwd/ctags debug print-members codegen remains unvalidated locally because `ctags` and `ccache` are missing; the default libcwd-OFF `ava_debug` fallback is covered by the passing default build/tests.
