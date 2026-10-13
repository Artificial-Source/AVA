# Appearance customization

## Current browser-only scope — revision 14

**Settings → Appearance** now has working customization, implemented with **Astra
MAX** and accepted by one integrated **GPT-6.1 Sol MAX** source/actual-visual gate:
**No material findings.** Open the normal app at **`http://127.0.0.1:5182/`**.

Changes apply immediately for this visit and survive Back/reopen. Refresh restores
defaults; there is no configuration, storage or backend persistence. This
supersedes dated appearance wording in [revision-13 history](provider-resource-chat-refinement.md),
not its other features or preserved captures.

### Available controls

| Control | Choices and actual effect |
| --- | --- |
| Dark palette | **Default / Charcoal / Midnight** across canvas, raised panels, user bubble, sidebar, inspector and rail. Default preserves the existing neutral dark appearance. |
| Interface font | Local **Geist / System sans**; code retains its existing monospace stack. No font download, service or path input. |
| Conversation text | **14 / 15 / 17px** parent and read-only child prose. Fixed-size UI labels are not globally scaled. |
| Line spacing | **1.5 / 1.7 / 1.9** prose spacing; default remains **15px / 25.5px**. |
| Reading width | **620 / 700 / 780px** maximum, shared by transcript, title and composer; smaller viewports clamp to available space. |
| Contrast | **Follow system / Higher contrast**. Higher contrast strengthens text/edges and makes glass opaque. System higher contrast, reduced transparency and unsupported blur take precedence. |
| Sidebar material | **Glass / Opaque**, synchronized with the existing sidebar control through one canonical reducer boolean. |
| Motion | **Follow system / Reduce motion**, now across chat, sidebar drift, status rotation, inspectors, disclosures and dialogs. OS reduction always wins. |
| Settings-row density | **Comfortable / Compact**, explicitly Settings rows only; prose spacing is separate. |

Colors, Reading and Surfaces & motion are grouped with an actual local sidebar/
reading sample. Native named controls validate fixed enums. Search finds palette,
font, prose, spacing, width, contrast and glass terms. No arbitrary CSS, external
URLs, custom-font loading, unsafe color inputs, Light theme or System theme
switching is claimed. Status palette/semantics, model glyphs, syntax and accounting
are unchanged.

**Reset appearance** changes only visual fields, density, material and motion. It
preserves Enter behavior, current/child/new-chat-default models, user-defined
Subagent metadata and chats/drafts/references. Existing confirmed **Reset
preferences** retains its former new-chat-default scope and restores new visuals
too; existing chats and child facts stay intact.

Mounted Settings retains parent expansion/sidebar/inspector state and restores
visible opener focus. As before this task, inspecting a child unmounts the parent's
local Conversation disclosure state; cross-child expansion persistence is not
added or promised. Standalone screen review has independent preferences and its
original opaque baseline outside Appearance; its appearance reset restores opaque,
while normal-app reset restores glass.

### Ownership, motion and entry size

`Preferences` adds optional readonly presentation enums, not backend config or
model authority. Absent values preserve defaults. AppShell owns visit-local state;
view/host/request guards reject inactive, stale, duplicate, closed and nested
control intents. Existing reducer `opaque` remains canonical; explicit
`setOpaque(boolean)` is validated and idempotent. Static scoped CSS/data attributes
apply effects without arbitrary inline styles or global document mutations.

Manual reduction is a scoped React context OR'ed with the existing system
external-store hook; both hooks run unconditionally. CSS stops movement and logical
Presence exits disappear immediately, not merely visually. Default Working remains
an icon-only **2.4s** rotation. No new animation, glow, shimmer, focus trap or watcher.
Global tokens/styles, Composer CSS and protected F03 remain byte-identical.

The first integrated build exceeded the unchanged 500,000-byte entry cap. A genuine
whole optional **read-only ChildConversation** body now loads on Inspect. Root
identity/title focus/Back/model restrictions remain immediate. Loading and
contained import failure retain Back and the parent draft; late completion cannot
revive a departed child. No arbitrary widget fragmentation, `manualChunks`,
threshold, package/config change or cap inflation.

Final entry: **498,306 raw bytes / 152,305 Node-default gzip**, **1,694 bytes below**
the cap, down from 499,990 raw bytes. The 3.15kB optional child chunk is deferred
ownership, not removed functionality.

## Independent verification — 2026-10-11

Main inspected actual before-copy/source/hash deltas and passed:

- **194 nearest tests / 16 files**, typecheck, **480 full tests / 34 files** (27.28s,
  four workers), warning-free **482-module build**. Default 34-isolated-worker
  attempts had resource/loading timeouts; bounded verification passes without
  changed timeouts, configuration, F03 or product behavior.
- **98 native checks**: 37 normal-app, 25 layout/modal, 10 preservation/model/
  definition/child, 22 independent-review/search/Tab and four actual narrow/short
  Reset visibility/action checks.
- **135 existing scenarios / 14 families**, six explicit request/catalog checks;
  no invented scenario increase or assistant turn.
- **1440×960, 850×820, 520×850, 1440×560 and 3440×1440**, with actual 17px/1.9/
  system-font/higher-contrast rendering, fixed 44px rail at y=0, no horizontal
  overflow, native modal/Back/Reset and readonly-child restrictions. Actions stay
  ordinary-flow and scroll-reachable.
- A separate owned probe deliberately blocked only the local child chunk; its
  real error pane stayed read-only and native Back recovered the parent. Expected
  probe errors are isolated, not ordinary app-console failures.
- **16 finite painted-text states**, minimum **4.819:1** (normal charcoal chat),
  including presets, higher contrast, narrow/readability, Reset hover and model
  states. Icons, native popup values/placeholders, hidden/clipped/disabled and
  unsampled states excluded; no all-state WCAG/AT/GPU certification.
- Ordinary console/page errors empty; **310 requests / 44 unique URLs**, all HTTP
  loopback. Existing logos/fonts stay local; no provider/backend invocation.

Sol actually read **18 images** (one before, 17 fresh), confirmed by native model
and image-tool metadata; exact verdict **No material findings.** MAX was commissioned
for source/review; wire-level effort is not exposed by worker transcripts. Closed
PROVIDER13-F01, NORMAL12-M01 and SCREEN11-F03 remain protected.

One early ultrawide test clicked a partially visible Back control's offscreen
center. Instrumentation proved zero click events, then one native event/dismissal
after explicit visible scroll. This was a harness assumption, not an app defect;
no sticky footer or focus rewrite. Other label/default-draft query assumptions
remain diagnostic artifacts, not manufactured findings.

## Current evidence and preservation

[18 current captures](concepts/react-appearance/) include
[Appearance](concepts/react-appearance/ava-react-appearance-default-1440.png),
[narrow controls](concepts/react-appearance/ava-react-appearance-bottom-520x850.png),
[short Reset](concepts/react-appearance/ava-react-appearance-reset-visible-1440x560.png),
[customized chat](concepts/react-appearance/ava-react-chat-custom-520x850.png),
[readonly child](concepts/react-appearance/ava-react-child-custom-1440.png) and
[contained load error](concepts/react-appearance/ava-react-child-load-error-1440.png).
Before images remain comparison only; older revision folders are unchanged.

Acceptance artifacts: `/tmp/ava-appearance-baseline-18ajooxs/` holds the reviewed
map/brief, 1,853-file baseline, 130 before copies, Git snapshots, source patch/report,
Main tests/native/contrast/network, child-error probe, native model/image evidence,
review ledger and final documentation/preservation checks.

Final preservation: **1,853 → 1,880 files**, **27 modified / 27 added / zero deleted**,
**1,826 unchanged**. Additions include eight source/test files, this document and
18 PNGs. Full older README/prototype suffixes and existing backend/legacy/
dependencies/config/highlighter/global tokens/Composer CSS/F03/logos/licenses/
notices/historical screenshots, HEAD/staged/unrelated tracked-unstaged changes
are preserved. Documentation links, structure, verifier tests and whitespace pass.
No worktree, install, stage, commit, reset or stash.

Production/native accessibility, durable preferences, Light/System themes,
packaging/live transport/legal qualification and arbitrary theme import/export
remain separate work, not implied by this browser-preview acceptance.
