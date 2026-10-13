# Settings scroll layout

## Revision 17: quieter Settings navigation

The left Settings sections list no longer shows its scrollbar or global reset
button. Native overflow remains: trusted wheel, keyboard scrolling and automatic
focus reveal still work. Right content and the ≤1100px combined stack retain their
scrollbar behavior; the persistent header and 16-page grouping/search stay intact.

**Reset all preferences…** is now a quiet, link-like native action at the bottom of
**General**, after and distinct from **Reset General**. It opens the same native
confirmation; Cancel/Confirm returns to the moved action. Global scope remains
preferences/future-chat model only, preserving existing current chats/models/
drafts/mode and user definitions. Normal and independent standalone review share
placement. No new menu, notes, preference/backend/persistence or modal-focus owner.

Independent verification — **2026-10-12**:

- Astra MAX changed two production files and six affected tests. Main inspected
  the actual patch/hashes and independently passed **85 nearest tests / 8 files**,
  typecheck, **614 full tests / 45 files**, four workers, warning-free **493 modules**.
  Three new regressions cover scoped scrollbar overflow, semantic/keyboard/footer
  placement and hidden/inert/inactive/stale/unmounted callback rejection. Existing
  appearance/definition/reset and closed F01/scroll/focus coverage remains.
- **44 native Chrome checks** at **1440×560, 1440×960, 850×820, 520×850, 3440×1440**:
  zero left scrollbar gutter while genuinely overflowing, scoped scrollbar-width/
  WebKit suppression, independent trusted wheel/PageDown/Tab, right/stack bars,
  16 leaves/search, General-only footer/hover/native keyboard/Cancel/Confirm/focus,
  normal and standalone placement. Different current Orion/future Aster Mini and
  original draft/mode prove global reset restores future Aster/Enter defaults
  without replacing current chat facts. A native radio-readiness probe was fixed
  in QA with a precise input wait/quoted native click, not a product workaround.
- **Three finite painted-glyph states**, minimum **9.287:1**, cover normal/hover/
  keyboard-focused reset text; actual normal/transparent-text screenshot sampling,
  not computed colors alone. Empty ordinary console/page errors; **180 requests /
  35 unique URLs**, zero external HTTP. Native input uses the same owned normally
  sandboxed Chrome and previously verified coordinate-bearing CDP wheel bridge.
  No synthetic DOM event/offset assignment substitutes for native wheel.
- The sole **GPT-6.1 Sol MAX** integrated gate read all **eight actual changed files**
  and **nine actual PNGs** (one before/eight fresh): **No material findings**.
  Main independently verified native model metadata and nine successful unique
  required image-tool reads. MAX commissioned; wire effort is not exposed.
- Entry **499,991 raw / 153,020 Node-default gzip bytes**, unchanged **500,000 cap /
  9-byte headroom**. No source correction after worker delivery, extra review,
  dependencies/config/chunking/cap tricks or protected finding reopened.

[Eight reviewed current captures](concepts/react-settings-nav-polish/) include
[hidden-bar scrolling](concepts/react-settings-nav-polish/ava-react-nav-scrolled-no-bar-1440x560.png),
[quiet footer hover](concepts/react-settings-nav-polish/ava-react-general-reset-hover-1440x560.png),
[keyboard focus](concepts/react-settings-nav-polish/ava-react-general-reset-keyboard-focus-1440x560.png),
[unchanged confirmation](concepts/react-settings-nav-polish/ava-react-reset-confirmation-1440x560.png),
[narrow footer](concepts/react-settings-nav-polish/ava-react-general-reset-520x850.png)
and [standalone footer](concepts/react-settings-nav-polish/ava-react-standalone-general-reset-520x850.png).
Before/diagnostic frames stay comparison evidence; all older captures remain.

Acceptance artifacts: `/tmp/ava-settings-nav-polish-baseline-tpl02ooi/` holds the
approved map, 1,950-file baseline/160 before copies/Git snapshots, source/worker/
Main reports, actual native/glyph/network evidence, model/image metadata, ledger
and final audits. Final preservation: **1,950 → 1,958 files**, **14 modified / eight
added / zero deleted**, **1,936 unchanged/frozen hashes verified**. Eight source/
test changes plus six documentation prepends; additions are eight reviewed PNGs.
Complete older document bodies, assets/licenses/notices/dependencies/config/global/
tokens/highlighter/Composer/F03/backend/legacy and HEAD/staged/unrelated tracked-
unstaged state remain. Documentation links/structure/verifier tests/whitespace pass.
No installs/stage/commit/reset/stash/worktrees or unrelated changes. Browser-preview
acceptance does not qualify all-browser/assistive technology/GPU/performance/native/
backend/packaging/legal behavior. The complete revision-16 body below is history.

## Current browser-only layout — revision 16

At **http://127.0.0.1:5182/**, desktop Settings keeps its header and **Back to chat**
visible. Left navigation and right content are separate native scroll panes:
wheel input affects the pane under the pointer, including at its boundary.
Navigation scrolls only when its 16-page list exceeds the available height.
The header is outside the scrollers, not an overlay covering controls.

At the existing **≤1100px** breakpoint, wrapped navigation and content share one
vertical stack under the persistent header. There are no nested vertical pane
scrollers. A new section starts that stack at search/navigation; it does not
skip navigation or steal focus. Keyboard-focusable named panes support native
Tab, arrows and PageDown. Ordinary edits/search/reset/overlay close preserve
reading position; section changes alone restart content. Focus cues stay inside
clipped pane edges.

This is a layout follow-up to [five working Settings pages](settings-behavior.md),
[Appearance](appearance-customization.md) and
[provider/resource/tool refinement](provider-resource-chat-refinement.md).
All 16 leaves, groups, immediate page resets and confirmed global reset remain.
Draft/current model/mode/title/mounted transcript/tool choices survive Back.
Model-only modal geometry and independent standalone review ownership are retained.
Preferences remain visit-local; no persistence/backend/native authority is added.

## Independent validation — 2026-10-11

Astra MAX implemented five production-file changes and one added test.
Main independently passed:

- **77 nearest tests / 8 files**, typecheck, **611 full tests / 45 files**, four
  workers; warning-free **493-module build**. Six added seam regressions fail
  against preceding source and pass with this layout. DOM offset tests are not
  browser geometry evidence.
- **324 native Chrome checks**: 19 independent wheel/keyboard, 214 layout/search/
  16-page checks, 69 navigation/preservation including 32 native Tab observations,
  13 standalone/catalog and 9 deep-overlay/reset/resize checks. Native page resets
  were visible, enabled, hit-tested and clicked in **30 page/layout states**
  (Appearance plus the five behavior pages).
- **1440×960, 1440×560, 850×820, 520×850, 3440×1440**. Both desktop offsets and header
  rectangles were measured; wheel at navigation's end never moved content/page.
  Narrow mode has one native stacked scroller. Section starts, real edit/search
  offsets, overlay Cancel/confirmed reset receipt, Back, model-only dialogs and
  standalone Settings/non-Settings family/catalog were exercised. Chat's 44px
  rail at y=0 remains.
- Empty ordinary console/page errors; **417 recorded requests / 43 unique URLs**,
  zero external HTTP. Existing color/font tokens and authority/focus owners remain
  unchanged. No new all-state contrast/accessibility/GPU/native qualification.

The installed browser CLI's wheel command was instrumented sending `(0,0)` despite
prior pointer movement. QA therefore sends native, coordinate-bearing CDP wheel
input to the same normally sandboxed, loopback-only, owned browser. Actual
`isTrusted` events and pane offsets are recorded; no DOM wheel dispatch or offset
assignment stands in for native wheel. Fixture-text, lazy tool-detail readiness
and native locator/auto-modal assumptions were corrected in QA only; no product
workaround or waived check.

The sole integrated **GPT-6.1 Sol MAX** actual-source/actual-PNG gate returned
**No material findings.** All six actual changed files and **21 actual PNGs**
(one before, 20 fresh) were inspected. Main independently verified native model
metadata and all 21 successful unique required image-tool reads. MAX was
commissioned; wire-level reasoning effort is not exposed. No extra broad review
or corrective source wave was needed.

## Evidence and preservation

Entry remains **499,991 raw bytes / 153,022 Node-default gzip**, unchanged
**500,000-byte** cap and **9-byte** headroom. No dependencies/config/chunking/cap
tricks. Closed F03/model-footer/provider-hover/preference-no-op findings remain
protected; no wheel handlers, focus trap, observer, timer, sticky action footer
or new feature.

[20 reviewed current captures](concepts/react-settings-scroll/) include
[right-pane scrolling](concepts/react-settings-scroll/ava-react-shortcuts-content-scroll-1440x560.png),
[independent navigation](concepts/react-settings-scroll/ava-react-shortcuts-nav-bottom-1440x560.png),
[keyboard focus](concepts/react-settings-scroll/ava-react-content-keyboard-focus-1440x560.png),
[narrow navigation](concepts/react-settings-scroll/ava-react-general-top-520x850.png),
[narrow reset](concepts/react-settings-scroll/ava-react-appearance-reset-520x850.png),
[deep reset receipt](concepts/react-settings-scroll/ava-react-reset-receipt-both-scroll-1440x560.png)
and [standalone review](concepts/react-settings-scroll/ava-react-standalone-settings-scroll-520x850.png).
Before and diagnostic frames stay comparison evidence. Earlier folders and complete
older documentation remain untouched.

Temporary acceptance artifacts: `/tmp/ava-settings-scroll-baseline-xmufia74/`
contains the approved map, 1,928-file baseline/158 before copies/Git snapshots,
actual source patch/hash manifest, worker/Main checks, native wheel diagnosis/
bridge, layout/focus/preservation/network results, images, finding ledger and
final audits.

Final preservation: **1,928 → 1,950 files**, **10 modified / 22 added / zero deleted**,
**1,918 unchanged and frozen hashes verified**. Additions are one test, this document
and 20 PNGs. Five source files and five current documentation prepends changed;
complete older document bodies, HEAD/staged/unrelated tracked-unstaged changes,
assets/licenses/notices/backend/legacy/dependencies/config/global/token/highlighter/
Composer/F03 remain. Documentation links/structure/verifier tests and whitespace
pass. No installs/stage/commit/reset/stash/worktrees or unrelated changes.

Acceptance is browser-preview scope, not Electron/native/backend/assistive-
technology/performance/packaging/legal qualification.
