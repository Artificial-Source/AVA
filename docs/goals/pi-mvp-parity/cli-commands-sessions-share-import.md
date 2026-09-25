# CLI, Slash Commands, Sessions, Import, Share, And Export

## Goal Objective

Close Pi product-command parity for AVA's CLI, slash commands, session workflows, import/export/share, and copy/logout flows, while preserving AVA's permission and local-first session model.

Suggested Codex command:

```text
/goal Bring AVA CLI, slash-command, session import/share/export parity to documented closure using docs/goals/pi-mvp-parity/cli-commands-sessions-share-import.md. First inspect Pi command/session docs and AVA command/session code, then implement or explicitly defer every 100 percent criterion with tests, docs, and smoke evidence. Stop for approval before adding public sharing, GitHub CLI dependencies, or cloud behavior.
```

## Pi References To Inspect First

| Topic | Pi paths |
| --- | --- |
| CLI args | `docs/reference-code/pi/packages/coding-agent/src/cli/args.ts`, `docs/reference-code/pi/packages/coding-agent/src/main.ts` |
| Slash registry | `docs/reference-code/pi/packages/coding-agent/src/core/slash-commands.ts` |
| Interactive handlers | `docs/reference-code/pi/packages/coding-agent/src/modes/interactive/interactive-mode.ts` |
| Sessions | `docs/reference-code/pi/packages/coding-agent/src/core/session-manager.ts`, `docs/reference-code/pi/packages/coding-agent/src/core/agent-session.ts` |
| Export HTML | `docs/reference-code/pi/packages/coding-agent/src/core/export-html/` |
| Import/share handlers | `handleImportCommand`, `handleShareCommand`, `handleExportCommand` in Pi interactive mode |
| User docs | `docs/reference-code/pi/packages/coding-agent/docs/usage.md`, `sessions.md`, `session-format.md`, `rpc.md`, `tui.md` |
| Tests | `docs/reference-code/pi/packages/coding-agent/test/session-manager/`, `interactive-mode-import-command.test.ts`, export HTML tests |

## AVA References To Inspect First

| Topic | AVA paths |
| --- | --- |
| CLI parse | `src/ava/app/app.cpp` |
| Commands | `src/ava/app/commands.cpp`, `src/ava/app/command_catalog.cpp`, `src/ava/app/command_sessions.cpp`, `src/ava/app/command_palette.cpp` |
| RPC | `src/ava/app/rpc/handlers.cpp`, `src/ava/app/rpc/protocol.h`, `docs/headless-protocol.md` |
| Runtime session | `src/ava/app/runtime/Session.cpp`, `src/ava/app/runtime_prompt.cpp`, `src/ava/app/runtime_run_outcomes.cpp` |
| Session store | `src/ava/session/session_store.cpp`, `src/ava/session/session_branch.cpp`, `src/ava/session/session_tree.cpp`, `src/ava/session/export.cpp` |
| TUI selector | `src/ava/tui/runtime.cpp`, `src/ava/tui/composer_select_list.cpp` |
| Tests | `tests/session_tests.cpp`, `tests/app_runtime_tests.cpp`, `tests/app_rpc_tests.cpp`, `tests/tui_composer_tests.cpp`, `tests/cli_headless_*.cmake` |

## Current Gap Summary

AVA already has strong session tree/fork/clone/name/label/archive/export/copy behavior. This area closes local session import, raw JSONL export, and Pi-compatible session CLI aliases. Public share/cloud upload, provider logout mutation, dedicated RPC import/share, `--models`, `--export` exit flow, and cross-session content search are explicitly deferred or left to later scoped areas because they need product/security design beyond minimal MVP parity.

## 100 Percent Criteria

| Criterion | Required AVA State |
| --- | --- |
| Slash command matrix | All Pi slash commands are implemented, AVA-superior, deferred, or excluded: `/settings`, `/model`, `/scoped-models`, `/export`, `/import`, `/share`, `/copy`, `/name`, `/session`, `/changelog`, `/hotkeys`, `/fork`, `/clone`, `/tree`, `/trust`, `/login`, `/logout`, `/new`, `/compact`, `/resume`, `/reload`, `/quit`. |
| CLI flag matrix | Pi CLI flags are implemented, aliased, deferred, or excluded: `--resume`, `--session-id`, `--fork`, `--models`, `--thinking`, `--extension`, `--skill`, `--prompt-template`, `--theme`, `--no-context-files`, `--export`, `--offline`, trust flags, `@file`. |
| Session import | `/import <path.jsonl>` and RPC import exist or are deferred. If implemented, import validates session format, asks before replacing current session in TUI, handles missing CWD, avoids unsafe paths, and records evidence. |
| Share decision | `/share` is implemented as a local/GitHub gist flow or explicitly deferred/excluded. If implemented, it requires explicit user action, clear dependency on `gh`, private gist by default, sanitized HTML export, and no silent cloud upload. |
| Copy flow | `/copy` can copy the last assistant message, tool details, diff, permission detail, and empty/error states through TUI clipboard or documented fallback. |
| Logout flow | `/logout` removes provider credentials safely with confirmation and docs, or is explicitly deferred. It must not delete unrelated provider credentials. |
| Export parity | `/export` supports Markdown, HTML, explicit file destinations, raw JSONL if accepted, and `--export` process exit flow if accepted. |
| Session content search | Either implemented for current session/all sessions, or deferred with rationale. |
| Tests | Unit, RPC, print, and TUI/PTY tests cover each implemented command and flag. |

## Implementation Slices

| Slice | Work |
| --- | --- |
| C1. Command matrix | Add/update a markdown table in this file and `docs/product/mvp-baseline.md` with every Pi command/flag disposition. |
| C2. `/import` | Implement safe import of AVA JSONL sessions first. Decide whether Pi JSONL import requires a converter or is unsupported. Add confirmation in TUI and RPC/headless behavior. |
| C3. `/share` | Product decision first: local-only export link, `gh gist` private upload, or defer. If accepted, make dependency and privacy explicit. |
| C4. `/copy` and `/logout` | Close low-risk UX gaps with focused command handlers and tests. |
| C5. CLI flag parity | Add or verify `--session-id`, `--models`, `--thinking`, `--export`, and `--resume` semantics only where they fit AVA. Do not add ambiguous aliases that conflict with existing `--mode build|plan`. |
| C6. Docs and smokes | Update `README.md`, `docs/core/usage.md`, `docs/headless-protocol.md`, and terminal smoke scripts for user-visible flows. |

## Design Constraints

| Constraint | Requirement |
| --- | --- |
| Import safety | Never load future-version or invalid session entries silently. Preserve AVA session validation invariants. |
| Share privacy | No public upload by default. The user must understand the exported content includes prompt, assistant output, and tool metadata. |
| Clipboard | OSC52 and local clipboard helpers must fail visibly and non-destructively. |
| Session IDs | `--session-id` must validate safe IDs and avoid path traversal or cross-workspace ambiguity. |

## Verification

Targeted commands:

```sh
ctest --test-dir build -R 'ava_tests\.(session|app_runtime|app_rpc|tui_composer)$' --output-on-failure
ctest --test-dir build -R 'ava_cli\.headless_(print|rpc|context|tool|permission|mode|session)' --output-on-failure
```

For TUI command behavior:

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

- 2026-07-03: Initial goal file created. Current status: sessions/export strong; import/share/logout/selected flags remain the major gaps.
- 2026-07-04: Area checkpoint plan: C1 inspect Pi command/session/import/share/export paths and AVA CLI/session code; C2 implement local AVA JSONL `/import` plus `/export jsonl`; C3 defer cloud `/share`; C4 preserve existing TUI `/copy` and defer provider `/logout`; C5 add low-risk `--session-id` and `--resume`/`-r`; C6 update docs/product ledgers and run targeted/full validation plus material review.
- 2026-07-04: Inspected Pi references listed above for CLI args, slash registry, interactive import/share/export handling, sessions, HTML export, and usage/session docs; inspected AVA `app.cpp`, command catalog/dispatcher/palette/session commands, RPC protocol docs, session store/export/validation code, and existing runtime/session tests.
- 2026-07-04: Implemented local JSONL export/import and CLI alias closure.
  - Changed `src/ava/app/command_sessions.cpp` and `.h`: `/export` now accepts `jsonl`/`raw` or `.jsonl` destinations and serializes current AVA session entries as raw JSONL; `/import <path.jsonl>` validates a regular non-symlink file with bounded session-line parsing and `validate_session_replay`, previews without `--confirm`, creates a new local session and switches on `--confirm`, cleans up failed partial imports, and normalizes legacy missing-version entries to the current session-entry version before writing.
  - Changed `src/ava/app/commands.cpp`: dispatches `/import` before provider fallback.
  - Changed `src/ava/app/command_catalog.cpp`: enables `/import`, documents `/export [markdown|html|jsonl]`, and registers disabled `/share`, `/changelog`, and `/logout` with explicit reasons.
  - Changed `src/ava/app/command_palette.cpp`: adds `jsonl` export completion, `/import` path completions, and `--confirm` completion.
  - Changed `src/ava/app/app.cpp`: accepts Pi-compatible `--session-id` as `--session` and `--resume`/`-r` as `--continue`, with updated help/conflict wording.
  - Changed `tests/app_runtime_tests.cpp`: covers `/export jsonl`, `/import` preview/confirm, legacy missing-version normalization, command completions, disabled-command catalog surface, and command outputs.
  - Changed `tests/cli_headless_print_session_startup_options.cmake`: covers `--session-id` and `--resume` in the built CLI against the fake provider while preserving custom session-dir behavior.
  - Changed docs: `README.md`, `docs/core/usage.md`, `docs/headless-protocol.md`, `docs/product/mvp-baseline.md`, and `docs/product/mvp-coverage-ledger.md` describe JSONL export/import, aliases, and deferrals.

## Command And Flag Disposition Matrix

| Item | Disposition | Evidence / rationale |
| --- | --- | --- |
| `/settings` | Implemented | Existing TUI settings command remains in catalog and TUI smoke coverage; no backend change in this area. |
| `/model`, `/models`, `/scoped-models` | Implemented / AVA-superior | Existing model list/select/cycle/scoped-cycle flows; providers/models area closed deeper model parity. |
| `/export` | Implemented | Markdown, safe self-contained HTML, explicit file writes, `.html` inference, RPC `export_html`, and new raw JSONL `/export jsonl` / `.jsonl` inference. |
| `/import` | Implemented for local AVA JSONL | Validates regular non-symlink JSONL with bounded line parsing and replay validation, previews without `--confirm`, imports into a new local session on confirmation. Pi-to-AVA conversion remains deferred. |
| `/share` | Deferred | Public/cloud sharing and GitHub/Gist dependency require explicit product/security approval; local substitute is `/export html <path>`. Disabled command explains this. |
| `/copy` | Implemented for MVP TUI local copy | Existing `/copy` supports assistant/tool/diff/permission empty/error states; no cloud/system clipboard expansion in this backend area. |
| `/name`, `/session`/`/sessions`, `/tree`, `/fork`, `/clone`, `/new`, `/resume`, `/compact`, `/quit` | Implemented | Existing session tree/fork/clone/name/label/archive/compact/resume/quit flows remain covered by `ava_tests.session`, `ava_tests.app_runtime`, and TUI smoke coverage. |
| `/changelog` | Deferred | Disabled command points to README/product docs; interactive release-note feed is not a coding-agent MVP safety requirement. |
| `/hotkeys` | Implemented / AVA-superior | Existing keybinding init/import/set/reset/validate/reload flows and settings integration. |
| `/trust`, `/login`/`/connect`, `/reload` | Implemented | Closed in prior/provider/context/settings work; this area preserves catalog visibility. |
| `/logout` | Deferred | Removing one provider credential safely needs provider-specific confirmation UX; disabled command directs users to edit only the provider entry or rerun `/connect`. |
| `--resume`, `-r` | Implemented alias | Maps to `--continue`; covered by CLI fake-provider startup-options smoke. |
| `--session-id` | Implemented alias | Maps to `--session`; uses existing safe session-id/prefix resolution and conflict checks; covered by CLI fake-provider startup-options smoke. |
| `--thinking <level>` | Implemented alias | Maps to existing runtime reasoning selection: `off` clears explicit reasoning, and other levels use active-model `reasoning_levels`/provider validation. Covered by CLI/RPC startup alias tests. |
| `--fork`, `--name`, `--session-dir`, `@file` | Implemented | Existing behavior retained; `--fork` conflict wording now mentions aliases. |
| `--models`, `--extension`, `--skill`, `--prompt-template`, `--theme`, `--no-context-files`, `--export`, `--offline`, trust flags | Deferred / excluded for this area | These affect provider/model selection, context/resource loading, settings, process exit export, or trust policy. They are intentionally left for settings/packages/resources or future product design to avoid ambiguous aliases and weakened safety boundaries. |
| Dedicated RPC import/share | Deferred | RPC already covers Markdown/HTML export; local JSONL import is slash/TUI only until the RPC contract for archive replacement/switching is designed. |
| Session content search | Deferred | Current `/sessions [query]` searches session metadata/tree rows; full transcript search across sessions needs indexing/UX design and is not required for safe local import/export closure. |

## Validation Evidence

```sh
clang-format -i src/ava/app/app.cpp src/ava/app/command_catalog.cpp src/ava/app/command_palette.cpp src/ava/app/command_sessions.cpp src/ava/app/command_sessions.h src/ava/app/commands.cpp tests/app_runtime_tests.cpp
cmake --build --preset dev --target ava_tests ava
ctest --test-dir build -R 'ava_tests\.(app_runtime|session|app_command_registry|app_rpc)$|ava_cli\.headless_print_session_startup_options' --output-on-failure
# Passed: 5/5 targeted tests.
ctest --test-dir build -R 'ava_tests\.app_runtime$|ava_cli\.headless_print_session_startup_options' --output-on-failure
# Passed after legacy-version import normalization fix: 2/2 targeted tests.
ctest --test-dir build -R 'ava_tests\.app_runtime$|ava_tests\.(session|app_command_registry|app_rpc)$|ava_cli\.headless_print_session_startup_options' --output-on-failure
# Passed after import rejection/cleanup tests: 5/5 targeted tests.
ctest --preset dev --output-on-failure
# Passed: 57/57, with expected skips for provider live smoke and opt-in TUI smokes.
git --no-pager diff --check
# Passed with no whitespace errors.
```

Manual/headless line-shell smoke:

```sh
# In a temp workspace with isolated XDG dirs and NO_COLOR=1:
printf '/import valid-import.jsonl\n/import valid-import.jsonl --confirm\n/export jsonl\n/share\n/logout\n/quit\n' | build/ava --continue
# Exit status: 0
# Evidence: preview rendered "session import is ready"; confirm rendered "imported session ...";
# `/export jsonl` printed a version:3 session_start line; `/share` and `/logout` rendered disabled reasons;
# no stderr output.
```

## Decisions, Deferrals, And Residual Risks

- Decision: `/import` never replaces the current session file. It creates a new local session and switches to it, preserving append-only session history and avoiding destructive import semantics.
- Decision: import path uses `symlink_status`, requires a regular non-symlink file, bounded line reads, parser validation, and replay validation before any new session is created; rejection paths for empty, malformed, future-version, and symlink archives are covered by `ava_tests.app_runtime`.
- Decision: `/import` is a user-initiated local archive load, not a model-visible `/read` tool operation. It reads the selected file directly after regular-file/symlink checks and strict session validation; docs call out that distinction.
- Decision: legacy AVA JSONL entries without top-level `version` may be accepted by the parser/validator but are written back as current version entries to avoid creating an invalid `version:0` session archive.
- Deferral: Pi JSONL conversion is not implemented; current import supports AVA session JSONL only.
- Deferral: `/share` remains disabled until the product explicitly chooses local-only link generation or a private cloud/GitHub flow with clear dependency, privacy, and confirmation semantics.
- Deferral: `/logout` remains disabled until provider-specific credential deletion UX can remove one provider entry without deleting unrelated credentials.
- Deferral: dedicated RPC import/share and `--export` process-exit flow are postponed until the stdio RPC/archive contract is designed.
- Residual risk: raw JSONL export includes session content and metadata by design; docs steer users to local files and do not imply sanitization beyond the existing Markdown/HTML export behavior.
- Pending questions: none blocking safe progress for this area.

## Review Findings

- Reviewer finding (medium): command-layer import rejection paths lacked coverage. Fixed with `ava_tests.app_runtime` cases for empty archives, malformed JSONL, future session entry versions, and symlink sources, each asserting no session switch.
- Reviewer finding (low): reopening failure after an otherwise successful import would leave the newly written session file. Fixed by removing the just-created session path on reopen failure, matching append-failure cleanup.
- Reviewer finding (low): `/import` directly reads the user-selected archive rather than the model-visible permissioned `/read` tool. Accepted as an explicit UX/security decision because import is a slash/local archive load with regular-file checks and strict replay validation; documented in `docs/core/usage.md` and above.
- Security-auditor result: no material security/safety findings for `/import`, `/export`, session aliases, or disabled `/share`/`/logout` after the above fixes and documentation.

## 2026-07-04 Backend Aggregate Closure Audit

- Re-audited this area against the backend aggregate goal after all backend areas were processed. The slash-command and CLI flag matrices close every Pi command/flag as implemented, AVA-superior, deferred, or excluded; session import/export/share/logout/copy/session-search dispositions are explicit, and no frontend/TUI/editor implementation is needed for this backend closure.
- Current aggregate verification rerun: `cmake --preset dev`, `cmake --build --preset dev`, the CLI/session targeted CTest command, `ctest --preset dev --output-on-failure`, and `git --no-pager diff --check` all passed locally. Default-gated live-provider and opt-in TUI smokes remained skipped, which is acceptable for this backend-only area.
