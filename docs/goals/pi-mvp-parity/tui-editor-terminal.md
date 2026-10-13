# TUI, Editor, Terminal, And Product UX

## Goal Objective

Bring AVA's terminal UX to Pi coding-agent parity where it matters for MVP: editor behavior, autocomplete, markdown, selectors, tool cards, settings entry points, terminal protocol handling, accessibility, performance, and deterministic test evidence.

Suggested Codex command:

```text
/goal Bring AVA TUI/editor/terminal parity to documented closure using docs/goals/pi-mvp-parity/tui-editor-terminal.md. First compare Pi packages/tui plus coding-agent interactive mode against AVA src/ava/tui, then implement or defer every 100 percent criterion with CTest and PTY smoke evidence. Stop for approval before broad TUI architecture rewrites or new terminal dependencies.
```

## Pi References To Inspect First

| Topic | Pi paths |
| --- | --- |
| TUI library | `docs/reference-code/pi/packages/tui/src/tui.ts` |
| Editor | `docs/reference-code/pi/packages/tui/src/components/editor.ts`, `editor-component.ts` |
| Components | `docs/reference-code/pi/packages/tui/src/components/` |
| Markdown | `docs/reference-code/pi/packages/tui/src/components/markdown.ts`, `docs/reference-code/pi/packages/tui/test/markdown.test.ts` |
| Autocomplete/fuzzy | `docs/reference-code/pi/packages/tui/src/autocomplete.ts`, `fuzzy.ts`, `docs/reference-code/pi/packages/tui/test/autocomplete.test.ts` |
| Key parsing | `docs/reference-code/pi/packages/tui/src/keys.ts`, `keybindings.ts`, `docs/reference-code/pi/packages/tui/test/keys.test.ts` |
| Terminal | `docs/reference-code/pi/packages/tui/src/terminal.ts`, `terminal-colors.ts`, `terminal-image.ts`, `stdin-buffer.ts` |
| Virtual terminal tests | `docs/reference-code/pi/packages/tui/test/virtual-terminal.ts`, `tui-render.test.ts`, `tui-shrink.test.ts`, `terminal-colors.test.ts` |
| App interactive mode | `docs/reference-code/pi/packages/coding-agent/src/modes/interactive/interactive-mode.ts` |
| App components | `docs/reference-code/pi/packages/coding-agent/src/modes/interactive/components/` |
| Docs | `docs/reference-code/pi/packages/coding-agent/docs/tui.md`, `keybindings.md`, `terminal-setup.md`, `tmux.md`, `themes.md` |

## AVA References To Inspect First

| Topic | AVA paths |
| --- | --- |
| Runtime | `src/ava/tui/runtime.cpp`, `src/ava/tui/runtime.h` |
| Composer | `src/ava/tui/composer.cpp`, `composer.h`, `composer_internal.h` |
| Editor | `src/ava/tui/composer_editor.cpp`, `composer_editor.h`, `composer_input.cpp` |
| Palettes/selectors | `src/ava/tui/composer_palette.cpp`, `composer_select_list.cpp` |
| Transcript/markdown | `src/ava/tui/composer_transcript.cpp`, `src/ava/tui/text.cpp`, `src/ava/tui/composer_text.cpp` |
| Tool cards/diffs | `src/ava/tui/tool_cards.cpp`, `src/ava/tui/composer_diff.cpp` |
| Permission/question UI | `src/ava/tui/composer_permission.cpp` |
| Terminal | `src/ava/tui/terminal.cpp`, `terminal.h`, `terminal_image.cpp` |
| Theme/keybindings | `src/ava/tui/theme.cpp`, `keybindings.cpp`, `src/ava/app/tui/tui_display_settings.cpp` |
| Tests | `tests/tui_composer_tests.cpp`, `tests/tui_tmux_smoke.py`, `tests/tui_kitty_image_smoke.py`, `tests/tui_osc8_smoke.py` |

## Current Gap Summary

AVA already has a strong native ncurses TUI with editor, palettes, tool cards, permissions, questions, mouse selection, image protocols, keybindings, themes, and real PTY smokes. Pi is ahead in reusable component architecture, markdown richness, virtual terminal tests, startup selectors, terminal color detection, and some polished editor affordances. The goal is product parity, not necessarily copying Pi's component architecture.

## 100 Percent Criteria

| Criterion | Required AVA State |
| --- | --- |
| Editor parity | Multiline editing, undo/redo, kill ring, history, large paste markers, `!`/`!!`, file/path completion, slash completion, mouse selection, word movement, CJK/emoji/grapheme width, external editor, suspend, and active-run follow-up behaviors are implemented or explicitly deferred. |
| Markdown parity | Headings, lists, task lists, blockquotes, fenced code, code highlighting, tables, links, OSC8 hyperlinks, inline styles, narrow fallback, and performance are covered by deterministic tests. |
| Selectors | Model, scoped models, settings, sessions/tree, trust, keybindings, hotkeys, and permission views have usable keyboard-only behavior and clear disabled/error states. |
| Tool cards | Every model-visible tool has readable lifecycle, progress, output, truncation, spill, diff/changed-path, permission, cancel, and failure display where data exists. |
| Terminal capabilities | Kitty keyboard protocol, modifyOtherKeys fallback, bracketed paste, mouse, resize, OSC52, OSC8, Kitty/iTerm2 images, NO_COLOR, tmux/screen fallback, and terminal color inference are tested or documented. |
| Accessibility | Keyboard-only operation works for all flows; plain/no-color mode preserves information; no critical state is color-only; headless alternatives exist for automation. |
| Performance | Large transcript, large tool output, narrow terminal, resize, and active-run rendering stay bounded with tests or smoke evidence. |
| Deterministic terminal tests | AVA has either a virtual-terminal-style screen model or a documented decision to rely on renderer tests plus PTY smokes, with gaps listed. |
| Docs | `docs/core/usage.md`, `docs/core/configuration.md`, `docs/operations/testing.md`, and product ledgers describe current TUI behavior and smoke commands. |

## Implementation Slices

| Slice | Work |
| --- | --- |
| T1. Virtual terminal decision | Decide whether to add a small screen-model parser, tmux capture normalizer, or keep renderer tests plus PTY smokes. Document the decision and add missing tests. |
| T2. Markdown closure | Compare Pi markdown tests against AVA `text.cpp`/transcript behavior. Add missing table/code/link/narrow/performance tests and implementation. |
| T3. Editor closure | Close remaining large-paste, `!`/`!!`, path completion, CJK/grapheme, active-run, and accessibility gaps. |
| T4. Selector polish | Ensure settings/model/session/keybinding/trust selectors have Pi-like discoverability, disabled reasons, mouse/keyboard behavior, and tests. |
| T5. Tool/permission cards | Finish card affordances for non-shell tools, denied tools, narrow/plain mode, and copy/diff/permission commands. |
| T6. Terminal capability evidence | Expand tmux/Kitty/OSC8 smoke assertions only where renderer tests cannot prove behavior. Keep smokes stable and gated. |
| T7. Startup/onboarding UI | Decide if Pi first-time theme/analytics wizard maps to AVA. If not, document AVA's auth-first onboarding as the MVP equivalent. |

## Non-Goals Unless Approved

| Item | Reason |
| --- | --- |
| Rewriting AVA into a reusable TUI component library | Product parity can be achieved through existing C++ composer/runtime seams. A broad architecture rewrite is not required for MVP. |
| Pixel-perfect Pi visuals | AVA should preserve its native visual language and safety affordances. Match behavior, not aesthetics. |
| New heavy terminal dependencies | Add only if they improve deterministic testability and are acceptable for C++ build/release. |

## Verification

Targeted deterministic tests:

```sh
scripts/run-tests.sh --build-dir build -R '^ava_tests\.tui_composer$'
```

Runtime/app tests for TUI-backed commands:

```sh
scripts/run-tests.sh --build-dir build -R '^ava_tests\.(app_runtime|app_rpc|session)$'
```

Opt-in terminal smokes:

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 23 -R '^ava_tui\.tmux_smoke_'
AVA_TUI_KITTY_IMAGE_SMOKE=1 scripts/run-tests.sh --build-dir build -R '^ava_tui\.kitty_image_smoke$'
AVA_TUI_OSC8_SMOKE=1 scripts/run-tests.sh --build-dir build -R '^ava_tui\.osc8_smoke$'
```

Before area completion:

```sh
scripts/build.sh --build-dir build
scripts/run-tests.sh --build-dir build
git --no-pager diff --check
```

## Progress Log

- 2026-07-03: Initial goal file created. Current status: behavior baseline high, product polish and deterministic terminal-screen evidence remain the focus.
- 2026-07-03: Area checkpoint plan completed:
  1. inspect the listed Pi TUI/editor/terminal files and the corresponding AVA TUI/runtime/test files;
  2. compare the 100 percent criteria against current AVA behavior and existing docs;
  3. make only low-risk AVA-native fixes needed for safety or evidence;
  4. run deterministic TUI/headless tests and opt-in PTY smokes; and
  5. record review findings, residual deferrals, and final validation here before goal closure.
- 2026-07-03: Pi inspection covered `packages/tui/src/{tui.ts,components/editor.ts,components/markdown.ts,autocomplete.ts,fuzzy.ts,keys.ts,terminal.ts,terminal-colors.ts,terminal-image.ts,stdin-buffer.ts}`, Pi virtual-terminal/render/shrink/key/autocomplete/markdown tests, and coding-agent interactive components/docs. AVA inspection covered `src/ava/tui/{runtime,composer,composer_editor,composer_input,composer_palette,composer_select_list,composer_transcript,text,composer_text,tool_cards,composer_diff,composer_permission,terminal,terminal_image,theme,keybindings}.*`, `src/ava/app` TUI command wiring, and the TUI/PTY tests.
- 2026-07-03: T1 virtual-terminal decision: defer a Pi-style TypeScript virtual terminal or new screen-model dependency for MVP. AVA's closure evidence is deterministic CTest renderer/editor/event-state tests plus gated real PTY/tmux smokes for terminal-only behavior. Revisit a screen parser only if renderer tests plus PTY smokes become flaky or cannot prove a new terminal protocol.
- 2026-07-03: Criteria closure summary:
  - Editor parity: implemented for multiline editing, history, undo/redo, kill/yank/yank-pop, large paste markers with atomic movement/deletion/submit expansion, `!`/`!!` through permissioned `/bash` aliases, slash/file/path/fuzzy completion, mouse and keyboard selection, punctuation/CJK/grapheme-aware movement/wrapping, external editor, Unix suspend/resume, image paste, model/reasoning shortcuts, active-run follow-up/steer/restore semantics, and resize smoke coverage. Excluded from MVP: Pi-style automatic shell-output injection into provider context. Deferred: broader terminal compatibility polish.
  - Markdown parity: implemented/tested for headings, inline styles, lists/task lists, blockquotes, fenced code with lightweight highlighting, dividers, links, OSC8, pipe tables with narrow fallback, emoji/regional-indicator width stability, and large-render budgets. Deferred: richer theme hooks and exhaustive syntax highlighting beyond MVP coding-session needs.
  - Selectors/settings: model/scoped-model, settings, session/tree, trust, keybinding/hotkey, question, connect/login, and permission views have keyboard-only navigation, filtering, disabled reasons, mouse selection where terminal reports coordinates, and CTest/tmux evidence. AVA keeps domain-specific settings rather than a Pi-style merged settings file.
  - Tool and permission cards: model-visible tools render lifecycle/progress/status, bounded output/truncation/spill, changed paths/diffs, non-shell cards, cancellation/failure, permission audit links, copy payloads, and narrow/plain permission details where backend data exists. Future per-tool visual affordances and deeper diff navigation remain product polish.
  - Terminal capabilities: Kitty keyboard push/query/pop, xterm modifyOtherKeys fallback, bracketed paste, SGR/legacy mouse, resize, OSC52 copy, OSC8 hyperlinks, Kitty/iTerm2 images with tmux/plain fallback, `NO_COLOR`, `COLORFGBG` inference, and terminal cleanup have deterministic or opt-in smoke evidence. Pixel-level image validation in a real Kitty/iTerm2 app remains manual supplemental evidence.
  - Accessibility/performance: keyboard-only flows and headless/RPC alternatives exist, `NO_COLOR`/plain mode preserves critical permission/tool/settings text, print-mode TTY output now sanitizes terminal control bytes, and large transcript/tool-output/render budgets are covered. Broader screen-reader review and release-workload profiling remain future polish.
  - Startup/onboarding: AVA's auth-first onboarding (`/connect`/`/login`, provider env/auth guidance) is the MVP equivalent. Pi-style analytics/theme wizard behavior is excluded because telemetry/self-update/marketplace behavior is not an AVA MVP goal.
- 2026-07-03: Changes/evidence for this area are documented in `docs/core/usage.md`, `docs/core/configuration.md`, `docs/operations/testing.md`, `docs/product/mvp-baseline.md`, and `docs/product/mvp-coverage-ledger.md`. Relevant code/test changes in the current tree include `src/ava/tui/composer.h`, `src/ava/tui/runtime.{h,cpp}`, `src/ava/tui/tool_cards.cpp`, `src/ava/app/print_mode.{h,cpp}`, `tests/tui_composer_tests.cpp`, `tests/tui_tmux_smoke.py`, and `tests/app_print_tests.cpp`.
- 2026-07-03: Validation run:
  - `cmake --preset dev` passed.
  - `cmake --build --preset dev` passed.
  - `ctest --test-dir build -R 'ava_tests\\.tui_composer$|ava_tests\\.(app_runtime|app_rpc|session)$' --output-on-failure` passed 4/4.
  - `AVA_TUI_TMUX_SMOKE=1 ctest --test-dir build -R ava_tui.tmux_smoke --output-on-failure` passed.
  - `AVA_TUI_OSC8_SMOKE=1 ctest --test-dir build -R ava_tui.osc8_smoke --output-on-failure` passed.
  - `AVA_TUI_KITTY_IMAGE_SMOKE=1 ctest --test-dir build -R ava_tui.kitty_image_smoke --output-on-failure` passed.
  - `ctest --test-dir build -R 'ava_tests\\.app_print$' --output-on-failure` passed after the terminal-control sanitization fix.
  - `ctest --preset dev --output-on-failure` passed 58/58, with expected provider-live and TUI opt-in skips in the default run.
  - `git --no-pager diff --check` passed.
- 2026-07-03: Material review results:
  - General reviewer found no material TUI/editor/terminal correctness, DX, architecture, performance, or test-adequacy findings. Two low-severity non-blockers were accepted as intentional MVP contracts: deferred package placeholders return 0 with explanatory text, and tmux smoke path assertions were relaxed while deterministic tool-card path/diff coverage remains.
  - Security review found one material issue outside the visual TUI but inside terminal/headless safety: text print mode wrote model/tool-controlled terminal escapes to TTY streams. Fixed by sanitizing TTY-bound text print final output and diagnostics while preserving raw non-TTY pipes and JSONL, with regression coverage in `ava_tests.app_print`.
  - Follow-up security verification reported no material remaining issues for the print-mode fix.
- 2026-07-03: Residual risks/deferrals recorded for MVP closure: no Pi virtual-terminal dependency, no reusable TUI component rewrite, no pixel-perfect Pi visuals, automatic shell-output injection into provider context intentionally excluded from MVP, no analytics/theme wizard, no terminal-pixel image assertion beyond PTY emission plus manual supplement, broader screen-reader audit deferred, broader release-workload profiling deferred, and richer product polish for selectors/tool cards/diffs/themes deferred.
- 2026-07-04: Full-goal TUI visual re-verification after backend closure:
  - Extended `tests/tui_tmux_smoke.py` to persist representative successful tmux pane captures under `build/tui-tmux-smoke/evidence/` while preserving the existing stable visible-text assertions. Captures are regenerated by `AVA_TUI_TMUX_SMOKE=1 ctest --test-dir build -R '^ava_tui\.tmux_smoke$' --output-on-failure`.
  - Captures inspected: `startup-ready-composer.txt`, `settings-plain-no-color.txt`, `settings-model-selector.txt`, `settings-scoped-model-selector.txt`, `active-run-follow-up-queued.txt`, `active-run-follow-up-restored.txt`, `permission-prompt-risk-request.txt`, `permission-denied-tool-card.txt`, `permission-denied-narrow-no-color.txt`, `write-tool-card-success.txt`, `visible-diff-card.txt`, `visible-tool-details.txt`, `permission-audit-summary.txt`, `session-selector.txt`, `large-paste-marker.txt`, `resize-redraw.txt`, `slash-attach-palette.txt`, and `attachment-text-fallback.txt`.
  - Capture inspection confirmed visible startup/onboarding composer state, settings/model/scoped-model selectors, active-run queue/restore state, OpenCode-aligned permission prompt with request id/risk/reason/remembered-rule choices, denied/narrow plain permission details, write success, diff/tool cards, permission audit summary, session selector, large-paste marker, resize redraw, slash attach palette, and tmux text-only attachment fallback. A direct scan of the evidence files found no ESC/control-sequence bytes in the saved pane captures.
  - Additional PTY evidence passed: `AVA_TUI_KITTY_IMAGE_SMOKE=1 ctest --test-dir build -R '^ava_tui\.kitty_image_smoke$' --output-on-failure` verified Kitty graphics transmit command and visible attachment metadata; `AVA_TUI_OSC8_SMOKE=1 ctest --test-dir build -R '^ava_tui\.osc8_smoke$' --output-on-failure` verified OSC 8 hyperlink emission and visible Markdown fallback behavior. Provider request logs for active-run and OSC8 fake-provider flows remain in `build/tui-tmux-smoke/*provider-requests.log` and `build/tui-osc8-smoke/provider-requests.log`.
- 2026-08-03 historical note, superseded: a local-only Wave 4 setup wizard was implemented after the 2026-07-03 residual record. A later product decision removed that wizard completely; current AVA retains only ordinary auth-first `/connect` guidance and no-telemetry behavior.
- 2026-08-09: Current parity closure records 23 isolated tmux scenarios including `mermaid`, the bounded `--line-shell`, bounded OSC 52 and transcript selection/navigation affordances, disabled-by-default literal Mermaid projection, cursor/Alacritty contracts, latest-assistant copy plus half-page actions, and the bounded process-only prompt stash. The current reference pins are Pi `936aff00918de1187f085f123c2812d8f2d67745` (`coding-agent`/TUI `0.84.1`), OpenCode `38e10eb1408feb700021b8e8766fb0ab41bf84e2` (TUI `1.18.15`), and Grok Build `8a14c91d88875a831a38b3a066b1683116bcb31c`. Retained non-goals are session-tree transcript/content filters, LaTeX, arbitrary extension renderers, blocking startup probes, and broad screen-reader certification.
