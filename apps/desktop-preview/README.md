# AVA desktop UI preview

## Revision 18: stable chat tools and supplied runs (current)

Compact tool rows now have individual disclosures, stable live output and supplied
read-only diffs. Explicit **Run preview** plays Completion, Failure and Burst
snapshots; Send still adds only a local user turn. Draft/selection, reading-away,
Latest, Stop/retry and departed-run guards passed independent native checks.
Astra MAX delivery passed **699 tests / 106 native checks / eight painted-text
states**; the sole Sol 6.1 MAX **38-file / 24-actual-PNG** review returned
**No material findings.** Entry is **351,358 raw bytes**, with genuine optional
owner loading and **148,642 bytes headroom** under the unchanged 500,000 cap.
[Scope and 22 current captures](../../docs/desktop/chat-run-refinement.md).
Browser-only/visit-local, not real provider or tool execution. Earlier “current”
sections below are preserved history.

A standalone **browser-only React/TypeScript prototype** with invented, in-memory
fixture data. It is not an Electron client, a backend integration, or a replacement
for `apps/desktop/`. Refreshing resets everything.

## Revision 17: quieter Settings navigation (current)

The left Settings scrollbar is hidden without disabling native wheel/keyboard
scrolling. Global reset is no longer a navigation button: **Reset all preferences…**
is a quiet text action at the bottom of General, separate from Reset General,
opening the same confirmation. Right/stack scrolling and all 16 pages remain.
Astra MAX delivery passed 614 tests/44 native checks/three painted-text states;
one Sol 6.1 MAX eight-file/nine-actual-PNG gate returned **No material findings**.
[Scope and eight current captures](../../docs/desktop/settings-scroll-layout.md#revision-17-quieter-settings-navigation).
Earlier “current” sections below are preserved history.

## Revision 16: independent Settings scrolling (current)

Settings keeps its header/Back visible. Desktop navigation and content scroll
independently with native wheel and keyboard input; at ≤1100px they share one
stacked scroller. All 16 pages, searches/resets and chat state remain intact.
Astra MAX implementation passed 611 tests and 324 native checks across five sizes;
one Sol 6.1 MAX source/21-actual-PNG gate returned **No material findings**.
Entry remains 499,991 raw bytes under the unchanged 500,000 cap.
[Scope, verification and 20 current captures](../../docs/desktop/settings-scroll-layout.md).
Earlier “current” sections below are preserved revision history.

## Revision 15: five working Settings screens (current)

General, Shortcuts, Conversation behavior, Notifications & sounds and Context
management now have real visit-local controls: future-chat defaults, native binding
recording/dispatch, tool/output/wrap/scroll preferences, explicit Web Audio tests and
safe context-policy projections. Settings totals 16 searchable pages.

Astra MAX delivery passed Main's 605 tests, typecheck, warning-free 493-module build,
210 native checks and 138 scenarios. Sol 6.1 MAX inspected all 42 changed files and
27 actual images: **No material findings.** Native-test F01 is fixed. Entry:
499,991 raw bytes, nine below the unchanged cap; no dependency/config changes.

[Current scope/evidence](../../docs/desktop/settings-behavior.md) and
[26 current captures](../../docs/desktop/concepts/react-settings-behavior/) own
acceptance. Refresh resets edits. No real notifications, compaction, persistence or
backend integration. Earlier “current” labels/counts/cap notes below are unchanged
**dated history**, superseded for this scope.

## Revision 14: live Appearance customization (current)

Settings → Appearance now has Default/Charcoal/Midnight dark palettes, local UI-font
choice, 14/15/17px prose, line spacing, 620/700/780px reading width, higher contrast,
synchronized sidebar material and app-wide reduced motion. A live sidebar/reading
sample and **Reset appearance** apply immediately without changing chat behavior,
models, definitions or drafts. Settings density remains clearly Settings-only.

Astra MAX implementation passed Main's typecheck, **480 tests/34 files** (four
workers), warning-free 482-module build, 98 native checks and 135 existing scenarios.
One Sol 6.1 MAX source/actual-visual review read 18 images: **No material findings.**
Entry is 498,306 raw bytes, 1,694 below the unchanged cap. A genuine optional readonly
child workspace owns Loading/error/Back handling; no config or dependency change.

[Current scope/evidence](../../docs/desktop/appearance-customization.md) and
[18 current captures](../../docs/desktop/concepts/react-appearance/) own acceptance.
Visit-local only; no Light/System theme, arbitrary CSS/font downloads or persistence.
Earlier appearance/cap records below remain unchanged **dated history**.

## Revision 13: providers, resource pages and chat tools (current)

Real provider identities now have 15 neutral logo/name cards using 14 pinned local
original SVGs. Providers stays top-level; Resources has separate Subagents, Skills,
Prompts and Plugins pages; Connections contains only MCP and LSP. Settings owns
13 individual pages. User-owned definitions can be created/edited for this visit,
including description and **local model preference**, not backend-applied routing.

Chat has conversation-wide **Expand all tools / Collapse all tools**, compact
activity rows and supplied read-only inline file diffs. No inferred edits, live
connections, file execution, persistence, prices or model calls are introduced.
Astra MAX implemented these changes. Main passed 378 nearest tests, typecheck and
all 454 tests, a 477-module build, 50 normal-native checks, 135 scenarios and 16
finite contrast states. Sol 6.1 MAX's integrated source/actual-visual gate and named
hover recheck are clear. Both editor submit buttons retain 18.423:1 hovered contrast.
Entry remains 499,990 bytes under the unchanged 500,000-byte cap (10 bytes headroom).

See [current scope/evidence](../../docs/desktop/provider-resource-chat-refinement.md)
and [30 current captures](../../docs/desktop/concepts/react-provider-resources-tools/).
The prior approval-held and combined-resource-page records below are unchanged
**dated history**, superseded by this browser-only revision, not production claims.

## Revision 12 follow-up: Settings-only navigation and model browser (current)

Settings now hides **both** the chat sidebar and workspace, uses the full viewport,
and returns to the same chat/sidebar state via Back. Retained hidden controls
cannot navigate, create chats or mutate the sidebar. Its own section nav remains.

The model picker now has a selected-profile summary, search and neutral selectable
cards grouped by supplied family/provider. The repeated mock/scope/default and
reasoning-setting paragraphs are gone; meaningful badges, compact scope and
required unavailable/security text remain. Narrow cards stack; short dialogs scroll.
No live catalog, model invocation, price or reasoning setting is invented.

Main passed **104 nearest tests**, typecheck, **351 full tests/21 files**, a
warning-free **468-module build**, **43 sidebar/page checks**, **18 model-browser
checks**, all **114 scenarios/14 families**, six request/catalog checks and five
finite painted-text contrast states (minimum **7.825**). Entry is **499,783 bytes**,
below the unchanged500kB warning. One short-card/footer overlap was reproduced and
fixed in model-only CSS; native selection/Use now passes. **Sol6.1 MAX reviewed
source and14fresh images: no material findings.**

See [current evidence](../../docs/desktop/normal-app-refinement.md#settings-only-navigation-and-model-browser-follow-up)
and [16 fresh captures](../../docs/desktop/concepts/react-model-browser/).
Previous revision12 and older records below remain unchanged history. No new
provider assets/dependencies/backend/native integration are adopted.

## Revision 12: normal-app refinement (current UI phase)

Astra at max reasoning has implemented the normal-app refinements. Open
**`http://127.0.0.1:5182/`**, not the optional scenario catalog, to review them.
Settings is a dedicated page with nav-local search, remembered section and a
Phosphor cog. Shared native modals no longer have a visible title/subtitle slab;
accessible names, required body explanations, Escape and focus guards remain.
The Enter preference now operates the local composer with Shift/IME safeguards.

Normal tool rows show supplied outcomes and tasteful status accents. Generated
image names become “Pasted image”; raw invented paths remain escaped opt-in details.
AVA headings show immutable supplied turn model/thinking facts, never the current
composer selection. Unknown facts stay omitted. Send remains user-only and adds
no fictitious assistant invocation, token usage or thinking setting.

Main independently passed typecheck, **337 tests in 21 files**, a warning-free
**468-module build**, **52 normal-app checks**, **114 scenario observations**,
six request/catalog checks and six actual-motion checks. Entry JS is **499,945
bytes**, below the unchanged 500kB warning. Fourteen finite contrast samples pass;
normal/tool minimum is **5.405**. All **35 layout checks** across seven shell/
Settings/model variants pass. Main also verified **48 native Tab observations**;
BODY plus foreground focus reproduced in vanilla `showModal` with no AVA JS,
not an app regression. No trap, timer or focus-source change was added.

**Sol 6.1 MAX inspected source and 25 fresh normal-app images: no material findings.**
There are **44 fresh PNGs** in
[react-normal-app](../../docs/desktop/concepts/react-normal-app/).
UI-phase checks and preservation are complete. Provider-logo adoption awaits
explicit approval; no new dependency or brand asset is imported.
See [normal-app scope and provider sourcing](../../docs/desktop/normal-app-refinement.md).
Composer CSS, dependencies/configuration/backend/legacy source are unchanged.
The revision-11 and earlier text below is preserved verbatim as historical evidence,
including its date-relative “current” labels—not the current Settings-page contract.

## Revision 11: everyday screens and main-chat integration (current)

Astra at max reasoning owns this first-party frontend source/design pass.
**Independently mock-validated on 2026-10-07: 316 tests and all 114 screen scenarios.**
Open Settings at the bottom of the project sidebar or the model button in the
main composer. Both lazily load a small local screen host, not the review
controller or catalog. While content loads, a cancellable native dialog blocks
background Send and navigation. The default entry does not load screen content.

Settings presents the same eight sections in an opaque, wide native dialog.
Inner model, provider and reset dialogs preserve the base Settings view. Escape
clears an inner search or backs a connection step before dismissing its dialog,
then backs the settings section before closing Settings; IME Escape is ignored.
Close restores a visible opener immediately, including root dismissal with an
inner modal open. About contains the small `?view=screens` review link.

The shared model picker changes only the captured local parent chat, with latest
view/identity/version/lifetime guards. It preserves draft, mode, references,
expansion, pins, recents, title, messages and Context disclosures. Working is a
fixed mock status, not a model-editing lock. During read-only child inspection,
the current model is disabled with a reason; the new-chat default remains usable.
Main shows reasoning **facts only** and emits no chosen level: PreviewState has
no reasoning setting. The standalone review still rehearses reasoning choices.

The local default starts at Aster and actually supplies the model to New chat;
Build remains the default mode. New Mini/Orion chats have unknown usage and cost,
not copied telemetry. Reset confirms only preferences and the new-chat default,
never old chats. Density, motion, help, Enter practice and notifications survive
Settings close/reopen but affect only these review views, not native chat behavior.
Dark only; Light/System/rebinding remain explicitly unavailable. Trust/resources
are read-only examples scoped to the actual example parent, not backend state.

`?view=screens` remains a separately lazy workspace with **14 families and 114
manual scenarios**, including settings, model selection, provider directory and
masked dummy-key/browser/device steps, session/command search, rename,
archive/branch previews, workspace recovery, message/tool projections and
permission/question/run dialogs. It has its own resettable fixture controller.
No real login, URLs, clipboard, polling, persistence, credentials, configuration,
private data, backend/native integration or permission settlement is introduced.
The separate component catalog retains its native controlled model select.

### Revision 11 independent validation

Main independently ran `npm run typecheck`, **316/316 tests in 19 files** (9.57s),
including **23 focus-hook regressions**, and a warning-free **468-module build**.
Entry JS stays **499,889 bytes**, below the unchanged 500kB warning. Screen content remains lazy; neither default chat nor opening main
Settings loads the standalone review/catalog controllers.

Normally sandboxed Chrome 154 passed **114 observations across 14 families**,
**107 interaction checks** (50 screen, 23 main integration, 23 preserved chat,
11 actual motion), **28 transition/non-interference checks**, 24 native Tab/
Shift-Tab observations and secret-answer Enter submission. Seven shell layouts
and **13 modal viewport/internal-scroll/focus layouts** passed, including narrow,
short and ultrawide variants. Native Tab may visit browser chrome at a boundary;
this is not replaced with a JavaScript trap.

**SCREEN11-F01/F02/F03 are fixed.** Connected/success fixtures use completed white;
model Escape clears search first. Explicit accepted dialog transitions recover
usable focus in the same top modal. Chromium can keep the just-disabled captured
control focused through the layout effect before dropping to BODY; the narrow
repair handles that transient too, never another/current enabled control, typing,
other modal, browser chrome or a closed/unmounted view. No timer or focus watcher.
The integrated source review and the final named F03 delta recheck have no
remaining material findings. Intermediate worker checks below are coding history.

Finite contrast at eleven sampled states: shell/Context **97/123 active ranges,
minimum 5.405**; Settings/Appearance/models/device/success/permission/question/run
**25/30/35/14/11/16/16/10 ranges, minimum 6.805**; initial catalog **32 active ranges,
minimum 7.130**, with one disabled range reported separately. This excludes icons,
native values/placeholders, clipped/hidden text, modal backgrounds and unsampled
states; it is not all-state accessibility or assistive-technology qualification.

**45 fresh PNGs** are under
[`react-screen-pack`](../../docs/desktop/concepts/react-screen-pack/).
Console/page errors were empty; recorded HTTP page requests were loopback-only.
Documentation/link/structure checks, tracked/untracked whitespace and the quiet
1,654-file preservation audit pass: **18 authorized modifications, 70 additions,
no deletions; 1,636 protected files unchanged**. Git snapshots, Composer CSS,
backend, legacy client, dependencies/configuration, R10/older history and evidence
are preserved. No C++/Electron/live-provider or production qualification is claimed.

Open the review at **`http://127.0.0.1:5182/?view=screens`**; ordinary chat and
`?view=components` remain separate. The page supplies resettable manual scenarios,
not authentication, saved settings, permission settlement or native authority.

### Revision 11 source map

- `src/components/AppShell.tsx`: focused screen request, latest scope guards,
  local preferences/default model, lazy `MainScreens` and native pending dialog.
- `ProjectSidebar.tsx` / CSS: one Settings button plus optional New-chat callback;
  `Composer.tsx` forwards the optional model-open intent.
- `src/chat/widgets/ChatComposer.tsx`, `ModelChoiceButton.module.css`: optional
  native model button; catalog select and Mode/Context/Send/Stop are preserved.
  **`Composer.module.css` remains byte-identical.**
- `src/state/types.ts`, `reducer.ts`: only optional validated New-chat model input;
  no preference/reasoning/provider store or schema expansion.
- `src/screens/MainScreens.tsx`: lazy Settings/models/provider/reset adapter using
  the shared views, never a router, RPC bridge or review scenario controller.
- `SettingsScreen.tsx`, `ModelDialog.tsx`, `ProviderViews.tsx`, `ScreenPrimitives.tsx`:
  shared controlled screens. `contracts.ts` bounds local presentation intents.
  `src/ui/DialogFrame.tsx` owns native focus/modality and the optional wide frame.
- `screenExamples.ts` shares only the existing model/provider/preference values;
  `fixtures.ts` re-exports them. Even opening main Settings does not load the
  standalone case index; no examples or scenarios were added or changed.
- `ScreenReview.tsx`, `fixtures.ts`, `FinderDialog.tsx`, `ChatDialogs.tsx` and
  `useViewGuard.ts`: the existing standalone pack/index remains intact.
  `useDialogTransitionFocus.ts` is a lazy, explicit-transition-only focus helper;
  native Tab and shared DialogFrame remain unchanged.
- `src/main.tsx` keeps separate lazy review/catalog routes; `contextPreview.ts`
  still shares the existing read-only profile facts without changing Context math.

### Revision 11 Phase-2 coding checks

- Nearest integration/dialog/model/default/ownership checks: **187/187 in 11 files**
  (8.38s). `npm run typecheck` passed. Full `npm test`: **293/293 in 18 files**
  (9.37s), including **24 added tests**. Dialog support is test-only, not a
  production platform shim. No worker browser/reviewer/capture wave was run.
- Warning-free `npm run build`: **467 modules**, 1.61s. Entry JS **499.89kB /
  151.68kB gzip**; lazy MainScreens **5.28/2.31kB**, shared settings/screens
  **27.93/8.97kB**, and ScreenReview **30.80/10.00kB**. A first build identified
  an unnecessarily large multi-weight gear import; the new Settings button uses
  a small first-party inline glyph instead. No threshold/config/chunk workaround.
- **SCREEN11-F01:** success/connected examples now use completed white, not Working
  green; a source-palette regression checks the mapping. **SCREEN11-F02:** model
  search clears on first Escape, with IME/focus/modal-lifetime regression coverage.
- `git diff --check`, untracked-text checks and the quiet 1,654-file baseline
  comparison pass; no unexpected delta/deletion. Git snapshots, Composer CSS and
  R10/older history are preserved. Main owns the category-link/inventory additions.
- These are source/DOM/build checks, not native-browser Tab, layout, contrast or
  final acceptance evidence. Main owns the subsequent independent combined pass.

### Revision 11 Phase-1 coding checks

- Nearest new suites: **56/56 across 3 files** (5.42s), including a traversal of
  every indexed scenario. `npm run typecheck` passed. Full `npm test`:
  **269/269 across 16 files** (7.95s); the existing 213 tests remain unchanged.
- Warning-free `npm run build`: **464 modules**, 1.57s. Main JS **494.67kB /
  149.77kB gzip** remains below the 500kB warning; screen-review JS is a separate
  **59.16/17.70kB** lazy chunk, with **9.21/2.45kB** CSS. No chunk threshold or
  configuration workaround. Existing `dist` was rebuilt, not the server.
- These are coding/DOM checks, not native-browser Tab-loop, painted layout,
  contrast or independent acceptance evidence. Main owns that subsequent pass.

The revision-10 and earlier design/evidence text below is preserved verbatim as
history, not revision-11 independent acceptance.

## Current design: dedicated Context page (revision 10)

Code and design by **Astra at max reasoning**. Revision 10 and the logo-only
empty-chat follow-on are independently mock-validated: **213 tests, 63 browser
checks and seven layouts**, plus the checked empty-chat/long-raw-text variants.
One integrated source review found no material issue. Older validation remains
history, not revision-10 evidence or production qualification.

The shared ParentDetails row is removed. **Agents**, **Changes**, and **Context**
each own one attached, flat, scrolling page. Agents still starts open. The rail
keeps Show/Hide first, then Agents, Changes, Context; same-module rail activation
closes it, while Show restores the last module. Close/Escape returns immediate
focus to the corresponding rail button. No parent identity or Context/change/
activity action row repeats above Agents or Changes.

A small neutral native button **inside the existing opaque composer** explicitly
opens Context; repeated activation never closes it or invokes Send/Stop. The
fictional Aster Lab / Aster configured profile supplies **173,120 / 214,000 →
`173.12k (81%)`**. Aster Mini has its own **48,160 / 128,000 → `48.16k (38%)`**
snapshot, not a rescaled Aster measurement. Selecting Orion exposes a known
96,000-token window with **unknown** active usage; new/unsampled chats also show
unknown. Configured windows are supplied examples, not marketing limits or
output-reserved effective input budgets.

Context separates the selected profile and active estimate from cumulative
input/output/reasoning/cache-read/cache-write, independently supplied total,
cost estimate/coverage, available snapshot timestamps, and committed message
counts. Missing, invalid, stale, or profile-mismatched values never become zero;
supplied zero remains zero, and over-window percentages are not clamped. The
sample cumulative total does not add reasoning/cache subsets. Complete and
partial cost examples are available; missing cost is unknown, not `$0`.
Drafts, reference chips, local Send, and mode changes do not simulate tokenization
or billing. All Context data follows the parent during read-only child inspection.

**System prompt** and **Raw messages** use existing controlled native disclosures.
The prompt is first-party invented text. Raw messages serialize only an escaped
projection of committed fixture/local parent messages, including visible code/
notes, never drafts, private reasoning or actual provider payloads. Counts derive
from that projection. One compact qualifier names this boundary. No copy/export,
file access, upload, RPC, provider, session, or native integration is introduced.
Tab/Enter/Space remain native; Escape from focused expanded text first closes its
disclosure and restores the trigger. The next Escape closes the pane. Parent
navigation resets disclosures; typing/model changes and Back do not remount them
or steal focus. Full pane unmount/module replacement also resets local disclosures.

The one title stays at the **canvas top-left** with no subtitle or repeated sidebar
identity. The single child Back, Inspect/Back title focus, parent draft/model/mode/
reference ownership, pins/recents, and parent-only Stop remain unchanged. The pane
still scrolls as a whole and attaches **below the canvas at ≤1100px**. Existing
780px reading/composer bounds, 15px / 1.7 prose, 300px / 360px ultrawide caps, local
font stack, first-party marks, neutral palette and status-only accents are retained.
Composer CSS, including chip shrink protection, 70px cap and 22px removal targets,
is unchanged; the meter has its own CSS and uses existing control wrapping.

Existing **180ms feedback**, **200ms ease-out** disclosure/pane/module motion,
**28s neutral sidebar drift**, and **2.4s icon-only Working** motion remain intact.
Logical close is immediate; retained exits are inert, aria-hidden and guarded.
Reduced motion removes them immediately. No new global listener, focus trap,
absolute accessibility summary, dependency, store or timer-based activity was added.
The separately lazy-loaded catalog remains unchanged; its composer needs no usage
prop. The new controlled Context views import no preview state or fixture data.

### Revision 10 coding checks

- Before edits, nearest four suites: **60/60** (7.71s). Updated nearest checks:
  **125/125 across 7 files** (7.30s), covering shell/composer/Context, numeric
  validation and generic inspector memory.
- `npm run typecheck` passed. Full `npm test`: **212/212 across 13 files** (8.08s).
  Warning-free `npm run build`: **452 modules**, 1.71s; main JS **494.45kB /
  149.74kB gzip**, CSS **27.59kB / 6.14kB gzip**. Lazy catalog **19.88/6.89kB**
  JS and **4.03/1.20kB** CSS; highlighter **327.69/64.26kB** unchanged. No chunk
  threshold/config workaround or additional lazy boundary was needed.
- Focused regressions cover finite/safe counts, positive windows, zero/over-window,
  stale/provider/model/profile mismatches, independent totals and nullable costs,
  escaped projections/counts, optional composer compatibility, explicit meter opens,
  three-module ARIA/focus/memory, parent ownership, reduced motion and inert exits.
- Source owners: `src/chat/contextUsage.ts`, controlled `ContextInfo`/`ContextMeter`
  widgets, and `src/components/contextPreview.ts` for invented profiles/projections.
  Only the inspector type union and generic module tests changed in state; fixtures,
  reducer/selectors, catalog, other widgets, tokens/config/dependencies and historical
  captures remain frozen. Obsolete `ParentDetails.tsx/.module.css/.test.tsx` are removed.
- These are code/DOM checks, not painted geometry or visible-focus acceptance.
  No worker browser/reviewer/capture wave, server, install, provider or C++ checks.
  Existing `dist` was rebuilt; Main owns independent acceptance and final evidence.
- Quiet preservation audit: **1,620 baseline files**, **13 allowed changes**, **3
  obsolete removals**, **1,604 unchanged**, and **9 allowed new files**. HEAD,
  staged and tracked-unstaged snapshots match. `git diff --check` and owned-file
  whitespace checks pass; no outside-scope edit. Revision-9/older evidence sections
  remain verbatim. This is the coding audit, not Main's independent acceptance.

#### Logo-only empty-transcript follow-on

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

### Revision 10 independent validation

Main repeated typecheck, **213 tests / 13 files** (7.49s), and a warning-free
**452-module build** (1.55s). Final raw/gzip kB: main JS **494.24/149.68**, shared CSS
**27.45/6.11**; catalog **19.88/6.89** JS and **4.03/1.20** CSS; unchanged highlighter
**327.69/64.26**. No install, config change or backend/provider call.

Normally sandboxed Chrome checked **seven layouts** (1600×1000, 1440×960,
1366×768, 850×768, 2560×1440, 3440×1440, 1440×640). **63 browser checks = 23
preserved + 11 actual motion/preferences + 17 Context/meter/unknown/logo + 10
native disclosure/long-scroll/race + 2 catalog/lazy compatibility**. Four empty-chat
variants have the static 44px AVA mark precisely centered in the available transcript;
four long-raw-text variants scroll the entire Context pane without document overflow.
Native first Send, model/profile ratio, unknown/independent accounting, parent-owned
child inspection, Escape/IME/Tab, inert exits and rapid module switching passed.
One integrated source review: **no material findings**.

Finite contrast: **95 default / 121 Context / 151 raw / 74 closed active samples**,
each minimum **5.405:1**, none below 4.5. The method excludes SVGs, native input
values/placeholders, hidden/clipped ranges and other states; not all-state accessibility
or native/GPU certification. Console/page errors were empty; **54 recorded URLs**
were loopback-only; the default route did not load the catalog. Other unsupported
preference fallbacks remain source-checked, not newly emulated.

**28 PNGs** in `docs/desktop/concepts/react-context-page/`; no new recording or
production/live integration. [Captures and preservation](../../docs/desktop/react-prototype.md#revision-10-independent-validation).
The settings/model/provider-login/dialog screen expansion is separately authorized
and being inventoried; it is not implemented by this revision.

### Revision 9 coding checks

- Before editing: closest four suites **59/59** (6.96s Vitest). Updated
  AppShell, ParentDetails, InspectorComposer, and ShellMotion suites: **60/60**
  (7.04s). Full `npm test`: **179/179 across 11 files** (8.41s).
- `npm run typecheck` passed. Warning-free `npm run build`: **446 modules**, 1.61s.
  Main JS **487.39kB / 147.52kB gzip**, CSS **27.86kB / 6.17kB gzip**; lazy catalog
  JS **19.88kB / 6.89kB gzip**, CSS **4.03kB / 1.20kB gzip**; lazy highlight
  **327.69kB / 64.26kB gzip**; HTML **0.61kB / 0.36kB gzip** (Vite-reported sizes).
- DOM regressions cover one stable canvas title, no title-line subtitle or sidebar
  identity, named-parent Back description, committed Inspect/Back and same-child
  focus, resize/module/show-hide stability, inert exits, and no ordinary or deferred
  StrictMode editing focus steal. Obsolete placement-media expectations were
  replaced; existing Context, readonly, parent Send/Stop, and race coverage stays.
- Only existing shell/identity/details source, the four affected tests, and these
  two current documentation surfaces changed in the coding pass. State, body and
  composer CSS, kit/catalog, materials/status, dependencies/config, backend/native,
  and prior captures remain frozen. Revision-8 and older records below are verbatim.
- Quiet preservation audit: **1,603 baseline files**, **11 owned changes** plus
  **three Main-owned doc changes**, **1,589 unchanged**, no additions/deletions.
  HEAD/staged/tracked-unstaged snapshots match; `git diff --check` and baseline
  whitespace checks for all 11 owned files pass. Historical records match exactly.
- DOM tests do not establish painted top-left geometry or visible focus. Main owns
  browser acceptance, fresh captures, final preservation gates, and one integrated
  review. No worker browser, new server, install, or native integration was used.

### Revision 9 independent validation

Main repeated typecheck, **179 tests / 11 files** (9.51s), and a warning-free
**446-module build** (1.75s). Final raw/gzip kB: main JS **487.39/147.52**, shared
CSS **27.86/6.17**, catalog JS **19.88/6.89** and CSS **4.03/1.20**, highlighter
**327.69/64.26** unchanged. [Open revision 9](http://127.0.0.1:5182/?revision=9).

Normally sandboxed Chrome checked **seven layouts**, including 850×768,
1440×640 and both 2560×1440 / 3440×1440 targets. The title is always canvas-top-left,
without subtitle or sidebar identity; the reading column/composer remain centered
≤780px. **50 browser checks = 16 title/focus/Context/scroll/race + 23 preserved
identity/composer/navigation + 11 actual motion/preference checks**. Stable title
focus, named-parent Back, read-only children, short/narrow scrolling and the existing
containment fix passed. One integrated source review: **no material findings**.

Finite default/expanded/closed text samples: **101/117/73 ranges**, each minimum
**5.405:1**, none sampled below 4.5. Console/page-error records were empty; **48
recorded URLs** were loopback-only; default chat did not load the catalog. Opaque
and reduced-motion were rendered; other preference/unsupported-blur fallbacks remain
source-checked. This is not all-state accessibility/native/GPU qualification.

**17 fresh PNGs** in `docs/desktop/concepts/react-chat-title/`; selected captures and
exact preservation are in the [prototype record](../../docs/desktop/react-prototype.md#revision-9-independent-validation).
The controlled kit/state/Composer CSS/config/backend and all prior evidence remain
unchanged. No backend, provider, persistence or production integration is implied.

### Revision 8 coding checks

- Before editing: closest four suites **50/50**; all **1,576** baseline hashes and
  HEAD/staged/tracked-unstaged snapshots matched.
- Updated four suites: **59/59** (6.08s Vitest / 6.39s wall). Full preview suite:
  **178/178 across 11 files** (6.85s Vitest / 7.16s wall). Typecheck passed (0.29s).
- Warning-free Vite build: **446 modules**, 1.60s build / 1.83s wall. Main JS
  **488.26kB / 147.85kB gzip**, CSS **28.27kB / 6.23kB gzip**; lazy catalog JS
  **19.88kB / 6.89kB gzip**, CSS **4.03kB / 1.20kB gzip**; lazy highlight
  **327.69kB / 64.26kB gzip**; HTML **0.61kB / 0.36kB gzip** (Vite-reported sizes).
- Regression coverage includes inline Context Tab/Escape, parent ownership,
  preserved disclosure during module/child changes, nested inert exits, full and
  rapid reopening, reduced-motion removal, cleanup, one current title/Back, and
  committed inspect/return focus. A StrictMode ref-replay focus regression was
  caught and fixed; ordinary editing retains focus across identity relocation.
- DOM/media tests do not claim browser geometry. No browser/server/capture,
  install, backend/native, persistence, provider, or authority work was performed.

### Revision 8 independent validation

Main repeated typecheck, **178 tests / 11 files** (7.01s), and a warning-free
**446-module build** (1.60s). Final raw/gzip kB: main JS **488.26/147.84**, shared
CSS **28.29/6.23**; catalog **19.88/6.89** JS and **4.03/1.20** CSS; highlight
**327.69/64.26** unchanged. The built loopback preview remains at
<http://127.0.0.1:5182/?revision=8>.

Normally sandboxed system Chrome checked 1600×1000, 1440×960, 1366×768, 850×768,
2560×1440, 3440×1440 and 1440×640. The **70** browser checks comprise **23** preserved
identity/composer/navigation checks, **19** new sidebar/native-focus checks,
**11** actual motion/preference checks, **12** Context/module/scroll/race checks
and **5** separate-catalog/lazy-route smokes. Default/expanded/closed finite text
samples were **102/118/74 ranges**, each minimum **5.405:1**; these exclude SVGs,
native values, hidden/clipped content and other states, not all-state accessibility.

**SIDEBAR8-F01 fixed:** hidden absolute accessibility summaries escaped the new
unpositioned details block, adding invisible narrow-page overflow. Native A/B and
scroll regressions proved `position: relative` contains them without changing
visible layout. Astra made that sole correction; the named source recheck agreed.
No remaining material findings. Reduced-motion and opaque behavior were rendered;
other preference/unsupported-blur fallbacks were source-checked, not emulated.
Console/page-error records were empty; **156 recorded URLs** were loopback-only.

Fresh evidence: **24 PNGs and one actual 1440×960 VP8 WebM** (2.4s, 230,586 bytes)
in [`react-sidebar-details`](../../docs/desktop/concepts/react-sidebar-details/ava-react-sidebar-details-1440.png).
The [prototype record](../../docs/desktop/react-prototype.md#revision-8-independent-validation)
links selected layouts/motion and preservation. No backend/provider, native/Electron,
new install, persistence, production accessibility/performance or licensing claim.

## Reusable frontend kit (Phase 2)

Open **`http://127.0.0.1:5182/?view=components`** for the separately lazy-loaded
catalog. The default route remains the existing chat shell, with no new main-chat
button or toolbar. Reset/refresh clears catalog state; Back to chat returns to the
preview. All scenarios and receipts are **invented/local**, not execution results.

- `src/ui/`: native Button/IconButton, Surface, shared Status, and the existing
  controlled Disclosure/Presence/reduced-motion foundations. Semantic neutral
  spacing, radius, control and surface tokens are actually reused.
- `src/chat/contracts.ts`: readonly, discriminated **UI view models**, not RPC
  schemas. `src/chat/widgets/` implements MessageTurn, ToolActivityGroup,
  PermissionCard, QuestionCard, RunNotice, ChatComposer, and ChangeReviewView.
  They do not import preview state/actions/fixtures or parse transport data.
- `components/Conversation`, `ToolActivity`, `Composer`, and `ChangeReview` are
  fixture adapters. `components/contextPreview.ts` owns the separate invented
  usage profiles and committed local projection for Context. Existing labels,
  parent drafts/settings, IME shortcuts, context focus/blur/Escape, safe Markdown/
  code, and frozen composer/chip CSS remain. ChatComposer accepts optional controlled
  usage/open intent; existing catalog consumers need neither.
- Permission/question intents carry caller-supplied opaque IDs and UI versions.
  Only ready requests can emit. Session permission must be advertised explicitly;
  unknown/duplicate choices, invalid answers, unsupported declared flags, and
  pending/error/stale/closed requests fail closed. A mounted guard permits one
  intent per ID/version; **the host must retain authoritative pending/settled state
  across unmounts**. Explicit fixture revision increments the version. No resolver,
  effective scope, persistent grant, or exactly-once transport is created.
- Questions support native single/multiple choice, filtering, optional custom
  answers, and masked secret inputs. Receipts never echo answers. Run retry is an
  explicit optional callback; attempts/cancel/terminal states are supplied, not
  timed. Streaming/partial turns, all tool lifecycles and truncated output, and
  loading/empty/error/stale/ready change excerpts are exercised in the catalog.

This is a bounded presentation kit, not production wiring. Host adapters, routing,
replay/reconnect, permissions/resolver settlement, real attachments, public-job
mapping, child transcripts, persistence/native integration, and light/system themes
remain unsupported. The composer's reference popover retains its established
immediate-unmount behavior. Revision 10 replaces the separate parent-details row
with one Context page using existing inline disclosure motion; composer CSS stays
frozen and the optional usage extension leaves existing consumers unchanged.

## Run locally

Use **Node 24.13.1** and npm 11.18.0 (the validation environment). From the repo root,
for a fresh checkout:

```sh
cd apps/desktop-preview
npm ci --ignore-scripts --no-audit --no-fund
npm run dev
```

Open <http://127.0.0.1:5182>. Development and built preview both bind only
`127.0.0.1`, with strict port **5182**. Stop with Ctrl+C when finished. The dev server
allows filesystem serving only within this package; no broad host, CORS, or
filesystem exceptions are configured.

For the built UI, without Vite's development HMR connection:

```sh
npm run build
npm run preview
```

Do not run dev and preview simultaneously on the same port. Keep normal npm
security/release-age policy and browser sandboxing enabled; stop and investigate
rather than bypassing a failed install or launch.

## Try the local interactions

- **Projects** is the complete chat library. Expand/collapse projects and switch
  chats without pinning them or changing their mock status.
- **Pins start empty.** Each chat row has a labeled Pin/Unpin button. Pinning adds
  a shortcut to **Pinned**, below Projects. Unpinning removes only that shortcut,
  never the chat or current view. The section disappears when empty.
- **Recent**, below Projects and Pinned, tracks only actual views and new chats:
  newest first, unique, at most five in memory. Only the initial view is seeded.
  Pinned chats are omitted from the displayed recent list to avoid redundant
  shortcuts. Removing a recent shortcut never deletes or stops a chat; selecting
  the chat again records a new view. An empty Recent section is omitted.
- All three sections share one independently scrollable sidebar flow, not a
  fixed bottom bar. Shortcuts include the project name and reference the same
  chat identities and statuses as the project library.
- Each parent retains its own draft, fictional model choice, Build/Plan mode,
  attached example references, and inline composer expansion across selection,
  pinning, recent removal, project collapse, and child inspection. These choices
  do not configure a provider or grant any permission.
- **New chat** creates an in-memory chat in the viewed project without pinning it.
- **Send** (or Ctrl/Cmd+Enter) adds one escaped local user message. It does not
  generate a response, start a timer, run a model, or simulate successful work.
  Its tooltip and accessible shortcut label retain the keyboard hint.
- The fixed Working parent has an explicit **Stop mock work** action. It changes
  only the viewed parent fixture to Stopped, never a background child or another
  parent. Child liveness does not change the parent's status or execution target.
  Navigation, unpinning, and recent removal never stop work. Approval is
  explanatory, with no Allow/Deny decisions.
- **Agents** lists children of the selected parent, Working first, with bounded
  snapshot timing/model metadata. Remember search filters has Working, Completed,
  and Failed examples; Export date range has an Unavailable child; Simplify route
  config has none. These are fixed examples, not live jobs or verified check results.
- Select a child to inspect its committed user/assistant conversation in the
  center. Parent identity stays clear. **Back to parent** restores the same parent
  draft/settings/context/expansion; both transitions focus the updated heading.
  Inspection never creates a top-level chat, pin, or recent view. Any sidebar view
  or New clears inspection. No child Send/Stop/Steer/Restart/Promote is offered;
  Send/Stop and composer edits are also blocked by the reducer while inspecting.
- **Add example context** opens an ordinary disclosure with invented file, image,
  and note references. Native buttons toggle per-parent chips in memory. No OS
  dialog, file input, filesystem read, upload, or attachment processing occurs.
  Escape closes it and returns focus to Add; chip removal also returns focus.
  Leaving the composer closes the disclosure. Attached references remain after
  local Send, which appends only text.
- **Expand draft** grows the same textarea within the canvas. Collapse or Escape
  retains the text; Escape returns focus to the expand control. A first-party
  model glyph remains beside the name in the labeled native picker. Build/Plan
  stays a local choice, not a permission switch.
- The rail's first **Show inspector** / **Hide inspector** button restores the last
  selected module without changing parent/child selection, drafts, settings,
  shortcuts, or statuses. Hiding/showing keeps focus on that button.
- **Context** is a dedicated rail page, not a shared header row. The composer's
  **173.12k (81%)** button explicitly opens it and retains focus. Choose Aster Mini
  for its different snapshot, Orion for unknown active usage, or New chat for
  unavailable usage/cost with an actual zero local-message count. System prompt
  and Raw messages are read-only native disclosures with escaped example text.
  Model changes, typing and local Send keep disclosures stable; only the committed
  raw projection/message counts change on Send, never token/cost measurements.
- Context and Changes follow the selected parent during child inspection. Context
  has no Send/Stop, export, upload, clipboard or file operation. Close/Escape
  returns to its rail icon; Escape inside expanded raw/prompt text closes that
  disclosure first and restores its trigger.
- Tool activity opens with click, Enter, or Space. All activity/file/code/change
  records are invented. **Changes** follows the selected parent even during child
  inspection. Close or Escape inside any inspector returns focus to its rail
  icon. The rail remains present when both panels are closed.
- The half-circle **Opaque materials** icon beside AVA switches the sidebar
  material. Reduced motion, reduced transparency, higher contrast, and
  unsupported-blur fallbacks remain in CSS.

The HTML title/description, accessible app description, action tooltips, and
screen-reader announcements retain the truthful preview boundary without a
visible developer bar. There is no persistence, session/auth/provider integration,
filesystem API, IPC/Node bridge, command execution, real tool use, git/worktree,
native window, editor, terminal, or cloud behavior. There is no running agent
behind the Working label.

## Validation

```sh
npm run typecheck
npm test
npm run build
```

### Revision 7 combined independent validation

On normally sandboxed system Chrome, the main agent verified **88 browser checks**:
11 icon/panel/disclosure/material/preference checks, three module-entry/race/resize
checks, 23 preserved inspector/composer/identity checks, seven parent-header/focus
checks, and 44 catalog/rendering/capability/privacy checks. Actual intermediate
opening/closing widths, disclosure heights and module-entry opacity were sampled;
rapid reopen/switch/resize reached one active panel without duplicate IDs. Exiting
content was immediately inert/aria-hidden and guarded. Reduced motion stopped
animation/transition and removed closed content without retention. Opaque mode
removed drift/blur; reduced-transparency/contrast and unsupported-blur fallbacks
were source-checked, not independently emulated.

Layouts passed at **1600×1000, 1440×960, 1366×768, 850×768, 2560×1440 and
3440×1440**: connected edges, shared 76px header, first-in-rail toggle/44px rail,
42px/16px desktop and 38px/14px compact controls, no document/header overflow,
300px/360px ultrawide panel caps, and centered composer ≤780px. Catalog horizontal
bounds also passed at laptop/narrow/ultrawide sizes. Native focus/dismissal,
parent-only Stop, read-only child inspection, controlled values, escaped content,
permission capabilities, inactive request states, repeat intents, and masked/no-echo
secret inputs passed. The mounted resolver guard is not durable delivery authority.

Independent typecheck, **169 tests/11 files (6.08s)** and a warning-free **444-module
build (1.57s)** passed; chunk sizes match the Phase-2 table below. The catalog is
on demand. Browser errors/console were empty and recorded requests were loopback-only.
One read-only integrated source reviewer reported **no material findings**.

Finite contrast: **103 shell text ranges, minimum 5.405:1**; 59 catalog ranges across
two positions, with **58 active ranges, minimum 6.805:1**. One native disabled-control
label measured 4.321:1 and is excluded from the active minimum. SVGs, native values/
placeholders, hidden/clipped text, other scroll positions/states and motion sweeps
are excluded. This is not all-state accessibility or performance certification.

Fresh evidence: **20 PNGs and one actual WebM** under
[`react-frontend-kit`](../../docs/desktop/concepts/react-frontend-kit/).
The [prototype record](../../docs/desktop/react-prototype.md#revision-7-combined-independent-validation)
owns captures and preservation details. Backend, legacy client, fixture state,
composer CSS, dependencies/configuration/notices, Git state and prior evidence
remain preserved. No Electron/live-provider/native/production qualification follows.

### Revision 7 Phase 2 coding-pass validation

Baseline adapter checks: **44 passed (5.65s)**. New widget/catalog checks:
**64 passed (2.18s)**. Typecheck and **169 tests in 11 files (6.81s)** passed,
retaining all 105 Phase-1 cases. The tool disclosure assertion now checks immediate
logical inaccessibility rather than mistaking inert exit paint for active content.
Tests cover controlled values, malformed metadata, stale/duplicate intent guards,
secret masking/no echo, escaping, native keyboard/focus, reset, and unmount safety.

The build is warning-free: **444 modules / 1.76s**. Main JS **488.34 kB / 147.92 kB
gzip**; lazy catalog JS **19.88 / 6.89 kB**; shared CSS **29.34 / 6.40 kB**; catalog
CSS **4.03 / 1.20 kB**. Highlighter source and size remain unchanged at
**327.69 / 64.26 kB**. The existing preview's `dist` is refreshed. No install,
server, browser, capture, staging, or commit operation was performed. Browser
motion/geometry/accessibility and one combined source review remain main-owned;
these tests do not qualify rendered motion or production behavior.

### Revision 7 Phase 1 coding-pass validation

Baseline nearest tests: **28 passed**. Updated nearest tests: **44 passed (5.01s)**.
Typecheck and all **105 tests in 8 files (6.22s)** passed, retaining the original
89 cases with the exiting-DOM structural assertion updated to logical visibility.
New checks cover inert/aria-hidden exits, callback guards, immediate reduced-motion
unmount, focus preservation, rapid reopen/switch/resize state, duplicate IDs, media
listener cleanup, pending-exit unmount, and the icon-only source contract. These
are DOM/state/source checks, **not rendered motion, geometry, or contrast claims**.

The build is warning-free: **425 modules / 1.68s**; main JS **481.99 kB / 145.63 kB
gzip**, highlighter **327.69 kB / 64.26 kB gzip**, CSS **26.97 kB / 5.94 kB gzip**.
The existing preview's `dist` was refreshed. No install, server, browser, capture,
staging, or commit operation was performed. Frozen state/fixtures/reducer, composer
chips, dependencies/configuration, backend, legacy client, and history are untouched.
Whitespace and the **1,527-file baseline** passed: 15 owned existing paths changed,
6 owned files were added, and 1,510 protected files remain unchanged. Two existing
main-owned docs and its new comparison record are allowed separately. HEAD, staged
and tracked unstaged patches, and all historical assets remain unchanged.
Main owns new revision-7 browser evidence and the later integrated review.

### Revision 6 coding-pass validation

On Node **24.13.1** / npm **11.18.0**, the baseline shared-status run passed **10
tests**; the updated status + inspector/composer run passed **23 tests**. Typecheck,
all **89 tests in 6 files**, and a **warning-free 421-module build** passed.
The existing Working-only hooks now drive CSS rotation and text glow; no markup
change was needed. Regression checks cover hook removal for every nonworking
status, including Stopped and idle, accessible names/distinct glyphs, and no timers.
These are DOM/state checks, **not proof of rendered motion or contrast**.

Build sizes: main JS **480.11 kB / 145.04 kB gzip**, highlighter
**327.69 kB / 64.26 kB gzip**, CSS **24.96 kB / 5.58 kB gzip**.
Owned whitespace and the **1,521-file baseline** check passed: four coder-owned
paths plus two concurrent main-owned index/research docs changed; **1,515 protected
files**, HEAD/staged/tracked unstaged patches, and all prior captures are unchanged.
This coding pass changed only shared status CSS/tests and the two current mock
records; the build refreshes `dist` for the existing preview. No install, server,
browser, or capture operation was performed. After the named interpolation fix
below, nearest/all tests, typecheck and the build passed again; final CSS is
**24.99 kB / 5.58 kB gzip**, JS sizes unchanged. The 13 v5 captures and all earlier
evidence remain unchanged history.

### Revision 6 independent browser validation

Main repeated typecheck, all **89 tests / six files** and the warning-free build.
In adequately sandboxed **Chrome 154.0.8037.97**, **10 targeted checks** passed:
actual cyclic icon motion and smooth text glow; only Working animates; matching
parent/child behavior; fixed readable green/opacity/status bounds; unchanged
mock lifecycle; reduced-motion removal; parent-only Stop; read-only child safety.
**13 unforced live frames over 3.2s showed 13 distinct shadow intensities** and
icon angles crossing a repeat boundary; separate 0°/45°/180° samples corroborate.
1440×960, 1600×1000, 1366×768 and 850×768 retain the existing geometry, no document
overflow and neutral non-status computed colors.

Finite base-background contrast: **103 ranges, minimum 5.782:1**, none below 4.5:1.
Text fill/shadows hidden, motion disabled, clipping included; excludes SVG/halo/glow
pixels, native values and other states. Not all-state accessibility, WCAG, Electron
or performance certification. No browser errors/warnings or non-loopback app requests.

**WORK6-F01 is fixed:** Chrome did not smoothly interpolate the initial deferred
`currentColor` shadow mix. Controlled A/B demonstrated interpolation/paint changes
with the existing `var(--status-working)` token. Astra changed just those four mix
inputs; the same reviewer rechecked the named delta and reported **fixed, no material
findings**. No palette/timing/radii/markup/state changes or new dependency.

Six fresh artifacts under `docs/desktop/concepts/react-working-motion/` include
[actual parent/child motion GIF](../../docs/desktop/concepts/react-working-motion/ava-react-working-motion-loop.gif)
and [WebM](../../docs/desktop/concepts/react-working-motion/ava-react-working-motion-loop.webm),
plus phase/reduced-motion captures. GIF is an enlarged crop of live capture, not
fabricated animation or a seamless-cycle/performance benchmark. The
[prototype record](../../docs/desktop/react-prototype.md#revision-6-independent-browser-validation)
lists evidence/limits. Final preservation: 6 existing authorized paths changed,
6 artifacts added, no deletions; **1,515 protected baseline files unchanged**,
including prior captures, Git state, TSX/layout/state/palette, backend/legacy,
dependencies/config/lock/notices.

**The revision-5 coding and browser results below are historical, not revision-6 acceptance.**

### Revision 5 coding-pass validation

On Node **24.13.1** / npm **11.18.0**, the baseline inspector/composer run passed
**13 tests**; the nearest status + inspector/composer run passed **23 tests**.
Typecheck, all **89 tests in 6 files**, and a **warning-free 421-module build**
passed. Ten new status tests cover semantic labels, decorative distinct glyphs,
empty idle, the Working-only halo hook and its removal, accessible row names,
and no timer-based settlement. These are DOM/state checks, **not composited color,
motion, contrast, or browser acceptance**.

Build sizes: main JS **480.11 kB / 145.04 kB gzip**, highlighter
**327.69 kB / 64.26 kB gzip**, CSS **24.45 kB / 5.48 kB gzip**.
Owned whitespace and the **1,507-file preservation baseline** passed: 5 coder-owned
existing paths changed and one focused status test was added. Apart from concurrent
main-owned desktop index/research doc edits, all protected hashes and
HEAD/staged/tracked unstaged patches remain unchanged. No state, fixture, reducer,
other component logic, dependency/config/lock/notices, backend, legacy client,
or historical capture changed. Only the build refreshes `dist` for the main-owned
preview; no install, server, browser, or capture operation was performed.

### Revision 5 independent browser validation

Main independently repeated typecheck, **89 tests / six files**, and the warning-free
build, reusing the loopback preview in adequately sandboxed **Chrome 154.0.8037.97**.
**33 browser invariants** passed: 10 targeted palette/motion checks plus 23 preserved
parent/child/composer/identity/focus/Send/Stop checks. At 1600×1000, 1440×960,
1366×768 and 850×768, no document overflow or layout regression was observed;
non-status computed colors stayed neutral. Sampled Working halo at 0/1400/2800ms
showed **.14/.30/.14** opacity and **.9/1.05/.9** scale with static readable label,
unchanged bounds/status data. Reduced motion disables the animation while retaining
green; opaque mode retains the palette. Explicit parent Stop remains parent-only.

Finite initial 1440×960 text contrast: **103 ranges, minimum 5.782:1**, none below
4.5:1. Excludes SVG/halo pixels, native values/placeholders, clipped/hidden text and
other states; not all-state accessibility, assistive-technology or WCAG certification.
No browser errors/warnings or non-loopback app requests. One bounded independent
source reviewer reported **no material findings**, without rerunning main's checks.

**13 fresh captures** are under `docs/desktop/concepts/react-status-accent/`.
The [prototype record](../../docs/desktop/react-prototype.md#revision-5-independent-browser-validation)
lists evidence and limitations. Final preservation: 7 authorized existing paths
changed, one status test + 13 captures added, no deletions; **1,500 protected baseline
files unchanged**. Git state, backend/legacy client, layout/state/fixtures/chip CSS,
dependencies/config/lock/notices and all historical captures were preserved.
Revision-4 results below remain historical, not current acceptance.

### Revision 4 validation

**SUPERSEDED REVISION-4 HISTORY — not revision-5 acceptance.**

On Node **24.13.1** / npm **11.18.0**, typecheck, **76 focused tests** (32 reducer,
18 shell, 13 inspector/composer, 13 toolbar/rail), all **79 tests in 5 files**, and
a **warning-free 421-module build** passed. The full suite retains the 3 real
Shiki tests. Build sizes: main JS **479.93 kB / 144.96 kB gzip**, highlighter
**327.69 kB / 64.26 kB gzip**, CSS **23.51 kB / 5.19 kB gzip**.

Coverage retains the prior meaningful state, keyboard, and safety assertions while
replacing obsolete picker expectations: first-toggle DOM order, remembered module,
retained toggle focus, same-module header opens, scoped context/count updates,
parent-owned child inspection, native disclosure focus/Tab/Escape/outside dismissal,
listener cleanup, and unchanged identity/settings/history/status/execution boundaries.
These are state/DOM/focus checks, **not viewport geometry or browser acceptance**.

Owned whitespace and the **1,489-file preservation baseline** passed: 7 authorized
existing paths changed, 3 obsolete `InspectorControls.*` files deleted, 3 focused
`ChatToolbar.*` files added. All other baseline files, including revision-3 images,
HEAD/staged/tracked unstaged work, dependencies/config/notices, backend/legacy,
and protected `.chips { flex-shrink: 0 }` styling remain unchanged. No install,
server, browser, capture, or provider operation was performed; only the build
refreshes `dist` for the main-owned preview.

Main independently repeated typecheck/all **79 tests**/build. Five viewport sizes,
**23** preserved browser invariants and **22** new toolbar/rail checks passed,
including first-toggle placement, truthful context/count updates, parent-owned
child inspection and keyboard/dismissal behavior. Finite initial contrast sampling:
**103 text ranges, minimum 5.782:1**; not accessibility certification.
Eighteen fresh captures and exclusions are in the
[revision-4 browser record](../../docs/desktop/react-prototype.md#revision-4-independent-browser-validation).
Source/delta review reported **no material findings**; final documentation and
preservation gates passed. Revision-3 qualification below is preserved
superseded history; `docs/desktop/concepts/react-agents/` remains untouched.

### Superseded revision 3 baseline

Revision 3 top-right inspector-control coding-pass checks on Node 24.13.1 /
npm 11.18.0: typecheck, **74 tests in 5 files**, and a **warning-free build**
(421 modules). The nearest run passed **71 tests**: **32** reducer, **18** shell,
**13** inspector/composer, and **8** header-control tests. The full suite also
retains **3** real Shiki tests. The remembered-module regression failed before
implementation and passed afterward. These are state/DOM/focus tests, not geometry
claims. The previous 62-test shared-header/chip baseline is extended, not replaced;
its chip shrink protection, composer bounds, and reading styles remain untouched.
Build sizes: main JS **476.56 kB / 143.91 kB gzip**, highlighter **327.69 kB /
64.26 kB gzip**, CSS **22.61 kB / 5.00 kB gzip**.

Owned whitespace (including untracked files) and supplied preservation-baseline
checks are recorded in the implementation notes. No install, server startup,
browser launch, screenshot, backend, or provider call was performed. The build
refreshes `dist`; the existing preview remains under main ownership.

Main independently repeated typecheck, all **74 tests**, and the warning-free
build, then verified the current built preview in normally sandboxed Chrome.
Five viewport sizes (1600×1000 through 850×768), **23** inspector/composer checks,
**17** header-control checks, and expanded-context/chip bounds at laptop/narrow
sizes passed. Finite initial contrast sampling covered **96 visible text ranges**,
minimum **5.782:1**; not accessibility certification. Fifteen current captures and
scope/exclusions are in the [browser record](../../docs/desktop/react-prototype.md#revision-3-independent-browser-validation).
Source/state and named shared-header/chip/header-control reviews reported **no
material findings**; documentation/preservation gates passed.
These bounded checks do not establish GPU performance, complete assistive-technology
behavior, production readiness, or Electron acceptance.

Regression coverage includes ownership/unknown-child rejection, immutable parent
identity, read-only reducer Send/Stop guards, parent return/view/New clearing,
exclusive/closed/empty inspector behavior, remembered-module show/hide, explicit
same-module picker selection, rail/header consistency, dismissal without focus
stealing, unchanged parent/child state, derived counts, per-parent context
isolation/deduplication/removal, expansion and text retention, keyboard/focus,
leading statuses, pins/recency, model marks, escaped text, and safe Markdown.
Original meaningful sidebar, Send/Stop, and highlighter assertions remain.

### Superseded history

Revision 2 completed typecheck, **36 tests in 3 files** (16 reducer, 17 shell,
3 highlighter) and a warning-free build. Its coding pass also reused dependencies
and left the running preview untouched. Revision 1 completed **26 tests in 3
files**. All revision 1/2 captures, bundle metrics, browser/contrast checks, and
review results are preserved in the [prototype history](../../docs/desktop/react-prototype.md#superseded-revision-2-history).
They are **HISTORY, not revision 5 acceptance evidence**; no artifacts are overwritten.

The highlighter remains a local on-demand chunk with a cached JavaScript regex
engine, one TypeScript grammar, and one first-party neutral theme. No CDN, Wasm,
or theme bundle is used. All 18 direct dependencies and their lock remain
unchanged, including **Shiki 4.4.3** and **Vitest 5.0.2**. The existing installation
was reused; the npm **seven-day release-age guard** and sandbox settings were not
modified.

Live child inspection is **not RPC-ready**. It needs a future backend/protocol
decision; these renderer fixtures do not expose internal inspector frames, read
session files, or imply a public child-transcript endpoint.

See [implementation notes](../../docs/desktop/react-prototype.md) for the exact-pin
table, source map, current checks, superseded evidence, and remaining limits.
[Third-party notes](THIRD_PARTY_NOTICES.md) distinguish direct license inspection
from the not-yet-completed transitive/shipped-artifact audit. The six inspected
direct runtime license texts remain in `public/third-party-runtime-licenses.txt`
and are copied to the build.
