# Historical Pi-Inspired TUI Feature Expansion

**Status: completed historical planning record.** Retained Waves 0–3 and 5–6 are complete. Wave 4 was removed and superseded by product decision; this document is not current implementation authority or active authorization.

This plan originally proposed six user-facing terminal capabilities after comparing AVA with Pi. Five remain implemented; the fourth was subsequently removed:

1. persisted image visibility and sizing controls;
2. a deeper unified settings interface with reversible live previews;
3. an expandable startup overview;
4. ~~a local-only first-run setup wizard~~ — removed by product decision;
5. safe abandoned-branch summarization from the session tree; and
6. a narrow, host-rendered plugin UI surface.

The work is intentionally Pi-inspired rather than a port. AVA keeps its native C++ TUI, backend ownership boundaries, append-only session model, out-of-process plugins, permission system, and terminal safety rules. Pi source under `docs/reference-code/` is comparison material only.

## Baseline And Handoff State

The plan was written against `develop` at `f54f8d318fbcccd1d6c7c36c858c3755159922b1`.

At that baseline:

- the full opt-in tmux TUI wave passes 18/18 scenarios;
- `ava_tests.tui_composer` passes;
- todos are already native AVA UI: transcript tool cards, a persistent active-todo dock, a wide-layout sidebar, and session snapshot hydration;
- AVA already has selectors for models, scoped models, sessions/tree/resume, settings, themes, keybindings, permissions, tools, plugins, MCP, and jobs;
- AVA already has question/permission modals, tool cards and diffs, transcript search and selection, image paste/preview, external-editor handoff, follow-up/steering queues, reasoning controls, and a live subagent workspace;
- `display.json` currently persists the theme only;
- the session backend has branch-summary records and append helpers, but the TUI does not expose them and provider history does not consume them;
- plugin UI/render slots, plugin keybindings, and plugin themes are explicitly deferred by current plugin documentation.

Wave 0 rebaseline record (2026-08-03):

- `HEAD`, local `develop`, and `origin/develop` all remained exactly at the written baseline `f54f8d318fbcccd1d6c7c36c858c3755159922b1`; there were no intervening commits.
- The display, session, runtime/TUI, plugin, and permission seams named by this plan were re-audited and remain valid. The optional `display_reload.{h,cpp}` seam is absent because reload ownership remains in the existing TUI/application callbacks; the plan already qualifies those files with “if still present.”
- Historical note, now superseded: this rebaseline proposed a separate `onboarding_state.{h,cpp}` seam. That seam and the local wizard were later removed; `onboarding.{h,cpp}` remains ordinary disconnected auth guidance.
- `BranchSummaryOptions` still lacks explicit read limits and its preparation path still performs an unbounded source load; Wave 5's bounded exact-lease work remains required.
- The product-approval documentation was updated without presenting any expansion-plan capability as implemented.
- `scripts/build.sh --build-dir build` and the full default `scripts/run-tests.sh --build-dir build` baseline passed. Optional live-provider, dependency, tmux, and direct-terminal tests retained their normal skip gates; no paid provider call was used.

Wave 0 completed the required startup checklist by reading `AGENTS.md`, `docs/AGENTS.md`, and `docs/development/cpp-safety-rules.md`; inspecting the tree and baseline; preserving the incoming plan/index edits; re-auditing the named seams; and running the current build and test baseline. No material authority-model change required reordering the implementation waves.

## Product Decisions

These decisions resolve the ambiguous or unsafe parts of the initial idea.

### Native UI Versus Plugin Control

Core AVA owns:

- layout and geometry;
- curses and terminal output;
- themes and styling;
- keyboard and mouse input;
- focus and modal arbitration;
- editor contents;
- transcript rendering;
- settings navigation and previews;
- startup flows;
- image protocol emission;
- session/provider/config authority; and
- all filesystem, auth, session, and permission writes.

Plugins may request only:

- bounded, sanitized plain-text status updates;
- bounded, sanitized plain-text widgets in fixed host-owned placements; and
- one host-rendered select or confirm dialog during an eligible direct user command.

Plugins do not receive raw terminal control, arbitrary overlays, custom components, render callbacks, custom editors, custom headers/footers, arbitrary geometry, styles, themes, key handlers, transcript mutation, session authority, auth access, or provider interception.

### Branch Summary Semantics

This plan implements a narrow metadata-only abandoned-parent summary, not Pi-style carry-forward context.

The eligible relationship is exact:

- the active session is a direct branch of the selected source session;
- the active session has a valid `branch_from_entry_id` in that source;
- the selected source has a suffix after that fork point;
- the selected source is persistent, valid, and leased for the append operation; and
- the exact source suffix is not already covered by a matching final branch summary.

The range identity is exact and follows the existing inclusive branch-summary schema: generated content excludes `branch_from_entry_id` and includes the captured source tip; persisted `branch_root_entry_id` is the first source entry after the fork point; and persisted `branch_tip_entry_id` is the captured tip. If no entry follows the fork point, the action is ineligible. Duplicate detection keys on the exact final-summary identity `(source_session_id, branch_root_entry_id, branch_tip_entry_id)`, including summaries created through existing RPC/caller-supplied paths.

The generated `BranchSummary` is appended to the selected source session. It is visible in source-session/tree inspection but is not injected into the active provider context and is not presented as if the model already knew it.

Exact Pi carry-forward behavior is out of scope because it requires a separately reviewed cross-session schema and replay/export/provider-context contract. If product requirements change to carry the summary into the active conversation, stop this wave and create that session-versioning plan instead of encoding the summary as a fake user/assistant message or ephemeral context.

### Image Sizing

Image width is expressed in terminal cells. AVA preserves image aspect ratio using intrinsic image dimensions and an explicit cell-pixel model. It must not claim pixel-perfect sizing when the terminal does not expose reliable cell geometry.

This plan does not add unbounded or timing-sensitive terminal pixel queries. The renderer keeps a documented fallback cell geometry when exact values are unavailable. A future separately tested terminal capability may improve that estimate without changing the persisted cell-width contract.

### First-Run Product Decision

Wave 4's local setup wizard was removed and superseded by product decision. AVA retains its ordinary disconnected auth-first guidance (`! OpenAI not connected · /connect`) and continues to collect no telemetry. It does not read, write, migrate, or delete a local onboarding marker; stale files are ignored.

## Goals

- Deliver the five retained capabilities without weakening backend safety boundaries.
- Keep every wave independently buildable, testable, reviewable, and revertible.
- Reuse existing selectors, commands, snapshots, permission paths, and async-run machinery.
- Make all persisted configuration backward compatible and atomically updated.
- Keep preview state presentation-only until explicit confirmation.
- Keep provider work asynchronous, cancellable, bounded, and fake-provider testable.
- Keep plugin UI host-rendered, attributed, bounded, command-scoped, and unreachable from unapproved invocation paths.
- Add deterministic coverage first and real-terminal evidence for terminal-visible behavior.

## Non-Goals

- Do not copy Pi architecture or source.
- Do not add an in-process plugin ABI or shared-library plugins.
- Do not add arbitrary plugin terminal access or extension-supplied render code.
- Do not add plugin-controlled editor, header, footer, transcript, theme, or keybinding replacement.
- Do not add telemetry, analytics, remote update checks, or automatic provider login.
- Do not add a remote plugin marketplace or package manager.
- Do not rewrite all current TUI selectors into a new framework.
- Do not change the minimal AVA footer merely to imitate Pi.
- Do not make branch summaries active model context in this plan.
- Do not hide append ambiguity or automatically retry session mutations.
- Do not use paid live-provider calls for validation.

## Cross-Cutting Invariants

### Configuration

- All writes go through existing approved XDG config/state helpers.
- Files are bounded, validated, atomically replaced, owner-only where required, and never written by renderer code.
- Existing theme-only `display.json` remains valid.
- Field-specific updates preserve every other recognized field.
- Unknown top-level JSON fields are preserved on a successful field update for forward compatibility, subject to the existing bounded document limit.
- A malformed recognized field prevents a destructive rewrite and produces an actionable error.
- Reset removes only the selected key; it does not replace the document.
- Runtime reload retains the last known-good presentation when a hand-edited file becomes invalid.

### TUI Authority

- TUI snapshots contain presentation data, identifiers, and bounded labels only.
- TUI callbacks never receive or retain session leases, stores, auth secrets, plugin processes, or provider credentials.
- Application code re-resolves identifiers immediately before authority-sensitive work.
- Worker threads communicate through existing runtime events/snapshots; they never call curses directly.
- Modal focus, cancellation, and cleanup remain host-owned.

### Sessions And Providers

- Current-runtime history reads use `SessionReadAuthority` and explicit `SessionReadLimits`.
- Persistent appends use the exact active/duplicated lease and preserve `append_commit_state`.
- Pre-append validation failures leave bytes unchanged.
- Post-write errors may be `partial_or_unknown` or `committed_to_leased_inode`; the UI must report the stable state and never auto-retry.
- One absolute deadline covers branch acquisition, provider generation, stale-tip verification, and append preparation where practical.
- Provider errors remain sanitized and never expose raw HTTP/SSE bodies or provider payloads.

### Terminal Safety

- Treat all labels, plugin text, paths, terminal input, and config strings as untrusted.
- Sanitize control characters before width calculation and rendering.
- Never pass plugin bytes through as ANSI, OSC, DCS, hyperlinks, cursor movement, or terminal titles.
- Keep layout bounded at narrow widths and short terminal heights.
- Preserve terminal restoration after normal exit, cancellation, suspend/resume, and failure.

## Wave 0 — Rebaseline And Record Approval

**Status: Complete — 2026-08-03.** This wave changed documentation only; it did not itself implement any proposed user-facing capability.

### Work

- Reconcile this plan with current `develop`.
- Confirm the display, session, runtime, plugin, and permission seams named below still exist.
- Historical approval record, now superseded in part: the user originally approved a local-only setup wizard and a constrained host-rendered plugin UI protocol. The wizard approval was later revoked; the plugin UI remains.
- Keep current product docs truthful: planned features must not be described as available before their landing wave.

### Likely Files

- `docs/history/tui-pi-feature-expansion.md`
- `docs/roadmap/frontend.md`
- `docs/goals/pi-mvp-parity/context-extensions-mcp-lsp.md`
- `docs/extensions/plugin-system.md`
- `docs/plugin-compatibility-policy.md`

### Completion Gate

- Baseline build/tests pass.
- No current descriptive documentation claims unimplemented behavior.
- The implementation order and branch-summary metadata-only scope remain explicit.

### Rollback

Documentation-only; revert the approval update without changing runtime behavior.

## Wave 1 — Typed Display Settings And Image Controls

**Status: Complete — 2026-08-03.** Typed `display.json` persistence, image visibility/width controls, snapshot plumbing, commands/settings actions, focused CTest coverage, and opt-in Kitty PTY suppression evidence are landed in this tree.

### Wave 1 evidence

- `scripts/build.sh --build-dir build` succeeded after the Wave 1 implementation.
- Focused `scripts/run-tests.sh --build-dir build -R 'ava_tests\.(app_runtime|tui_composer)'` passed (2/2).
- Opt-in Kitty and iTerm2 image PTY smokes passed, covering enabled graphics emission plus `/images off` persistence and no graphics payload on a later attach.
- The complete opt-in 18-scenario tmux TUI wave passed.
- The full default CTest suite passed 122/122 after the expansion plan was added to every exact package/install documentation allowlist.
- Integrated review finding `WV1-001` (unsynchronized effective-display snapshot state) was fixed with one narrow mutex and delta-verified with no remaining material finding.
- `git --no-pager diff --check` was clean for the Wave 1 edits.
- No paid/live provider calls were used.

This wave establishes the persistence and runtime foundation used by later settings UI.

### Data Model

Extend the display document with optional recognized fields:

```json
{
  "theme": "dark",
  "show_images": true,
  "image_width_cells": 60
}
```

Defaults when keys are absent:

- `theme`: existing automatic/current behavior;
- `show_images`: `true`, preserving current behavior;
- `image_width_cells`: `60`, matching the current effective maximum unless the current implementation has changed.

Choose and document a conservative inclusive width range after checking current layout limits. The initial target is 8–160 cells, always clamped again to the available viewport. Reject booleans, strings, floating-point values, negatives, overflow, and out-of-range integers for `image_width_cells`.

### Persistence Design

Replace theme-only whole-file replacement with one `DisplaySettingsDocument` read/validate/update/serialize path.

Every setter must:

1. open and bounded-read the current document through approved config helpers;
2. require a top-level object;
3. validate every recognized field;
4. preserve unknown fields verbatim within the bounded document;
5. update or erase only its owned field;
6. atomically write the resulting object; and
7. refresh the display watcher only after successful persistence.

Required operations:

- set/reset theme;
- set/reset image visibility;
- set/reset image width;
- read validated effective settings;
- reload while retaining last known-good state on error.

Changing theme must never erase image fields. Changing image settings must never erase theme. Resetting one setting must never reset another.

### Runtime And Rendering

Carry effective image settings through the application-owned runtime snapshot.

When `show_images` is false:

- do not load attachment bytes for preview;
- do not prepare Kitty/iTerm2 payloads;
- do not emit graphics protocol bytes;
- keep safe textual attachment metadata; and
- clear any previously emitted overlay through the existing host cleanup path.

When enabled:

- use the configured cell width as a maximum;
- clamp to the current content width;
- preserve intrinsic aspect ratio;
- use validated image dimensions and the documented cell-pixel fallback; and
- retain current text-only behavior for unsupported terminals, tmux/plain mode, or `NO_COLOR`.

The renderer does not write config. Add application/backend commands or callbacks that own persistence and return a refreshed snapshot.

### Likely Files

- `src/ava/app/tui/tui_display_settings.{h,cpp}`
- `src/ava/app/display_reload.{h,cpp}` if still present
- `src/ava/app/tui/interactive_tui.cpp`
- `src/ava/tui/runtime.h`
- `src/ava/tui/composer.h`
- `src/ava/tui/runtime_actions_internal.cpp`
- `src/ava/tui/terminal_image.{h,cpp}`
- `src/ava/tui/composer.cpp`
- `tests/app_runtime_tests.cpp`
- `tests/tui_terminal_input_tests.cpp`
- `tests/tui_composer_rendering_tests.cpp`
- `tests/tui_composer_tests.cpp`
- `docs/interfaces/themes-keybindings.md`
- `docs/operations/terminal-setup.md`
- relevant schema/config documentation if `display.json` has a schema by implementation time

### Deterministic Tests

- Theme-only legacy file uses image defaults.
- Empty/missing file uses all defaults.
- Alternating theme/image setters preserve every field.
- Each reset removes only its key.
- Unknown fields survive successful updates.
- Invalid known field types/ranges reject writes and reload.
- Last known-good runtime state survives malformed external edits.
- Disabled images perform no attachment-byte preview load.
- Disabled images emit no Kitty/iTerm2 graphics bytes and clear prior overlays.
- Width clamps at configuration and viewport boundaries.
- Aspect ratio remains stable for portrait, landscape, tiny, and extreme dimensions.
- Text fallback remains visible when graphics are disabled or unavailable.

### Real-Terminal Evidence

Add isolated tmux/PTY scenarios for:

- `/settings` or command-level image disable/enable persistence;
- no graphics emission while disabled;
- width persistence and restart;
- Kitty/iTerm2 protocol suppression through the existing opt-in PTY harness.

### Completion Gate

- Existing users see no behavior change until they change a setting.
- No field setter can destroy another field.
- Focused display/image tests and existing terminal image tests pass.
- Current 18 tmux scenarios remain green.

### Rollback

Remove the two optional fields and command rows; legacy theme behavior remains readable because new keys are optional and current defaults preserve old behavior.

## Wave 2 — Nested Unified Settings With Reversible Preview

**Status: Complete — 2026-08-03.** Shallow nested `/settings` root sections with section-local filter/back stack, Theme/Display highlight previews that never write config, Enter confirm through app authority, Esc/replacement/error/shutdown restore of authoritative presentation, external reload rebase+reapply, app-delivered custom theme palette/revision (no renderer FS reads), and no attachment load during image preview are landed in this tree. Existing model/reasoning/keybinding/trust/tool/extension routes remain reachable from nested sections.

### Wave 2 evidence

- Fixed `DisplayPreviewOverlay` designated-init `-Wmissing-field-initializers` and completed the interrupted compile/test loop.
- `scripts/build.sh --build-dir build` succeeded.
- Focused `scripts/run-tests.sh --build-dir build -R 'ava_tests\.(app_runtime|tui_composer)'` passed (2/2), covering nested section inventory, section-local filter, 8–12 row visibility + mouse hit testing, preview begin/update/cancel/rebase/confirm, NO_COLOR over preview, invalid custom theme rejection, and image overlay without attachment bytes.
- Opt-in focused tmux scenarios `theme_env`, `theme_persisted`, `nested_settings_preview`, and `main_startup_trust_keybinds` passed.
- Complete opt-in 19-scenario tmux TUI wave passed (`AVA_TUI_TMUX_SMOKE=1 ... --jobs 19 -R '^ava_tui\.tmux_smoke_'`), including new `ava_tui.tmux_smoke_nested_settings_preview`.
- Full default CTest passed 123/123 with expected opt-in/live skips.
- Docs link/structure CTests passed (`markdown_link_verifier`, `markdown_links_source`, `documentation_structure_checker`, `documentation_structure_source`).
- `git --no-pager diff --check` was clean for the Wave 2 edits after formatting.
- No paid/live provider calls were used.

### Wave 2 review findings

| ID | Severity | Status | Summary |
| --- | --- | --- | --- |
| W2-001 | medium | Fixed and validated | Applied display reload no longer infers change from post-hydration presentation equality under an active overlay. App optional snapshot remains the applied/unchanged signal; TUI maps it to `DisplaySettingsReloadPollOutcome` (`TerminalFailure` / `Unchanged` / `Applied`). On `Applied`: capture the selected actionable row hidden `value` and staged overlay token before rebuild, hydrate without final render, always rebase settings preview, refresh staged candidates from app-delivered options (keep last-known-good when the token no longer resolves), rebuild Display rows preserving query, reselect by exact hidden value via `reselect_settings_display_row_after_rebuild` (fallback: staged overlay action if still present, else clamp prior index/non-action rules), re-align highlight overlay to the restored row, then render once. Active-run callers with no settings preview also render applied reloads. |
| W2-002 | medium | Fixed and validated | Application-owned display watch tracks a bounded deterministic validated custom-theme catalog (stable name order), not only the configured custom theme. Discovery uses one no-follow descriptor open+fstat+bounded-read path per candidate with caller-supplied remaining aggregate budget (`min(64KiB per file, remaining 256KiB aggregate)`), rejects symlinks/special files, never reads past `max_bytes` (no post-budget probe byte), classifies complete vs truncated from pre/post descriptor metadata only: below-cap complete requires `pre_size == post_size == bytes_read`, exact-cap complete requires stable pre/post equal to `max_bytes`, and any growth/shrink/inconsistent metadata is fail-closed Truncated while still charging physical `bytes_read` (pre-fstat known-oversized may charge 0 without reading), orders by normalized absolute path, keeps first valid file per name for listing/catalog prefix, and fail-closes configured/named load on duplicates **or incomplete discovery** (candidate cap / aggregate budget) even when one match appeared before the boundary. `load_tui_display_settings_watch_state` performs one discovery and reuses it for configured custom resolution and catalog fingerprint. Public `discover_tui_custom_themes` exposes complete/incomplete reason and `aggregate_bytes_read` for observable bounds. Conservative app constants unchanged: `kMaxTuiCustomThemeFileBytes=64KiB`, `kMaxTuiCustomThemeCandidates=64`, `kMaxTuiCustomThemeCatalogAggregateBytes=256KiB`. Valid edits to an unconfigured previewed custom theme trigger reload, deliver updated palette/revision through `TuiRuntimeStateSnapshot.custom_themes`, and refresh the staged overlay. Invalid/oversized/unreadable unconfigured files are skipped and cannot break configured built-in display reload; they drop from the catalog, retain last-known-good overlay when previously staged, cannot be newly selected, and confirm fails closed without rewriting `display.json`. Configured-invalid remains fail-closed and never becomes authoritative. Renderer stays FS-free; app watch/effective lock order is unchanged. |

Post-fix validation (W2-001/W2-002 only; no Wave 3):

- `scripts/build.sh --build-dir build` succeeded after clang-format of touched sources.
- Focused `scripts/run-tests.sh --build-dir build -R 'ava_tests\.(app_runtime|tui_composer)'` passed (2/2), including equal-value applied-reload rebase + Esc authority restore, value-stable Display reselect after custom-theme insertion (overlay/Enter stay aligned), controller hydrate-then-stage-then-render ordering, unconfigured custom-theme catalog watch, invalid retain/fail-closed confirm, oversized/candidate-cap/aggregate-budget/duplicate-order boundary coverage, and no-write-on-highlight coverage.
- Focused opt-in `ava_tui.tmux_smoke_nested_settings_preview` passed, including unconfigured custom-theme catalog reload during preview and invalid-theme non-persistence.
- Complete opt-in 19-scenario tmux TUI wave passed (`AVA_TUI_TMUX_SMOKE=1 ... --jobs 19 -R '^ava_tui\.tmux_smoke_'`).
- Full default CTest passed 123/123 with expected opt-in/live skips.
- Docs link/structure CTests passed.
- `git --no-pager diff --check` clean.
- No paid/live provider calls were used.

Remaining W2-002 re-fix (bounded reader budget + incomplete fail-closed + single watch discovery; no Wave 3, no commit):

- `clang-format` on touched sources; `scripts/build.sh --build-dir build --target ava_tests` succeeded.
- Focused `scripts/run-tests.sh --build-dir build -R 'ava_tests\.(app_runtime|tui_composer)'` passed (2/2), including exact per-file cap acceptance, observable `aggregate_bytes_read <= 256KiB` with `incomplete_reason=AggregateBudget`, late-duplicate-after-candidate-cap named-load fail-closed (`candidate_cap`), aggregate-incomplete named-load fail-closed, and watch catalog/configured revision consistency with one discovery snapshot.
- Focused opt-in `ava_tui.tmux_smoke_nested_settings_preview` passed.
- Docs link/structure CTests passed (`markdown_link_verifier`, `markdown_links_source`, `documentation_structure_checker`, `documentation_structure_source`).
- `git --no-pager diff --check` clean for the re-fix edits.
- No paid/live provider calls were used. Full default/complete tmux waves not re-run; residual risk limited to unexercised cross-suite interactions outside focused app_runtime + nested settings smoke.

Final W2-002 bounded-reader re-fix (no post-budget probe; charge truncated work; no Wave 3, no commit):

- Removed the `max_bytes+1` probe byte. Complete vs truncated classification uses pre/post `fstat` only; growth/shrink is fail-closed; exact 64 KiB stable files remain Complete.
- Discovery charges `bytes_read` before skip/incomplete decisions so truncated candidates cannot under-report aggregate work. Known-oversized pre-fstat rejects may still charge 0 without reading content.
- Tests cover exact-cap acceptance with charged bytes and static +1 oversize rejection (later R1/R2 removed the probabilistic inotify growth race).
- `clang-format` on touched sources; `scripts/build.sh --build-dir build --target ava_tests` succeeded.
- Focused `scripts/run-tests.sh --build-dir build -R 'ava_tests\.app_runtime'` passed (1/1).
- Docs link/structure CTests passed; `git --no-pager diff --check` clean.
- No paid/live provider calls were used.

W2-002-R1/R2 re-fix (below-cap triple-equality + remove probabilistic race; no Wave 3, no commit):

- **R1 fixed:** below-cap completion now requires `pre_size == post_size == bytes_read` (not post-size alone). Any growth/shrink/inconsistent descriptor metadata is Truncated/fail-closed. Exact-cap rule and physical-byte charging unchanged.
- **R2 fixed:** removed the blocking/probabilistic inotify `IN_ACCESS` growth race test and its unused includes/helpers (`atomic`, `thread`, `fcntl`, `inotify`, `unistd`). No production-only hook and no scheduler race added. Deterministic boundary coverage retained/added: below-cap valid, exact-cap valid, known oversize (+1 and large), aggregate budget, incomplete/late-duplicate fail-closed. Mutation between pre/post `fstat` has no normal production seam for a deterministic test; the algorithm is conservatively evident from the classification predicates above.
- `clang-format` on touched sources; `scripts/build.sh --build-dir build --target ava_tests` succeeded.
- Focused `scripts/run-tests.sh --build-dir build -R 'ava_tests\.app_runtime'` passed (1/1).
- Docs link/structure CTests passed (`markdown_link_verifier`, `markdown_links_source`, `documentation_structure_checker`, `documentation_structure_source`).
- `git --no-pager diff --check` clean for the R1/R2 edits.
- No paid/live provider calls were used. |

### Navigation Model

Evolve `/settings` into a native root selector with shallow, explicit sections rather than replacing every select-list implementation.

The original Wave 2 root sections were:

- Display
- Models And Reasoning
- Input And Keybindings
- Sessions And Workspace
- Tools And Extensions
- Privacy
- About

A later usability pass simplified the shipped root to Theme, Display, Model, Input, Workspace, and Tools. Theme choices moved out of Display; informational Privacy/About and status-dashboard rows left the settings panel. Empty search chrome is hidden until typing, and root rows are label-only. The same shallow navigation, reversible preview, and application-authority boundaries remain in force.

Rows reuse existing commands/selectors for models, scoped models, reasoning, keybindings, trust, permissions, tools, and plugin/MCP/LSP diagnostics. Backend semantics remain outside TUI reducers.

Add a small settings navigation stack:

- Enter opens a section or executes a row.
- Esc returns to the parent section before closing settings.
- Search/filter applies only to the current section.
- Selection remains visible at 8–12 row heights.
- Mouse hit testing uses the same rendered row window.
- Existing selector keybindings outside settings remain unchanged.

### Preview Transaction

Highlighting a previewable row must never write config.

Introduce presentation-only preview state with:

- the latest authoritative display snapshot;
- an optional validated preview overlay;
- the watcher generation or equivalent reload identity; and
- an explicit lifecycle: begin, update, confirm, cancel/rebase.

Rules:

1. Highlight applies only a TUI palette/image candidate.
2. Confirm persists exactly once through the application-owned writer.
3. Esc, interruption, selector replacement, error, or shutdown removes the preview overlay.
4. Restoration uses the latest authoritative snapshot, not a stale snapshot captured before an external edit.
5. If automatic reload occurs during preview, rebase the authoritative baseline and then reapply the preview overlay; do not silently discard the external edit.
6. Custom theme preview uses already parsed/validated palette data, never an untrusted path or raw theme JSON inside the renderer.
7. Preview never loads attachment bytes, invokes a provider, launches a plugin, mutates a session, or writes any file.

Initial live previews:

- theme;
- image visibility; and
- image width against existing safe preview metadata/layout only.

Other settings may remain confirm-only until they have equally safe reversible semantics.

### Likely Files

- `src/ava/tui/runtime_views.cpp`
- `src/ava/tui/runtime.h`
- `src/ava/tui/runtime_state_internal.h`
- `src/ava/tui/runtime_actions_internal.cpp`
- `src/ava/tui/composer.{h,cpp}`
- `src/ava/app/tui/interactive_tui.cpp`
- `src/ava/app/command_palette.cpp`
- `tests/tui_composer_tests.cpp`
- `tests/app_runtime_tests.cpp`
- `tests/tui_tmux_scenarios/`
- `docs/core/usage.md`
- `docs/interfaces/themes-keybindings.md`

### Deterministic Tests

- Root/section/back-stack navigation.
- Section-scoped filtering.
- Narrow-height selected-row visibility.
- Mouse and keyboard selection agreement.
- Zero config writes while highlighting.
- One write on confirmation.
- Escape/interruption/error restores the authoritative state.
- External reload during preview rebases correctly.
- Invalid custom theme cannot become a preview candidate.
- Existing model, theme, keybinding, trust, and extension rows still reach their existing authoritative actions.

### Real-Terminal Evidence

Add isolated tmux scenarios for:

- nested settings navigation at normal and short heights;
- theme preview followed by Escape;
- theme preview followed by confirmation and restart;
- external display-file edit during preview;
- image visibility/width preview and persistence; and
- mouse selection in nested sections.

### Completion Gate

- The settings hierarchy is discoverable and bounded.
- Preview has no persistence before confirmation.
- Cancellation restores exactly the latest authoritative display state.
- Existing selector workflows remain green.

### Rollback

Retain Wave 1 commands/settings and restore the flat settings list. No config migration is needed.

## Wave 3 — Expandable Startup Overview

**Status: Implemented.**

### Wave 3 evidence

- App-owned `StartupOverviewSnapshot` builder in `src/ava/app/tui/startup_overview.*` consumes only already-loaded in-memory resources (session context/freshness spans, effective keybindings by const pointer, active theme). No TUI FS reopen, session JSONL read/append, provider/plugin/MCP/LSP invocation, catalog/session-tree scan/copy, or prompt/secret/path leakage.
- Collapsed chrome: 2 rows at height ≥12, 1 row at 8–11, hidden below 8; quiet footer unchanged. Expanded view is the host-owned read-only `overview_select_list_view` select-list.
- `/overview` exact submit bypass + unbound `app.overview.toggle` + mouse hit region on the collapsed card. Process-local only; modal replacement/session-switch/prompt acquisition/exit clear expanded state. Shared snapshot sync rebuilds an open overview list in place for idle, active-run, and periodic display-reload paths.
- Session titles, raw session ids, and free-form/prompt-derived session text are omitted entirely (W3-PRIV-001).
- MCP/LSP rows omitted (not retained path-free). Plugin failure counts use the stable dual-zero freshness contract from failed plugin resource loads only.
- First-64 input aggregation never renders exact-looking partial group counts: DTO carries `count_is_lower_bound` / `plugin_resource_failure_count_is_lower_bound`; capped instruction/freshness groups and plugin-failure counts render `N+` (including truthful `0+` when shown). Instruction-source total stays exact via O(1) span size. Named-list truncation markers are unchanged.
- Focused checks: `ava_tests.app_command_classification` / `ava_tests.app_runtime` (bounds/order/dedupe/redaction/key hints/UTF-8/lower bounds), `ava_tests.tui_composer` (0/1/2-row chrome + short select-list + overview sync/close + display-reload rebuild), opt-in `ava_tui.tmux_smoke_startup_overview`.

### Wave 3 review findings

| ID | Severity | Status | Summary |
| --- | --- | --- | --- |
| W3-PRIV-001 | high | Fixed and validated | Free-form session titles/ids/origins removed from startup overview entirely; collapsed and expanded surfaces never show session/user content. |
| W3-BOUND-002 | medium | Fixed and validated | First-64 aggregation never renders exact-looking partial counts. DTO carries `count_is_lower_bound` / `plugin_resource_failure_count_is_lower_bound`; capped instruction/freshness groups and plugin-failure counts render `N+` (including truthful `0+` when shown). Instruction-source total stays exact via O(1) span size. Named-list truncation unchanged. |
| W3-ACTIVE-003 | medium | Fixed and validated | Every applied periodic display reload routes through `apply_runtime_state_snapshot_with_overview_sync(..., active_select_list_)` so an expanded overview rebuilds immediately from the refreshed theme/startup DTO while preserving query/selection. Outer idle path still owns Wave 2 settings-preview rebase + single final paint; active-run still renders once on Applied. |
| W3-UTF8-004 | medium | Fixed and validated | Overview label truncation is UTF-8/codepoint-safe with in-budget ellipsis; crossing-boundary tests cover multibyte, invalid, and control bytes. |
| W3-SELECTION-005 | medium | Fixed and validated | Overview hit exclusion retained for new presses; owned transcript drags treat overview chrome as the upper autoscroll edge. |

Post-fix validation (remaining W3-BOUND-002 / W3-ACTIVE-003 only; no Wave 4, no commit):

- `clang-format` on touched sources; `scripts/build.sh --build-dir build --target ava_tests` and `--target ava` succeeded.
- Focused `scripts/run-tests.sh --build-dir build -R 'ava_tests\.(app_command_classification|app_runtime|tui_composer)'` passed (3/3).
- Focused opt-in `ava_tui.tmux_smoke_startup_overview` passed (after relinking `ava`).
- `git --no-pager diff --check` clean for touched sources/docs.
- No paid/live provider calls were used. Nested-settings tmux not required (settings preview path untouched beyond existing outer idle rebase).

### Snapshot Ownership

Build one bounded, path-safe overview snapshot in application code from resources already loaded and validated for the runtime. Do not let the TUI reopen resource paths.

The snapshot may include:

- active model and mode;
- project trust state;
- counts and safe display labels for global/project instruction sources (exact O(1) total; first-N group aggregates may be lower bounds `N+`);
- counts/names for system, base, and append prompt resources without prompt contents;
- loaded skill and prompt-command names;
- enabled plugin names and bounded failure counts without launching plugins (failure counts may be lower bounds `N+` / `0+` when freshness input was capped);
- configured MCP/LSP counts/statuses already known at startup;
- resumable session/current branch summary metadata already available to the app;
- active theme; and
- essential key hints derived from effective keybindings.

Never include:

- prompt contents;
- credential values;
- environment values;
- raw provider/plugin errors;
- absolute private paths;
- session JSON payloads;
- plugin-produced terminal text; or
- unsanitized external labels.

### Presentation

Add a native collapsed startup card/banner that consumes a small fixed number of rows and can be expanded into a read-only host-owned view.

- `/overview` toggles or opens the full view.
- Add an unbound/customizable `app.overview.toggle` action unless a conflict-free default is approved during implementation.
- Mouse activation may be supported through the same rendered hit region.
- The collapsed state should show only a compact summary and the `/overview` hint.
- The expanded view uses existing list/viewport behavior and remains usable at short heights.
- Overview state is process-local presentation state. It is not appended to session JSONL, exported, sent to providers, or restored as conversation content.

### Likely Files

- `src/ava/app/tui/interactive_tui.cpp`
- `src/ava/app/command_catalog.cpp`
- `src/ava/app/command_registry.cpp`
- `src/ava/tui/runtime.h`
- `src/ava/tui/composer.h`
- `src/ava/tui/runtime_views.cpp`
- `src/ava/tui/composer.cpp`
- `src/ava/tui/keybindings.cpp`
- `tests/app_runtime_tests.cpp`
- `tests/tui_composer_tests.cpp`
- `tests/tui_tmux_scenarios/`
- `docs/core/usage.md`

### Tests

- Snapshot bounds and deterministic ordering.
- Sanitization and path/secret/prompt-content redaction.
- No session append or provider/plugin invocation.
- Collapsed/expanded rendering at narrow widths and short heights.
- Key hints reflect configured bindings.
- Reloaded resources update only through an application-provided refreshed snapshot.
- Real-terminal `/overview`, scrolling, close, resize, and mouse behavior.

### Completion Gate

- Startup remains compact by default.
- Expanded information is useful but contains no sensitive contents or raw paths.
- No backend authority moves into TUI code.

### Rollback

Remove the card/view/command; no persistent state or session migration exists.

## Wave 4 — Local-Only First-Run Setup Wizard (Removed)

**Status: Removed and superseded by product decision.** This wave is not implemented and is not a current product capability.

The former implementation and plan were removed completely: there is no wizard UI, automatic or deferred open, `/setup` command, Privacy setup row, onboarding-state read/write/eligibility logic, setup-only credential-presence probe, or wizard test scenario. AVA does not inspect or delete stale `$XDG_STATE_HOME/ava/onboarding.json` files; they are simply ignored.

The product retains `src/ava/app/onboarding.{h,cpp}` and the ordinary disconnected auth-first guidance shown by TUI and line-shell paths, including `! OpenAI not connected · /connect`. Generic `/settings` display/theme preview transactions and reload behavior also remain.

Historical Wave 4 implementation evidence is superseded and must not be used as a current capability claim. The rollback described by the former plan is now the landed product decision.

## Wave 5 — Abandoned-Parent Branch Summary Action

**Status: Implemented.** The metadata-only action described below is the landed contract.

### Eligibility

The application computes eligibility from fresh session metadata. The TUI receives only bounded labels and an eligibility reason.

An action is eligible only when:

- current and selected sessions are persistent;
- selected session is the current session's exact direct source/parent;
- the current branch has a valid `branch_from_entry_id` in the selected source;
- the selected source has a valid bounded suffix after that fork point;
- the source is not corrupt, replaced, or leased incompatibly;
- the suffix is not already covered by a final summary for the same root/tip identity; and
- no active run or conflicting branch-summary operation exists.

Do not offer arbitrary sibling, descendant, imported, ephemeral, or unrelated session summarization in this wave.

### Provider And Session Flow

The selector callback stages only stable IDs and returns control to the renderer.

Use the existing asynchronous active-run/worker machinery or a narrowly equivalent cancellable application task:

1. TUI asks for confirmation with immutable host wording naming the selected source.
2. Application captures current ID, selected source ID, the first post-fork root ID, expected source tip ID, and exact leased inode identity.
3. Recover the non-current source through `runtime::Session::recover_source_for_mutation` or its current exact-lease equivalent; retain that recovered lease through read, generation, revalidation, and append.
4. Extend `BranchSummaryOptions` and the preparation/append path to require explicit `SessionReadLimits` and use `load_bounded` rather than the current unbounded summary-helper load.
5. Read through the recovered source lease and build the provider prompt from the range exclusive of `branch_from_entry_id` and inclusive of the captured tip.
6. Generate through the selected model/provider with normal cancellation, sanitized errors, output bounds, and one finite deadline.
7. Re-resolve the relationship, inclusive root/tip identity, leased inode, and eligibility immediately before append without discarding the retained mutation authority.
8. Reject stale/replaced sources before append.
9. Append one `BranchSummary` to the selected source with provider/model/reason/actor metadata through the recovered source lease or existing current owner route.
10. Refresh the tree only after a committed append result.

Pre-append failures leave source bytes unchanged. Append failures expose the stable `append_commit_state`. Never auto-retry an ambiguous or committed-to-inode append error.

The action does not create a normal user/assistant turn and does not inject summary text into active provider context.

### UI

- Add a session-selector action and discoverable key hint.
- Prefer an unbound/customizable action ID or a selector-local key proven conflict-free against current bindings.
- Require an explicit host-owned confirmation before provider work.
- Show cancellable progress without freezing curses.
- On success, show the summary metadata in the selected source's detail/tree view.
- On ineligible selection, show the specific bounded reason.
- On append ambiguity, show the commit state and instruct the user to inspect rather than retry.

### Likely Files

- `src/ava/session/session_branch.{h,cpp}`
- `src/ava/session/session_store_append.cpp`
- `src/ava/app/runtime/Session.h`
- `src/ava/app/runtime.h`
- `src/ava/app/commands.h`
- `src/ava/app/tui/interactive_tui.cpp`
- `src/ava/app/command_sessions.cpp`
- `src/ava/tui/runtime.h`
- `src/ava/tui/composer.h`
- `src/ava/tui/runtime_actions_internal.cpp`
- `src/ava/tui/runtime_views.cpp`
- `src/ava/tui/keybindings.cpp`
- `tests/session_tree_tests.cpp` through the existing `tests/session_tests.cpp` aggregate
- `tests/app_runtime_tests.cpp`
- `tests/tui_composer_tests.cpp`
- `tests/tui_tmux_scenarios/`
- `docs/session-format.md` only if descriptive branch-summary behavior changes without a schema change
- `docs/core/usage.md`

### Deterministic Tests

- Every eligibility rule and disabled reason.
- Exact content range excludes the fork entry, begins at persisted `branch_root_entry_id`, and includes persisted `branch_tip_entry_id` at the captured tip.
- Duplicate suppression uses exact `(source_session_id, branch_root_entry_id, branch_tip_entry_id)` final-summary identity across TUI and existing caller-supplied paths.
- `BranchSummaryOptions` requires explicit read limits and the helper path performs no unbounded source load.
- Non-current mutation uses the recovered exact source lease through final append.
- Fake provider receives only expected bounded branch content.
- One summary is appended to the exact leased source session.
- Existing matching summary suppresses duplicate action.
- Cancellation, deadline, oversized output, auth/model failure, provider failure, corrupt source, stale tip, inode/path replacement, and relationship changes all fail before append with unchanged bytes.
- Append commit states are surfaced and never retried.
- Provider context projection remains unchanged and does not consume metadata summaries.
- Curses remains responsive during generation.

### Real-Terminal Evidence

Add an isolated tmux scenario using the fake provider that:

- creates a parent and direct child branch;
- leaves a suffix on the parent;
- opens the tree and selects the eligible parent;
- confirms summarization;
- observes progress and completion;
- verifies source detail visibility; and
- verifies the active conversation did not receive a synthetic turn.

### Completion Gate

- Semantics are explicitly metadata-only.
- Authority remains in session/app code.
- Provider work is async and cancellable.
- All pre-append failures are byte-for-byte nonmutating.
- Append ambiguity is reported, latched where required, and never retried.

### Wave 5 evidence and review ledger

- The implementation is the exact metadata-only abandoned-parent summary: bounded read limits; a recovered source lease retained through append; exact stale/duplicate checks; one async cancellation path and absolute deadline; provider-context exclusion; and no automatic append retry.
- Focused build/tests, focused sanitizer coverage, and the isolated credential-free `branch_summary` tmux scenario passed during implementation. No paid provider was used.

### Rollback

Remove the selector/action/provider wiring. Existing branch-summary schema/helpers and RPC compatibility remain unchanged.

## Wave 6 — Narrow Host-Rendered Plugin UI

**Status: Implemented.** This was the highest-risk wave and landed after the native settings/modal/event seams were proven.

### Protocol Scope

Extend `ava.plugin.v1` additively. Existing plugins that do not declare UI capability continue unchanged.

Optional capabilities:

- `ui.status`
- `ui.widget`
- `ui.select`
- `ui.confirm`

Recommended initial host limits, subject to reduction during implementation review:

- one active status per invocation, at most 256 UTF-8 bytes;
- at most two active widgets per invocation;
- at most eight lines and 2 KiB per widget;
- at most four plugin widgets, 32 lines, and 8 KiB globally;
- one outstanding modal globally;
- at most 32 choices;
- at most 256 UTF-8 bytes per option label/description;
- at most 8 KiB total modal payload; and
- deadline bounded by the direct command deadline with a stricter UI maximum, initially 120 seconds.

All limits are checked before allocation/rendering and after UTF-8/control sanitization where relevant. Use existing bounded JSON depth/frame limits and fail closed on duplicate IDs, malformed records, flooding, or out-of-order replies.

### One-Command TUI Capability

Do not infer frontend authority from the presence of a question resolver.

Introduce a TUI-minted, one-command interaction capability that is:

- created only for a direct foreground user `/plugin run <plugin> <command>` submission;
- bound to exact plugin ID, command name, invocation ID, deadline, and current TUI runtime;
- nonserializable and nonreusable;
- consumed/closed when the command completes, fails, times out, is canceled, or the child exits; and
- absent from every RPC, ACP, print/headless, model tool, event hook, background job, queued synthetic submission, and plugin-to-plugin path.

`CommandRequest` or a narrower plugin command context may carry an opaque host-owned capability, but all constructors default to no UI authority. RPC's translation to `/plugin run` must remain incapable of minting or forwarding it. Tests must enumerate every invocation path.

Require:

- declared manifest capability;
- normal plugin execution/command permission;
- explicit host policy for `plugin.ui.present`; and
- interactive foreground/idle eligibility.

Do not permit persistent UI authority to be inferred from a previous run.

### Host Rendering And Anti-Spoofing

Every plugin UI surface has immutable host chrome containing:

- `Plugin` label;
- canonical plugin ID;
- command name; and
- cancel/timeout hint where relevant.

Plugin-controlled fields are plain labels, descriptions, option IDs, and status text only.

Reject or sanitize:

- C0/C1 controls, ESC, DEL, OSC/DCS sequences;
- carriage-return/newline abuse outside host line arrays;
- bidi overrides/isolates that can spoof host attribution;
- invalid UTF-8;
- ANSI styling and hyperlinks;
- terminal titles/cursor instructions;
- duplicate/empty/oversized IDs; and
- text resembling host-owned permission/auth chrome where a fixed prefix cannot disambiguate it.

The host owns all colors, borders, focus, dimensions, scrolling, keybindings, confirmation wording, and selected-state rendering.

No free-form or secret text input is allowed in v1. Plugin dialogs cannot grant AVA permissions, approve auth, choose filesystem paths, or request credential entry.

### Invocation Rules

Select/confirm is allowed only when:

- a direct user plugin command holds the exact capability;
- the TUI is foreground and interactive;
- no conflicting host modal is active;
- one modal is outstanding;
- the command and UI deadlines remain live; and
- the request passes capability, permission, attribution, and bounds checks.

Select/confirm is rejected from:

- model-dispatched plugin tools;
- plugin event hooks;
- startup/background work;
- task/subagent background jobs;
- queued synthetic submissions;
- RPC/ACP/print/headless mode; and
- commands without the exact invocation capability.

Statuses/widgets are command-scoped in v1. They are cleared on success, failure, timeout, cancellation, child exit, plugin disable/restart, session transition, TUI shutdown, or capability loss. They never enter session history, exports, provider context, diagnostics with raw content, or RPC payloads.

### Threading And Events

Plugin reader/worker threads never touch curses or TUI state directly.

- Runner validates and converts protocol records into bounded internal host events.
- Application checks invocation capability and policy.
- TUI reducer applies the latest allowed state on its own thread.
- Replies use the existing strict runner framing path.
- Backpressure/flooding is bounded; stale updates may be coalesced, but modal requests cannot be silently reordered.

### Likely Files

- `src/ava/plugin/manifest.{h,cpp}`
- `src/ava/plugin/runner_protocol.{h,cpp}`
- `src/ava/plugin/runner.{h,cpp}`
- `src/ava/plugin/tool_broker.{h,cpp}` or command broker equivalent
- `src/ava/event/` typed runtime event files
- `src/ava/app/command_plugins.cpp`
- `src/ava/app/commands.h` (`CommandRequest`) or a narrower new plugin-command context
- `src/ava/app/tui/interactive_tui.cpp`
- `src/ava/permissions/`
- `src/ava/tui/runtime.h`
- `src/ava/tui/composer.h`
- `src/ava/tui/runtime_actions_internal.cpp`
- `src/ava/tui/runtime_views.cpp`
- `src/ava/tui/composer.cpp`
- `tests/plugin_tests.cpp`
- `tests/app_runtime_tests.cpp`
- `tests/tui_composer_tests.cpp`
- plugin fixtures/sample under `examples/plugins/` only when necessary
- `tests/tui_tmux_scenarios/`
- `docs/extensions/plugin-system.md`
- `docs/plugin-compatibility-policy.md`
- permission/security documentation

### Deterministic Tests

Protocol and bounds:

- old manifests/plugins remain compatible;
- every optional capability parses independently;
- malformed/deep/oversized/flooded/out-of-order records fail contained;
- invalid UTF-8/control/ANSI/bidi content is rejected or visibly sanitized;
- widget/status/modal count, line, byte, choice, and deadline limits are exact;
- duplicate invocation/option IDs fail closed.

Authority:

- direct foreground TUI command can mint one exact capability;
- RPC, ACP, print/headless, model tools, event hooks, background jobs, queued submissions, and plugin-to-plugin calls cannot mint or reuse it;
- capability is bound to exact plugin/command/invocation/deadline;
- plugin disable/restart/session transition invalidates it;
- normal plugin command and UI policy permissions are both enforced.

Lifecycle:

- one modal globally;
- host modal conflicts reject safely;
- cancellation/deadline/child exit unblocks both sides;
- process-group cleanup remains bounded;
- statuses/widgets clear on every terminal state;
- no raw plugin UI content reaches sessions, exports, RPC, provider context, or public diagnostics.

Presentation:

- immutable plugin attribution is always visible;
- short-height/width layout remains usable;
- host keybindings/focus cannot be overridden;
- no plugin bytes reach terminal escape output.

### Real-Terminal Evidence

Use a checked-in local test plugin and private roots to prove:

- direct `/plugin run` status/widget lifecycle;
- attributed select/confirm modal;
- cancellation and timeout cleanup;
- plugin exit cleanup;
- terminal restoration; and
- RPC/headless rejection in separate non-TUI tests.

No test plugin receives credentials or network access.

### Completion Gate

- The protocol is additive and old plugins remain valid.
- Only bounded host-rendered text/select/confirm is possible.
- No unapproved invocation path can obtain UI authority.
- Raw plugin bytes never control the terminal.
- Cleanup is deterministic across all completion states.
- Security review has no unresolved severe finding.

### Wave 6 evidence and review ledger

- Focused protocol, application, RPC, and TUI tests plus the isolated credential-free `plugin_ui` tmux scenario passed during implementation.
- `W6-001` attribution/resize, `W6-002` disable revocation, and `W6-004` evidence findings were fixed. The focused final re-review closed `W6-001` after fresh-geometry checks covered queued and visible docks as well as modals. `W6-003` is closed by the current-state documentation reconciliation.
- The contract evidence is deterministic plus tmux; it makes no direct physical-terminal matrix claim. No paid provider was used.

### Rollback

Disable/remove UI capability negotiation and TUI event consumption. Existing plugin tools, commands, prompts, skills, and events remain compatible because additions are optional.

## 2026-08-09 TUI parity closure

The current retained-feature tree has 23 isolated tmux scenarios; `mermaid` is the new scenario. Run the complete wave with `AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 23 -R '^ava_tui\.tmux_smoke_'`. The completed contracts are:

- `--line-shell` is a bounded, sanitized, line-oriented accessibility and scrollback frontend. It bypasses ncurses and full-screen protocols, presents numbered permission/question choices, and is incompatible with print, RPC, and ACP modes. It is a useful text interface, not screen-reader certification.
- Transcript interaction includes tmux-safe bounded OSC 52 copy, double/triple-click selection, a transient scrollbar, previous/next user-turn navigation, latest-assistant copy on plain `/copy` or F5, and configurable unbound half-page actions.
- Optional Mermaid projection is disabled by default and runs only a bounded application-owned absolute helper for completed assistant `mermaid` fences. It is literal presentation only; disabled, pending, failed, or stale work falls back to the original fence. No TUI subprocess, semantic transcript mutation, arbitrary renderer, or full Mermaid support is implied.
- Display cursor fields and `/cursor <style> [blink|steady]` force DECSCUSR only while configured, restoring and reapplying it across terminal handoff. Direct environment-identified Alacritty uses a best-effort asynchronous strict DA2 version gate: versions through 0.14.0 (packed through 2401) keep conservative Kitty flags 5, while 2402 and later use 7. Mux paths skip the probe; startup never blocks on it.
- `app.prompt.stash` defaults to F6 and uses a process-only bounded stack: 16 entries, 64 KiB expanded per entry, 256 KiB aggregate, and no eviction. `/stash`, `/stash pop`, and `/stash clear` preserve drafts across in-process session transitions but not exit; pending images cannot be stashed, and restore cannot overwrite a nonempty draft.

Retained non-goals are session-tree transcript/content filters, LaTeX, arbitrary extension-owned renderers, blocking startup probes, and broad screen-reader certification. A dated reference rebaseline records Pi `936aff00918de1187f085f123c2812d8f2d67745` (`coding-agent`/TUI `0.84.1`), OpenCode `38e10eb1408feb700021b8e8766fb0ab41bf84e2` (TUI `1.18.15`), and Grok Build `8a14c91d88875a831a38b3a066b1683116bcb31c`; earlier dated pins/evidence remain unchanged.

## Integrated Validation

**Status: Complete — 2026-08-03.** The original six-wave integrated tree passed the following gates without a paid or live-provider call:

- normal build and full default CTest: 130/130 passed, with the documented optional/live gates skipped;
- all 23 then-registered isolated tmux scenarios passed together, including the later-removed setup-wizard scenario;
- Kitty, iTerm2, terminal-lifecycle, and OSC 8/52 direct PTY smokes passed together;
- full ASan/UBSan default CTest passed 130/130, followed by a clean focused rerun after the final plugin-geometry fix;
- focused TSan coverage passed for branch-summary, application-runtime, plugin UI, and TUI/composer concurrency surfaces;
- the integrated review and focused `W6-001` re-review closed all material in-scope findings; and
- `git diff --check`, Markdown links, documentation structure, and changed-line clang-format checks passed.

The subsequent Wave 4 removal and modal-padding closure was validated separately on the current retained-feature tree: normal build and full default CTest passed 129/129, all 22 then-current tmux scenarios passed together, all four direct PTY smokes passed, and documentation/diff checks passed. No post-removal sanitizer rerun is claimed; the sanitizer results above describe the original six-wave tree.

An intentionally broader, non-gating TSan run reported an RPC output/session lock-order inversion in unchanged `rpc_mode.cpp`; the integrated reviewer classified it as unrelated to these waves. The focused affected-surface TSan gate above remained clean.

The commands below remain the reproducible final runbook. Run nearest checks after each wave and broaden only when the wave is stable.

Normal build:

```sh
cmake -S . -B build -DAVA_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=BetaTest -DEnableDebug=OFF
scripts/build.sh --build-dir build
scripts/run-tests.sh --build-dir build
```

Real terminal wave:

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 23 -R '^ava_tui\.tmux_smoke_'
```

Run relevant direct PTY/Kitty/iTerm2 tests with their documented gates for image and terminal-protocol behavior. Use private XDG/session/plugin roots and fake providers.

Before handoff:

```sh
git --no-pager diff --check
python3 scripts/verify-markdown-links.py . --source-tree
python3 scripts/verify-documentation-structure.py .
```

Also run focused suites for every modified subsystem, including at minimum:

- display/app runtime;
- TUI composer, terminal images, input, keybindings, and runtime;
- sessions/branching/validation/append authority;
- providers/agent loop for branch generation;
- plugins/permissions/events/process cleanup; and
- sanitizer/TSan coverage when threading, runner, runtime-event, or authority code changes.

Do not build and test concurrently in the same build tree. Use `scripts/build.sh` and `scripts/run-tests.sh` so the fail-closed build-tree lock remains effective.

## Review Gates

Use one integrated reviewer per completed wave, selected by dominant risk:

- Waves 1–2: default reviewer, focused on config preservation and TUI regressions.
- Wave 3: default reviewer, focused on redaction and startup-view behavior.
- Wave 4: removed; no implementation review gate remains.
- Wave 5: session/database or architecture specialist, focused on lease authority, append states, schema semantics, and async cancellation.
- Wave 6: security specialist, focused on invocation authority, spoofing, terminal control, framing, bounds, process cleanup, and cross-frontend rejection.

Track material findings by stable ID as open/fixed/rejected/deferred. After fixes, resume the same reviewer for named findings and the changed delta rather than launching stacked broad reviews.

## Commit And Rollout Strategy

Prefer sequential, independently revertible commits:

1. display document/preserving writers;
2. image runtime behavior and tests;
3. nested settings state;
4. display preview transaction;
5. startup overview snapshot/view;
6. branch eligibility/preparation;
7. asynchronous branch-summary TUI action;
8. plugin protocol/capability parsing;
9. plugin invocation authority and permissions;
10. plugin host events/rendering/lifecycle;
11. final docs/evidence cleanup.

Do not combine unfinished waves into one merge. Every commit should build and retain existing behavior unless its user-visible change is fully tested. Push only after local validation and review appropriate to that wave.

## Final Definition Of Done

The five retained capabilities are complete only when:

- image visibility and width persist without destroying theme or unknown fields;
- disabled images avoid byte loading and graphics emission;
- nested settings work at narrow heights and preview without writes until confirm;
- preview cancellation/reload restores the latest authoritative state;
- startup overview is bounded, redacted, expandable, and never session/provider content;
- abandoned-parent summarization follows exact metadata-only semantics, uses bounded lease authority, runs asynchronously, and exposes append commit state without retry;
- plugin UI is optional, bounded, attributed, host-rendered, command-scoped, and unreachable from RPC/headless/model/background paths;
- existing plugin/session/config formats remain compatible or have an explicitly reviewed migration;
- deterministic focused tests, full default CTest, relevant sanitizer/TSan checks, direct PTY checks, and the complete tmux wave pass;
- terminal restoration and no-control-sequence leakage are verified;
- no paid live-provider validation was used;
- current user docs describe only landed behavior; and
- the working tree passes `git diff --check`, Markdown links, and documentation structure checks.

## Historical Handoff Checklist Before Implementation

1. Read this document completely.
2. Confirm current `develop` against the Wave 0 rebaseline record and inspect any later commits.
3. Confirm the tree is clean and all required state is committed before using worktree agents.
4. Start with Wave 1 only; do not parallelize overlapping TUI/app files.
5. Keep exactly one implementation todo in progress.
6. Use one coder for dependent steps and resume that coder when context continuity helps.
7. Verify returned worktree commits before integration.
8. Stop at the completion gate for each wave; do not opportunistically fix unrelated defects.
9. Preserve the metadata-only branch-summary decision unless the user separately approves a cross-session schema plan.
10. Preserve the constrained plugin UI boundary; do not expand it into arbitrary Pi-style components without a new security/architecture approval.
