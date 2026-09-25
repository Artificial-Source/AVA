# AVA Frontend Roadmap

**Status: F8 complete; retained Pi-inspired TUI Waves 1–3 and 5–6 implemented and fully validated; Wave 4 removed**

This is a post-MVP product-maturity roadmap for AVA's terminal frontend. It
sets the sequence, evidence, and scope boundaries for making AVA feel more
visually calm, responsive, and predictable in daily coding work, with
OpenCode as a behavior and quality reference.

OpenCode is never an architecture or source to copy. AVA remains a native
C++23/ncursesw application with narrow semantic frontend/backend seams. The
work described here should adapt useful user-visible behavior to AVA's
existing runtime and rendering boundaries, not adopt OpenTUI, Solid, web
components, or reference implementation details.

## 1. Product Goal

### Frontend maturity goal

AVA should present a terminal coding session with:

- clear visual hierarchy, so the active work, current decision, and next
  available action are apparent without reading every line;
- responsive layout, so wide, ordinary, and narrow terminals remain useful
  rather than merely fitting the same dense layout into fewer columns;
- predictable interaction, with stable keyboard, mouse, modal, draft, and
  transcript behavior;
- progressive disclosure, keeping routine messages compact while making tool,
  permission, diagnostic, session, and diff detail available on demand; and
- evidence, using deterministic renderer coverage, controlled PTY captures,
  and opt-in terminal-protocol smokes rather than aesthetic claims alone.

The goal is a mature AVA-native frontend, not visual or feature-count parity
with every OpenCode surface. Matching a behavior is useful only when it makes
coding sessions clearer, safer, or faster in AVA.

### Relationship to existing plans

`docs/roadmap/backend.md` owns backend capability, durability, provider,
runtime-event, and automation sequencing. This roadmap consumes the semantic
contracts that backend work exposes; it does not prescribe renderer details to
the backend or turn frontend presentation into a backend responsibility.

The Pi-first MVP TUI/editor/terminal goal is already documented as closed in
`docs/goals/pi-mvp-parity/tui-editor-terminal.md`, with the living baseline in
`docs/product/mvp-baseline.md`. This plan starts after that baseline. It must
not describe core editor, markdown, tool-card, permission, session, image,
or terminal features as if they were absent. Instead, it organizes the next
maturity pass: hierarchy, density, responsive behavior, discovery, and
well-bounded polish.

## 2. Scope and source map

### AVA frontend surface

| Area | Primary AVA paths | Planning focus |
| --- | --- | --- |
| Runtime and event routing | `src/ava/tui/runtime.{h,cpp}` | semantic-event consumption, redraw ownership, layout coordination |
| Composer and editor | `src/ava/tui/composer.{h,cpp}`, `composer_internal.h`, `composer_editor.*`, `composer_input.cpp` | draft stability, input geometry, completion, command discovery |
| Overlays and selections | `composer_palette.cpp`, `composer_select_list.cpp`, `composer_permission.cpp` | hierarchy, geometry, narrow layouts, keyboard/mouse clarity |
| Transcript and text | `composer_transcript.cpp`, `composer_text.cpp`, `text.*`, `text_wrap.*` | message grouping, reading rhythm, visible-window performance |
| Tools and diffs | `tool_cards.*`, `composer_diff.cpp` | compact summaries, detail disclosure, result and denial clarity |
| Terminal/platform | `terminal.*`, `terminal_image.*`, `event_state.*` | resize, mouse, images, links, cleanup, capability fallbacks |
| Interaction system | `keybindings.*`, `theme.*` | preserved controls, discoverability, visual tokens, theme limits |
| Application integration | `src/ava/app/interactive.cpp`, `command_palette.*`, `display_settings.*`, `interactive_run_queue.*`, `events.*`, `onboarding.*`, `clipboard_image.*`, `reasoning_controls.*`, `runtime_sessions.*` | semantic boundaries, application state, settings, session integration |
| Tests | `tests/tui_composer_tests.cpp`, `tests/tui_tmux_smoke.py`, `tests/tui_smoke_helpers.py`, `tests/tui_kitty_image_smoke.py` (shared parameterized Kitty/iTerm2 driver), `tests/tui_terminal_lifecycle_smoke.py`, `tests/tui_osc8_smoke.py`, `tests/CMakeLists.txt` | deterministic behavior and terminal evidence |

Current CTest inventory has 23 tmux scenarios:
`suspend_resume`, `keybind_conflict`, `theme_env`, `theme_persisted`,
`nested_settings_preview`, `startup_overview`, `mermaid`, `active_run`,
`restore_followup`, `streaming_scroll`, `transcript_search`,
`transcript_selection`, `subagent_workspace`, `branch_summary`,
`main_startup_trust_keybinds`, `main_models_selectors`, `main_editor_input`,
`main_slash_completions`, `main_permission_flow`, `main_question_flow`,
`main_session_mgmt`, `main_paste_scrollback_attach`, and `plugin_ui`;
plus four direct PTY CTests:
`ava_tui.kitty_image_smoke`, `ava_tui.iterm2_image_smoke`,
`ava_tui.terminal_lifecycle_smoke`, and `ava_tui.osc8_smoke`.

Large runtime, transcript, and keybinding orchestration files are known
pressure points, but their size alone does not justify a broad rewrite. Split
only at a demonstrated ownership or testability boundary.

### OpenCode reference snapshot

The local behavior reference is `docs/reference-code/opencode/`, snapshot
commit `7a8e7c8` dated 2026-07-04, with `packages/tui` version `1.17.13`.
It is useful for comparing user-visible quality, not for copying source,
architecture, data flow, or implementation details.

A dated 2026-08-09 rebaseline records Pi `936aff00918de1187f085f123c2812d8f2d67745` (`coding-agent`/TUI `0.84.1`), OpenCode `38e10eb1408feb700021b8e8766fb0ab41bf84e2` (TUI `1.18.15`), and Grok Build `8a14c91d88875a831a38b3a066b1683116bcb31c`. Earlier snapshot pins and their evidence remain historical.

| Reference area | OpenCode paths to compare behaviorally |
| --- | --- |
| App and shell | `packages/tui/src/app.tsx`, `routes/home.tsx`, `routes/session/index.tsx`, `routes/session/sidebar.tsx`, `component/startup-loading.tsx` |
| Transcript | `routes/session/index.tsx`, `routes/session/dialog-message.tsx`, `util/transcript.ts`, `util/layout.ts` |
| Prompt | `component/prompt/index.tsx`, `component/prompt/autocomplete.tsx`, `prompt/history.tsx`, `prompt/traits.ts`, `editor.ts`, `config/keybind.ts` |
| Tools and prompts | `routes/session/index.tsx`, `routes/session/permission.tsx`, `routes/session/question.tsx` |
| Overlays and navigation | `ui/dialog.tsx`, `ui/dialog-select.tsx`, `component/command-palette.tsx`, `component/dialog-session-list.tsx`, `routes/session/dialog-timeline.tsx`, `routes/session/dialog-fork-from-timeline.tsx`, `keymap.tsx` |
| Platform and polish | `clipboard.ts`, `util/selection.ts`, `component/prompt/local-attachment.ts`, `ui/link.tsx`, `attention.ts`, `feature-plugins/system/notifications.ts`, `ui/toast.tsx`, `component/dialog-status.tsx`, `feature-plugins/system/diff-viewer.tsx`, `feature-plugins/system/which-key.tsx`, `feature-plugins/sidebar/`, `theme/index.ts`, `context/theme.tsx` |
| Tests | `packages/tui/test/app-lifecycle.test.tsx`, `keymap.test.tsx`, `config.test.tsx`, `cli/tui/prompt-submit-race.test.ts`, `prompt/`, `clipboard.test.ts`, `theme.test.ts`, `cli/cmd/tui/notifications.test.ts`, `cli/tui/diff-viewer*.test.tsx`, `util/transcript.test.ts` |

## 3. Truthful current baseline

AVA already has a strong terminal frontend foundation:

- a native ncursesw runtime with a multiline composer, selection, undo/redo,
  completion, slash palette, draft preservation, mouse input, and configurable
  keybindings;
- transcript rendering for assistant text, markdown, reasoning, tool lifecycle
  updates, diffs, changed paths, bounded output, cancellation, and errors;
- permission and question flows, model/settings/scoped-model/session/tree and
  other selectors, session navigation, disconnected auth guidance, and active-run
  queue feedback;
- light, dark, plain, and local custom theme support, plus terminal capability
  handling for resize, paste, keyboard protocols, mouse, OSC 8/52, and
  Kitty/iTerm2 image paths with textual fallbacks; and
- deterministic renderer/editor tests, 23 opt-in tmux scenarios, and four
  direct PTY CTests for Kitty image transmit/delete, iTerm2 OSC 1337 emission,
  terminal lifecycle/termios cleanup, and OSC 8 links plus OSC 52 decoding.

Known residual limits are equally important:

- AVA deliberately has no deterministic terminal screen model. Renderer tests
  plus controlled PTY smokes are the accepted MVP strategy; this roadmap must
  make the resulting evidence clearer before proposing another renderer.
- Image sizing uses a fixed 9x18 terminal-cell pixel fallback. That is an
  explicit platform-quality limitation, not a claim of pixel-accurate image
  layout; external Kitty/iTerm2 pixel quality remains a manual check.
- Keyboard-only and plain/no-color behavior have MVP coverage, but a broader
  accessibility and screen-reader audit is deferred.
- The deterministic budgets are shipped; broader physical-terminal and
  external-workload profiling remains a future audit.
- Deeper standalone diff navigation remains deferred behind stable backend
  semantics.

### Baseline visual audit

Existing tmux captures provide useful, but viewport-specific, audit evidence.
They suggest that wide-terminal sessions can retain a permanent sidebar,
show verbose or repeated tool detail, render dense selector rows or raw
identifiers, and truncate modal or prompt hints at narrower widths. Startup
and authentication diagnostics can also be more verbose than the routine
first action needs. These are observations to validate across the baseline
matrix, not assertions that every viewport or session is already defective.

### Immediate P0 regression — ordinary Space completion closed in F0

**Trigger:** type a normal word, then press Space. **Closed behavior:** the
space remains one normal trailing draft cell and no file, path, or reference
menu opens. Passive completion still opens from a real `@` prefix or naturally
path-like token, and Tab still explicitly forces empty-token suggestions.

This was normal path completion, not literal `@` completion. The trigger-policy
seam in `src/ava/tui/composer_palette.cpp::find_path_completion_prefix()`
accepted an empty token after whitespace as a non-forced prefix, so its empty
query matched every workspace candidate. The narrow fix makes the
`start >= input.size()` branch return `std::nullopt` for non-forced completion
while preserving its empty prefix for forced completion. Space insertion,
runtime handling, keybindings, literal `@` parsing, fuzzy scoring, and
path-like token rules are unchanged.

The deterministic path-completion section now proves that non-forced
`inspect ` has no matches, keeps its palette hidden, and leaves selection text
and cursor unchanged; it also proves forced completion after that whitespace
still offers the workspace candidates. Existing positive real-`@`, `src/`,
equals/quoted, forced bare-token, and forced slash-argument checks remain.
The isolated Python real-TTY scenario types `ordinary`, observes the Space
cursor move from `13,29` to `14,29`, checks workspace palette rows remain
absent for a bounded interval, verifies the styled draft, saves evidence, and
then proves Tab opens forced suggestions.

The ordinary-Space gate remains part of the completed F0 record. The semantic
state inventory, character-cell matrix, named evidence catalog, and capture
policy are now recorded in
[`frontend-evidence-baseline.md`](frontend-evidence-baseline.md). The first
bounded F1 composer checkpoint is recorded below; the historical F0 evidence
remains unchanged.

## 4. Product principles and preserved controls

1. **Semantic seams first.** The frontend consumes semantic backend events and
   established application state. It must not require renderer-specific backend
   events, reconstruct paths or sessions from presentation text, or infer
   missing state from JSONL/session paths.
2. **One primary reading order.** A turn should lead from user intent to
   assistant outcome to any action requiring attention. Repeated summaries
   should collapse before primary content does.
3. **Responsive means changing policy.** Narrow terminals may hide or move
   secondary chrome, shorten labels, and alter overlay geometry. They must not
   silently lose critical state or controls.
4. **Details are earned.** Tool payloads, full IDs, diagnostics, diffs, and
   metadata remain available through clear expansion, copy, inspect, or
   navigation paths rather than occupying every resting screen.
5. **Input is sovereign.** Transcript navigation, overlays, and active-run
   state must not unexpectedly overwrite or relocate a draft.
6. **State is not color-only.** Plain/no-color and keyboard paths preserve
   permission, error, selection, running, and completion meaning.
7. **Change with evidence.** Each layout policy needs fixtures and captures at
   representative cell sizes, plus cleanup checks for terminal state and
   control sequences where a real PTY is involved.
8. **Use the smallest native change.** Do not introduce a broad reusable
   component rewrite or heavy terminal dependency without explicit approval.

### OpenCode-inspired terminal design direction

“Minimalistic yet premium” means restraint, alignment, predictable hierarchy,
crisp spacing, and fast interaction—not more borders, badges, colors,
animations, or metadata. Establish a neutral visual foundation with one
restrained accent; every state must still be textual and meaningful in
plain/no-color terminals. Avoid box or card nesting: use whitespace, small
gutters, and separators only when they communicate ownership.

Each screen has one clear focus target and one obvious reading order. Critical
states outrank decorative or status metadata. The same rules must make
idle, loading, streaming, success, error, canceled, and permission states
unambiguous; the treatment may be compact, but it must not rely on color or
ornament to explain what happened or what needs attention.

At idle, the composer has one quiet input surface and at most one visual
boundary or gutter: no nested frame/card, duplicate composer title, persistent
help row, empty attachment/queue/status row, or open menu. Only the
draft/placeholder and settled one-line footer persist; that footer may show the
active model, active conversation-context usage (`ctx count (percent)`, or estimated `ctx ~count` when model-window metadata is unavailable), and a fixed four-cell signal meter while processing. Ordinary draft/status/footer rows inherit terminal-default/`screenBg`; elevated `composerBg` remains for palettes and selectors. Contextual rows appear
only while populated or relevant and release their rows when empty. These are
terminal-native layout rules, not a pixel-look requirement.

### Settled AVA differences

The following are preserved product decisions, not candidates for accidental
OpenCode-style convergence:

1. Plain **Up/Down** scroll transcript history only. They never move or
   replace the composer draft. History and cursor-vertical actions remain
   configurable but are unbound by default.
2. The composer footer remains minimal: active model, active conversation-context usage (`ctx count (percent)` or estimated `ctx ~count`), and a fixed four-cell signal meter while processing. Do not reintroduce cwd, git, AVA branding,
   mode, provider, session metadata, cumulative session token usage, instruction-source counts, reasoning metadata, or a spinner glyph there.
3. Transcript mouse-wheel steps are three rendered rows per accepted event with 40ms same-direction coalescing and immediate reverse; selectors, questions, the sidebar drawer, and selection-edge autoscroll stay one row.
4. The frontend consumes semantic backend events. It does not create
   renderer-specific backend contracts or reconstruct path/session state.
5. No broad reusable-component rewrite or heavy terminal dependency is in
   scope without approval.

## 5. Priority categories

### P0 — baseline alignment

P0 is the product-maturity baseline. It covers responsive shell/sidebar policy;
transcript and message hierarchy; the composer and command discovery; command
palette/selectors; compact tool and diff cards; permission/question flows; and
session/navigation overlays. Work is accepted only when it improves the
resting screen and narrow behavior without regressing settled controls.

### P1 — platform polish

P1 covers the shipped in-TUI presentation of status, mouse/clipboard/images/
links, theme and accessibility refinement, terminal cleanup, and deterministic
performance evidence. It did not authorize a new desktop, audio, or terminal
notification surface; F7 accepts existing in-TUI attention and excludes
external notifications.

### P2 — optional or later decisions

F7 closes the optional decision record: terminal title and Which-Key are
excluded; local built-in/custom themes are complete while count parity, remote
packs, and marketplace delivery are excluded; and a standalone diff viewer is
deferred behind stable backend contracts. Session revert/redo remains a
separate product decision. Background-job controls landed after this matrix.
The constrained host-rendered plugin UI contract is implemented and recorded in the
[historical Pi-inspired TUI feature expansion record](../history/tui-pi-feature-expansion.md).
It does not approve arbitrary plugin renderers or terminal control.

Matching behavior does not mean adopting all OpenCode surfaces. AVA may choose
a smaller, safer path if it preserves the intended task outcome.

## 6. Historical gap matrix

This is the pre-closure planning matrix retained as acceptance history. F1–F6
closed its shipped work; its present-tense planning language is not an
unshipped-current claim.

| Area | Desired behavior | Current AVA state | Closure work | Acceptance / evidence | Backend dependency |
| --- | --- | --- | --- | --- | --- |
| Shell/sidebar | Secondary navigation yields space when terminals narrow; stable context remains reachable | Rich sidebar/session context exists; wide captures suggest it can be permanently visible | Define cell-width policy, collapse/reveal affordance, and focus/selection continuity | Baseline matrix captures at wide, ordinary, and narrow sizes; no hidden critical state | Existing session/model semantic state; do not infer from paths |
| Transcript | User, assistant, reasoning, tool, and error blocks have a calm, scannable hierarchy | Rich markdown and cards are present; dense visual grouping remains possible | Establish spacing, headings, compact states, and expansion hierarchy | Renderer fixtures and tmux capture review show one clear reading order | Existing message/tool lifecycle events |
| Composer | One quiet input surface with minimal chrome/footer; discovery is intentional and never competes with a draft | Mature multiline editor and minimal footer already exist | Preserve footer/input and transcript-only Up/Down behavior; allow passive menus only for `/`, real `@`, or path-like input, while explicit completion may force suggestions; keep contextual hints relevant and non-competing; anchor menus consistently without obscuring cursor/draft or losing focus on resize/cancel; keep attachments, queued work, and status above input and collapse them when absent | At each baseline width, focused editor renderer/capture assertions prove stable trailing spaces, cursor, and draft; no accidental popup or draft jump; preserved footer; and absence of duplicate/nested chrome and idle menu/status rows | None beyond existing commands/model context |
| Command palette | Commands and arguments are discoverable without raw-ID overload | Fuzzy palette and command integrations exist | Normalize labels, descriptions, disabled reasons, and argument states | Keyboard/mouse selection evidence at narrow widths | Command metadata and availability reasons where exposed |
| Selectors | Lists are readable, selected rows visible, and actions obvious at small heights | Many modal/select-list workflows exist; capture evidence suggests dense/raw rows | Shared row-window and label policy; preserve exact command selectors | Deterministic selection fixtures plus tmux evidence | Existing model/session/settings data |
| Tools | Resting transcript shows concise action/outcome; deep payload remains available | Lifecycle, output, diff, spill, and permission details are rich and may duplicate | F2 establishes only transcript placement/grouping/rhythm and a generic compact card shell; F5 owns status/target/outcome/duration anatomy, lifecycle and safety states including `permission required / awaiting decision`, progressive detail, specialized previews, inputs/output/IDs, permission linkage, copy, paths, diffs, truncation/spill, and detailed evidence | F2 evidence covers generic shell placement, spacing, one primary summary line, collapsed/expanded placement, and no adjacent duplicate summary; F5 compact/expanded renderer/PTY evidence at 160x48, 120x36, 80x24, and short height covers detailed states, reachable/copyable expansion, and actionable failures/permission decisions | Tool event summaries, changed paths, status, bounded payloads |
| Diffs | Changed paths and a useful compact diff entry point are visible without a full viewer | Inline diff rendering exists; deeper navigation is deferred | Improve card hierarchy only; gate standalone navigation/viewer | Focused card fixtures; no claim of full viewer | Stable diff/path metadata; full viewer requires a contract decision |
| Permissions/questions | Prompt risk, reason, choices, and outcome are easy to understand in narrow/plain layouts | Structured flows and remembered rules exist | Normalize geometry; treat `permission required / awaiting decision` as a safety-critical state distinct from queued/pending/running; narrow choices without hiding allow/deny meaning | Deterministic narrow/plain and Python tmux evidence cover the permission-required prompt/tool state, its human action, risk/reason, allow/reject follow-up, choices, denial, and result card; expanded/copy evidence retains request identity | Existing permission/question request and resolution events |
| Sessions/navigation | Switching, tree navigation, and session context are discoverable without permanent density | Session/tree contracts and selectors are present | Responsive sidebar and session-overlay information hierarchy | Session selector captures with keyboard and mouse continuity | Existing session-tree/name/label state; no pathname reconstruction |
| Status/attention | Existing completion, failure, queued, and attention states are visible without noisy persistent chrome | Startup, active-run, and auth diagnostics exist | Refine quiet in-TUI status treatment; keep every new notification surface behind F7 approval | Captures distinguish transient from persistent status | Event severity/terminal state if already exposed; otherwise backend work |
| Mouse/clipboard/images/links | Capability-enhanced paths degrade to useful text and clean up terminal state | Mouse, OSC 8/52, Kitty transmit/delete, iTerm2 OSC 1337 emission, and text fallback exist; the fallback size is fixed | Verify fallback, selection, link, image-row, and cleanup behavior | 23-scenario tmux suite; four direct PTY CTests for Kitty, iTerm2, lifecycle, and OSC8/OSC52; manual pixel supplement | Terminal capability data only; no backend presentation contract |
| Themes/accessibility | Meaning survives plain mode and themes remain coherent | Light/dark/plain/local custom themes and keyboard paths exist | Audit contrast/text affordances, names, and narrow plain layout; defer screen-reader breadth | Deterministic plain/theme cases and documented manual audit | None |
| Performance/testing | Layout behavior is repeatable and real terminals leave no state behind | Renderer budgets, terminal lifecycle, and tmux smokes are shipped MVP evidence | Retain deterministic budget and cleanup regressions; future-audit broader physical-terminal/external workloads | Focused C++ suite, isolated Python tmux scenarios, pane-capture inspection, control-sequence and cleanup checks | None |

## 7. Dependency gates

Frontend code must not guess missing semantic data. If a desired label, state,
action, ownership, risk reason, diff identity, session relation, or job state
is not exposed by a stable semantic backend contract, record it as backend
work and preserve a truthful fallback. Summary-only events need an AVA-native
frontend fallback that remains readable, rather than fabricated detail.

The following remain gated unless a stable backend contract already exists.
The constrained plugin UI row records the implemented separate-plan contract; it is not a general renderer or terminal-control gate.

| Capability | Gate |
| --- | --- |
| Session revert/redo | Explicit reversible-session semantics, conflicts, permission/audit meaning, and terminal event state |
| Background-job control | Job status, wait/result/cancel ownership, lifecycle events, and retained-result contract |
| Plugin UI | Implemented bounded host-rendered `ava.plugin.v1` request/response boundary with permission, trust, cancellation, and diagnostics semantics; arbitrary renderers remain excluded |
| Full diff viewer | Stable diff identity, path/content availability, bounded retrieval, navigation, and permission semantics |

A frontend may render a stable existing contract, but must neither add a
renderer-specific contract nor treat a temporary payload shape as one.

## 8. Milestones

### F0 — contract and evidence baseline

**Status: Complete — 2026-07-19**

**Purpose:** established a shared, reproducible starting point before changing
layout or interaction policy. The authoritative inventory, measured
character-cell matrix, evidence catalog, capture policy, and tracked future
gaps are in [`frontend-evidence-baseline.md`](frontend-evidence-baseline.md).

**Completed scope:**

- Recorded semantic event and application-state seams consumed by the TUI for
  transcript, tool, permission, question, session, model, queue, diagnostics,
  and terminal-capability presentation without inventing backend data.
- Captured the current idle/onboarding shell at 160x48, 120x36, 80x24, and
  100x12, and catalogued the existing plain/no-color, active-run, permission,
  selector, tool, diff, session, paste, resize, attachment, and protocol
  evidence.
- Recorded the accepted renderer-test plus PTY-smoke strategy, including the
  intentional absence of a deterministic terminal screen model.
- Defined evidence ownership, capture normalization, control-byte inspection,
  and terminal-state cleanup expectations.
- Retained the closed ordinary-Space path-completion regression as a tested F0
  gate before visual redesign.

**Closure criteria met:**

- Every P0 state has a named deterministic or real-TTY/protocol evidence target
  and a semantic source or named future gap.
- The matrix separates observed current behavior from future breakpoint policy.
- Saved text captures are plain, text-safe, and checked for unexpected control
  bytes.
- The ordinary-Space regression has deterministic C++ and isolated Python tmux
  evidence: non-forced `ordinary ` is hidden, trailing-space cursor movement is
  stable, and `@sr`, `src/`, and forced Tab still complete. The tmux scenario
  saves `composer-ordinary-space-no-completion.txt` after its cursor/no-palette
  assertions.
- F0 ships no visual redesign.

#### F0 implementation checkpoint — ordinary Space regression

The regression was reproduced red before the production change. The focused
C++ run failed the expected no-match, hidden-palette, and unchanged-selection
assertions; the unfixed tmux run failed because the workspace palette appeared
after Space. After the narrow prefix-policy fix, these commands passed:

```sh
scripts/build.sh --build-dir build --target ava ava_tests ava_fake_provider_server
scripts/run-tests.sh --build-dir build --jobs 1 -R '^ava_tests\.tui_composer$'
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 1 -R '^ava_tui\.tmux_smoke_main_slash_completions$'
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 13 -R '^ava_tui\.tmux_smoke_'
```

The isolated regression scenario and the complete 13-scenario tmux wave passed;
the full wave completed 13/13 with no failed terminal scenario. The persisted
artifact was found at
`build/tui-tmux-smoke/main_slash_completions/evidence/composer-ordinary-space-no-completion.txt`.
It contains the visible `ordinary` draft and no `src/main.cpp`, `src/`, ESC,
or other C0 control bytes except LF. The scenario observed cursor `13,29` to
`14,29`, completed its forced-Tab continuation, exited its TUI, and cleaned up
its private tmux/provider resources. The ordinary-Space checkpoint is retained as a completed F0 gate; the broader
F0 inventory and evidence closure are recorded below and in
[`frontend-evidence-baseline.md`](frontend-evidence-baseline.md).

#### F0 closure checkpoint — frontend evidence baseline

The completed F0 record is
[`frontend-evidence-baseline.md`](frontend-evidence-baseline.md). The following
commands passed against the current configured `build/` tree:

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 1 -R '^ava_tui\.tmux_smoke_main_startup_trust_keybinds$'
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 13 -R '^ava_tui\.tmux_smoke_'
```

The targeted baseline scenario passed, and the isolated full wave passed 13/13.
It saved and inspected these plain-text artifacts:

- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-wide-idle-shell.txt`
- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-ordinary-idle-shell.txt`
- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-narrow-idle-shell.txt`
- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-short-idle-shell.txt`

The scenario asserts exact tmux dimensions at 160x48, 120x36, 80x24, and
100x12; it synchronizes the target-specific composer/footer reflow at
zero-based rows `height - 3`/`height - 2`, asserts the current sidebar at
wide/ordinary widths and its absence at narrow/short widths, then restores
120x32 and asserts rows 29/30. Inspection confirmed the composer/footer in
every artifact, no ESC or unexpected C0 bytes, and cleanup of private
tmux/fake-provider resources. This is evidence closure only; no F1 visual
change is claimed.

### F1 — visual system and responsive shell

**Purpose:** give AVA a consistent visual language and responsive shell policy.

**Scope:**

- Define visual tokens and hierarchy using existing theme/runtime seams.
- Establish wide, ordinary, and narrow sidebar behavior, including discoverable
  reveal/navigation and focus continuity.
- Normalize outer gutters, section spacing, divider use, status placement, and
  header/footer priority without expanding the composer footer.
- Preserve the minimal model/known-context/spinner footer and existing input
  behavior.

**Acceptance criteria:**

- The same active state is understandable at every matrix width.
- Narrow policy never hides a permission, active run, selected modal choice,
  or draft without an accessible path to it.
- At every baseline width, renderer/capture assertions prove the idle composer
  has no duplicate/nested chrome or idle menu/status rows, while its draft and
  cursor remain stable.
- Footer assertions prove only active model, active conversation-context usage, and an active
  spinner while processing appear.
- Existing Up/Down draft behavior remains unchanged.

#### Bounded F1 checkpoint — quiet composer shipped

This checkpoint ships only the quiet composer slice; it does not complete F1.
The idle and single-line composer now occupies exactly the bottom two rows:
input, then footer, with no blank composer-surface padding row. Every visible
input, continuation, and footer row starts with the same three-cell `│  `
semantic boundary and gutter, and the former `❯` prompt glyph is absent.
Multiline height is the visible wrapped input-line count plus one footer,
clamped to 2..8 rows. The existing composer surface color and footer contract
remain unchanged: active model, known `ctx N`, and the active spinner only.

Sidebar policy is deliberately unchanged in this slice. It still requires
semantic sidebar data and width >=112 and remains bounded to 38 columns.
Responsive sidebar disclosure, breakpoint/reveal behavior, focus continuity,
and the remaining F1 visual-system/shell hierarchy work are still open. No
tool-card or sidebar redesign began here.

Test-first evidence was captured against the old implementation. The focused
C++ test ran red with 27 expected composer failures; representative failures
were `tui keeps a one-line draft in exactly the bottom input and footer rows
without composer-surface padding` and `tui composer input and continuation
rows share one three-column boundary and gutter`. The updated real-TTY matrix
also ran red against the old binary by timing out waiting for F1 rows 46/47 at
160x48 while the captured frame still showed `▎  ❯` at rows 45/46.

After the production change, these validations passed:

```sh
scripts/build.sh --build-dir build --target ava ava_tests ava_fake_provider_server
scripts/run-tests.sh --build-dir build --jobs 1 -R '^ava_tests\.tui_composer$'
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 1 -R '^ava_tui\.tmux_smoke_main_startup_trust_keybinds$'
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 13 -R '^ava_tui\.tmux_smoke_'
python3 -m py_compile tests/tui_tmux_smoke.py
```

The targeted scenario passed and the isolated tmux wave passed 13/13. Current
plain-text captures are:

- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-f1-wide-idle-composer.txt`
- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-f1-ordinary-idle-composer.txt`
- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-f1-narrow-idle-composer.txt`
- `build/tui-tmux-smoke/main_startup_trust_keybinds/evidence/frontend-f1-short-idle-composer.txt`

Inspection confirmed exact dimensions and zero-based input/footer rows 46/47,
34/35, 22/23, and 10/11; `│  ` prefixes; no `❯`; footer text exactly
`GPT-5.5 · ctx 1` after removing the leading gutter and wide sidebar separator;
the historical sidebar policy visible/visible/absent/absent; no ESC or
unexpected C0 bytes; final LF; and cleanup of private tmux/provider resources.
The ordinary-Space scenario and all other tmux interaction scenarios remained
green. `git diff --check` and explicit no-index whitespace checks for both new
roadmap documents also passed.

The F1 review found a task-caused packaged-documentation link mistake:
`docs/operations/testing.md` linked to the historical baseline roadmap document, but
roadmap documents are not in the package payload. This was not an
untracked/index condition, and committing would not correct it. The link is
now an inline source reference to
`docs/roadmap/frontend-evidence-baseline.md`, so the packaged documentation has
no local link to an omitted roadmap file.

The review ledger is closed: `F1-QC-001` fixed the packaged-documentation
link, and `F1-QC-002` restored the too-narrow Markdown fallback assertion so
it rejects table-border glyphs on transcript rows while exempting only the
exact quiet-composer rows.

After the review fixes, formatting `tests/tui_composer_tests.cpp` with the
repository `.clang-format`,
`scripts/build.sh --build-dir build --target ava_tests`,
`scripts/run-tests.sh --build-dir build --jobs 1 -R '^ava_tests\.tui_composer$'`,
and
`scripts/run-tests.sh --build-dir build --jobs 1 --output-on-failure -R '^ava_release\.package_linux$'`
all passed. The full default wrapper ran 99 CTest entries: the package test
passed, and the only two failures were the known unrelated dirty-backend cases
`ava_tests.acp` (missing MCP subprocess/exit 127 rather than the expected
registry error) and `ava_cli.headless_rpc_plugin_commands` (the newer sanitized
plugin failure rather than its older raw phrase).

#### Bounded F1 checkpoint — responsive sidebar shipped

This checkpoint completes only the approved responsive-sidebar slice; F1
remains in progress. The automatic semantic side rail now requires both width
`>=112` and height `>=16`, remains bounded to 38 columns, and is absent below
either threshold so the transcript and quiet composer use the full width. At
`112x16` the main pane is exactly 74 columns before its divider.

Exact frontend-only `/sidebar` is now discoverable in the slash catalog and
`/help`. It opens the existing `SidebarSnapshot` data as a temporary,
full-width `Session overview` drawer at every tested size, suppressing the rail
while open. Values are sanitized and wrapped with continuation indentation
instead of clipped. Page Up/Page Down, Home/End, and one-row mouse-wheel steps
reach all activity, modified-file, session, usage, context, and version data;
Esc resets the offset and restores composer focus. Resize preserves open state,
reflows, and clamps the offset. The drawer is local to the TUI snapshot and is
not session, RPC, provider, or backend state. Permission prompts, docked and
modal questions, and select-list views retain precedence and temporarily
suppress it. The quiet composer remains visible at full width, transcript
images are cleared, composer cursor/hit testing is disabled only while the
drawer owns focus, and headless dispatch reports truthful interactive-TUI
guidance without synthesizing semantic data.

The checkpoint was test-first. Before production edits, the focused C++ build
failed because `ComposerSnapshot` had no `sidebar_drawer_visible` field and
`sidebar_drawer_max_scroll_offset` did not exist. The updated tmux scenario
then ran against the old binary and failed at
`frontend-f1-short-wide-auto-sidebar-hidden`: the `160x12` frame still showed
`live session` and `Activity`, and its footer retained the side-rail divider.
After implementation, the focused composer, app-runtime, and command-registry
CTest cases and the targeted startup tmux scenario passed.

Current real-TTY evidence adds:

- `frontend-f1-short-wide-auto-sidebar-hidden.txt` at `160x12`;
- `frontend-f1-narrow-sidebar-drawer.txt` and
  `frontend-f1-narrow-sidebar-drawer-scrolled.txt` at `80x24`; and
- `frontend-f1-short-sidebar-drawer.txt` at `100x12`.

Inspection confirms the exact row counts, full-width quiet composer on rows
22/23 and 10/11, wrapped paths and identifiers, lower context/version reach,
no duplicated rail, hidden/restored cursor focus, no ESC or unexpected C0
bytes, and final LF. The existing `160x48`, `120x36`, `80x24`, and `100x12`
idle captures retain their prior visible/visible/absent/absent rail behavior.
The isolated tmux wave passed 13/13, and Python compilation and the Linux
package CTest passed. Earlier parallel waves exposed unrelated load-sensitive
failures: `ava_tests.command` once failed its prepared-environment value
comparison, while `ava_tests.tools` separately failed progress-event and
process-group-cancellation assertions. Each immediate single-job focused
rerun passed. The latest full default wrapper then passed all 101 registered
entries with the normal 17 skips and no recurrence. These are newly observed
suite flakes, not the previously recorded dirty ACP or plugin failures.

The review ledger is closed: `AVA-F1-001` makes every supplied drawer activity
item reachable with a textual status marker, and `AVA-F1-002` corrects this
checkpoint chronology. After these fixes, `clang-format` on the changed C++,
`scripts/build.sh --build-dir build --target ava ava_tests`, the focused
composer CTest, the opt-in startup tmux scenario, and the latest full default
wrapper all passed; that wrapper passed 101/101 registered entries with 17
normal skips. The live-provider smoke was skipped, so no live provider calls
ran.

The only remaining F1 scope is visual hierarchy, outer gutters, section
spacing, divider treatment, and status placement; F2, F4, and F5 have not
started.

#### Final F1 checkpoint — bounded visual system and shell complete

This checkpoint closes F1 without broadening it. AVA retains the existing eight
semantic theme roles and color machinery, keeps meaning textual rather than
color-only, and adds no global main-pane gutter, persistent top header,
horizontal transcript/composer separator, or footer field. Existing
transcript-local prefixes and the settled quiet composer remain unchanged.
The composer surface, its `│  ` input/footer boundary, and the optional
ownership rail are the shell boundaries.

The automatic rail keeps the responsive geometry established by the previous
checkpoint: semantic snapshot required, width `>=112`, height `>=16`, at most
38 columns, and a 74-column main pane at `112x16`. While those conditions hold,
visibility is stable rather than content-driven. The rail now has one dim
one-cell divider, a two-cell local content inset, and one bold non-accent
`Session` title with muted mode/provider/model metadata. `Activity` appears
only for currently running items and excludes completed history; `Modified
Files` appears only when nonempty; `Context` appears only for known branch,
reasoning, non-unknown usage/pressure, or context-source count, including zero.
Populated groups have one blank row between them. AVA/live branding, idle and
no-change placeholders, unknown values, raw session id/path/entry count,
workspace, and version are absent from the automatic rail but remain available
in the unchanged complete `/sidebar` drawer.

Nonmodal dock allocation now reserves the composer and existing
permission/question space first, then up to three error-category status-alert
lines, before spending the remainder on slash/file/path palettes, queued
messages, and pending attachments. Rendering order remains palette, queue,
attachment, alert, permission/question, composer, so an admitted alert is
immediately above the composer. Prompt precedence, contextual-row anatomy, and
empty-state omission are unchanged.

The checkpoint was test-first. Against the pre-change renderer, the focused
`ava_tests.tui_composer` case failed with exactly three new assertions:

- `tui reserves a three-line status alert before queue and attachment budgets and renders it immediately above the two-row composer`;
- `tui idle automatic rail is a bounded two-cell-inset Session summary with the quiet footer and no placeholders or drawer-only metadata`; and
- `tui populated automatic rail shows only running activity, modified files, and known sanitized Context values within cell bounds`.

The targeted startup tmux scenario also failed against the pre-change binary at
`frontend-f1-wide-idle-composer`: the captured right rail began with `AVA` and
`live session`, used a one-cell inset, rendered idle Activity and empty Modified
Files groups, and exposed raw session/path/workspace/version detail. This is the
real-terminal red evidence for curation. After implementation, the focused
renderer and targeted startup scenario passed. A condition-synchronized
active-run capture was attempted, but the fake-provider request log becomes
observable only when the pending response may already have settled, so no
reliable Running row or `frontend-f1-active-rail.txt` artifact is claimed;
Running lifecycle coverage remains deterministic.

Final inspected captures are:

- `frontend-f1-wide-idle-composer.txt` at `160x48`;
- `frontend-f1-ordinary-idle-composer.txt` at `120x36`;
- `frontend-f1-narrow-idle-composer.txt` at `80x24`;
- `frontend-f1-short-idle-composer.txt` at `100x12`;
- `frontend-f1-short-wide-auto-sidebar-hidden.txt` at `160x12`;
- `frontend-f1-narrow-sidebar-drawer.txt` and
  `frontend-f1-narrow-sidebar-drawer-scrolled.txt` at `80x24`; and
- `frontend-f1-short-sidebar-drawer.txt` at `100x12`.

Inspection confirmed dimensions, exact composer rows and footer, the curated
wide/ordinary rail, one divider and two-cell rail inset, no automatic branding
or raw metadata, full width below the breakpoint, complete drawer detail and
scroll reach, no ESC or unexpected C0 bytes, and final LF. Formatting and
Python compilation passed; the requested `ava`, `ava_tests`, and fake-provider
build passed; focused composer and targeted startup/active-run scenarios
passed; the isolated tmux wave passed 13/13; Linux packaging passed; and the
full default wrapper passed all 101 registered entries with 17 normal optional
skips. No paid or live-provider call ran.

The final F1 review accepted two additional material findings. Their regression
assertions were added before the production fix; the focused composer test then
failed with exactly:

- `tui dock-aware transcript scroll limit reaches the oldest transcript while a multiline alert, queue, and attachment preview remain docked`; and
- `tui automatic rail preserves legitimate values containing unknown while omitting exact normalized unknown sentinels`.

`AVA-F1-003` is closed by one composer-owned vertical-layout calculation shared
by rendering and every runtime transcript clamp. It covers quiet and reduced
composer reservations, normal and 12-line diff prompts, docked questions and
modal/select bases, alert-first palette/queue/attachment budgets, image-preview
row metrics without emitting an encoded image, the final transcript viewport,
and transcript detail/thinking flags. The deterministic regression proves the
old local clamp was smaller than the shared maximum, exactly two transcript rows
remain beside a multiline alert, queue, and pending image preview, maximum
scroll reaches the oldest labeled transcript, and a 12-line diff prompt leaves
the expected four transcript rows.

`AVA-F1-004` is closed by field-appropriate, case-insensitive exact normalized
sentinels: empty and exact `unknown` are absent, and token status additionally
omits exact `tokens unknown`; branch/provider/model values that merely contain
`unknown`, including `fix/unknown-token-count`, remain visible and sanitized.
Known-zero context behavior is unchanged.

For this ledger closure, `clang-format` on the changed C++ passed; the requested
`ava`, `ava_tests`, and fake-provider build passed; the focused composer test
passed; targeted active-run/startup tmux passed 2/2; the isolated opt-in tmux
wave passed 13/13; Linux packaging passed; and the full default wrapper passed
all 101 registered entries with the normal 17 optional skips. Python compilation
and whitespace checks passed, and no paid or live-provider call ran.

F1 is therefore complete. This historical checkpoint predates F2; neither F3
autocomplete redesign, F4 overlay geometry, nor F5 detailed tool cards was
begun here.

#### Implemented F2 checkpoint — transcript hierarchy and generic tool shell

This checkpoint implements only F2. The renderer now classifies every visible
entry locally as user, assistant flow, error, or system/audit. One shared logical-group-start decision drives full rendering, bounded reverse-tail
rendering, and message-start calculations. User and error entries always begin
a new group; assistant flow begins one only after non-flow content; system/audit
begins one only after non-system content. At widths `>=44` and heights above 12,
one blank row precedes each noninitial group. Narrow widths and short terminals
remove those inter-group blanks. Thinking, assistant, and tool parts remain
visually continuous; context-gathering runs keep one shared heading with no
per-tool blank; queue, delivery, and audit receipts are System entries while the
active queue dock remains primary; and the old trailing transcript blank is gone. F1 ownership markers, rail breakpoints,
composer/footer, and dock precedence are unchanged.

The generic tool shell now has one fitted first line containing the existing
status marker, tool name, lifecycle, and at most one conservative primary
summary. Running tools prefer argument or command text. Terminal shell tools
prefer existing exit/timeout/cancel status and duration. Other terminal tools
prefer result, then truncation, then arguments. Expanded content begins on the
next line and suppresses only a labeled detail that exactly repeats a complete,
untruncated chosen primary. Existing permission, output, truncation, spill, changed-path, diff,
and copy payloads remain intact; the running shell cancellation hint is still
visible. This does not redesign detailed lifecycle, permission, diff, copy, or
path anatomy reserved for F5.

A renderer-only adjacent-result rule handles the narrow duplicate case. When a
tool has a nonempty result and the immediately following assistant item is the
same nonempty sanitized text after trailing-whitespace trim, the tool's visual
result phrase is omitted and the shell falls back conservatively. The assistant
answer is never suppressed, stored transcript items are never mutated, and
nonadjacent or nonexact similarities remain visible. Empty/nonempty backend
text behavior and audit fallbacks are unchanged.

Focused deterministic coverage now proves representative roomy and
narrow/short grouping, F1 ownership markers, a single context heading, queue
placement, exact blank-row counts, no terminal blank, generic and shell primary
selection, expanded/collapsed placement, exact duplicate suppression and its
nonmatching controls, Unicode and `NO_COLOR` bounds, preserved copy payloads,
and full/tail/message-start parity across varied budgets in both spacing modes.
It additionally covers a visible user, system, or error item followed by hidden
standalone thinking and a visible assistant item, preserving the shared blank-row
boundary and full/tail/message-start parity without rendering the full tail.
Existing max-scroll, detached-output, and dock-aware F1 assertions remain in the
same focused suite. The 900-item redraw and 20,000-line tool-output budgets are
unchanged.

Real-terminal proof lives under
`build/tui-tmux-smoke/main_permission_flow/evidence/`. The expanded ordinary
`120x36` frame is
`frontend-f2-tool-shell-expanded-ordinary.txt`. Collapsed responsive captures
are `frontend-f2-transcript-wide.txt` (`160x48`),
`frontend-f2-transcript-ordinary.txt` (`120x36`),
`frontend-f2-transcript-narrow.txt` (`80x24`), and
`frontend-f2-transcript-short.txt` (`100x12`). The `/write` transcript retains
one fitted shell line for each emitted lifecycle record, expanded changed/diff
content when requested, and exactly one assistant `wrote 27 bytes` result with
no duplicate result row. Assertions
also cover exact dimensions, line bounds, final composer/footer placement,
wide/ordinary F1 rail curation, narrow/short full width, compact short rhythm,
and ESC/C0 hygiene. All waits are event/screen-condition based. The scenario
restores its prior `120x32` expanded state before continuing existing copy and
permission checks.

#### F2 formal review ledger — closed

- `AVA-F2-001`: fixed reverse-tail leading spacing by requiring a preceding
  visible item under the shared visibility predicate; leading-hidden full/tail/
  message-start parity now has bounded regression coverage.
- `AVA-F2-002`: expanded details now suppress an exact repetition only when the
  complete sanitized primary actually rendered untruncated and nonempty; narrow
  long-result and long-name/argument regressions retain the existing copy
  payload.
- `AVA-F2-003`: adjacent suppression trims trailing ASCII whitespace on raw
  result and assistant text before sanitization, then trims sanitized text;
  CR/LF, structured `Text`, sanitized-display identity for middle controls, and
  exact/case-sensitive controls are covered without history mutation.
- `AVA-F2-004`: corrected the roomy threshold to 44 columns and receipt
  ownership to System entries while retaining the active queue dock as primary;
  this closure replaces the obsolete package failure with the latest package
  evidence below.

Validation chronology is no-live-provider only. The test-only request-accounting
fix landed first; `restore_followup`, `active_run`, and `main_permission_flow`
passed, and the all-scenario opt-in tmux wave passed 13/13. `clang-format`,
Python compilation, and the requested `ava`/`ava_tests`/fake-provider build
passed; the focused composer CTest passed 1/1. The latest focused Linux package
CTest passed 1/1 (the earlier staged-documentation failure is obsolete). The
final default wrapper passed all 102 registered entries: 85 passed, 17 expected
optional skips, and no failures. Whitespace checks passed.

F2 is complete. F3 autocomplete, F4 overlays, and F5 detailed tool
lifecycle/permission/diff/copy/path redesign have not started at this checkpoint.

#### Implemented F3 checkpoint — composer discovery

F3 implements only composer discovery and autocomplete. The composer-anchored palettes retain their one-line candidate rows and `│  ` surface with no card, header, persistent help, or navigation footer. Responsive rows now prioritize the selected value, usage/key hint, useful description or disabled reason, and category last; they remain bounded at 80 and 40 columns. Every palette-visible command, alias, hint, category, description, and disabled reason is terminal-sanitized while selection keeps the raw canonical insertion value.

Argument completions now carry an optional renderer-only display label. Configured models display their display name (falling back to model id) while retaining canonical `provider/model` as secondary detail and insertion text. Named sessions display their effective title while their raw session id remains secondary and is still used for nested arguments and insertion. Labels and raw values both participate in fuzzy matching.

Disabled command arguments, `@` references, and path candidates are browseable but cannot mutate a draft through Enter, Tab, or mouse. They beep and report a concise command/reference/path disabled status. Explicit forced Tab rejects a single disabled path rather than inserting it. Palette mouse mapping now obtains its first screen row from the composer-owned layout calculation shared with rendering, including alerts, queues, attachments, compositor height, palette viewport, automatic rail main width, and modal/drawer suppression.

During an active run, explicit Tab has the same forced path fallback as idle (bare token, empty token after Space, and slash argument), and active mouse accepts slash/reference/path rows through the shared layout. A single muted contextual row appears only while processing and no palette/prompt/modal owns the dock: it shows `Esc stop` with an empty draft, or first configured queue/follow-up/dequeue key displays with a draft. Queued rows retain `/restore` and use the configured dequeue key when available. The contextual row is reserved by the shared layout and sits above any admitted error, which remains directly above the composer. The footer remains model, optional active context usage (`ctx count (percent)` or `ctx ~count`), and spinner only.

Test-first checkpoint: before production edits, the new deterministic `display_label` test was compiled with `scripts/build.sh --target ava_tests --jobs 2` and failed red because `SlashCommandArgumentCompletion` had no `display_label` member. After the narrow implementation, focused deterministic coverage passes for label matching/insertion, 80/40 palette priority/bounds, control sanitization, disabled reference/path queries, dock-aware row mapping, and active contextual keys/error placement. The existing trailing-Space, arrows, selector, footer, queue, attachment, and F1/F2 coverage remains in the same focused test. The session completion regression additionally proves a named title is primary while the raw session id is secondary.

Real-terminal scenarios extend `main_slash_completions` with ordinary `120x36` and narrow `80x24` palette evidence and `active_run` with fake-provider synchronized active forced-Tab/contextual-row evidence. Artifacts are written under `build/tui-tmux-smoke/main_slash_completions/evidence/` and `build/tui-tmux-smoke/active_run/evidence/` as `frontend-f3-*`; capture assertions retain bounded dimensions and control hygiene. This checkpoint uses no live or paid provider.

Validation recorded at this checkpoint: clang-format and Python compilation completed; `ava`, `ava_tests`, and `ava_fake_provider_server` built; focused `ava_tests.tui_composer`, `ava_tests.app_runtime`, and `ava_tests.app_command_registry` passed. The modified active/slash tmux scenarios passed, then the opt-in 13-scenario wave passed 13/13 after one F3-responsive test expectation was updated and its focused rerun passed. Linux package CTest passed. The final default wrapper passed all 103 registered entries: 85 passed and 18 expected optional/opt-in skips, with no failures. `git diff --check` and no-index roadmap whitespace checks passed. F3 leaves F4 modal geometry/selectors and F5 detailed tooling explicitly out of scope. The final named-finding re-reviews accepted F3-R1, F3-R2, F3-R3, and F3-R4; F3 is complete.

**F3 review ledger**

- **F3-R1 fixed:** active contextual hints now render through the shared composer gutter used by input and palettes; deterministic rendering asserts stripped `│  Esc stop...` text, and fake-provider `active_run` saves/checks the bounded plain capture.
- **F3-R2 fixed:** slash, `@`, and path palette hit APIs are explicit screen-position APIs and reject column zero, divider, and automatic-sidebar positions outside the compositor main width. Runtime mouse dispatch supplies both coordinates; deterministic dock/automatic-rail coverage and the wide tmux sidebar-coordinate rejection cover the boundary.
- **F3-R3 fixed:** both active action routes retain defense-in-depth guards before queue mutation: Submit has its earlier visible-selection pre-guard, the production-internal active nonblocking dispatch seam gates its callback with the exact completion snapshot, and MessageFollowUp retains the queue-entry guard. A blocked result preserves the draft/cursor and prevents history/reset/output; command/reference/path disabled statuses render as dock-aware compact alerts. Deterministic route-seam coverage proves a recognized `/jobs` draft with a disabled forced-path candidate suppresses both callback and queue mutation, while enabled and unrecognized controls retain their classifications. Fake-provider active `/share` tmux synchronizes Tab, Enter, and mouse rejection feedback on the visible configured deferred reason, rejects `commands run between turns`, and retains the draft/request count; it does not claim disabled `@`/path tmux synthesis.
- **F3-R4 fixed:** new evidence after the R1–R3 closure found the saved F3 resize captures were stale tmux reflow, from their artifact row positions rather than AVA redraw. The smoke scenario now waits for AVA's slash input and quiet model/context footer on the target viewport rows before assertions and capture, and proves restored cancel focus. The named-finding re-review accepted the corrected 36/24/36-row artifacts and condition-synchronized geometry.

The final named-finding re-reviews accepted F3-R1, F3-R2, F3-R3, and F3-R4. F3 is complete.

#### Implemented F4 checkpoint — responsive selectors and latency closure

F4 replaces dense reverse-video selector rows with one restrained overlay
language. Model rows lead with display names under provider groups and omit
canonical IDs, reasoning levels, and capability dumps at rest. Session rows
lead with titles and branch depth; stable IDs remain the selection value, while
paths are hidden by default and available through the existing disclosure
toggle. Slash, file-reference, and path palettes use the same quiet `›`
selection marker without duplicated path metadata. Fixed-height rows, shared
viewport windows, and shared mouse hit testing preserve keyboard/mouse parity
at ordinary, narrow, and 8–12-row terminal heights. Permission and question
meaning remains textual in plain mode, and selector acceptance paints a
truthful pending state before invoking synchronous model/session authority.

The reported latency paths were fixed at their synchronous sources rather than
masked with input delays. A value-owned ranked completion cache is shared by
navigation, Tab/Enter acceptance, rendering, and hit testing, formatting only
the visible rows. Routine input and provider/footer updates share one 16ms
completion-anchored frame scheduler, with full frames superseding footer-only
work and frame-scoped wheel-run coalescing. Detached transcript scrolling keeps
a generation-keyed layout frozen across draft redraws while provider output is
deferred, then synchronizes once before navigation, hit testing, or geometry and
presentation changes. Live-tail projection leaves cumulative pending text
unparsed so the incremental renderer retains only bounded viewport/current-line
carry for growing append-only assistant and tool streams. One
application-lifetime catalog coordinator serializes coherent
workspace/reference/slash/session snapshots. Workspace discovery runs once per
catalog generation and refreshes only after successful visible-path mutations
or explicit relevant reload. Session selectors reuse one tree index, update the
current node through retained read authority after ordinary turns, and consume
session-specific asynchronous title notifications without duplicate tree
rebuilds.

This checkpoint also ships bounded F5/F6 improvements without claiming those
milestones complete. Resting tool cards use at most one primary and one context
row, hide raw IDs/audit commands/output previews until details are expanded,
and retain textual lifecycle, permission decision, risk, and reason. The real
`/write` flow carries its workspace-relative target into completion, producing
`write · complete · src/main.cpp · wrote 27 bytes` once. First-run auth is one
actionable `! OpenAI not connected · /connect` row. Stored transcript, copy,
expanded detail, session, provider, permission, and tool semantics are
unchanged.

Deterministic regressions cover a 2,000-candidate completion source, repeated
navigation with zero re-ranking, 900-item detached scroll reuse, frame-request
coalescing and failure latching, frozen detached draft redraws with one-shot
navigation/resize synchronization, the actual 1,000-item eviction cap with
synthetic headings and hidden entries, many small streaming appends without
cumulative pending-text preparse, current and non-current asynchronous
session-title refresh, catalog concurrency, model/session acceptance repaint,
and the real command-to-TUI `/write` timeline. Focused normal, ASan/UBSan, and
TSan runs passed during implementation.

Real-terminal evidence is produced by the existing isolated tmux scenarios.
Representative artifacts include `model-selector-arrow-scroll.txt`,
`model-selector-quiet-100x12.txt`, `session-selector.txt`,
`session-selector-named-default-path-hidden.txt`,
`frontend-f3-file-reference-quiet.txt`, and
`frontend-f2-transcript-wide.txt` beneath their scenario evidence roots.
Inspection confirmed exact dimensions, quiet primary labels, path/ID and
capability omission at rest, the workspace-relative write target, one startup
warning marker, cursor/focus restoration, no ESC or unexpected C0 bytes, final
LF, and terminal/process cleanup.

The integrated review ledger is closed: FPR-1 fixed catalog concurrency; FPR-2
fixed session-specific title invalidation and duplicate topology rebuilds;
FPR-3 fixed detached anchors across capped eviction and synthetic-prefix
changes; FPR-4 fixed quadratic live-tail growth; PRESENT-1/2 fixed startup and
completed-write presentation; and TUI-SMOKE-1 updated real-terminal reload-row
synchronization. The final build and formatting checks passed, the opt-in tmux
wave passed 13/13, Linux packaging passed, and the default suite passed all 103
registered tests with 18 expected optional skips. No paid or live-provider call
ran.

F4 is complete. F5's full lifecycle/diff/permission/question evidence matrix
and F6's remaining terminal/platform/accessibility work remain open.

#### Implemented first F5 checkpoint — compact tools and permission decisions

This bounded checkpoint replaces the remaining lifecycle-heavy resting cards
with one fitted row: textual status marker, tool, one useful target/outcome,
and duration when known. Running, success, error, and canceled states remain
plain-text distinguishable; settled permission IDs with no decision read
`permission checked`, never as an active request. A genuinely unresolved audit
reads `permission required` only while its tool is running. Allow once, allow
session, allow always, and deny remain distinct, while expanded and copy
surfaces retain IDs, arguments, output, diagnostics, diffs, changed paths,
truncation, spill detail, risk, reason, and resolution metadata.

The permission dock now uses one quiet `! Permission required` attention state,
a human action label, sanitized command/target, risk and reason, and a restrained
`›` selected choice. It hides raw request IDs from the decision surface without
removing them from expanded/copy audit detail. The existing fail-closed Escape,
keyboard shortcuts, allow-once/session/persistent choices, persistent deny,
diff preview, narrow fitting, and tiny-height fallback remain authoritative.
Authorization is not painted as an ordinary tool failure.

Permission correlation is explicit. An exact permission or resolver ID wins;
otherwise only a newly observed request may attach to one unique running tool
with the same nonempty name. Ambiguous requests remain unattached even if a
later resolution arrives after one candidate settles. Local and RPC decisions
preserve allow-once/session/persistent-allow/deny meaning, and question events
remain outside the permission audit path. Batch fallback timelines now collapse
Running and settled entries with one nonempty call ID into one card, preserve
the first nonempty start arguments and first-invocation order, retain running-
only calls, and leave empty IDs uncorrelated. Backend timeline history is
unchanged.

Deterministic coverage spans lifecycle rows; resolved and unresolved permission
states; expanded/copy detail; exact, unique, ambiguous, late, and question
correlation; permission geometry at 20/28/40/80/120 columns and 1–12 row
budgets; plain-color selected-state meaning; and fallback/live timeline parity.
The `main_permission_flow` tmux scenario now captures
`frontend-f5-permission-prompt-roomy.txt` and
`frontend-f5-denied-tool-card.txt`. The inspected `120x36` prompt shows the
human action, command, critical risk, reason, and truthful choices with no ID or
reverse-video chrome. The settled capture contains exactly one denied bash row,
with no stale running duplicate, ESC/NUL bytes, or terminal cleanup leak.

The review ledger is closed. `F5-R1` prevents a later reply from re-running
name fallback after an ambiguous request; `F5-R2` preserves session and
persistent-allow meaning; and `F5-R3` preserves the first start arguments when
a fallback timeline settles. The targeted app-runtime/composer tests, focused
ASan/UBSan tests, Linux package test, and opt-in tmux wave passed; the tmux wave
passed 13/13. At maximum default parallelism, the previously recorded
load-sensitive `ava_tests.tools` progress assertion recurred once; ten
consecutive isolated runs passed, then the complete 103-entry wrapper passed at
`--jobs 8` with 18 expected optional/opt-in skips. Formatting, Python
compilation, and whitespace checks passed. No paid or live-provider call ran.

At this first checkpoint, specialized changed-path and diff entry points,
long/truncated/spilled output evidence, card-local mouse interaction,
question-surface refinement, and the full responsive lifecycle matrix remained
open. The closure checkpoint below completes that work without widening the
backend contract.

#### F5 closure — progressive tool details and question interaction

F5 now keeps routine tools to one fitted resting row while giving each original
card its own disclosure state. `Ctrl+O` remains the global compact/expanded
control. `/tool <query>` and a click on the visible card header toggle the
matching original card in place rather than appending a presentation duplicate;
scroll anchoring, the live/detached transcript boundary, modal/dock precedence,
and backend/session history remain unchanged. Width-aware action rows advertise
only complete existing `/tool`, `/copy tool`, `/diff`, and `/copy diff` commands;
if no safe query fits, no truncated or invented command is shown.

Expanded cards expose bounded supplied input, identities, permission audit,
changed paths, shell status/duration, result/output, diff, truncation, retained
bytes, and spill disposition only when those fields exist. The output preview
is capped at eight rows and uses authoritative total/omitted metadata. Final LF
or CRLF does not create a synthetic output line: the 20,000-line shell fixture
reads `8 shown/20000 lines · 19992 hidden`. Spill paths are disclosed as the
recorded locator, with `spill incomplete` only when the backend marks it; AVA
does not invent retrieval or read the spill during rendering. Full sanitized
copy payloads remain available. Duplicate suppression is conservative: only
complete, untruncated, sanitized exact payloads, ignoring trailing ASCII
whitespace only, may collapse; internal spaces, line structure, JSON
punctuation, and truncated payloads remain visible and copyable.

Question docks and modals now share the restrained `›` selection language,
plain-text multi-select marks, quiet `?` title, bounded short-height rows, and
no reverse-video selection bar. Their shared row model also drives hit testing.
Keyboard, SGR mouse, searchable/copy choices, custom text, masked secrets,
multiple selection, Escape cancellation, and resize all retain the established
authoritative question path. An explicit mouse activation cannot accidentally
submit custom/search text in place of the clicked option, and a retained
`/sidebar` drawer flag cannot block the question that has visual authority.

Deterministic coverage spans pending/running/success/error/canceled/denied
cards, permission-required and settled audits, exact versus nonexact result
suppression, supplied inputs and IDs, changed paths/diffs, bounded output,
truncation/spill counts, complete copy payloads, safe action-query round trips,
per-card hit testing, scroll anchoring, question keyboard/mouse/custom/copy
semantics, drawer precedence, plain mode, and narrow/short geometry. The
real-terminal `main_permission_flow` scenario records
`frontend-f5-tool-card-mouse-expanded.txt` and
`frontend-f5-lifecycle-{wide,ordinary,narrow,short}.txt` at `160x48`, `120x36`,
`80x24`, and `100x12`. The new isolated `main_question_flow` fake-provider
scenario records `frontend-f5-question-single.txt`,
`frontend-f5-question-multi-narrow.txt`, and
`frontend-f5-question-short.txt`, exercising keyboard and SGR mouse resolution
without live credentials.

The F5 review ledger is closed. `F5-R1` through `F5-R3` are the correlation,
permission-scope, and fallback-start fixes from the first slice. `F5-R4` gives
mouse clicks an explicit option-activation path; `F5-R5` restores exact-only,
nonlossy payload suppression; `F5-R6` aligns question hit testing with drawer
precedence; `F5-R7` requires parser-normalized, width-safe action queries; and
`F5-R8` corrects LF/CRLF output-line accounting. The final `ava`/`ava_tests`/
fake-provider build passed, the default wrapper passed all 104 registered
entries with 85 passes and 19 expected optional/opt-in skips, Linux packaging
passed, and the complete opt-in tmux wave passed 14/14. Focused ASan/UBSan
`ava_tests.tui_composer` and `ava_tests.app_runtime` passed. Python compilation,
formatting, capture dimension/final-LF/ESC-C0 inspection, cleanup, and whitespace
checks passed. No paid or live-provider call ran.

F5 is complete. F6 owns the remaining terminal/platform/accessibility audit and
broader profiling; a standalone diff/spill viewer remains an explicit later
product/backend decision rather than an F5 inference.

### F2 — transcript and message hierarchy

**Purpose:** make long conversations easier to scan while keeping details
available.

**Scope:**

- Refine transcript placement, grouping, spacing, and reading rhythm for user
  prompts, assistant answers, reasoning, tools, errors, queue state, and
  summary-only events.
- Define only the generic compact tool-card shell: where it appears, its
  spacing, one primary summary line, generic collapsed/expanded placement, and
  no adjacent duplicate summary.
- Do not define per-state lifecycle anatomy, specialized previews, inputs, full
  output, IDs, permission audit, diff details, permission linkage, or the full
  evidence matrix here; F5 exclusively owns that detailed tool UX.
- Preserve markdown and bounded transcript-rendering behavior.

**Acceptance criteria:**

- A representative multi-turn capture has one obvious reading order.
- Generic compact cards have stable placement and spacing, one primary summary
  line, predictable collapsed/expanded placement, and no adjacent duplicate
  summary.
- Summary-only backend events display a truthful useful fallback.
- Large transcript cases remain bounded by existing or added focused evidence.

### F3 — composer and command discovery

**Purpose:** keep the composer calm and make available actions discoverable.

**Scope:**

- Treat the composer as one quiet input surface with at most one boundary or
  gutter and the settled minimal footer: no nested frame/card, duplicate title,
  persistent help row, empty attachment/queue/status row, or idle open menu.
  Only the draft/placeholder and one-line footer persist at rest; contextual
  rows appear only while populated or relevant and release their rows when
  empty. Improve command palette, autocomplete, relevant inline hints,
  unavailable states, and overlap behavior at narrow widths.
- Make `/`, a real `@` prefix, path-like input, or an explicit completion
  action intentional discovery triggers; normal typing and ordinary Space
  never open a passive menu.
- Keep contextual hints from competing with the draft; keep attachments,
  queued work, and status above the input and collapse them when absent.
- Anchor menus consistently to the composer without obscuring the current
  cursor/draft, and preserve focus through resize and cancel.
- Normalize command and model/session-facing labels without requiring raw IDs
  as the primary visible text.
- Improve discovery of active-run follow-up, queue, restore, and cancellation
  behavior using existing semantics.
- Preserve draft ownership, configured custom keys, and default arrow policy.

**Acceptance criteria:**

- A draft is unchanged by plain Up/Down transcript scrolling.
- Palette and completion fixtures show visible selection, useful descriptions,
  and no essential hint truncation at supported narrow widths.
- At every baseline width, renderer/capture assertions prove no duplicate or
  nested chrome and no idle menu/status row, while footer and normal input
  semantics remain unchanged, including stable trailing spaces, cursor, and
  draft with no accidental popup or draft jump.
- Exact selector commands continue to open their intended selector behavior.

### F4 — overlays, selectors, and session navigation

**Purpose:** make modal workflows predictable and legible across terminal
sizes.

**Scope:**

- Apply a consistent overlay geometry, title, prompt, list, action, and help
  policy to command, model, settings, session/tree, permission, and question
  views.
- Keep selected rows visible and align rendering with mouse hit testing.
- Shorten or disclose secondary identifiers rather than crowding primary labels.
- Make responsive sidebar and session navigation work together without
  reconstructing session state from path text.

**Acceptance criteria:**

- Eight-to-twelve-row terminal fixtures keep selected rows and required actions
  visible.
- Keyboard-only and mouse selection choose the same semantic item.
- Plain/no-color captures preserve disabled, selected, and risk meaning.
- Session navigation uses existing stable IDs/state, not renderer parsing.

### F5 — tools, diffs, permissions, and questions

**Status: complete — 2026-07-23**

**Purpose:** reduce repeated operational detail while improving safety-critical
clarity.

**Scope:**

- Exclusively define detailed tool UX beyond F2's generic shell: status,
  target, outcome, and duration anatomy; every lifecycle and safety state;
  progressive detail; specialized preview rules; inputs, full output, IDs,
  permission audit/linkage, copy behavior, changed paths, diffs, and
  truncation/spill treatment.
- Give queued/pending, running, success, failed, canceled, denied,
  `permission required / awaiting decision`, truncated/spill, changed-path,
  and diff states distinct textual, never color-only treatment. The
  permission-required state is safety-critical and distinct from
  queued/pending/running: its compact treatment visibly says permission is
  required, shows risk, and points to the available decision action without
  color dependence.
- In the permission prompt, keep the human action, reason, risk, and
  allow/reject choices visible. Keep raw request identity available in expanded,
  copy, and audit detail rather than crowding the default decision surface.
  Avoid duplicate model-facing result text while allowing specialized previews
  under these detailed rules.
- Improve changed-path/diff entry points without implementing a full diff viewer.
- Normalize permission and question geometry, remembered choices, denial
  follow-ups, and narrow choice labels; improve non-tool denial presentation
  where current semantic data supports it.

**Acceptance criteria:**

- Queued/pending, running, success, failure, cancellation, truncation/spill,
  diff, permission-required/awaiting-decision, permission allow, permission
  deny, and question fixtures each expose the required next action.
- Default resting cards do not repeat equivalent result content.
- Expanded cards are keyboard/mouse reachable and copyable; failures and
  denials never hide their next action. Permission-required prompts retain the
  human action, risk, reason, choices, and follow-up, while expanded/copy detail
  retains the raw request identity.
- The detailed evidence matrix covers compact and expanded states at 160x48,
  120x36, 80x24, and short-height terminals. Deterministic narrow/plain and
  Python tmux evidence specifically cover the permission-required prompt/tool
  state, not merely allowed/denied outcomes.
- Any missing field is tracked as backend work rather than synthesized.

### F6 — terminal, platform, accessibility, and performance

**Status: complete — 2026-07-23**

**Purpose:** harden the mature layout in real terminal environments.

**Scope:**

- Exercise resize, tmux, mouse, clipboard, OSC 8, OSC 52, Kitty/iTerm2 image
  fallback/emission, no-color, and terminal-state cleanup.
- Audit focus, keyboard-only routes, non-color meaning, readable labels, and
  accessible text descriptions; defer broad screen-reader claims until audited.
- Add bounded real-workload profiling scenarios for transcript, tool output,
  resize, and active streaming.
- Revisit a screen-model investment only if the evidence strategy is shown
  inadequate or flaky.

**Acceptance criteria:**

- Opt-in terminal smokes leave terminal modes and control sequences clean.
- Capability fallbacks have readable textual behavior.
- Profile scenarios have stated input sizes, budgets, and repeatable collection
  instructions; no performance claim rests only on an anecdote.
- Any fixed 9x18 image-cell limitation remains documented until replaced.

**Closure record:** F6 preserves the deterministic and tmux coverage that
already exercises resize, Unicode cells, keyboard-only and mouse routes,
plain/no-color meaning, paste, scrollback, capability fallbacks, and bounded
large-workload rendering. It adds an isolated raw-PTY lifecycle driver for
clean Ctrl+D and SIGTERM exits. Both paths negotiate the xterm
`modifyOtherKeys` fallback and then require its disable sequence, Kitty keyboard
pop, bracketed-paste disable, explicit cursor restoration, alternate-screen
exit, exact termios restoration, expected exit code, and process-group cleanup.
The renderer now deletes every successfully emitted active Kitty preview before
keyboard/curses teardown on every post-entry exit. iTerm2 remains emission-only
because that protocol has no equivalent delete contract in AVA.

Direct protocol evidence now covers Kitty transmit and delete, iTerm2 OSC 1337
emission, three OSC 8 links, and a complete ST-terminated OSC 52 sequence whose
base64 payload decodes to the actual assistant response. The protocol drivers
use private roots, allowlisted environments, dummy loopback-only provider
configuration where needed, bounded captures, finite deadlines, and
TERM-to-KILL cleanup. Their closure log is generated at
`build/tui-f6-evidence/protocol-closure.log`.

The repeatable deterministic workload gates are: four `120x36` redraws of 900
mixed transcript items under five seconds; compact plus expanded rendering of
20,000 tool-output lines under two seconds; three scroll offsets over 1,320
mixed transcript items at `96x30` under twenty seconds; 86 mixed items across
40 width/height combinations; and append-only streaming parity/work bounds for
800–900-line Markdown/code/list sources, an 18,000-byte token, 180 incremental
appends, and a 90,000-byte streaming tool argument. These are regression
ceilings, not product-speed benchmarks. Run them with
`./build/tests/ava_tests tui_composer` or the focused CTest target.

The F6 review ledger is closed: `F6-R1` strengthened cursor and exact termios
restoration and fixed the exposed cursor-teardown defect; `F6-R2` replaced host
environment inheritance with credential-free allowlists; and `F6-R3` aligned
CTest timeouts with complete driver and cleanup budgets. The default 106-test
suite passed with 85 executed checks and 21 expected skips, all 14 tmux
scenarios passed, all four direct protocol smokes passed, and the focused
ASan/UBSan composer-plus-protocol set passed five checks. No live-provider call
was used.

F6 does not claim broad screen-reader support or pixel validation inside a
physical Kitty/iTerm2 application. The documented 9x18 fallback remains, and
the current deterministic/PTY strategy remained stable enough that no screen
model was added. Those are explicit limitations rather than hidden parity
claims.

### F7 — optional polish decisions

**Status: complete — 2026-07-23**

F7 records product decisions; no optional implementation was selected, so no
new code was authorized.

| Item | Decision | Record |
| --- | --- | --- |
| Attention/notifications | Existing in-TUI attention accepted; external notifications excluded | No new backend event or footer surface |
| Terminal title | Excluded | Privacy, multiplexer, restoration risk, and no session-title TUI contract |
| Standalone diff viewer | Deferred | Requires backend-owned stable diff identity, bounded permission-aware retrieval, and version/truncation/path semantics; `/diff`, expanded cards, and `/copy diff` remain the compact path |
| Which-Key overlay | Excluded | `/help`, `/hotkeys`, and `/keybindings` already expose effective configured bindings |
| Themes | Local built-in/custom selection accepted as complete | Theme-count parity, remote packs, and marketplace delivery excluded |

No optional surface expands the footer or changes default Up/Down behavior.

### F8 — release closure and documentation

**Status: complete — 2026-07-23**

F8 closes the frontend documentation audit. Updated: this roadmap,
`docs/roadmap/dogfood.md`, `docs/operations/testing.md`, `docs/product/mvp-baseline.md`,
`docs/product/features.md`,
`docs/core/providers.md`, `docs/product/backend-capabilities-1.0.md`,
`docs/core/usage.md`, and `docs/core/configuration.md`. Verified current with no edit:
`docs/operations/terminal-setup.md`, `docs/README.md`, and the historical
`docs/roadmap/frontend-evidence-baseline.md`.

Present behavior is marked shipped; standalone/deeper diff navigation,
broader physical-terminal/external-workload profiling, and broad screen-reader
certification remain deferred; external notifications, terminal title,
Which-Key, theme-count parity, remote packs, and marketplace delivery are
excluded. No live-provider calls were made for this documentation closure.

| Evidence area | Closure record |
| --- | --- |
| Renderer/performance | Deterministic `ava_tests.tui_composer` budgets recorded in F6 and `docs/operations/testing.md` |
| Real terminal | 14 isolated tmux scenarios with private roots, deadlines, and cleanup |
| Direct PTY | Kitty transmit/delete, iTerm2 OSC 1337, lifecycle/termios, and OSC8/OSC52 CTests |
| Manual limits | External pixel quality, fixed 9x18 fallback, and broad screen-reader certification remain explicitly unclaimed |

The final closure reran the default suite with 85 executed checks and 21
expected optional skips, all 14 tmux scenarios, all four direct PTY/protocol
smokes, and six focused ASan/UBSan checks covering composer, application
runtime, and the protocol drivers. Linux packaging, Python compilation, eight-
document local-link validation, and patch hygiene passed. Fifty-eight current
tmux evidence files across nine scenario roots had final newlines and no ESC
or unexpected C0 bytes; representative ordinary, narrow, short, permission,
and question captures retained their asserted dimensions. No live-provider
call was used.

Two initial 14-way closure waves exposed old 8- and 12-second per-step budgets
at the final allowed `!pwd` and bounded-output `/bash` outcomes under host CPU
contention. Isolated scenarios passed, and the preserved session evidence
showed resolver `allow` decisions for both commands rather than permission
failures. `F8-TMX-001` therefore raised only those outcome waits to 30 seconds,
still below each scenario's 60-second CTest deadline; the final 14-way wave
passed. Documentation review findings `F8-DOC-001` through `F8-DOC-003` are
closed: current summaries now state the malformed-v4 and unresolved-tool-pair
fail-closed boundary, the capability matrix records shipped RPC/TUI attachment
input, and `docs/operations/testing.md` labels the current closure rather than the old
59-test checkpoint as latest evidence.

A post-roadmap offline dogfood pass then closed `DGF-001` and `DGF-002`:
expanded denied shell cards now render and copy one curated permission audit
without hiding unrelated shell output, and `/new` receipts are title-first with
created and previous IDs available exactly once. Review approved both fixes.
The final default 106-test suite passed with 21 expected optional skips, all 14
tmux scenarios passed, and focused ASan/UBSan composer plus application-runtime
checks passed. The refreshed permission and session-receipt captures had no ESC
or unexpected C0 bytes. No live-provider call was used.

### Post-roadmap visual dogfood — quiet-canvas refresh

A 2026-07-24 headless tmux review found a cohesive design issue rather than a
functional gap: cyan carried too many hierarchy levels, the sparse rail
competed with ordinary conversations, selectors lacked visual depth, and
expanded tool output could repeat a long absolute workspace path. OpenCode was
used only as a behavioral reference for quiet surfaces, asymmetric hierarchy,
and progressive disclosure. No desktop terminal was used after the initial
capture mistake; all implementation evidence came from private tmux/PTY roots.

The built-in dark/light palettes now use neutral muted hierarchy and reserve
blue for focus/actions, while warning, error, success, plain mode, and custom
palette contracts retain their semantic boundaries. Modal questions and
selectors dim safe transcript context with the existing thinking tone before
drawing their composer surface, so focused work has depth without adding
borders or a new component system. User, assistant, system, tool, composer,
and select rendering keep their existing semantic roles.

The automatic rail is now progressive: actionable running activity or modified
files may disclose it from `144x16`, idle session/context metadata waits until
`176x16`, and permission prompts, questions, or selectors suppress it. The
exact `/sidebar` drawer remains available at every size and retains full
terminal width. Ordinary conversation, composer, prompt, and selector content
is capped at 120 columns on wider terminals and centered as one canvas;
`160x48` and short-wide captures therefore have a 20-column left inset, while
`120x36`, narrow, and automatic-rail main panes retain their established
origins and widths.

Expanded tool presentation may replace one explicit mutation-argument relative
path with one proven canonical absolute `changed_path` alias from the same
single-path item. The projection is renderer-only: canonical timeline data,
backend/session state, unrelated absolute paths, and copy payloads remain
unchanged. When Shift+Tab changes reasoning while the rail is hidden, one
muted one-action feedback row supplies feedback without expanding the settled
footer.

Deterministic coverage locks the two rail thresholds, prompt suppression,
modal backdrop geometry, palette role values, renderer-only path aliasing,
canonical copy preservation, and hidden-rail reasoning feedback. Updated tmux
coverage asserts `176x48` idle rail disclosure, the exact centered 120-column
canvas and derived SGR click at `160x36`, `176x36` rail-boundary hit rejection,
focused permission/question/select surfaces, terminal bounds, and cleanup. This
visual pass does not authorize a new framework, permanent chrome, footer
metadata, or pixel-perfect clone.

The visual review ledger is closed. `AVA-TUI-001` restricts path aliases to one
explicit relative mutation argument and one unique changed path, so multi-file
same-basename cards remain distinct. `AVA-TUI-002` maps muted, thinking, and
dimmed SGR values to the ncurses muted role before blue accent and keeps light
muted/active colors distinct. `AVA-TUI-003` moves reasoning-cycle confirmation
into one-action presentation state, suppresses it when the rail or another
focused surface owns the area, and clears it on the next actual user input
without changing rail scroll geometry. Re-review accepted all three fixes.

Final no-live-provider validation passed: the default 106-test suite had 85
passes and 21 expected optional skips; the opt-in tmux wave passed 14/14; the
four direct PTY/protocol smokes passed; focused ASan/UBSan composer and
application-runtime tests passed 2/2; Linux packaging passed inside the default
suite; and 61 refreshed plain evidence captures had final LF with no ESC or
unexpected C0 bytes. Representative centered, rail, palette, permission,
question, and selector captures were inspected, all smoke processes cleaned up,
Python compilation and whitespace checks passed, and no desktop terminal or
live provider was used for final evidence.

## 9. Historical first implementation slice: baseline-first visual/layout thread

This is the original pre-implementation plan, retained as acceptance history;
it is not a current unshipped work claim. Its constraints governed the F1–F5
implementation and remain useful when evaluating later changes.

### Deliverables

1. **Character-cell baseline matrix.** Use the completed F0 matrix at exact
   wide 160x48, ordinary 120x36, narrow 80x24, and short 100x12 dimensions as
   the before-redesign record. It reports the current implementation rather
   than an unreviewed compatibility promise.
2. **Named evidence.** Extend the completed F0 evidence catalog with stable
   names such as
   `frontend-wide-idle-shell`, `frontend-ordinary-active-run`,
   `frontend-narrow-command-palette`, `frontend-narrow-permission`,
   `frontend-permission-required-tool`, `frontend-short-selector`,
   `frontend-tool-result-compact`,
   `frontend-tool-result-expanded`, `frontend-diff-entry`,
   `frontend-session-navigation`, `frontend-plain-no-color`,
   `frontend-resize-redraw`, and `frontend-terminal-cleanup`.
3. **Responsive policy.** Define width/height breakpoints and sidebar policy:
   when it is visible, compacted, hidden, or invoked; how its state is
   disclosed; how modal focus returns; and which information cannot disappear.
4. **F2 generic transcript/card shell.** Establish only card placement,
   grouping, spacing, one primary summary line, generic collapsed/expanded
   placement, and no adjacent duplicate summary.
5. **F5 detailed tool/permission work.** Define status/target/outcome/duration
   anatomy; queued/pending, running, success, failed, canceled, denied,
   `permission required / awaiting decision`, truncated/spill, changed-path,
   and diff states; progressive inputs/output/IDs, specialized previews, copy,
   paths/diffs, permission linkage, and detailed evidence. The compact
   permission-required state visibly states the requirement and risk and points
   to an available decision action; its prompt/expanded detail retains reason,
   request identity, allow/reject choices, and follow-up.
6. **Overlay normalization.** Give palette, select-list, permission, and
   question overlays a shared geometry and narrow-width policy. Make permission
   choices concise without losing allow/deny/remembered-choice meaning.
7. **Preservation checks.** Prove the minimal footer and existing input/draft
   behavior remain intact, including plain Up/Down transcript-only scrolling.

### Slice exit evidence

- Renderer fixtures cover each named state that is deterministic in-process.
- Isolated tmux captures cover shell, narrow modal, selector, compact and
  expanded tool cards at 160x48, 120x36, 80x24, and short height, resize, and
  cleanup states that need a PTY. Deterministic narrow/plain fixtures and a
  Python tmux capture specifically cover the permission-required/awaiting-
  decision prompt/tool state, not merely allowed/denied outcomes.
- Capture review compares before/after hierarchy without claiming pixel-perfect
  terminal rendering.
- A control-sequence scan verifies saved text captures do not retain unexpected
  escape bytes, and PTY teardown restores terminal state.
- Any unavailable semantic field appears in the backend dependency ledger,
  rather than becoming a frontend inference.

## 10. Verification and evidence policy

Use repository wrappers, never raw `cmake` or `ctest`, so configured-tree
locking, parallelism, and project test conventions are preserved. Do not run
build and test wrappers concurrently in the same build tree.

For every interactive frontend change, the implementing coder/agent must add
or update deterministic C++ coverage first. When focus, cursor, resize, key
decoding, modal visibility, mouse behavior, or real screen composition is
affected, they must then add or update the closest isolated Python tmux
scenario. `tests/tui_tmux_smoke.py` together with
`tests/tui_smoke_helpers.py` is the required Python real-TTY harness for
interaction and layout behavior that unit rendering cannot prove. No handoff
may say only that it “looks correct from code.”

Focused deterministic work should start with the closest selected suite, for
example:

```sh
scripts/run-tests.sh --jobs 8 -R '^ava_tests\.tui_composer$'
```

For the ordinary-Space regression, the scenario calls the existing
evidence-saving helper after its cursor/no-palette assertions and persists
`composer-ordinary-space-no-completion.txt`. Run its exact isolated scenario,
then inspect and report that artifact together with the cursor
assertion/result:

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --jobs 1 -R '^ava_tui\.tmux_smoke_main_slash_completions$'
```

Run the 23 isolated tmux scenarios only when the change touches their
behavior or required visual evidence:

```sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --build-dir build --jobs 23 -R '^ava_tui\.tmux_smoke_'
```

Run protocol-specific opt-ins when the implementation affects them:

```sh
AVA_TUI_KITTY_IMAGE_SMOKE=1 scripts/run-tests.sh --jobs 1 -R '^ava_tui\.kitty_image_smoke$'
AVA_TUI_ITERM2_IMAGE_SMOKE=1 scripts/run-tests.sh --jobs 1 -R '^ava_tui\.iterm2_image_smoke$'
AVA_TUI_TERMINAL_LIFECYCLE_SMOKE=1 scripts/run-tests.sh --jobs 1 -R '^ava_tui\.terminal_lifecycle_smoke$'
AVA_TUI_OSC8_SMOKE=1 scripts/run-tests.sh --jobs 1 -R '^ava_tui\.osc8_smoke$'
```

When scope warrants it, run the full default suite through the wrapper:

```sh
scripts/run-tests.sh
```

Every documentation or implementation closure also runs:

```sh
git --no-pager diff --check
```

Run the targeted Python scenario and inspect its pane capture/evidence for
visible text, cursor position, dimensions, modal/menu absence or visibility as
appropriate, no leaked controls, and terminal cleanup; report what was tested
for real. Review visual captures at the baseline cell sizes rather than relying
only on text assertions. For PTY work, inspect terminal-state cleanup, expected
control-sequence emission, and absence of leaked control bytes in persisted
text evidence. Provider calls are unnecessary: use the fake provider or an
offline fixture for frontend visual validation.

## 11. Non-goals and deferred scope

The following are not implied by this roadmap:

| Item | Disposition |
| --- | --- |
| Pixel-perfect OpenCode clone | Excluded; AVA keeps a native terminal visual language |
| OpenTUI/Solid/reference architecture | Excluded; behavior reference only |
| Web, desktop, or SaaS surfaces | Excluded from this terminal roadmap |
| Theme-count parity, remote packs, marketplace delivery | Excluded by F7; local built-in/custom selection is complete |
| External notifications | Excluded by F7; existing in-TUI attention remains the accepted path |
| Workspace or working-copy systems | Excluded from this frontend scope |
| Marketplace | Excluded by F7; no remote plugin/theme marketplace is approved |
| Arbitrary plugin renderers or terminal control | Excluded from the approved extension UI boundary |
| Constrained host-rendered plugin UI | Implemented by the separate Pi-inspired TUI feature expansion plan; this historical F0–F8 roadmap did not implement it |
| Share, update, telemetry, or upsell flows | Excluded |
| Automatic Up/Down draft/history behavior | Excluded; settled AVA control differs |
| Composer-footer metadata expansion | Excluded; settled minimal footer differs |
| Broad reusable component rewrite | Deferred unless separately approved with evidence |
| Heavy terminal dependency | Deferred unless separately approved with a clear payoff |
| Standalone diff viewer | Deferred behind stable diff identity, bounded permission-aware retrieval, and version/truncation/path semantics |
| Broad screen-reader certification | Deferred pending a dedicated accessibility audit |

## 12. Decision ledger

| Decision | State | Rule for future work |
| --- | --- | --- |
| OpenCode is a behavior/quality reference, not source or architecture | Preserved | Compare outcomes only; do not port implementation patterns wholesale |
| Native C++23/ncursesw and narrow semantic seams | Preserved | Keep rendering out of backend contracts |
| Plain Up/Down scroll transcript only | Preserved | Do not move or replace drafts; vertical/history actions stay configurable and unbound by default |
| Minimal composer footer | Preserved | Show active model, active conversation-context usage (`ctx count (percent)` or estimated `ctx ~count`), and a fixed four-cell signal meter while active; ordinary draft/status/footer rows inherit `screenBg`, while palettes/selectors keep elevated `composerBg` |
| Transcript wheel step | Preserved | Three rendered rows per accepted transcript wheel event; same-direction 40ms coalescing; immediate reverse; selectors/questions/drawer/selection-edge stay one row |
| Human labels without authority swap | Preserved | Permission summaries, keybinding help labels, and completion display labels are human-primary; exact `permrule_…`/job/action ids remain hidden insertion/control authority and never yield to ordinals or labels |
| Ordinary Space completion policy | Preserved | Ordinary Space never opens file/reference completion; explicit Tab may force empty-token path suggestions, while real `@` and path-like prefixes remain valid triggers |
| Renderer tests plus PTY smokes as current evidence strategy | Preserved | Add a screen model only after demonstrated evidence failure or approval |
| Responsive sidebar/shell policy | Refreshed by visual dogfood | Disclose actionable activity/modified files from `144x16`, idle metadata from `176x16`, suppress the rail for prompts/selectors, and keep complete `/sidebar` disclosure |
| Compact tool/result hierarchy | Preserved | Keep one resting primary/context row while retaining complete expanded and copy detail |
| Shared overlay geometry and narrow permission choices | Preserved | Keep fixed-row selectors, shared hit testing, keyboard/mouse parity, plain-mode meaning, and safety detail |
| Existing in-TUI attention; external notifications | Accepted / Excluded | Keep existing attention; do not add backend events or footer notification surface |
| Terminal title | Excluded | Do not add without a privacy-safe restoration/multiplexer design and session-title TUI contract |
| Standalone diff viewer | Deferred | Require backend-owned stable diff identity and bounded permission-aware retrieval with version/truncation/path semantics; retain `/diff`, expanded cards, and `/copy diff` |
| Dedicated Which-Key overlay | Excluded | Keep `/help`, `/hotkeys`, and `/keybindings` as effective-binding discovery |
| Local built-in/custom themes | Complete | Do not infer theme-count parity, remote packs, or marketplace delivery |
| Session revert/redo | Deferred | Require an explicit reversible-session contract and product decision |
| Background-job controls | Complete in later work | Preserve the application-owned job lifecycle and existing `/jobs`/RPC surfaces |
| Constrained host-rendered plugin UI | Implemented / constrained | The separate Pi-inspired TUI feature expansion plan implements the bounded, attributed, command-scoped boundary only |
| Arbitrary plugin renderers or terminal control | Excluded | Do not grant plugins layout, curses, raw terminal, theme, editor, or input authority |
| Pixel-perfect clone, OpenTUI/Solid, web/SaaS, marketplace, upsell flows | Excluded | Do not add through frontend polish work |

## 13. Approval cut line

This historical cut line governed the staged F0–F6 implementation. F8 now
closes the roadmap: completed work is recorded above, and no further
implementation is authorized by this document alone. The
[historical Pi-inspired TUI feature expansion record](../history/tui-pi-feature-expansion.md)
records the retained expansion waves and their final integrated verification gate. Wave 4's local-only setup wizard was removed and superseded by product decision; the constrained host-rendered plugin UI remains implemented, while arbitrary plugin renderers and terminal control remain excluded.

## Approved post-roadmap rich-tool and responsiveness pass

A later explicit product decision supersedes the historical compact-first tool
checkpoints above. Tool cards now default to **Rich** presentation: the complete
human-readable call wraps under the tool header, useful sanitized output follows
on separate muted rows, shell previews favor the actual output tail, and hidden
output is reported exactly when authoritative logical-line totals are available
or conservatively otherwise. **Compact** remains an explicit low-noise mode and
**Expanded** retains bounded diagnostic/output/diff/spill detail. `/details
compact|rich|expanded`, bare `/details`, Ctrl+O, and per-card `/tool` work during
idle and active runs. Routine permission/question receipts, resolver identities,
and `permission checked/required` wording are absent from ordinary cards and
`/copy tool`; the active approval prompt still says `Permission required`, denied
cards retain an actionable human reason, and explicit `/permissions` plus `/copy
permission` remain the audit surfaces.

Active questions use a distinct configurable background and wrap their full text.
Routine idle and active-run key, wheel, provider, and signal-meter repaint requests are
coalesced into 16 ms frames; input state still mutates immediately, full-frame
requests supersede footer-only requests, draw failures latch, and immediate
lifecycle/terminal barriers remain synchronous. Transcript wheel accepts three
rendered rows per event, same-direction bursts coalesce at 40 ms, reversals remain
immediate, selectors/questions/drawer/selection-edge stay one row, detached
transcript layouts stay frozen during draft-only repaint, and pending
assistant/reasoning Markdown is not cumulatively parsed before the incremental tail
renderer. Nested permission and question prompts drain same-direction wheel bursts
without swallowing the following confirmation input.

Deterministic tests cover scheduler deadlines/failure, 100-request coalescing,
frame-scoped wheel runs, frozen detached layouts, streaming text projection,
Rich/Compact/Expanded cards, 1,000-line shell tails, near-512-KiB single-line
output, permission-audit omission, wrapped question surfaces, and cache identity.
The credential-free fake-provider tmux matrix now has 23 isolated scenarios, including `nested_settings_preview`, `startup_overview`, `branch_summary`, `plugin_ui`, and `mermaid`;
`streaming_scroll` and `active_run` measure idle/streaming flood responsiveness, active presentation
commands, draft preservation, detached stability, prompt wheel ordering, resize
synchronization, terminal hygiene, and cleanup.
