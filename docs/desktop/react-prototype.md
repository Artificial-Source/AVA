# React UI prototype

## Revision 18 — stable chat tools and supplied runs (current)

Quiet per-tool disclosures, retained output/diff nodes and independent reading-aware
following now support finite supplied incremental text/tool updates. Explicit Run
preview plays Completion/Failure/Burst scenes without replying to a draft or
calling a model. Send stays local-user-only; Stop/Retry and departure are scoped.
Astra MAX delivery passed **699 tests / 106 native checks / eight painted-text
states**. The sole Sol 6.1 MAX **38 actual files / 24 actual PNGs** gate returned
**No material findings.** Whole optional owners yield a **351,358-byte raw entry**
and **148,642 bytes headroom**, without changing the 500,000 cap or dependencies.
[Scope, limits, fixed native findings and 22 captures](chat-run-refinement.md).
Earlier “current” headings below remain revision history; backend/native wiring is
separate from this browser-only acceptance.

## Revision 17 — quieter Settings navigation (current)

Only Settings' left scrollbar chrome is hidden; native wheel/keyboard access,
right/stack scrolling and header stay intact. **Reset all preferences…** moves to
a quiet General footer text action, separate from page-local Reset General and
opening the existing confirmation. Astra MAX delivery passed 614 tests/44 native
checks/three painted-text states; the sole Sol 6.1 MAX eight-file/nine-actual-PNG
gate found no material issues. Entry remains 499,991 raw bytes/9-byte headroom.
[Scope and eight captures](settings-scroll-layout.md#revision-17-quieter-settings-navigation).
Earlier “current” headings below remain revision history.

## Revision 16 — independent Settings scrolling (current)

The header/Back stays visible while desktop navigation and content scroll
independently; ≤1100px uses one wrapped navigation/content stack. Native wheel,
keyboard seams, all 16 pages, resets, overlays and Back/chat preservation passed
324 checks/five sizes, alongside 611 tests/typecheck/warning-free build. Astra MAX
implemented; the sole Sol 6.1 MAX six-file/21-actual-PNG gate found no material
issues. Entry remains 499,991 raw bytes (9 below the unchanged cap).
[Current layout acceptance and 20 captures](settings-scroll-layout.md).
Older “current” headings below are retained revision history.

## Revision 15 — five working Settings bodies (current)

General and Shortcuts now have genuine local behavior; Conversation behavior,
Notifications & sounds and Context management add independent searchable pages.
All 16 leaves remain available. Native recording dispatches real app actions;
future-chat defaults, tool/wrap/scroll preferences, explicitly clicked bounded
Web Audio and safe context-policy projections work without backend authority.

Astra MAX delivery passed Main's 605 tests, typecheck, 493-module warning-free build,
210 native checks, 138 scenarios and 18 finite painted-text states (minimum 5.405).
Sol 6.1 MAX inspected all 42 changed files and 27 actual images: **No material
findings.** Named native context-edit F01 is fixed. Entry 499,991 bytes remains below
the unchanged cap; nine bytes headroom. No frozen dependency/config/asset/backend
or F03/Composer/global-style changes.

[Current details](settings-behavior.md) and
[26 current captures](concepts/react-settings-behavior/) own acceptance. Visit-local
only; no OS notifications, actual compaction, storage or native-host integration.
Earlier “current” headings/counts/entry notes below remain unchanged dated history.

## Revision 14 — working Appearance customization (current)

The normal app now has neutral dark presets, local font choice, conversation prose
size/spacing, bounded reading width, higher contrast, synchronized sidebar material
and application-wide logical reduced motion, with a live sample and appearance-only
reset. Defaults and backend/model/resource authority are unchanged; edits last only
for this visit. Light/System themes and arbitrary CSS/fonts are not implemented.

Astra MAX delivery passed Main's **480 tests/34 files**, typecheck, warning-free
482-module build, 98 native checks, 135 existing scenarios and 16 painted contrast
states (minimum 4.819). One Sol 6.1 MAX source/18-actual-image review is clear.
Entry 498,306 bytes is below the unchanged 500,000 cap; the genuine optional readonly
child body preserves Loading/error/Back/title/model authority. No frozen Composer,
global/token/F03/dependency/config/backend/asset/history changes.

[Current details and preservation](appearance-customization.md) and
[18 current captures](concepts/react-appearance/) supersede earlier limited
Appearance and byte-headroom wording. Older records below remain unchanged history.

## Revision 13 — provider identities, resource pages and chat tools (current)

The normal app at **`http://127.0.0.1:5182/`** now has 15 provider logo/name cards,
13 separate Settings pages, user-owned local Subagent definition editing and
conversation-wide tool Expand/Collapse with explicit supplied inline diffs.
Providers remains top-level; Connections has MCP/LSP only; Resources has separate
Subagents/Skills/Prompts/Plugins pages. Actual backend definition models still
inherit the parent; the new model preference is local preview metadata only.

Astra MAX implementation is independently verified: 378 nearest tests, typecheck,
**454 tests/29 files**, warning-free 477 modules, 50 normal-native checks, 135 scenarios,
12 actual diff projections and 16 finite contrast states (minimum 5.405). One Sol 6.1 MAX
source/actual-visual gate found only the new editor's hover contrast; its one-rule
scoped correction and same-reviewer completion pass. Hover Create/Save: 18.423:1.
Entry: 499,990 raw / 152,308 Node-default gzip; unchanged 500,000 cap; 10 bytes headroom.
No package/config/backend/legacy/ComposerCSS/F03 authority change; only approved
static SVG/license/provenance and scoped notice adoption. No real services connected.

[Current scope/evidence](provider-resource-chat-refinement.md) and
[30 current captures](concepts/react-provider-resources-tools/) own acceptance.
All prior records below, including earlier asset-approval holds and eight-section
geometry, remain unchanged **dated history** and are superseded by this section.

## Settings-only navigation and model browser follow-up (current)

At **`http://127.0.0.1:5182/`**, Settings hides the chat sidebar/workspace and owns
the full viewport. Back restores the same mounted sidebar, chat, draft/model/mode,
Context/child inspection and explicit opener focus. Hidden sidebar callbacks fail
closed even during loading or nested dialogs.

Astra MAX also replaced the note-heavy model selector with an original AVA-neutral
browser: selected-model summary, compact current/default scope, search and supplied
family/provider radio-card groups. No repeated caveat slab, guessed vendors,
fake prices or applied reasoning state; About/global accessible context retain
mock limitations. Necessary key/permission warnings remain. Narrow cards stack;
short dialogs scroll with actions in ordinary flow, not overlaid on choices.

Main independently passed **104 focused tests/5 files**, typecheck, **351 tests/
21 files**, warning-free **468 modules**, **43 Settings/sidebar checks**, **18
model-browser checks**, **114 native scenarios/14 families**, six request/catalog
checks and five finite painted-text states (minimum **7.825**). Entry JS is
**499,783 bytes** under the unchanged500kB warning. Sol6.1 MAX inspected the source,
14fresh app images and the2authorized reference images: **no material findings**.

[Current scope/evidence](normal-app-refinement.md#settings-only-navigation-and-model-browser-follow-up)
and [16 fresh PNGs](concepts/react-model-browser/) include the
[main model browser](concepts/react-model-browser/ava-react-model-browser-main-1440.png)
and [Settings without chat navigation](concepts/react-model-browser/ava-react-settings-only-1440x960.png).
Previous revision12/11/older records and captures below remain unchanged history.
This is still browser-only, not live provider/auth/Electron/backend integration.

## Revision 12 — normal-app refinement (current UI phase)

The normal entry is **`http://127.0.0.1:5182/`**. Astra at max reasoning has
implemented a dedicated Settings page with nav-local search/remembered section,
a precise existing-library cog, minimal shared native-modal chrome, visible
supplied tool outcomes, human clipboard-image labels and immutable per-turn
model/thinking facts beside AVA. Current-model changes never rewrite past headings;
unknown facts remain omitted. The local Enter preference now operates the composer
with Shift+Enter/IME guards; Send remains user-only, not an invented model call.

Main independently passed typecheck, **119 nearest tests/eight files**, full
**337 tests/21 files** (10.06s), and a warning-free **468-module build**. Entry is
**499,945 bytes** under the unchanged 500kB warning. Context/Changes uses a real
optional inspector boundary; accounting math/projection/cost semantics and
Composer CSS are preserved, with no bundler/configuration workaround.

Chrome passed **52 normal-app checks**, **114 native scenario observations/14
families**, six request/catalog checks and six actual-motion checks. Fourteen
finite contrast states have no sampled active range below 4.5, normal/tool minimum
**5.405**. **35 layout checks/seven viewport variants** pass. Main verified **48
native Tab observations**: BODY/foreground at a native edge reproduced in a bare
`showModal` control with no AVA JS, so no app fix/trap/timer was introduced.
Explicit provider/request transition focus checks and the narrow F03 guard remain.

**Sol 6.1 MAX reviewed the integrated source and read 25 fresh normal-app images:
no material findings.** There are **44 fresh PNGs** in
[react-normal-app](concepts/react-normal-app/), including the
[normal chat](concepts/react-normal-app/ava-react-normal-app-normal-initial.png),
[Settings page](concepts/react-normal-app/ava-react-normal-app-settings-general.png)
and [main model picker](concepts/react-normal-app/ava-react-normal-app-main-model-picker.png).
The **1,724 → 1,773** file audit has **44 modifications, 49 additions** (including
44 PNGs), no deletions and **1,680 protected unchanged** files. HEAD/staged/tracked
unstaged state, Composer CSS, backend/legacy/config/dependencies and the complete
R11/older record suffixes remain unchanged. Docs/structure/verifier/whitespace
checks pass. UI-phase validation is complete; provider-logo adoption is held for
explicit approval, with no brand assets or dependency added. Read the
[normal-app refinement and provider-mark record](normal-app-refinement.md) for
scope, pinned behavioral references and the bounded local-SVG proposal.

This is still browser-only/in-memory, not provider/auth/backend/Electron/file or
persistence integration. Revision 11 and earlier sections below remain verbatim
history; their date-relative “current” labels do not supersede revision 12.

## Revision 11 — everyday screen pack (current)

The first-party everyday screen pack remains separately lazy at `?view=screens`:
**14 families / 114 manual scenarios**, including all eight settings sections.
Its canonical case index remains
[`src/screens/fixtures.ts`](../../apps/desktop-preview/src/screens/fixtures.ts).
The [screen inventory](screen-inventory.md) records the scope/reference comparison;
no reference source, theme, fonts or assets were copied.

Main chat now opens the same settings/model views from one sidebar Settings
button and its composer model button. A separate lazy `MainScreens` host owns
only presentation of wide native Settings plus nested models/providers/reset;
the review controller and catalog are not default-entry imports. Native pending
modality and latest identity/version/lifetime guards prevent background Send,
retargeting and stale callbacks during load, cancellation and reopen. The base
Settings stays mounted under inner dialogs; root close closes them all. Escape
handles search/inner steps before section/root dismissal and ignores IME input.

Current-model selection affects only the captured local parent and preserves
its draft/mode/context/expansion/pins/messages/title and Context disclosures.
Read-only child inspection disables that selection, but not the new-chat default.
Fixed Working fixtures remain editable. Main's reasoning display is **facts only**:
there is no reasoning field or invocation binding. The review picker retains its
functional local reasoning rehearsal.

Aster is the initial local new-chat default. The only reducer schema extension is
optional `newChat.model`, validated with Aster fallback; new chats still use Build
and have unknown usage/cost rather than inherited snapshots. Settings preferences
persist across close/reopen in focused UI state and affect only the review views.
A native confirmation resets preferences/default without changing existing chats.
Trust/resources remain read-only, and About offers the small screen-review link.
Providers use only the same ephemeral dummy key and manual browser/device examples;
no live authentication, URLs, clipboard, polling, storage or backend/configuration
authority. Light/System/rebinding remain truthfully unavailable.

**Astra at max reasoning owns this frontend source/design phase. Main independently
mock-validated the combined result on 2026-10-07: 316 tests and all 114 scenarios.**
Existing Context math/snapshots, composer CSS, canvas geometry, centered empty-chat
mark, catalog native-select fallback and all older design/evidence history are
preserved. This is a browser-only review, not live auth/backend or native-client
qualification.

Current source ownership: `src/components/AppShell.tsx` holds focused UI state and
latest scope guards; `ProjectSidebar.tsx` adds the Settings/New-chat callbacks;
`Composer.tsx` and `chat/widgets/ChatComposer.tsx` add only optional model-open UI.
`screens/MainScreens.tsx` lazily reuses `SettingsScreen`, `ModelDialog`,
`ProviderViews` and shared primitives. `DialogFrame` adds the wide variant;
`contracts.ts` adds facts-only/model-scope presentation types. `state/types.ts`
and `reducer.ts` add only the optional bounded creation model. The existing
`ScreenReview` controller and 114-case index remain unchanged. Only shared model,
provider and preference values move to `screenExamples.ts`, re-exported by fixtures,
so opening main screens does not load the standalone case index. See the updated
[README source map](../../apps/desktop-preview/README.md#revision-11-source-map).

### Revision 11 independent validation

Main's final checks: typecheck; **316/316 tests in 19 files** (9.57s), including
**23 focus-hook tests**; warning-free **468-module build**, entry **499,889 bytes**
with the unchanged 500kB warning. Main Settings/model views stay lazy; opening
Settings does not load the standalone ScreenReview index/controller or catalog.

Normally sandboxed Chrome 154 passed **114 native scenario observations in all
14 families**, **107 interaction checks** (50 standalone, 23 main integration,
23 preserved chat, 11 actual motion), **28 transition/non-interference checks**,
24 native Tab/Shift-Tab observations and actual secret-answer Enter submission.
Seven shell layouts plus **13 modal viewport/internal-scroll/focus layouts** cover
normal, narrow, short and ultrawide variants; no checked document overflow,
duplicate IDs or lost foreground modal focus after explicit transitions. Native Tab's browser-chrome
boundary is allowed, not replaced with a JavaScript trap.

**SCREEN11-F01/F02/F03 are fixed.** Provider success is completed white, model
Escape is filter-first, and accepted explicit dialog transitions restore usable
focus locally. The named Chromium disable-in-place transient keeps a captured
control active during a layout effect, then drops to BODY; the helper accepts
that now-disabled captured control too, never another/enabled focus destination.
Foreground, top-modal visibility, IME, dismissal/unmount and scope guards remain;
no timer, observer or focus watcher. The integrated source review and the final
named F03 delta recheck have no remaining material findings. Coding history below
is not the final native acceptance record.

Finite contrast across eleven snapshots: shell/Context **97/123 active ranges,
minimum 5.405**; Settings/Appearance/models/device/success/permission/question/run
**25/30/35/14/11/16/16/10, minimum 6.805**; initial catalog **32 active, minimum
7.130**, one disabled range separate. Computed foreground/rendered backgrounds,
clipping and paint controls were used; icons, native values/placeholders, hidden
text, background-modal content and other positions/states are excluded. Not
all-state WCAG, assistive-tech, GPU/performance or Electron qualification.

**45 fresh PNGs** are in [react-screen-pack](concepts/react-screen-pack/), including
[Settings](concepts/react-screen-pack/ava-react-screen-pack-settings-1440.png),
[model picker](concepts/react-screen-pack/ava-react-screen-pack-models-1440.png),
[nested provider](concepts/react-screen-pack/ava-react-screen-pack-provider-1440.png)
and all fourteen screen families. Console/page errors are empty; recorded HTTP
page requests are loopback-only. Eight catalog/lazy checks preserve the native
select fallback. Markdown/structure/verifier tests and tracked/untracked whitespace
pass. The **1,654 → 1,724** preservation audit has **18 authorized changes,
70 additions, no deletions, 1,636 protected files unchanged**; Git snapshots,
Composer CSS, dependencies/configuration/backend/legacy source and R10/older
history/evidence remain intact. No C++/Electron/live-provider checks are claimed.

Preview **`http://127.0.0.1:5182/?view=screens`** supplies resettable manual fixtures;
Settings/model controls also work locally from main chat. Production/native auth,
public adapter gaps, persistence, light/system, legal/SBOM and packaging remain
separate. These results do not pre-approve the user's subsequent refinements.

### Revision 11 Phase-2 coding validation

Nearest new/affected checks: **187/187 in 11 files** (8.38s); typecheck passed.
Full tests: **293/293 in 18 files** (9.37s), including **24 added regressions** for
lazy pending cancellation/reopen, latest scope/parent ownership, native stacking,
shared select fallback, facts-only reasoning, new-chat defaults/unknown telemetry,
local reset and preference retention. Native dialog support exists only in tests.

Warning-free production build: **467 modules**, 1.61s. Entry JS **499.89kB /
151.68kB gzip**; lazy MainScreens **5.28/2.31kB**, shared settings/screens
**27.93/8.97kB**, ScreenReview **30.80/10.00kB**. The first build exposed a large
multi-weight gear import; the new button now uses a small first-party inline glyph,
not chunk shuffling or a warning threshold/configuration change. `dist` was rebuilt;
no server restart, dependency or configuration change.

**SCREEN11-F01** is fixed in source: connected/success examples use completed
white, not Working green; palette mapping is regression-tested. **SCREEN11-F02**
is fixed in the shared ModelDialog: first Escape clears a model filter, next
Escape dismisses; IME, focus and preserved Settings are covered by DOM tests.
Quiet 1,654-file/Git preservation and tracked/untracked-text whitespace checks
pass, with only authorized changes and no deletions; Main’s category link and
inventory are classified separately. R10/older history remains verbatim.
Native-browser geometry, Tab loops, contrast and combined final acceptance remain
Main-owned follow-up, not claims of this coding pass.

### Revision 11 Phase-1 coding validation

Nearest new tests: **56/56 in 3 suites** (5.42s); typecheck passed. Full tests:
**269/269 in 16 suites** (7.95s), preserving the existing 213-test baseline.
Warning-free production build: **464 modules**, 1.57s, main JS **494.67kB**
(**149.77kB gzip**), separately lazy screen-review JS **59.16kB**
(**17.70kB gzip**). No chunk-warning threshold/configuration change. `dist` was
rebuilt; the existing server was not restarted. These code/DOM checks do not
claim browser geometry, native focus-loop or independent acceptance evidence.

## Revision 10 main-chat baseline (preserved)

**Current: revision 10 — dedicated Context page and selected-profile composer meter.**
Astra at max reasoning removes the shared ParentDetails row and adds one flat,
attached Context module alongside Agents and Changes. The composer explicitly
opens it with **173.12k (81%)**, derived from a supplied fictional Aster profile
and active estimate, never cumulative billing or draft text. Provider/model,
configured window, independent cumulative usage/cost, local message counts and
read-only invented prompt/raw projection share one scrolling page. The neutral
AVA style, one canvas-top-left title, parent/child ownership and existing motion
remain intact. Empty new chats show only the centered AVA mark above the composer.
[Revision-10 independent validation](#revision-10-independent-validation) is complete:
**213 tests, 63 browser checks and seven layouts**, with no material source findings.
These are bounded mock checks, not production qualification.

Revision 9's [coding checks](#revision-9-coding-pass-validation) and
[independent validation](#revision-9-independent-validation) are preserved history:
**179 tests, 50 browser checks and seven layouts**. Its title behavior remains;
its shared details row is superseded, not revision-10 evidence. The native client
remains unconnected: no backend, Electron, provider or persistence integration.

Revision 8's [coding checks](#revision-8-coding-pass-validation) and
[independent validation](#revision-8-independent-validation) remain protected
history: **178 tests, 70 browser checks and seven layouts**. Its sidebar identity
and conditional reading-aligned title cue are superseded, not revision-9 evidence.
SIDEBAR8-F01 remains closed. Revision 10 deletes the obsolete ParentDetails
component; its new positioned Context view and native long-scroll checks preserve
the containment outcome without retaining the deleted CSS declaration.

Revision 7's [Phase-1 coding checks](#revision-7-phase-1-coding-pass-validation),
[Phase-2 kit checks](#revision-7-phase-2-coding-pass-validation), and
[combined independent validation](#revision-7-combined-independent-validation)
remain preserved baseline history: **169 tests, 88 browser checks**, six layouts
including both ultrawide targets, and one integrated source review with no material
findings. Its shared-header layout is superseded, not evidence for revision 8.
The separately lazy-loaded `?view=components` catalog is unchanged. The composer
now accepts optional usage/open intent; other existing controlled widgets remain
unchanged.

Revision 6's [coding checks](#revision-6-coding-pass-validation) and
[independent browser validation](#revision-6-independent-browser-validation),
including its [icon/text recording](concepts/react-working-motion/ava-react-working-motion-loop.webm),
remain **superseded history**, not revision-7 acceptance: 89 tests, 10 targeted
browser checks, 13 unforced frames, four layouts, and 103 finite contrast samples
(minimum 5.782:1). Its WORK6-F01 correction and bounded review do not qualify the
new icon-only treatment or shell motion.

Revision 5's shared status accents remain; its [coding checks](#revision-5-coding-pass-validation)
and [independent browser validation](#revision-5-independent-browser-validation)
are preserved **superseded history**, not revision-6 acceptance: **33 browser
invariants**, four viewport layouts, **13 captures**, and **103 finite contrast
samples (minimum 5.782:1)**. Its bounded source review reported **no material
findings**; these were mock checks, not production certification.

Revision 4's rail-toggle behavior remains; revision 8 supersedes its parent-context
header composition. Its
[coding](#revision-4-coding-pass-validation) and
[browser evidence](#revision-4-independent-browser-validation) are preserved
**superseded history**: **79 tests, 45 browser checks, 18 captures, 103 contrast
samples (minimum 5.782:1)**, and no material source-review findings. These do not
qualify revision 5.

Revision 3 was independently qualified: **74 tests, 40 browser checks, 15 captures,
96 contrast samples (minimum 5.782:1)**, with no material source-review findings.
Its [coding](#revision-3-coding-pass-validation) and
[browser evidence](#revision-3-independent-browser-validation), including the shared
header/chip correction, remain preserved **superseded layout history**, not revision-5
acceptance. Revision [1](#superseded-revision-1-history) and
[2](#superseded-revision-2-history) evidence also remains historical. None of these
bounded mock checks establishes production or Electron qualification.

**Authorized scope:** a standalone browser React/TypeScript prototype with
invented, in-memory fixture data. Code and design are by Astra at max reasoning.
This is not approval or qualification of a production Electron client.

Source: [`apps/desktop-preview/`](../../apps/desktop-preview/README.md). Revision 10
changes only the approved Context/inspector/composer seams and focused tests.
Usage profiles and message projection live in a separate preview adapter, not in
shared controlled widgets or state fixtures. The reducer remains generic and
unchanged; only the module union and generic module tests extend to Context.
AVA marks, palette/local fonts, composer/chip CSS, catalog, dependencies/config,
legacy `apps/desktop/`, backend and all prior captures remain preserved. External
reference behavior informed semantics only; no source, assets or design were copied.

## Current design

- The viewport remains connected edge-to-edge: **no outer frame, wallpaper,
  gutters between permanent regions, or faux OS controls**. The full workspace
  title bar is removed, not hidden. Canvas, attached opaque right pane, and
  persistent **44px rail** begin at the desktop workspace top. The full-height
  project navigation/brand is unchanged. At **1100px and below**, the same pane
  attaches below the canvas; the rail retains the full right edge. No drawer,
  modal, new glass, dashboard cards, or replacement toolbar/footer is introduced.
- The rail's first native **Show inspector / Hide inspector** button restores
  the last chosen module and retains focus. Agents starts open. Same-module rail
  clicks close; another module switches. Panel Close/Escape returns to its rail
  button. Three modules are available: **Agents**, **Changes**, then **Context**.
- The shared **ParentDetails** block and its repeated Context/change/activity row
  are deleted. Agents and Changes contain only their own module. Context is one
  flat page; its header and content scroll together, including short and
  below-canvas attachments. Session title is a stat value, not another identity
  heading. Its CSS contains no absolute accessibility summary.
- The small neutral native composer meter explicitly **opens** Context, never
  toggles it closed or invokes Send/Stop. Fictional configured profiles supply
  Aster Lab / Aster **173,120 / 214,000 → 173.12k (81%)**, Aster Mini
  **48,160 / 128,000 → 48.16k (38%)**, and Orion Lab / Orion's known **96,000**
  window with **unknown** active usage. New/unsampled chats also show unknown.
  No output reserve, fallback limit or marketing capacity is invented. Changing
  profiles selects a matching snapshot or invalidates usage; it never rescales
  the old model's measurement.
- Context separates active estimates from cumulative input/output/reasoning/cache
  read/write, independently supplied total, cost estimate/complete-partial-unknown
  coverage, and supplied timestamps. Safe finite nonnegative integer counts and
  positive windows are validated; missing/invalid/stale/mismatched values do not
  become zero, while supplied zero remains zero. Numeric percentages can exceed
  100%. No subset summation, byte-to-token conversion or simulated local billing.
- **System prompt** and **Raw messages** use controlled native `Disclosure` and
  `Presence`. One compact qualifier identifies fictional fixed snapshots, invented
  prompt and committed local message projection—not actual sent context/provider
  payload. React-escaped prewrapped text stays horizontally contained. Message
  counts come from the supplied projection, not a fixture constant. No private
  reasoning, file/session access, exports, clipboard or upload. Tab/Enter/Space
  remain native; Escape inside an expanded view closes only it and restores its
  focused-body trigger, then Escape closes the pane. Parent navigation, module
  replacement and full pane unmount reset disclosures; model/typing/Back do not
  remount or steal focus. Context remains parent-owned during child inspection.
- **No horizontal chat navigation or full-width identity bar.** One quiet,
  unboxed title line always starts at the **chat-canvas top-left inset**, including
  at ultrawide widths; it is not centered with the reading column. The line uses
  the full available canvas, while title text wraps within a readable 780px cap.
  There is no project or relationship subtitle. A child shows its actual title and
  **one Back to parent**; the host supplies the named parent in Back's native
  tooltip/accessibility description, not a visible subtitle. One stable
  `h1#conversation-title` names the canvas through pane show/hide, module changes,
  and resize. Explicit Inspect/Back focus the committed current title; same-child
  inspection refocuses it. Normal draft/Send/settings/navigation keep focus. No
  placement media subscription, relocation callback, hidden/exiting title, or
  deferred StrictMode focus request remains. Attachment geometry stays in CSS.
- **Projects is the primary, complete chat library.** Optional Pinned and Recent
  sections follow it in the same independently scrollable sidebar flow. They
  are not bottom utilities. Empty sections take no row. Pins start empty. A shared
  lifecycle glyph + label precedes the truncated title; idle has no label.
  Distinct glyphs and labels preserve recognition without relying on color.
  Completed is a fixed fixture label until reload, not unread state or a timer.
- The composer is a **bounded enclosure, maximum 780px**, aligned with the
  transcript reading column. Textbox and controls are inside its restrained
  border/radius. Responsive inset and bottom space use the **same opaque canvas**
  as the transcript: no full-width material stripe, floating/absolute card, or
  heavy shadow. There is no persistent helper or shortcut row under the input.
  A compact Add/context disclosure selects invented file/image/note references;
  removable chips and inline draft expansion are per-parent in-memory state.
  Three original geometric model marks sit beside the native labeled Aster,
  Aster Mini, and Orion picker. No real provider branding or file picker is used.
- Child rows show Working first, then fixed terminal outcomes, with lifecycle
  before the short task title and bounded snapshot/model metadata. Selection opens
  a **read-only central child conversation** with parent identity and Back to parent.
  Its read-only/model metadata sits with the transcript, not a second footer/Back.
  The composer is replaced, not retargeted. No child Send/Stop/Steer/Restart/Promote,
  tool/reasoning stream, progress percentage, or fake verified check is presented.
- Off-black/charcoal surfaces, secondary text, hairlines, primary text, and Send
  remain neutral. **Only shared status glyphs and static labels** gain
  the user-authorized accents: Working green `#8bd5a0`, Completed bright white
  `#ffffff`, Stopped/Failed soft red `#efaaa5`, Attention (approval) warm yellow
  `#e6c77d`, Unavailable muted gray (`--muted`: `#b8b8b8`, `#d5d5d5` with increased
  contrast). This mapping is identical in left parent/shortcut rows, the right
  child roster, and selected parent/child transcripts. Titles, prose, code,
  composer, model marks, focus, glass, bubbles, selections, changes, and details/rail
  summary counts remain neutral. No global hue/filter or theme change.
- **Working only:** the `aria-hidden` glyph wrapper repeats a smooth **360° rotation
  every 2.4s, linear**. The label is static green. **All text-shadow glow and radial
  halo CSS is removed**, not paused or retained as a static highlight. Reduced
  motion disables the cycle. Every nonworking status stays static; no run timer,
  liveness, progress, or settlement is introduced.
- Controlled `Presence`, `Disclosure`, and `useReducedMotion` in `src/ui/` provide
  the reusable foundation. Project and prompt/raw disclosures/carets and
  attached inspector tracks use **200ms ease-out**, with **180ms hover/selection** feedback. Module replacement
  has one finite 200ms opacity/4px entry, with no old-module overlay. Logical close
  and focus changes happen immediately; exiting DOM is inert, aria-hidden, and
  callback-guarded, then removed within 200ms. Reduced motion unmounts immediately.
  Timers are UI-only exit bounds, never fixture/execution state. No resize listener
  or document keyboard handler drives this motion.
- Only the sidebar retains smoked material. A faint neutral **28s alternate** drift
  animates opacity/transform below content, with the existing single blur and no
  nested filters. Drift is absent for opaque, reduced transparency/motion,
  increased contrast, and unsupported blur. Transcript, code, and composer remain
  opaque. Prose stays **15px / 1.7**, centered with the composer at **780px maximum**.
  Sidebar/inspector grow only to **300px / 360px** on ultrawide displays; standard
  widths keep 264px / 304px (244px / 280px below 1200px). Below 1100px the panel
  animates as a row capped at 32% / 260px. No permanent spacer tracks, wallpaper,
  or broad prose are added. Ordinary icons remain Phosphor.
- The old sidebar utility footer and global preview/material footer are gone.
  A labeled **Opaque materials** icon beside the AVA brand replaces the footer
  toggle. Truthful prototype scope stays in the HTML title/description,
  accessible app description, relevant tooltips, screen-reader announcements,
  and this documentation—not a replacement developer bar.
- Geist / Geist Mono resolve only if installed locally, with the same non-Apple
  named fallbacks. **No font, external image, animation library, or dependency is added.**
- Dark-only. Reduced motion, reduced transparency, increased contrast, manual
  opaque, and unsupported-blur fallbacks remain in CSS. Browser glass is not
  native optics or an OS-preference support promise. Light/system design is later.

## Revision 10 coding-pass validation

These are worker implementation/DOM checks only. Main subsequently completed
[combined independent validation](#revision-10-independent-validation), including
rendered geometry, contrast and visible-focus checks.

- Before editing: nearest AppShell, InspectorComposer, ParentDetails and ShellMotion
  suites **60/60** (7.71s). After implementing, from `apps/desktop-preview`:

  ```sh
  npm test -- src/chat/contextUsage.test.ts src/chat/widgets/ContextInfo.test.tsx src/components/ContextPage.test.tsx src/components/AppShell.test.tsx src/components/InspectorComposer.test.tsx src/components/ShellMotion.test.tsx src/state/reducer.test.ts
  npm run typecheck
  npm test
  npm run build
  ```

  Nearest **125/125 across 7 files** (7.30s); typecheck passed; full **212/212
  across 13 files** (8.08s); warning-free **452-module build** (1.71s).
- Vite raw/gzip kB: main JS **494.45/149.74**, CSS **27.59/6.14**, lazy catalog
  JS **19.88/6.89** and CSS **4.03/1.20**, highlighter **327.69/64.26** unchanged,
  HTML **0.61/0.36**. No threshold/config workaround or additional lazy boundary.
- Tests replace obsolete shared-row assumptions with dedicated-page ownership,
  three-module memory/ARIA/refs and explicit meter opening. They cover matching
  fictional capacities, unknown/stale/invalid values, zero/over-window percentages,
  independent totals, nullable costs, actual projection counts/escaping, stable
  model/typing focus, native disclosures, parent/child/Stop isolation, guarded
  inert exits, reduced motion, cleanup and optional composer compatibility.
- New owners: controlled `ContextInfo`/`ContextMeter`, `chat/contextUsage.ts` math,
  and separate host `components/contextPreview.ts` fixtures/projection. No thick
  store or transport types; old `ParentDetails.tsx/.module.css/.test.tsx` deleted.
  State exceptions are only the inspector union and generic module test extensions.
- Composer/chip CSS, state fixtures/reducer/selectors, other shared widgets/catalog,
  identity/styles/motion primitives, tokens/highlighter/dependencies/config,
  backend/native and all historical evidence remain unchanged. Existing `dist`
  was rebuilt. No install, worker browser/reviewer/capture wave, server, provider
  or C++ check was run. Main owns final browser acceptance, captures and audits.
- Quiet preservation audit: **1,620 baseline files**, **13 allowed changes**, **3
  obsolete removals**, **1,604 unchanged**, and **9 allowed new files**. HEAD,
  staged and tracked-unstaged snapshots match. `git diff --check` and owned-file
  whitespace checks pass; no outside-scope edit. Revision-9/older evidence sections
  remain verbatim. This is the coding audit, not Main's independent acceptance.

### Logo-only empty-transcript follow-on

After the Context checkpoint above, empty parent transcripts now show **only the
existing first-party AVA mark**, neutral and static at **44px**, with an accessible
`AVA` image name. Empty-only grid centering uses the available transcript region
above the unchanged composer, with no vertical transcript padding. Welcome,
project/instruction and local-preview helper copy and their CSS are removed.
The canvas title, composer/meter, sidebar/panels and Send ownership are unchanged;
the first committed local message restores the existing transcript layout.

Only `Conversation.tsx`, `Conversation.module.css`, the minimal obsolete
`projectName` prop/caller removal in `AppShell.tsx`, and its regression suite changed
in source. No new asset/font/glyph/animation, other-screen implementation, or
Context/profile/snapshot/mode behavior changed. These two current notes are the
only documentation changes.

From `apps/desktop-preview`: `npm test -- src/components/AppShell.test.tsx` passed
**18/18 before** (4.59s) and **19/19 after** (4.62s). `npm run typecheck` passed;
full `npm test` passed **213/213 across 13 files** (7.98s). Warning-free
`npm run build`: **452 modules**, 1.67s; main JS **494.24/149.68kB** raw/gzip,
CSS **27.45/6.11kB**. Catalog/highlighter sizes remain unchanged; `dist` rebuilt.
DOM regressions cover logo-only accessible content, preserved controls and escaped
first Send, plus restoration of normal transcript/status content. They do not
prove painted centering. Main's [combined independent acceptance](#revision-10-independent-validation)
subsequently checked actual centering; no worker browser/capture/review wave was run.

Quiet follow-on audit: **1,626 checkpoint files**, **six allowed changes**,
**1,620 unchanged**, no additions/deletions; Git snapshots and owned whitespace
checks match/pass. The complete Context feature and prior evidence remain intact.

## Revision 10 independent validation

Main independently repeated typecheck, **213 tests across 13 files** (7.49s) and a
warning-free **452-module build** (1.55s). Final raw/gzip kB: main JS **494.24/149.68**,
shared CSS **27.45/6.11**, catalog JS **19.88/6.89**, CSS **4.03/1.20**; highlighter
**327.69/64.26**, unchanged. No installs/dependencies/config or C++/provider calls.

- **Seven rendered layouts:** 1600×1000, 1440×960, 1366×768, 850×768, 2560×1440,
  3440×1440, 1440×640. Canvas/pane/44px rail remain y=0 on desktop, attachment
  below canvas at ≤1100px, one canvas-left title/no subtitle or sidebar title heading,
  centered ≤780px reading/composer, 15px/25.5px prose, 300/360px ultrawide caps.
  Every visible composer control fits, including the new neutral usage meter.
- **63 browser checks = 23 preserved + 11 actual motion/preferences + 17
  Context/meter/viewer/unknown/logo + 10 disclosure/scroll/race + 2 catalog smokes.**
  Default Agents and Changes contain no shared details/action row. The meter opens
  Context explicitly without toggling or sending. Aster 173120/214000→81% and
  Mini 48160/128000→38% synchronize with native model selection; Orion's 96000
  window has unknown usage, not zero or a reassigned old-model snapshot. Cumulative
  total remains independent. Raw JSON reflects escaped committed local messages;
  counts and parent ownership during read-only child inspection remain correct.
- Native disclosure Tab/Shift+Tab, trigger Space, first/second Escape, IME Escape,
  return focus, last-module reopen, stable title, rapid Context/Agents/Changes swaps,
  inert/hidden guarded exits and reduced-motion removal passed. Existing actual
  intermediate pane widths/project heights, icon-only Working and opaque fallback
  passed; other experimental/unsupported preferences remain source-checked.
- **Four empty-chat variants** (1440×960, 850×768, 3440×1440, 1440×640) show only the
  accessible, neutral, static **44px AVA mark**, exactly centered in the available
  transcript. Composer/title/panels remain usable. Native first Send removes it and
  restores normal escaped conversation rendering.
- **Four long-raw-text variants** (1440×640, 850×768, 850×640, 3440×1440) scroll the
  entire Context pane including its header, contain a 2400-character unbroken local
  string plus HTML-like text, and retain document/rail bounds. No actual HTML is
  created. The old SIDEBAR8-F01 containment outcome remains; no new finding.
- Finite contrast: **95 default / 121 Context / 151 raw / 74 closed active ranges**,
  each minimum **5.405:1**, none sampled below 4.5. Foreground/background sampling
  excludes SVGs, native values/placeholders, hidden/clipped content, other states
  and full material-motion sweeps; not accessibility/assistive-tech certification.
- Console/page errors empty; **54 recorded URLs loopback-only**. Normally sandboxed
  system Chrome reported “You are adequately sandboxed.” Default chat excluded the
  catalog chunk; separate eight-section catalog rendered with its backwards-compatible
  composer and no unsupplied meter. Full tests, not another catalog browser matrix,
  cover the unchanged controlled request variants.
- **One integrated source review: no material findings.** This was a source gate,
  not an independent rerun of browser/tests. No additional broad review or source fix.
- **Preservation:** **1,620 → 1,654** accounted files; **18 authorized existing
  changes**, **3 obsolete ParentDetails deletions**, **37 additions** (9 source/test
  files and 28 PNGs), **1,599 protected files unchanged**. HEAD/staged/tracked-unstaged
  snapshots and all revision-9/older evidence remain intact. **27 changed/new text
  files** passed whitespace checks. State fixtures/reducer/selectors, Composer CSS,
  other widgets/catalog/primitives, identity/status/material/font/model foundations,
  highlighter/dependencies/config/backend/legacy and prior captures are preserved.

New evidence in `concepts/react-context-page/`:
[Context](concepts/react-context-page/ava-react-context-page-context.png),
[prompt](concepts/react-context-page/ava-react-context-page-prompt.png),
[raw messages](concepts/react-context-page/ava-react-context-page-raw.png),
[model snapshot](concepts/react-context-page/ava-react-context-page-model.png),
[unknown](concepts/react-context-page/ava-react-context-page-unknown.png),
[new chat](concepts/react-context-page/ava-react-context-page-new.png),
[narrow new chat](concepts/react-context-page/ava-react-context-page-new-narrow.png),
[ultrawide](concepts/react-context-page/ava-react-context-page-ultrawide.png).
No new recording is needed; prior motion captures remain history and actual motion
checks above ran on this revision. [Open mock](http://127.0.0.1:5182/?revision=10).
All measurements remain invented/supplied UI examples. No actual resolved system
prompt, current provider payload, private session read or live active-estimate API
is added. Native/Electron/production/legal/packaging/light/all-state performance and
final user acceptance remain outside this evidence. The separately requested screen
expansion is next, not a revision-10 feature.

## Revision 9 coding-pass validation

- Before editing, the nearest four suites passed **59/59** (6.96s Vitest).
  `npm test -- src/components/AppShell.test.tsx src/components/ParentDetails.test.tsx src/components/InspectorComposer.test.tsx src/components/ShellMotion.test.tsx`
  then passed **60/60** after the change (7.04s).
- `npm run typecheck` passed. Full `npm test`: **179/179 across 11 files** (8.41s).
  Warning-free `npm run build`: **446 modules**, **1.61s**. Vite-reported main JS
  **487.39kB / 147.52kB gzip**, CSS **27.86kB / 6.17kB gzip**; lazy catalog JS
  **19.88kB / 6.89kB gzip**, CSS **4.03kB / 1.20kB gzip**; lazy highlight
  **327.69kB / 64.26kB gzip**, HTML **0.61kB / 0.36kB gzip**.
- Updated expectations verify the single title inside the canvas with no sidebar
  heading or title-line subtitle; parent/Project facts remain inside expanded
  Context. Back has a named-parent tooltip and accessible description. Committed
  Inspect/Back, same-child refocus, stable identity through resize/show-hide/module
  changes, inert ghosts, and ordinary draft/model/Send/navigation focus are covered.
  Obsolete MQL title-relocation tests are replaced by stable-node/no-placement-
  subscription expectations and a StrictMode no-deferred-focus regression.
  Existing Context, parent ownership, readonly, Send/Stop and race coverage remains.
- Existing AppShell source, ConversationIdentity/ParentDetails source and CSS,
  four affected suites, and two current documentation surfaces are the coding
  scope. No fixture/reducer, composer/body CSS, widget/catalog, material/status,
  dependency/config, backend/native or previous capture changes. Revision-8 coding
  and independent sections and all older records remain verbatim.
- Quiet audit: **1,603 baseline files**, **11 owned + three Main doc changes**,
  **1,589 unchanged**, no additions/deletions. HEAD/staged/tracked-unstaged snapshots
  match. `git diff --check` and baseline whitespace checks for all 11 owned files
  pass; historical sections match exactly apart from the current source map.
- These are DOM/source checks, not jsdom geometry or focus-paint claims. No worker
  browser, capture, new server, install, or native connection. Main owns desktop,
  laptop, narrow/short and 2560/3440 acceptance, fresh evidence, final audits and
  the integrated source review.

## Revision 9 independent validation

Main repeated typecheck, **179 tests across 11 files** (9.51s) and a warning-free
**446-module build** (1.75s). Final raw/gzip kB: main JS **487.39/147.52**, shared CSS
**27.86/6.17**; catalog JS **19.88/6.89**, CSS **4.03/1.20**; highlighter
**327.69/64.26**, unchanged. Existing `dist` rebuilt without an install/config change.

- **Seven rendered layouts:** 1600×1000, 1440×960, 1366×768, 850×768, 2560×1440,
  3440×1440 and 1440×640. Exactly one title stays at the **canvas's left inset**,
  not the centered reading-column edge—even ultrawide. No project/relationship
  subtitle or duplicate sidebar identity. Canvas/pane/44px rail retain headerless
  geometry; narrow pane stays below. No document/control overflow in checked states.
  Transcript/composer remain centered **≤780px**, prose **15px / 25.5px**;
  ultrawide side panels retain **300/360px** caps.
- **50 browser checks:** **16** title/focus/Context/scroll/race, **23** preserved
  inspector/composer/identity/keyboard and **11** actual shell/icon/preference checks.
  The same title node survives show/hide, modules, resize and child/Back. Explicit
  Inspect/Back focus it visibly; typing, native model changes and navigation retain
  destination focus. A child remains read-only with one Back and named-parent native
  tooltip/accessibility description; no visible subtitle is restored. Expanded
  Context retains parent-owned facts, not a sidebar heading.
- Native short/narrow/narrow-short Context scrolling has no hidden-summary global
  overflow; the closed SIDEBAR8-F01 fix remains. Immediate logical exit, inert/hidden
  guarded retention, actual intermediate widths, rapid reopen, reduced-motion removal,
  icon-only Working and opaque material passed. Other CSS preference/unsupported-blur
  fallbacks were source-checked, not newly emulated. No new motion or dependency.
- Finite text contrast: **101** default, **117** expanded and **73** closed active
  ranges, each minimum **5.405:1**, none below 4.5. Rendered-background sampling
  excludes SVGs, native input values/placeholders, hidden/clipped content, other
  states and full material-motion sweeps; not all-state accessibility certification.
- Console/page-error records empty; **48 recorded URLs** loopback-only. Default
  chat did not load the catalog. System Chrome was normally sandboxed and reported
  **“You are adequately sandboxed.”** No safety bypass or live provider/backend call.
- **One integrated source review: no material findings.** No new finding or second
  correction pass. Frozen controlled-kit behavior retains its full tests and source;
  this targeted layout pass is not another full catalog or native qualification.
- **Preservation:** **1,603 → 1,620** accounted files; **14** authorized existing
  changes and **17 PNG additions**, **no deletion**, **1,589 protected files unchanged**.
  HEAD/staged/tracked-unstaged snapshots and revision-8/older histories remain intact.
  State, Composer CSS, shared kit/catalog/UI, panels/rail, status/material/font/model
  foundations, highlighter/dependencies/config, backend/legacy and prior captures
  are preserved. All **14 changed text files** passed whitespace checks.

Fresh captures under `concepts/react-chat-title/`:
[1440](concepts/react-chat-title/ava-react-chat-title-1440.png),
[narrow](concepts/react-chat-title/ava-react-chat-title-narrow.png),
[ultrawide](concepts/react-chat-title/ava-react-chat-title-ultrawide.png),
[closed](concepts/react-chat-title/ava-react-chat-title-closed.png),
[child](concepts/react-chat-title/ava-react-chat-title-child.png),
[Context](concepts/react-chat-title/ava-react-chat-title-context.png).
No new recording is needed for this title-only correction; revision-8 motion evidence
is retained history, while the actual motion checks above ran against revision 9.
The mock is not connected to AVA; production/native/Electron, legal/packaging,
all-state accessibility/performance, light/system and final user acceptance remain
outside these checks. [Open current mock](http://127.0.0.1:5182/?revision=9).

## Revision 8 coding-pass validation

- Nearest pre-edit baseline: **50 tests / 4 files** passed (4.90s Vitest / 5.20s
  wall). All **1,576** baseline hashes and HEAD/staged/tracked-unstaged snapshots
  matched before editing.
- Affected suites after revision: **59/59** (6.08s / 6.39s wall). Full preview:
  **178/178 across 11 files** (6.85s / 7.16s wall); typecheck passed (0.29s wall).
- Warning-free Vite build: **446 modules**, **1.60s / 1.83s wall**. Reported
  main JS **488.26kB / 147.85kB gzip**, CSS **28.27kB / 6.23kB gzip**;
  lazy catalog JS **19.88kB / 6.89kB gzip**, CSS **4.03kB / 1.20kB gzip**;
  lazy highlight **327.69kB / 64.26kB gzip**, HTML **0.61kB / 0.36kB gzip**.
- Updated obsolete header/popover expectations preserve the related rail,
  parent ownership, state, execution-boundary, and safe-content meanings.
  Added regressions cover single identity/Back, closed-pane child returns,
  committed title focus, media subscription cleanup, native inline Context
  Tab/Escape, nested inert/guarded exits, full/rapid reopen, module/resize races,
  reduced motion, and unmount cleanup. A failing StrictMode ref-replay regression
  exposed a stale title-focus request; the fix clears same-node replay rather
  than stealing later editing focus. All tests above pass after that correction.
- No jsdom geometry claim or worker browser/capture/server activity. Main owns
  independent 1440/1366/850 and ultrawide/short checks, scrolling, visual focus,
  motion/material/contrast/network evidence, and one integrated source review.
  Revision 7's kit/tests/captures and all older history remain preserved.

## Revision 8 independent validation

Main independently repeated typecheck and **178 tests across 11 files** (7.01s),
then a warning-free **446-module build** (1.60s). Final raw/gzip kB: main JS
**488.26/147.84**, shared CSS **28.29/6.23**; lazy catalog JS **19.88/6.89** and CSS
**4.03/1.20**; lazy highlight **327.69/64.26**, unchanged. Existing `dist` was rebuilt;
no install, configuration, lockfile or dependency changed.

- **Seven rendered layouts:** 1600×1000, 1440×960, 1366×768, 850×768, 2560×1440,
  3440×1440 and 1440×640. Desktop canvas/pane/44px rail start at **y=0**, with no
  workspace header or outer frame. The narrow pane attaches below the canvas.
  No document/control overflow at checked settled states. Reading/composer remain
  centered at **≤780px**, **15px / 25.5px** prose; ultrawide panels cap at **300/360px**.
  The composer preserves its existing bottom inset (24px ordinarily, 16px short).
- **70 browser checks:** **23** preserved inspector/composer/identity/keyboard
  meanings; **19** new parent details/inline title/native focus checks; **11** actual
  shell/project/icon/preference checks; **12** Context/module motion/scroll/race
  checks; **5** catalog/lazy-route smokes. The unchanged full kit retains its
  controlled-prop suite; revision 7's larger catalog browser study remains history.
- Native Tab/Escape, open-versus-toggle semantics, parent-owned child metadata,
  one visible current title/Back, visible inspect/return and breakpoint focus,
  ordinary editing/model-change focus, retained drafts/settings and parent-only
  Stop passed. Expanded Context/module content was reached by actual pane scrolling
  at short, narrow and narrow-short sizes, without global page overflow.
- Actual intermediate panel widths, Context heights and keyed-module opacity were
  observed. Logical exits immediately become inert/aria-hidden/callback-guarded;
  rapid reopen/switch/resize resolves one module/title/context. Working's icon moves
  while its green text stays static without glow/halo. Opaque disables drift/blur;
  reduced motion removes retention and stops animation/transition. Reduced
  transparency, increased contrast and unsupported blur were source-checked only.
- **SIDEBAR8-F01 — fixed.** Expanded narrow details exposed absolute hidden summary
  spans using the viewport containing block: document height **894px** despite a
  **768px** viewport. Same-element A/B proved `position: relative` on `.details`
  alone restores 768px; reverting reproduced 894px. Astra made only that CSS
  correction. Native short/narrow scrolling and the full suite/build passed again;
  one integrated source review plus the named delta recheck left **no material
  findings**. No second broad review or global-style change was made.
- Finite text contrast: **102** default, **118** expanded Context and **74** closed
  ranges, all active, each minimum **5.405:1**, none sampled below 4.5. Same rendered
  background/un-antialiased-foreground method as revision 7; animation paused for
  sampling. SVGs, native input values/placeholders, clipped/hidden ranges, other
  states and full material sweeps are excluded. Not all-state WCAG/assistive-tech
  certification. Native target interactions await finite CSS motion; separate
  frame sampling checks immediate exits and actual intermediate motion.
- Console/page-error records were empty; **156 recorded request URLs** were
  loopback-only. The default route did not load the catalog. Normally sandboxed
  system Chrome reported **“You are adequately sandboxed.”** No bypass flag used.
- **Preservation:** **1,576 → 1,603** accounted files; **14** authorized existing
  changes, the **three obsolete ChatToolbar files deleted**, and **30 additions**
  (five narrow source/test files and 25 evidence artifacts). **1,559 protected
  files unchanged**, HEAD/staged/tracked-unstaged snapshots preserved. State,
  Composer CSS, shared widgets/UI/catalog, tokens/status/project foundations,
  dependencies/config/highlighter, backend/legacy and earlier captures are intact.
  All **19** changed/new text files passed whitespace checks.

Fresh evidence under `concepts/react-sidebar-details/`: **24 PNGs** and an actual
**VP8 1440×960 WebM, 2.4s / 230,586 bytes**. Selected views:
[desktop](concepts/react-sidebar-details/ava-react-sidebar-details-dark.png),
[1440](concepts/react-sidebar-details/ava-react-sidebar-details-1440.png),
[narrow](concepts/react-sidebar-details/ava-react-sidebar-details-narrow.png),
[short](concepts/react-sidebar-details/ava-react-sidebar-details-short.png),
[ultrawide](concepts/react-sidebar-details/ava-react-sidebar-details-ultrawide.png),
[Context](concepts/react-sidebar-details/ava-react-sidebar-details-context.png),
[closed](concepts/react-sidebar-details/ava-react-sidebar-details-closed.png),
[child without pane](concepts/react-sidebar-details/ava-react-sidebar-details-child-closed.png),
[motion](concepts/react-sidebar-details/ava-react-sidebar-details-motion.webm).
The recording shows actual browser paint, not fabricated interpolation, a seamless
loop or a native/GPU benchmark. This is independently validated **mock behavior**,
not final user acceptance or live backend/Electron/provider/persistence, production
accessibility, performance, packaging/legal/SBOM or light/system qualification.

## Controlled frontend kit (Phase 2)

`src/chat/contracts.ts` defines readonly discriminated **UI models**, not AVA RPC
schemas. Controlled MessageTurn, ToolActivityGroup, PermissionCard, QuestionCard,
RunNotice, ChatComposer, and ChangeReviewView live in `src/chat/widgets/`.
They accept supplied views and explicit callbacks, not PreviewState, fixtures,
raw transport JSON, backend IDs generated by the renderer, or resource authority.
Conversation/ToolActivity/Composer/ChangeReview retain fixture adaptation. Existing
native labels, IME shortcuts, context focus/Escape/blur, independent parent drafts,
model/mode and chips remain; composer CSS and highlighter source are untouched.

Native Button/IconButton, Surface, shared Status and neutral semantic tokens join
Phase 1's reused Presence/Disclosure/reduced-motion foundations. The catalog is
available only at **`?view=components`**, lazy-loaded through `main.tsx`; no extra
main-chat navigation is added. It contains resettable turn/tool lifecycle, permission
and question, run/error/retry/cancel, controlled composer, and change-resource
variants. Receipts say only **local fixture intent**; answers are never echoed,
and no permission, resolver, retry, Stop, upload, or command is claimed executed.

Resolver widgets require valid caller IDs/versions/capabilities. Pending, submitted,
error, stale and closed states are inactive. Session choice is never implicit;
invalid/unknown/duplicate selections and unsupported declared modes fail closed.
The per-mounted-card guard suppresses duplicate ID/version intents; **host-owned
pending/settled state is still required across unmounts**. Fixture revision explicitly
increments UI version. Secret custom inputs use password fields without answer
receipts/logs. Questions use native forms/choices; Escape never acts as cancellation.
Run attempts/state are supplied, with no countdown or automatic recovery.

Remaining capabilities include host routing/replay/reconnect, authoritative resolver
and run settlement, real context admission, public-job mapping, child transcripts,
filesystem/Git/native integration, persistence and light/system themes. No reference
implementation, asset, font, transport, dependency or config change was introduced.
Context popovers retain immediate-unmount behavior; their optional motion extension
was deliberately not added during the extraction.

## Run and checks

Use Node **24.13.1** / npm **11.18.0**. From the repository root, with the existing
package installation:

```sh
cd apps/desktop-preview
npm run typecheck
npm test
npm run build
```

For a fresh checkout, installation and server commands are in the
[package README](../../apps/desktop-preview/README.md#run-locally). Development and
built preview bind **127.0.0.1:5182**, with `strictPort: true`; development filesystem
serving is package-local. No host/CORS/engine/release-age/security exception was
added. Built preview omits the local development HMR WebSocket. Do not start a
second server on the same port.

### Revision 7 Phase 2 coding-pass validation

| Check | Result |
| --- | --- |
| Baseline adapter tests | **44 passed / 5.65s** |
| New widget/catalog tests | **64 passed / 2.18s** |
| Typecheck / complete suite | Passed; **169 tests in 11 files / 6.81s**, including all 105 Phase-1 cases |
| Build | Warning-free; **444 modules / 1.76s** |
| Main JS / lazy catalog JS | **488.34 / 19.88 kB**, gzip **147.92 / 6.89 kB** |
| Shared CSS / catalog CSS | **29.34 / 4.03 kB**, gzip **6.40 / 1.20 kB** |
| Highlighter | Source unchanged; **327.69 kB / 64.26 kB gzip** |

Controlled-prop, malformed metadata/answer, stale/duplicate intent, secret masking,
escaping, keyboard/focus, reset and unmount checks pass. The retained tool disclosure
case now asserts immediate logical inaccessibility while inert exit paint may remain.
These are **DOM/state/source checks**, not rendered layout, animation, contrast, or
production certification. The existing preview's `dist` is refreshed; no install,
server, browser/capture or Git mutation occurred. Main owns combined browser
qualification, comparison updates and one integrated review after both phases.

### Revision 7 combined independent validation

Independent main checks on **2026-10-06**, against both completed phases:

| Check | Result |
| --- | --- |
| Typecheck / full suite | Passed; **169 tests in 11 files / 6.08s** |
| Build | Warning-free; **444 modules / 1.57s**, sizes match the Phase-2 table |
| Browser | **88 passed**: 11 icon/panel/disclosure/material/preference, 3 module-entry/race/resize, 23 preserved inspector/composer/identity, 7 parent-header/focus, 44 catalog/state/capability/privacy checks |
| Shell layouts | **1600×1000, 1440×960, 1366×768, 850×768, 2560×1440, 3440×1440**; no document/header overflow; connected shared-header/panel/44px rail edges |
| Sizing | Header controls **42px / 16px**, compact **38px / 14px**; ultrawide navigation/inspector **300px / 360px**; centered transcript/composer **≤780px** |
| Catalog | Separate lazy route; laptop/narrow/ultrawide horizontal bounds, native controlled actions and reset, no main-chat toolbar addition |
| Finite contrast | **103 shell text ranges / minimum 5.405:1**; 59 catalog ranges across two positions, **58 active / minimum 6.805:1**; one native disabled-control label 4.321:1 excluded from the active minimum |
| Browser diagnostics | Normally sandboxed system Chrome; empty console/page-error records; recorded requests loopback-only |
| Integrated source review | One read-only reviewer; **no material findings**. Not an independent test/browser rerun |
| Documentation / whitespace | **150 Markdown files, 132 reachable documents, 18 required indexes, 30 llms links, 16 verifier tests**; tracked whitespace and 50 changed/new text files passed |
| Preservation | **1,527-file baseline**: 22 authorized existing changes, 27 new source/test files, one comparison document, **21 new artifacts**, no deletion; **1,505 protected files unchanged**. HEAD/staged/tracked-unstaged snapshots preserved |

Natural icon angles changed while Working's green foreground, readable label and
text shadow stayed static/no-shadow. There is no radial halo. Actual intermediate
inspector widths, project-disclosure heights and keyed module-entry opacity were
observed, not inferred from declared CSS durations. Closing immediately changed
logical state, made exit content inert/aria-hidden and blocked callbacks; bounded
paint retention ended. Rapid reopen/switch/resize settled one active panel without
duplicate IDs. Reduced motion stopped animations/transitions and removed closed
contents without retention. Opaque mode removed drift/blur. Reduced-transparency,
increased-contrast and unsupported-blur fallbacks were **source-checked**, not
independently emulated in this browser wave.

Preserved native keyboard/focus/dismissal, parent draft/model/mode/context/expansion,
read-only child identity, pins/recents and parent-only Stop behavior passed. Catalog
checks exercised partial/terminal turns, escaped user/tool content, truncation,
one-shot vs explicitly advertised session choice, inactive resolver states,
invalid required answers, duplicate intent suppression, native multiple-choice
filtering, custom/masked secret inputs, no-answer receipts, supplied retry/cancel
states, controlled Send/Stop and read-only change-resource states. Unit tests cover
malformed ID/version/capability/answer and unsupported-mode cases not all exposed
as browser case choices. Mounted guards do not provide durable exactly-once delivery;
host-owned pending/settled state is still required across unmounts.

Fresh captures (**20 PNGs**) replace no historical evidence:

| View | Capture |
| --- | --- |
| Standard desktop | [1600×1000](concepts/react-frontend-kit/ava-react-frontend-kit-dark.png), [1440×960](concepts/react-frontend-kit/ava-react-frontend-kit-1440.png) |
| Laptop / narrow | [1366×768](concepts/react-frontend-kit/ava-react-frontend-kit-laptop.png), [850×768](concepts/react-frontend-kit/ava-react-frontend-kit-narrow.png) |
| Ultrawide | [2560×1440](concepts/react-frontend-kit/ava-react-frontend-kit-wide.png), [3440×1440](concepts/react-frontend-kit/ava-react-frontend-kit-ultrawide.png) |
| Inspector / preferences | [Changes](concepts/react-frontend-kit/ava-react-frontend-kit-changes.png), [closed](concepts/react-frontend-kit/ava-react-frontend-kit-closed.png), [opaque](concepts/react-frontend-kit/ava-react-frontend-kit-opaque.png), [reduced motion](concepts/react-frontend-kit/ava-react-frontend-kit-reduced.png) |
| Parent / child | [expanded composer](concepts/react-frontend-kit/ava-react-frontend-kit-composer.png), [read-only child](concepts/react-frontend-kit/ava-react-frontend-kit-child.png), [failed child](concepts/react-frontend-kit/ava-react-frontend-kit-failed.png), [unavailable child](concepts/react-frontend-kit/ava-react-frontend-kit-unavailable.png), [parent context](concepts/react-frontend-kit/ava-react-frontend-kit-context.png) |
| Fixture catalog | [desktop](concepts/react-frontend-kit/ava-react-frontend-kit-catalog.png), [narrow](concepts/react-frontend-kit/ava-react-frontend-kit-catalog-narrow.png), [ultrawide](concepts/react-frontend-kit/ava-react-frontend-kit-catalog-ultrawide.png), [permission/question](concepts/react-frontend-kit/ava-react-frontend-kit-requests.png), [masked secret](concepts/react-frontend-kit/ava-react-frontend-kit-secret.png) |

[Actual shell-motion recording](concepts/react-frontend-kit/ava-react-frontend-kit-motion.webm)
shows icon-only Working, inspector close/open/module switches and project collapse/
reopen. It is browser capture, not fabricated interpolation, a seamless loop, or
GPU/performance evidence.

Contrast uses visible DOM text ranges and rendered background pixels with text fill/
shadow hidden, animation/transition disabled and paint awaited. It excludes SVGs,
native values/placeholders, hidden/clipped text, other positions/states and a full
material-motion sweep. Finite browser/source checks do **not** certify production
accessibility, assistive technologies, long-history behavior, GPU/Electron/native
performance, packaging/licenses/SBOM, backend readiness or final user acceptance.
The backend, legacy desktop, frozen fixture state/selectors, composer/chip CSS,
highlighter, dependencies/configuration/notices and all prior captures remain intact.

### Revision 7 Phase 1 coding-pass validation

Using the existing installation, with no dependency/configuration changes:

| Check | Result |
| --- | --- |
| Baseline nearest tests | **28 passed** |
| Updated nearest tests | **44 passed / 5.01s** |
| Typecheck / complete suite | Passed; **105 tests in 8 files / 6.22s**, retaining all 89 baseline cases |
| Build | Warning-free; **425 modules / 1.68s** |
| Main JS / highlighter / CSS | **481.99 / 327.69 / 26.97 kB**, gzip **145.63 / 64.26 / 5.94 kB** |
| Whitespace / baseline | Passed: **1,527 hashes**, 15 owned existing + 6 owned new files; **1,510 protected files unchanged**. Two main-owned doc edits and its new comparison record allowed separately; no deletion; HEAD/staged/tracked unstaged patches unchanged |

The structural panel test now checks logical visibility while inert exit paint may
remain. Added regressions cover callback guards, reduced-motion immediate unmount,
focus/native controls, rapid close/reopen/module replacement, duplicate IDs, resize
state safety, listener cleanup, and pending-exit cleanup under StrictMode. Status
checks assert an icon-only source hook and no timed settlement. These are
**DOM/state/source checks, not browser motion, geometry, or contrast evidence**.

Owned source paths are shell/sidebar/inspector/status components, toolbar CSS,
semantic motion/material tokens, global neutral feedback, and `src/ui/` foundations
and tests. Frozen fixtures/reducer/types/selectors, composer chips, dependencies,
backend, legacy client, and earlier captures remain untouched. Only these current
records are updated; historical sections below are not rewritten. The build
refreshes `dist` for the existing loopback preview. No install, server, browser,
capture, staging, or commit operation was performed. Main owns preservation review,
new v7 browser evidence, comparison documentation, and the later integrated review.
Phase 2 is not implemented in this pass.

### Revision 6 coding-pass validation

Using the existing Node **24.13.1** / npm **11.18.0** installation:

| Check | Result |
| --- | --- |
| Baseline nearest tests before editing | **10 shared-status tests passed** |
| Updated status + inspector/composer tests | **23 tests passed** |
| `npm run typecheck` / `npm test` | Passed; **89 tests in 6 files**, including the existing real Shiki tests |
| `npm run build` | Warning-free; 421 modules; main JS **480.11 kB / 145.04 kB gzip**, highlighter **327.69 kB / 64.26 kB gzip**, CSS **24.96 kB / 5.58 kB gzip** |
| Owned whitespace / preservation | Passed; **1,521** baseline hashes: 4 coder-owned + 2 concurrent main-owned index/research doc changes, **1,515 protected files unchanged**, no deletions; HEAD/staged/tracked unstaged patches unchanged |

This coding pass changed only `src/components/ChatStatus.module.css`, its focused test, package
README, and this record. Existing `ChatStatus.tsx` hooks suffice; no markup or
caller changes. Tests cover Working-only cycle/halo and glow selector hooks,
removal for every nonworking status including Stopped and idle, accessible names,
distinct decorative glyphs, and no timed settlement. They establish **DOM/state
semantics, not rendered animation, geometry, or contrast**.

No install, server, browser, or capture operation was performed; only the build
refreshes `dist` for the existing loopback preview. Initial CSS size was
**24.96 kB / 5.58 kB gzip**. After the named interpolation correction below,
nearest tests, all **89 tests**, typecheck and the warning-free build passed again;
final CSS is **24.99 kB / 5.58 kB gzip**, with unchanged JS sizes. New evidence belongs
only in `concepts/react-working-motion/`; all 13 v5 captures and earlier assets are unchanged.

### Revision 6 independent browser validation

Main independently repeated typecheck, **89 tests / six files**, and the final
warning-free build. The existing loopback preview was reused in adequately
sandboxed system **Chrome 154.0.8037.97**; no install, backend/provider, or second
server was started.

- **10 targeted actual-browser checks passed.** A 3.2-second natural-animation
  observation sampled **13 live frames with 13 distinct text-shadow values** and
  changing icon angles across the repeat boundary. No timeline seeking was used
  for that observation. Base green, opacity and the **57.484×17px** status box
  remained fixed; the rotating icon stayed within the line height, and statuses
  did not auto-settle. Only Working animates; parent and child use the same effects.
- Separate deterministic **0° / 45° / 180°** samples demonstrate the icon cycle
  and low/high text glow. The 2.4s linear rotation, 2.8s ease-in-out glow and retained
  halo all stop under actual browser reduced-motion emulation, keeping static green.
  Explicit parent Stop removes only that parent's effects/redraws red; its Working
  child continues. Read-only child inspection retains its own status without a composer.
- **1440×960** reading layout plus **1600×1000, 1366×768 and 850×768** geometry
  checks retained joined edges, shared header, 44px rail, narrow attached-below panel,
  ≤780px composer and no document overflow. Non-status computed colors remain neutral.
- Finite initial base-background contrast: **103 text ranges, minimum 5.782:1**,
  none below 4.5:1. Text fill/shadows hidden, motion disabled, own/ancestor clipping
  included, two paint frames awaited. Excludes SVG/halo/glow pixels, native values,
  hidden/clipped text and other states; not all-state accessibility, assistive-technology,
  WCAG, Electron or GPU/performance certification. No page errors or console warnings;
  app requests remained loopback/local assets.

**Named finding ledger:** **WORK6-F01 — fixed.** Initial actual Chrome sampling
showed only two shadow intensities, despite the easing declaration. A controlled
same-element/radii/timing A/B isolated deferred `currentColor` inside shadow
`color-mix` as non-interpolating here. The existing `var(--status-working)` color
source produced intermediate alpha values and actual paint changes. Astra changed
only four mix inputs inside `workingGlow`; halo `currentColor`, palette, radii,
amplitudes, periods, markup and state were untouched. Main's natural-frame check
then passed. The same source reviewer rechecked only this ID/four-token delta and
confirmed **fixed, no material findings**; it did not rerun main's browser/tests.

**Six fresh motion artifacts**, preserving all prior evidence:

| View | Artifact |
| --- | --- |
| 0° / 45° / glow peak | [Soft](concepts/react-working-motion/ava-react-working-motion-soft.png), [cycle](concepts/react-working-motion/ava-react-working-motion-cycle.png), [glow](concepts/react-working-motion/ava-react-working-motion-glow.png) |
| Static reduced motion | [Reduced](concepts/react-working-motion/ava-react-working-motion-reduced.png) |
| Actual live capture | [WebM](concepts/react-working-motion/ava-react-working-motion-loop.webm) |
| Parent/child enlarged crop | [Animated GIF](concepts/react-working-motion/ava-react-working-motion-loop.gif) |

The finite recording is not a frame-rate/performance benchmark or a seamless
combined-cycle loop. Its GIF crops the captured parent/child rows side-by-side,
scaled 2×; no icon/text animation was fabricated in export.

Final **1,521-file** baseline preservation: **6 authorized existing paths changed,
6 captures added, no deletions; 1,515 protected files unchanged**. HEAD/staged/tracked
unstaged patches, shared TSX/markup, palette/global styles, state/fixtures/other
components/layout/chips, package/config/lock/notices, backend/legacy client and
all historical captures remained unchanged.

**The revision-5 coding and browser results below are historical, not revision-6 acceptance.**

### Revision 5 coding-pass validation

Completed using the existing Node **24.13.1** / npm **11.18.0** installation:

| Check | Result |
| --- | --- |
| Baseline nearest tests before editing | **13 inspector/composer tests passed** |
| `npm test -- src/components/ChatStatus.test.tsx src/components/InspectorComposer.test.tsx` | **23 tests passed**: 10 shared-status + 13 inspector/composer |
| `npm run typecheck` | Passed (`tsc --noEmit`) |
| `npm test` | **89 tests in 6 files passed**, retaining all 79 prior tests, including 3 real Shiki tests |
| `npm run build` | Warning-free; 421 modules; main JS **480.11 kB / 145.04 kB gzip**, highlighter **327.69 kB / 64.26 kB gzip**, CSS **24.45 kB / 5.48 kB gzip** |
| Owned whitespace / preservation | Passed including untracked source/docs; **1,507** baseline hashes, 5 coder-owned existing paths changed, 1 focused status test added; apart from concurrent main-owned desktop index/research doc edits, all protected hashes and HEAD/staged/tracked unstaged patches unchanged |

Changed only `src/components/ChatStatus.{tsx,module.css}`, `src/styles/tokens.css`,
package README, and this record; added `src/components/ChatStatus.test.tsx`.
The ten status tests cover labels/data-status, accessible row names, distinct
hidden glyphs, empty idle, Working-only halo hook/removal, and absence of timed
settlement. They verify **DOM/state semantics, not composited color, motion,
geometry, or contrast**. State, fixtures, types, reducer, other component logic,
fonts, layout, global preference rules, package/config/lock/notices, backend,
legacy client, and every prior capture remain unchanged.

No install, server, browser, or capture operation was performed. Build output
refreshes `dist` for the existing main-owned preview on `127.0.0.1:5182`.
Independent main checks are recorded below. New captures belong only in
`concepts/react-status-accent/`; no prior revision's evidence qualifies revision 5.

### Revision 5 independent browser validation

Main repeated typecheck, all **89 tests / six files**, and the warning-free build.
The existing loopback preview was reused in normally sandboxed system
**Chrome 154.0.8037.97**; `chrome://sandbox` again confirmed adequate sandboxing.
No install, backend/provider, or second server was started.

- **1600×1000, 1440×960, 1366×768, 850×768:** no document overflow, joined edges,
  unchanged shared header/44px rail/attached inspector, and ≤780px inset composer.
  Computed chromatic colors occur only inside shared status elements; all other
  surfaces remain neutral. The narrow panel still attaches beneath the canvas.
- **10 targeted color/motion invariants:** exact parent/child palette at three
  desktop/laptop sizes; Working-only 2.8s pseudo animation; static glyph/label;
  sampled halo at 0/1400/2800ms with opacity **.14/.30/.14**, scale
  **.9/1.05/.9**, unchanged label color and status bounds (**57.484×17px**),
  and no fixed-state mutation. System reduced-motion emulation removes working
  animation while retaining green. Manual opaque fallback retains status colors.
  Explicit parent Stop turns only the parent red and removes its pulse, leaving
  its child green Working. Attention and read-only Unavailable stay distinct.
- **23 preserved browser invariants** passed: parent/child identity, independent
  draft/model/mode/context/expansion, fixed Working-first roster, rail/panel and
  native focus/Escape, read-only child with Back, failed/unavailable/empty states,
  parent-owned Changes and Stop, safe escaped local Send, pins/recents and New.
- Finite initial **1440×960** rendered-background contrast: **103 visible DOM text
  ranges, minimum 5.782:1**, none below 4.5:1. Text fill hidden, motion disabled,
  own/ancestor clipping included and two paint frames awaited. Excludes SVG/halo
  pixels, native values/placeholders, hidden/clipped text and other states; not
  all-state accessibility, assistive-technology, GPU or WCAG certification.
- No page errors or console warnings. App requests stayed on loopback document,
  local JavaScript/CSS and on-demand highlighting. Increased-contrast status-token
  adaptation is source-reviewed, not a new OS-preference qualification.
- One independent read-only integrated source reviewer reported **no material
  findings**. It did not independently rerun main's tests/browser observations.

**13 new captures**, without replacing historical assets:

| View | Capture |
| --- | --- |
| 1600×1000 | [Dark](concepts/react-status-accent/ava-react-status-accent-dark.png) |
| 1440×960 | [Reading layout](concepts/react-status-accent/ava-react-status-accent-1440.png) |
| 1366×768 | [Laptop](concepts/react-status-accent/ava-react-status-accent-laptop.png) |
| 850×768 | [Narrow](concepts/react-status-accent/ava-react-status-accent-narrow.png) |
| Halo samples | [Soft](concepts/react-status-accent/ava-react-status-accent-pulse-soft.png), [peak](concepts/react-status-accent/ava-react-status-accent-pulse-peak.png) |
| Preferences | [Reduced motion](concepts/react-status-accent/ava-react-status-accent-reduced.png), [opaque](concepts/react-status-accent/ava-react-status-accent-opaque.png) |
| Parent Stop | [Stopped parent, working child](concepts/react-status-accent/ava-react-status-accent-stopped.png) |
| Composer | [Context/expansion](concepts/react-status-accent/ava-react-status-accent-composer.png) |
| Child | [Read-only child](concepts/react-status-accent/ava-react-status-accent-child.png) |
| Terminal examples | [Failed](concepts/react-status-accent/ava-react-status-accent-failed.png), [unavailable](concepts/react-status-accent/ava-react-status-accent-unavailable.png) |

Final preservation against the **1,507-file** pre-revision baseline: **7 authorized
existing source/doc paths changed, 14 additions** (one test + 13 captures), no
deletions; **1,500 protected files unchanged**. HEAD/staged/tracked unstaged patches,
state/fixtures/reducer, toolbar/rail/layout/chip CSS, backend/legacy client,
dependencies/config/locks/notices and all historical captures were preserved.

### Revision 4 coding-pass validation

**SUPERSEDED REVISION-4 HISTORY — not revision-5 acceptance.**

Actually completed on Node **24.13.1** / npm **11.18.0**, reusing installed dependencies:

| Check | Result |
| --- | --- |
| Baseline nearest tests before editing | 71 passed across reducer, shell, inspector/composer, and old header controls |
| `npm test -- src/components/ChatToolbar.test.tsx src/components/AppShell.test.tsx src/components/InspectorComposer.test.tsx src/state/reducer.test.ts` | **76 focused tests passed**: 13 toolbar/rail + 18 shell + 13 inspector/composer + 32 reducer |
| `npm run typecheck` | Passed (`tsc --noEmit`) |
| `npm test` | **79 tests in 5 files passed**, including all 3 real Shiki tests |
| `npm run build` | Warning-free; 421 modules; main JS **479.93 kB / 144.96 kB gzip**, highlighter **327.69 kB / 64.26 kB gzip**, CSS **23.51 kB / 5.19 kB gzip** |
| Owned whitespace / preservation | Passed, including untracked source/docs; of **1,489** baseline hashes, 7 authorized existing paths changed, 3 obsolete files deleted, all other hashes unchanged; 3 focused files added |

Added `src/components/ChatToolbar.{tsx,module.css,test.tsx}`; deleted replaced
`src/components/InspectorControls.{tsx,module.css,test.tsx}`. Updated shell wiring,
rail component/CSS, nearest shell/inspector assertions, package README, and this
record. No typed state, selectors, fixtures, composer/chip CSS, or shared layout
CSS needed changes. Meaningful prior state/keyboard/safety coverage remains:
remembered modules, rail-toggle focus, panel Close/Escape-to-rail, explicit repeated
header opens, truthful parent counts after attach/remove/Send/view, read-only child
identity, and unchanged draft/model/mode/context/pins/recents/statuses. New disclosure
coverage checks initial details-region focus, native Close/Enter/Space/Tab/Shift+Tab,
IME-safe Escape, outside/blur dismissal without focus stealing, and listener cleanup.
No geometry is claimed from jsdom.

HEAD/staged/tracked unstaged patches, dependencies/config/lock/notices, backend,
legacy client, and all existing captures—including `concepts/react-agents/`—remain
unchanged. `.chips` retains `flex-shrink: 0`. No install, server, browser, capture,
or provider operation was performed. The build refreshes `dist`; the existing
loopback preview remains main-owned.

### Revision 4 independent browser validation

**SUPERSEDED REVISION-4 HISTORY — not revision-5 acceptance.**

Main independently repeated typecheck, all **79 tests**, and the warning-free build.
The existing loopback preview was reused in normal system **Chrome 154.0.8037.97**;
`chrome://sandbox` confirmed adequate sandboxing. No backend/provider or second
server was started.

- **1600×1000, 1440×960, 1180×900, 1366×768 and 850×768:** no document overflow;
  joined edges, shared header above the body, persistent 44px rail, desktop or
  attached-below narrow inspector, and ≤780px inset composer with 24px bottom space.
- Passed **23** preserved browser invariants for parent/child identity, draft/model/
  mode/context retention, safe local Send, explicit parent-only Stop, panels,
  failed/unavailable/empty examples, disclosure/Escape/focus and shortcuts.
- Passed **22** new rail/toolbar checks: show/hide is the first rail button above
  Agents and absent from the header; title/control/context-popup bounds at
  1600/1366/850px; native toggle focus and remembered module; repeated header
  review/activity actions keep the selected panel open; Context focus, Close,
  Escape, Tab exit and outside dismissal without focus stealing. Context updates
  after reference attachment, model/mode choice, local Send, parent view and New.
  During read-only child inspection, counts/model/references remain explicitly
  parent-owned and no composer appears or execution target changes.
- Expanded draft + open example context + all three chips + open Agents passed at
  laptop/narrow dimensions, including complete chip/remove-button bounds. Neutral
  computed colors, no nested material, opaque fallback and reduced-motion zero
  transitions remained intact. No page errors/console warnings; app requests stayed
  loopback document/local JavaScript/CSS/on-demand highlighting.
- Finite initial **1440×960** rendered-background contrast: **103 visible DOM text
  ranges, minimum 5.782:1**, none below 4.5:1. Text fill hidden, motion disabled,
  own/ancestor clipping included and two paint frames awaited. Excludes SVG icons,
  native input values/placeholders, hidden/clipped ranges and other states. Not
  WCAG, all-state accessibility, assistive-technology or GPU certification.

Eighteen new captures were recorded under `concepts/react-rail-header/`; representative
views were visually inspected. All revision-3 captures remain unchanged:

| View | Capture |
| --- | --- |
| Main / laptop / narrow | [1600](concepts/react-rail-header/ava-react-rail-header-dark.png), [laptop](concepts/react-rail-header/ava-react-rail-header-laptop.png), [narrow](concepts/react-rail-header/ava-react-rail-header-narrow.png) |
| Context details / child parent-details | [Details](concepts/react-rail-header/ava-react-rail-header-details-1600.png), [narrow details](concepts/react-rail-header/ava-react-rail-header-details-850.png), [child details](concepts/react-rail-header/ava-react-rail-header-child-details.png) |
| Changes / closed / child | [Changes](concepts/react-rail-header/ava-react-rail-header-changes.png), [closed](concepts/react-rail-header/ava-react-rail-header-closed.png), [child](concepts/react-rail-header/ava-react-rail-header-child.png) |
| Expanded context / opaque | [Laptop context](concepts/react-rail-header/ava-react-rail-header-context.png), [narrow context](concepts/react-rail-header/ava-react-rail-header-context-narrow.png), [opaque](concepts/react-rail-header/ava-react-rail-header-opaque.png) |

Revision-4 integrated source/delta review reported **no material findings**. It
was a read-only source review, not an independent browser/test rerun. There are
no open material findings. Documentation gates passed: 149 Markdown files, 131
reachable documents, 18 required indexes, 30 llms links, all 16 verifier tests,
and whitespace. Final preservation across the 1,489-file baseline confirmed only
nine authorized existing paths changed, three obsolete `InspectorControls.*`
files deleted and three `ChatToolbar.*` source files plus 18 new captures added;
1,477 protected files remained unchanged. HEAD/staged/tracked unstaged patches,
state/fixtures/chip CSS, backend/legacy, dependency/config/lock/notices and all
prior captures were unchanged. No revision-3 image or validation result is
presented as revision-4 acceptance.

### Revision 3 coding-pass validation

**SUPERSEDED REVISION-3 HISTORY — not validation of revision 4.** The recorded
checks below are retained unchanged; the old module picker is no longer current.
On **2026-10-05**, after approving the monochrome mock and reviewing
[right-sidebar/status/composer research](sidebar-activity-research.md), the user
authorized Astra at max reasoning to implement revision 3. Its later cropped
header reference authorized the then-current top-right inspector toggle/picker.

Actually completed on Node **24.13.1** / npm **11.18.0**, reusing the installed
package dependencies. Results below cover the revision-3 top-right inspector controls:

| Check | Result |
| --- | --- |
| `npm run typecheck` | Passed (`tsc --noEmit`) |
| `npm test -- src/state/reducer.test.ts src/components/InspectorControls.test.tsx src/components/AppShell.test.tsx src/components/InspectorComposer.test.tsx` | **71** focused tests passed: 32 reducer + 8 header controls + 18 shell + 13 inspector/composer |
| `npm test` | **74 tests in 5 files passed**, including the 3 real Shiki cache/token/escaping tests |
| `npm run build` | Passed without warnings; 421 modules; main JS 476.56 kB / 143.91 kB gzip, on-demand highlighter 327.69 kB / 64.26 kB gzip, CSS 22.61 kB / 5.00 kB gzip |
| Owned whitespace | All 12 changed/new source/doc paths passed, including untracked files; `git diff --check` also passed |
| Supplied preservation baseline | Of 1,485 file hashes, 1,476 stayed unchanged; only 9 authorized existing paths changed and 3 focused component/CSS/test files were added. HEAD, staged and tracked unstaged patches, package/config/lock/notices, backend, legacy client, prior captures, and protected composer/chip CSS remained unchanged |

The new remembered-module regression failed before implementation and passed
following the typed state/actions. Tests cover explicit same-module selection,
shared rail/header state, native Enter/Space/Tab, selection/Escape focus return,
blur/outside-pointer dismissal without focus stealing, header hide retaining focus,
preserved panel close/Escape-to-rail, and identical parent/child/settings/history/
status state through inspector actions. The shell containment assertion still
checks the shared header before the canvas/inspector/rail body, now with exactly
two header controls. None of these tests assert jsdom geometry.

The prior shared-title-bar/chip baseline had 62 passing tests and a warning-free
417-module build. The 61-test pre-header pass and that 62-test baseline are
superseded by the coding results above, not erased. The `.chips` shrink protection,
composer bounds, reading styles, panel placement, and 44px rail are unchanged in
this control delta. No dependency, server, browser, capture, backend, or provider
operation was run by this coding pass. The build refreshes `dist`; the existing
built loopback preview remains under main ownership. All **18 direct pins**, npm
seven-day release-age guard, sandbox configuration, and license files remain
untouched.

### Revision 3 independent browser validation

**SUPERSEDED REVISION-3 HISTORY — not validation of revision 4.** Its 40 browser
checks, 15 captures, finite contrast sample, and completed source review below are
preserved. Revision-4 independent browser/source evidence is recorded above.

Main independently repeated typecheck, **74 tests**, and the warning-free build,
then checked the current built preview in normally sandboxed **Google Chrome
154.0.8037.97**. `chrome://sandbox` reported **“You are adequately sandboxed.”**
The existing loopback preview was reused; no second server or backend was started.

- Checked **1600×1000, 1440×960, 1180×900, 1366×768, and 850×768**. No document
  overflow; connected sidebar/canvas edges; shared header spans to the right edge.
  Chat and 44px rail begin at **y=76**, with the desktop inspector below that bar
  or the narrow inspector attached beneath the canvas. Composer remains inset,
  at most **780px**, with **24px bottom space** at these heights.
- Passed **23** real-browser parent/child/composer/keyboard invariants: exclusive
  panels, Escape/focus return, working-first parent-owned rows/count, read-only
  child selection, no new pin/recent/chat identities from inspection, preserved
  parent draft/model/mode/context/expansion, parent-bound Changes, failed/unavailable/
  empty states, local escaped Ctrl+Enter Send, New, and explicit parent-only Stop.
- Passed **17** additional header checks: control/picker bounds at 1600/1366/850px,
  first-button focus, selection/Escape focus return, same-module selection staying
  open, remembered Changes/rail choice after hide/show, outside-pointer dismissal
  without focus stealing, native Tab exit to the transcript, and unchanged child
  inspection/parent draft through header actions.
- Expanded draft + open context disclosure + all three chips + Agents passed at
  **1366×768 and 850×768**. **MOCK3-QA01 — fixed:** the chip row originally shrank
  and vertically clipped text/remove buttons at the latter size. Astra protected
  it with `flex-shrink: 0`; main verified each chip and **22px** remove button fits
  its own list, all composer controls remain bounded, and no document overflow.
- Computed colors remained neutral, with no nested material; opaque mode removed
  sidebar filtering and reduced motion yielded `0s` control/status transitions.
  No page errors or console warnings; app requests were only loopback document,
  local JavaScript/CSS, and on-demand highlighting.
- Finite initial **1440×960** rendered-background sample: **96 visible DOM text
  ranges, minimum 5.782:1**, none below 4.5:1. Text fill was hidden, motion disabled,
  and two paint frames awaited. Own text-container clipping was included so a
  truncated title cannot sample a sibling icon as its background. SVG icons,
  hidden/clipped ranges, native input values/placeholders, other states and assistive
  technology remain outside this sample. **Not WCAG/accessibility certification.**

Revision-3 captures were regenerated after its top-right control/chip corrections;
revision 1/2 captures remained untouched. These fifteen revision-3 views are now
preserved historical evidence, not revision-4 captures:

| View | Capture |
| --- | --- |
| Initial 1600×1000 | [Dark](concepts/react-agents/ava-react-agents-dark.png) |
| 1440×960 / 1180×900 / 1366×768 / 850×768 | [1440](concepts/react-agents/ava-react-agents-1440.png), [compact](concepts/react-agents/ava-react-agents-compact.png), [laptop](concepts/react-agents/ava-react-agents-laptop.png), [narrow](concepts/react-agents/ava-react-agents-narrow.png) |
| Changes / closed inspector / header picker | [Changes](concepts/react-agents/ava-react-agents-changes.png), [closed](concepts/react-agents/ava-react-agents-closed.png), [picker](concepts/react-agents/ava-react-agents-picker.png) |
| Child / failed / unavailable | [Child](concepts/react-agents/ava-react-agents-child.png), [failed](concepts/react-agents/ava-react-agents-failed.png), [unavailable](concepts/react-agents/ava-react-agents-unavailable.png) |
| Expanded composer / all-context laptop / narrow | [Composer](concepts/react-agents/ava-react-agents-composer.png), [context](concepts/react-agents/ava-react-agents-context.png), [context narrow](concepts/react-agents/ava-react-agents-context-narrow.png) |
| Opaque fallback | [Opaque](concepts/react-agents/ava-react-agents-opaque.png) |

The original source/state review reported no material findings, but excluded the
then-pending header placement. The completed named shared-header/chip/header-control
delta review also reported **no material findings**; these were read-only source
reviews, not independent reruns of main's tests/browser checks. **MOCK3-QA01 is fixed;
no open material findings.**

Final documentation gates passed: 149 Markdown files, 131 reachable documents,
18 required indexes, 30 llms links, all 16 verifier tests, and whitespace.
Final preservation against the original 1,459-file manifest confirmed only 20
owned existing paths changed and 30 authorized source/capture additions, with no
deletions. Against the 1,485-file pre-header-control manifest, only 25 owned source/
doc/current-capture paths changed and four were added. HEAD, staged/tracked unstaged
patches, backend, legacy client, package/config/lock/notices, and revision 1/2
concepts/captures remained unchanged. No revision 1/2 evidence qualifies revision 3.

### Revision 4 browser targets

Useful stable targets for main's new checks (prefer role/name before CSS):

- `group "Parent chat details"` in the shared header: **Context**, **N changes**,
  and **Child activity: N of M working**. Context controls
  `region "Parent chat context"` / `#parent-chat-context`, with **Close context
  details**. Initial focus is the labeled region; Close/Escape returns to Context,
  while outside/blur dismissal never redirects focus. The other two actions always
  open their parent-owned module and never execute work.
- **Show inspector** / **Hide inspector** is the first button in
  `navigation "Inspectors"`, above Agents. It retains focus and restores the last
  selected module. Expanded/control attributes reference only mounted panels.
- `navigation "Inspectors"`, buttons **Agents** / **Changes**, `#agents-inspector`,
  `#change-review`, and **Close agents** / **Close changes**. Open Agents starts
  with one Working child count; **Simplify route config** has none.
- `[data-child-id="search-return"]`, `search-outline`, `search-edge-cases` on
  **Remember search filters**; `keyboard-focus` on **Keyboard navigation**;
  `export-summary` (Unavailable) on **Export date range**.
- `#conversation-title`, `#conversation-canvas`, region **Read-only child context**,
  and **Back to parent**. Inspect/Back focuses the updated heading; closing an
  inspector focuses its rail icon. The title's closest `header` is the shared bar;
  its next sibling, `[data-inspector]`, is the body grid containing canvas/panel/rail.
- `#message-draft`, `#model-choice`, `#mode-choice`, **Add example context**,
  `#fixture-context`, **Expand draft** / **Collapse draft**;
  `form[aria-label="Message composer"]` and its `data-expanded` attribute.
- `[data-context-id]` chips, `svg[data-model-mark]`, `[data-chat-id]` sidebar
  references, and `[data-status]` leading lifecycle text/glyphs.

## State and interaction boundaries

`useReducer` retains small, typed, immutable, deterministic in-memory state. No
router/store, generated backend types, persistence, or service layer is added.
StrictMode can repeat initialization and transitions without duplicated identities
or side effects.

- Projects contains every chat. Selection from a project, pin, or recent shortcut
  resolves the **same chat ID/entity**, with its existing status and messages.
- `pinChat` is explicit and deduplicated; `unpinChat` removes only a pin. Initial
  pins are empty. Per-row buttons are labeled, keyboard-operable, have pressed
  state, and are siblings of selection buttons—never nested buttons. Removing a
  shortcut returns focus to the keyboard-focusable library when its row disappears.
- `recentChatIds` records only **view/new-chat actions**, newest first, unique,
  bounded at **five**. Only the initial current view is seeded. Pin/unpin, editing,
  Send/Stop, and disclosures do not create or reorder history. Pinned IDs are
  omitted from displayed Recent shortcuts to avoid a redundant second shortcut.
- `removeRecentChat` removes only the recent reference. Selecting that chat again
  records a view. Pin/unpin/removal/navigation never delete, archive, duplicate,
  cancel, or change chat status. Projects works with **no pins or recents**.
- Draft, fictional model, Build/Plan mode, attached example-context IDs, and inline
  expansion belong to each parent and survive project collapse, child inspection,
  and all navigation. New chat gets a deterministic local ID in
  the viewed project, expands that project, and records its view without a pin/run.
- Send appends **one escaped local user message** and clears that chat's draft.
  It never invents an assistant success, timer, stream, tool result, or live run.
  Ctrl/Cmd+Enter is the same action; plain Enter and IME composition do not submit.
  The live announcement says no model was called, without a visible helper row.
- Only explicit **Stop mock work** changes the viewed Working parent fixture.
  Other parents and all child fixtures retain their state. Background child
  liveness never changes the parent's Send/Stop target or status. Approval remains
  a secondary explanation, with no permission decisions or Allow/Deny controls.
- Tool activity remains a keyboard-operable disclosure. Native labeled model/mode
  selects use the same fictional catalog without repetitive visible “mock” labels.
  Changes reuses the read-only fixture excerpts inside the shared inspector and
  always follows the selected **parent**, even during child inspection. Close or
  Escape inside the panel returns focus to its rail button.
- `inspector` holds **Agents**, **Changes**, **Context**, or closed. The typed
  `lastInspectorModule` remembers the most recent choice, initially Agents.
  `toggleInspector` retains rail toggle semantics and records its module;
  `toggleInspectorVisibility` hides/restores it; `showInspector` always opens the
  requested module and is a no-op for the current one. Close preserves the memory.
  These transitions change only inspector fields, never parent/child selection,
  drafts, model/mode/context/expansion, pins/recents, messages, statuses, or notices.
  Nothing persists beyond this preview instance.
- Context is a dedicated parent-owned module, not a shared header disclosure.
  The composer meter calls `showInspector('context')`, so repeated activation
  never closes it. Rail hide/show retains focus; Close/Escape returns to the module
  rail. Prompt/raw disclosure state is local and resets on parent navigation or
  full unmount, not model changes, typing or Back. Counts derive from the escaped
  committed message projection. Usage/cost examples stay fixed after Send or
  reference/draft edits; matching selected-profile usage is independent of
  cumulative session counters. The host adapter supplies data; shared widgets
  have no fixture, state, filesystem or transport authority.
- `activeChatId` remains the parent. Separate `inspectedChildId` selection requires
  an existing child owned by that parent. Unknown/cross-parent children are ignored;
  any valid sidebar view (including the same parent) and New clear inspection.
  Inspect/Back never creates a chat, pin, or recent entry. Send/Stop **and composer
  edits** are guarded in the reducer while inspecting, not merely hidden in the UI.
- Add/context is an ordinary disclosure with native buttons, not an ARIA menu
  lacking arrow navigation. It focuses the first choice, closes when leaving the
  composer, and Escape returns focus to Add. Chip removal also focuses Add.
  Escape from an expanded draft collapses it and focuses the expand control.
  Expansion/model/context edits do not remount the textarea. References persist
  across local Send; only text is appended, with no upload or attachment processing.
- Fixture Markdown in both parent and child views keeps `skipHtml` and the default
  URL handling. User messages, code, and excerpts render as escaped React text.
  Shiki uses token spans, not
  `dangerouslySetInnerHTML`.
- All projects, models, activity, changes, and statuses are invented. There is no
  session/auth/provider/filesystem API, command/tool execution, IPC/Node bridge,
  git/worktree, cloud, native window, IDE, or terminal integration. Reload resets
  everything. No real task runs behind the Working status.

## Component and source map

All implementation paths below are relative to `apps/desktop-preview/`.

| Owner | Responsibility |
| --- | --- |
| `src/components/AppShell.tsx` | Headerless workspace, parent/child wiring, one stable canvas title and explicit committed Inspect/Back focus, accessible scope and live announcements |
| `src/components/contextPreview.ts` | Separate fictional provider/model/window profiles, fixed usage snapshots, invented system prompt and committed parent message projection; no live data |
| `src/components/ConversationIdentity.tsx` | Quiet title-only line at the canvas top-left inset and one child Back intent with host-supplied parent description |
| `src/components/{InspectorRail,Inspector,AgentList}.tsx` | Visibility first, then Agents/Changes/Context rail with remembered module, correct IDs/ARIA/focus; one attached scrolling page and selected-parent roster |
| `src/components/ChildConversation.tsx` | Committed user/assistant examples, distinct failure/unavailable states, read-only context and parent return |
| `src/components/ProjectSidebar.tsx` | Complete project library, disclosure, local New chat, explicit pins, contextual recent shortcuts, one scroll flow, opaque icon |
| `src/components/Conversation.tsx` | Opaque transcript, safe fixture Markdown and escaped local text, secondary mock-status explanation |
| `src/components/ToolActivity.tsx` | Keyboard-operable fictional activity disclosure |
| `src/components/Composer.tsx` | Stable draft/chips/expansion/model/mode/Send/Stop adapter plus selected-profile usage and explicit Context-open intent |
| `src/components/{ModelMark,SafeMarkdown}.tsx` | Original fictional catalog glyphs; shared safe Markdown boundary |
| `src/components/ChangeReview.tsx` | Read-only invented change excerpts inside the shared inspector; no duplicate header |
| `src/components/CodeBlock.tsx`, `src/highlight.ts` | On-demand cached Shiki core, one TypeScript grammar, first-party `ava-monochrome` theme, JavaScript regex engine; React token spans |
| `src/components/AvaMark.tsx`, `ChatStatus.tsx` | Existing first-party mark; shared status glyph/label palette and Working-only rotating icon; no glow or halo |
| `src/ui/` | Native controls/surfaces/status, controlled Presence/Disclosure and reduced-motion subscription |
| `src/chat/{contracts,requestValidation,useRequestIntent}.ts`, `src/chat/widgets/` | Controlled UI views/intents, fail-closed request validation and mounted intent latch; existing widgets plus optional composer usage/open intent |
| `src/chat/contextUsage.ts`, `src/chat/widgets/ContextInfo.tsx`, `ContextMeter.tsx` and their CSS | Safe snapshot math/formatting; controlled flat Context stats and escaped native prompt/raw disclosures; compact composer meter, no preview imports |
| `src/chat/contextUsage.test.ts`, `src/chat/widgets/ContextInfo.test.tsx` | Count/window/mismatch/stale/unknown/zero/over-window math, independent totals/costs, escaped projection and optional controlled composer checks |
| `src/catalog/`, `src/main.tsx` | Resettable invented examples on a separately lazy-loaded component route |
| `src/state/{types,fixtures,reducer,selectors}.ts` | Invented parent-owned children/context; pure guarded transitions; derived child order/count/ownership |
| `src/styles/{tokens,global}.css`, component CSS Modules | Neutral shell/materials and status-only tokens, bounded composer, connected geometry, focus/preference fallbacks |
| `src/state/reducer.test.ts` | Identity/status invariants, explicit pins, recent order/uniqueness/bounds, per-chat state, local-only actions, Stop target and purity |
| `src/components/{AppShell,ContextPage,InspectorComposer,ShellMotion,ChatStatus,CodeBlock}.test.tsx` | Preserved navigation/Send/Stop; three-module memory and explicit meter opens; parent Context/projection/focus; single child identity/Back; nested exits/races; status/icon-only motion; safe cached neutral tokens |
| `vite.config.ts`, `tsconfig.json`, `package.json` | Unchanged renderer-only toolchain and package-local checks; no Electron installed |

## Dependencies and licensing

The prototype uses only the necessary subset of the approved families: React,
TypeScript, renderer-only Vite/plugin-react, native CSS Modules, Phosphor,
react-markdown + remark-gfm, Shiki, and Vitest/Testing Library/jsdom. Native
controls avoid an unnecessary menu package. No Electron/electron-vite host,
packager, router/store, animation library, Tailwind, UI kit, editor, or terminal
is installed. Direct versions and the npm lock are package-local.

Exact direct pins in `package.json` and `package-lock.json`:

| Package | Pin | Role / declared license |
| --- | --- | --- |
| `react` / `react-dom` | 19.3.0 / 19.3.0 | Runtime / MIT |
| `@phosphor-icons/react` | 2.1.10 | Runtime icons / MIT |
| `react-markdown` | 10.1.0 | Runtime Markdown / MIT |
| `remark-gfm` | 4.0.1 | Runtime GFM / MIT |
| `shiki` | **4.4.3** | Runtime highlighting / MIT |
| `typescript` | 7.0.2 | Development compiler / Apache-2.0 |
| `vite` | 7.3.6 | Development/build / MIT |
| `@vitejs/plugin-react` | 5.2.0 | Development/build / MIT |
| `@types/node` | 24.13.1 | Development types / MIT |
| `@types/react` / `@types/react-dom` | 19.3.0 / 19.3.0 | Development types / MIT |
| `vitest` | **5.0.2** | Development tests / MIT core; bundled notices also include BSD-3-Clause and ISC |
| `@testing-library/react` | 16.3.3 | Development DOM tests / MIT |
| `@testing-library/user-event` | 14.6.7 | Development interaction tests / MIT |
| `@testing-library/jest-dom` | 7.0.1 | Development assertions / MIT |
| `@testing-library/dom` | 10.4.2 | Required Testing Library peer / MIT |
| `jsdom` | 29.1.1 | Development DOM / MIT |

Primary registry records use `https://registry.npmjs.org/<package>/<pin>`;
`dist.tarball`, `engines`, and `peerDependencies` identify the exact artifacts and
constraints. The original researcher verified those fields and direct runtime
LICENSE files. The revision 1 coding pass additionally inspected the installed six
runtime license files and both corrected-pin tarballs. Vitest 5.0.2 supports Node 24 and
Vite 7; Shiki 4.4.3 requires Node ≥20. jsdom **30.1.2 is not selected**, because
its Node 24.15 floor excludes the validated Node 24.13.1 environment.

The inspected direct runtime license texts are retained in
[`public/third-party-runtime-licenses.txt`](../../apps/desktop-preview/public/third-party-runtime-licenses.txt)
and copied into the built preview by Vite. This is deliberately a direct-package
notice record, **not** an assurance that every transitive shipped notice has
been inventoried.

See [local third-party notes](../../apps/desktop-preview/THIRD_PARTY_NOTICES.md)
for direct declared/inspected license evidence versus a full transitive,
grammar/theme provenance, or shipped-artifact audit. No production compliance
certification or new-stack Electron qualification is implied.

## Superseded revision 2 history

**Everything in this section is HISTORY for the approved monochrome revision 2,
not validation of revision 3.** Revision 2 introduced the bounded composer,
sidebar-only navigation, and neutral palette. It had trailing status metadata,
a header Changes button, an initially closed Changes panel, and no child inspector
or richer composer controls. All eight captures and the following checks remain
preserved and must not be overwritten or reused as revision 3 acceptance evidence.

### Historical revision 2 coding-pass validation

Actually completed on Node **24.13.1** / npm **11.18.0**:

| Check | Result |
| --- | --- |
| `npm run typecheck` | Passed (`tsc --noEmit`) |
| `npm test -- src/state/reducer.test.ts` | **16** state tests passed |
| `npm test -- src/components/AppShell.test.tsx` | **17** UI/keyboard tests passed |
| `npm test -- src/components/CodeBlock.test.tsx` | **3** real Shiki token/cache/escaping tests passed |
| `npm test` | **36 tests in 3 files passed** |
| `npm run build` | Passed without warnings; main JS 440.34 kB / 135.66 kB gzip, local on-demand highlighter 327.69 kB / 64.26 kB gzip, CSS 16.18 kB / 3.97 kB gzip |
| Scoped source/doc checks | Whitespace (including untracked files), both edited Markdown files' local links/anchors, and 63 equal-RGB color literals passed |
| Supplied preservation baseline | Protected package config/manifest/lock/notices, 39 legacy-client files, 18 existing concept files, HEAD, and staged changes unchanged |

The existing installation was reused. All **18 direct pins**, manifest, lock,
license files, Vite boundary configuration, npm seven-day age guard, and browser
sandbox settings remain untouched. No server, browser, or capture was started or
stopped by this coding pass; the parent-owned loopback preview was left available
for refresh after that build.

### Historical revision 2 browser evidence

The parent independently repeated **typecheck, all 36 tests, and a warning-free
build**, then checked the built revision 2 in a dedicated `agent-browser` session
using `/usr/bin/google-chrome-stable`, **Chrome 154.0.8037.97**, with its normal
sandbox. `chrome://sandbox` reported namespace/PID/network/seccomp-BPF protections
and “You are adequately sandboxed.” No sandbox exception or unrelated browser
session was changed.

Eight new revision 2 captures were visually inspected; the older blue images
remain unchanged and do not qualify this revision.

| Revision 2 view | Preserved historical capture |
| --- | --- |
| Monochrome, 1600×1000 | [Primary](concepts/react-monochrome/ava-react-monochrome-dark.png) |
| Monochrome, 1440×960 | [Standard desktop](concepts/react-monochrome/ava-react-monochrome-1440.png) |
| Monochrome, 1180×900 | [Compact desktop](concepts/react-monochrome/ava-react-monochrome-compact.png) |
| Monochrome, 1366×768 | [Laptop](concepts/react-monochrome/ava-react-monochrome-laptop.png) |
| Pinned/Recent, 1440×960 | [Sidebar shortcuts](concepts/react-monochrome/ava-react-monochrome-shortcuts.png) |
| Changes, 1440×960 | [Attached right inspector](concepts/react-monochrome/ava-react-monochrome-changes.png) |
| Changes, 850×768 | [Attached lower inspector](concepts/react-monochrome/ava-react-monochrome-changes-narrow.png) |
| Opaque, 1440×960 | [Manual material fallback](concepts/react-monochrome/ava-react-monochrome-opaque.png) |

Actually checked in the built browser:

- All four desktop/laptop sizes had **no horizontal or vertical document
  overflow**, a flush sidebar/canvas edge, no tab row, and an inset **780px**
  composer with 24px bottom space. The outer composer area matched the opaque
  reading canvas, with no material filter or full-width border/stripe.
- Projects precedes optional Pinned and Recent in one sidebar scroll flow.
  Empty optional sections disappear; removing every shortcut leaves the full
  project library usable. The old sidebar/footer utilities are absent.
- Seventeen interaction invariants passed: keyboard project/tool disclosures,
  neutral 2px focus outlines, explicit pinning/same-entity navigation, pin/recent
  deduplication, five-item newest-first recency, shortcut-removal focus return,
  per-chat draft/model/mode retention, local New chat, escaped Ctrl+Enter Send,
  Working submission suppression, and explicit-only targeted mock Stop.
- Changes shared the right canvas edge at 1440×960 and attached below at 850×768,
  with no document overflow and a bounded composer. Close returned focus to the
  Changes button. The narrow transcript/inspector remain independently scrollable.
- Sampled computed colors—including syntax spans—were neutral equal-RGB values.
  No nested material was present; only the sidebar used backdrop blur. Manual
  opaque removed its filter. Emulated reduced motion disabled button transitions.
- No page errors or console warnings were observed. Requests were only the
  loopback document and local JS/CSS/on-demand highlighter assets; no external
  font/image/provider or HMR connection was observed in the built preview.
- Finite **1440×960 glass-mode** rendered-background sampling covered **70 visible
  DOM text ranges**, minimum **5.782:1**, none below 4.5:1. Text fill was hidden,
  animations/transitions disabled, and two paint frames awaited. This excludes
  SVG icons, hidden/clipped ranges, native input values, and placeholders. It is
  **not WCAG certification, all-state coverage, assistive-technology validation,
  or GPU qualification**.

One integrated read-only source/state/CSS/test/documentation review reported **no
material findings**. It inspected the actual implementation but did not independently
run tests or browser checks. Historical Markdown-link/structure gates, all **16**
documentation-verifier tests, scoped whitespace checks, and the **1,452-file**
preservation baseline passed. Only authorized prototype/source/documentation
changes occurred; prior concepts, legacy client, frozen package/lock/config/notices,
HEAD, and staged work were preserved.

These are bounded mock-data browser checks, not production or Electron acceptance.

## Superseded revision 1 history

**Everything in this section describes the earlier blue/tabbed/full-width-dock
revision. It is retained as historical evidence, not as current design guidance,
browser evidence, or user approval.** Original concept and React artifacts
remain unchanged. Do not overwrite them with new monochrome captures.

Revision 1 used a 38px horizontal explicit-pin working set, mineral-blue smoked
sidebar, `#151b22` opaque canvas, ice-blue Send, amber attention, full-width glass
composer dock, and visible preview/material footer. All those color/navigation/
composer/footer choices are superseded by the second user request.

### Historical coding and dependency checks

Revision 1 completed typecheck, 13 reducer tests, 11 UI/keyboard tests, and 2
highlighter tests (**26 total in 3 files**). Its warning-free build measured main
JS 445.68 kB / 137.11 kB gzip, highlighter 354.02 kB / 68.51 kB gzip, and CSS 19.81 kB /
4.81 kB gzip. The parent independently repeated those checks before browser review.
These counts, bundles, and UI checks are superseded by the current table above.

The original package-local install (`npm install --ignore-scripts --no-audit
--no-fund`) installed 261 packages. `npm ls --depth=0` and `npm ls --all` reported no
dependency problems; direct manifest/lock versions and SHA-512 integrity entries
were checked. `cmp` confirmed the six runtime license texts were copied unchanged
to the build. Scoped whitespace and local Markdown-link checks passed.

The initial install rejected Shiki 4.5.0 under unchanged `min-release-age=7`;
registry dates also put Vitest 5.0.3 inside that window. The parent authorized only
**Shiki 4.4.3 / Vitest 5.0.2** after engine/peer/license checks. Installation then
succeeded with the age guard still **7**. No force, age, engine, permission, or
security bypass was used. These corrected pins remain unchanged through revision 3.

### Historical browser review and captures

These six captures are of the **superseded built React revision 1**, not the current
monochrome implementation or the original static concept. The parent visually
inspected all six at that time.

| Revision 1 view | Preserved historical capture |
| --- | --- |
| Blue/dark, 1600×1000 | [Primary](concepts/react-preview/ava-react-connected-dark.png) |
| Blue/dark, 1440×960 | [Standard desktop](concepts/react-preview/ava-react-connected-1440.png) |
| Blue/dark, 1180×900 | [Compact desktop](concepts/react-preview/ava-react-connected-compact.png) |
| Blue/dark, 1366×768 | [Laptop](concepts/react-preview/ava-react-connected-laptop.png) |
| Attached Changes, 1440×960 | [Read-only fixture inspector](concepts/react-preview/ava-react-connected-changes.png) |
| Opaque, 1440×960 | [Manual material fallback](concepts/react-preview/ava-react-connected-opaque.png) |

The historical parent review used **Google Chrome 153.0.8010.36**, a dedicated
`agent-browser` session, and `/usr/bin/google-chrome-stable` with normal sandboxing.
`chrome://sandbox` reported namespace, PID/network namespace, and seccomp-BPF
protection enabled: “You are adequately sandboxed.” No sandbox flags or unrelated
browser sessions were changed.

Historical observations, **revision 1 only**:

- All four desktop sizes shared edges without document overflow; tabs measured
  38px, and the full-width composer met the integrated status footer. The opaque
  transcript scrolled independently. This geometry has since been replaced.
- Changes measured 330px attached right at 1440×960 and attached below at 850×768.
  Opaque mode removed all three then-existing material filters.
- Enter/Space disclosures, a 2px focus outline, native selects, Ctrl+Enter local
  Send, independent drafts, pin deduplication, zero-tab navigation, explicit mock
  Stop, and literal escaped markup were checked in that earlier browser build.
- No page errors were observed. Requests stayed on the loopback document and
  local JS/CSS/highlighter assets; no external fonts/images, provider, or HMR
  connection was observed in that built preview.
- A finite **1440×960 glass-mode** rendered-background sample covered **86 visible
  DOM text ranges**, minimum **5.03:1**, none below 4.5:1. It excluded SVG icons,
  hidden/clipped ranges, native input values, and placeholders. It was not WCAG
  certification, all-state contrast coverage, or GPU qualification—and does not
  apply to the new neutral surfaces.

One integrated read-only revision 1 source/CSS/state/license-boundary review
reported no material findings; it did not independently rerun tests/browser checks.
Historical documentation/link gates, 16 documentation-validator tests, whitespace,
and preservation-baseline checks passed. None of these historical review results
substitute for a later revision's browser and integrated review.

## Sources inspected

Revision 3 visually inspected the approved [monochrome baseline](concepts/react-monochrome/ava-react-monochrome-dark.png)
and all three supplied layout references (leading statuses, joined right rail,
compact composer/model glyph). A later full-workspace reference informed only the
shared title bar above the conversation/inspector/rail. The subsequently supplied
OpenChamber header crop informed only the compact right-end inspector split control;
its usage meter, avatar/account, Local/Git controls, colors, and other modules were
not adopted. The reviewed context map and
[sidebar research](sidebar-activity-research.md) informed ownership and mock-only
boundaries. No competitor code/brand assets were copied. The fictional glyphs are
first-party SVGs. Pen authentication, external design uploads, and new tools or
dependencies were not needed.

The subsequent revision-4 user correction moved visibility above Agents on the
rail and replaced that generic header control with truthful local parent details.
The approved context map and current first-party files informed implementation;
frontend-design and relevant React patterns were read, with the user's palette,
fonts, and bounded scope taking precedence. No new documentation/API retrieval or
competitor implementation was needed for this presentation-only change.

Previously retrieved React [`useReducer`](https://react.dev/reference/react/useReducer),
[DOM refs](https://react.dev/learn/manipulating-the-dom-with-refs), and MDN
[`aria-expanded`](https://developer.mozilla.org/en-US/docs/Web/Accessibility/ARIA/Reference/Attributes/aria-expanded)
documentation were retrieved for immutable StrictMode-safe transitions and ordinary
disclosure/focus behavior. No documentation CLI was installed.

Historical revision 2 visual inputs are the user's two rejected-UI screenshots (composer
helper/shortcut row and empty horizontal pinned strip), revised monochrome/sidebar
requirements, and current first-party React source/fixtures. The original AVA mark
and local type stack are retained; no legacy or competitor implementation/assets
were used.

Primary API guidance consulted for revision 2:

- [React `useReducer`](https://react.dev/reference/react/useReducer): pure immutable
  transitions, initializer, stable dispatch, and StrictMode double calls.
- [Shiki custom themes](https://shiki.style/guide/load-theme),
  [fine-grained bundles](https://shiki.style/guide/bundles), and
  [regex engines](https://shiki.style/guide/regex-engines), plus installed 4.4.3
  theme-registration types: a first-party theme object, core + one selected
  grammar, cached JavaScript engine, no theme bundle/Wasm/CDN. The suggested
  `/guide/themes` URL returned 404; `/guide/load-theme` is the current custom-theme
  guide used instead. No documentation CLI was installed.

Preserved context from revision 1:

- [Original HTML/CSS study](concepts/liquid-glass/index.html) and its
  [1600×1000 capture](concepts/liquid-glass/ava-liquid-glass-dark.png) supplied the
  first-party AVA mark; the old palette/composer/navigation are superseded.
- [Design direction](liquid-glass-design.md), [desktop scope](README.md),
  [window/navigation decisions](window-navigation-research.md), and
  [approved library families](library-research.md).
- [react-markdown security](https://github.com/remarkjs/react-markdown#security),
  [Phosphor React README](https://github.com/phosphor-icons/react#readme), and
  [Vite server options](https://vite.dev/config/server-options) informed the
  preserved Markdown, icon-import, and loopback boundaries.

## Remaining limits

This is a short-fixture desktop browser prototype, not a scalable transcript or
mobile app. Revision-6 coding/browser-motion/finite-contrast/geometry/local-state
checks and the bounded source review/recheck are complete; **WORK6-F01 is fixed**
and no material findings remain.
Revision-5 and earlier evidence is preserved history, not current acceptance.
Neither DOM tests nor bounded browser evidence establishes all-state accessibility, GPU or production
performance, or final user acceptance. The live child inspector is
**not RPC-ready**: the research
identifies a future backend/protocol decision, not a public serializer or renderer
filesystem shortcut. Long-history virtualization, production
CSP/sandbox/performance, complete keyboard/screen-reader/contrast coverage,
light/system-theme design, full shipped-license/SBOM review, and real Electron/
backend behavior remain unqualified and outside this implementation. Material
choice is local and resets on refresh; CSS preference support depends on the browser.
