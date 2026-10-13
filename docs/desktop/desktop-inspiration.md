# Desktop Interaction Inspiration

Dated snapshot of **interaction and UI references** that could inform a later
optional AVA desktop client. This is research, not an AVA layout, visual spec,
color/type token set, or implementation plan. No apps were installed, run, or
accessibility-certified. Published screenshots are **links only**; do not copy
assets, fonts, or trademarks.

**Retrieved:** 2026-10-02 (machine local); 2026-10-03 UTC. Official docs,
product pages, GitHub LICENSE/config files, and published screenshots from that
snapshot.

**Claimed** means vendor documentation, repository text, or what an official
screenshot visibly shows. **Interpretation** is a hypothesis for later review,
not decided UX. Landing-page impression is weaker evidence than app screenshots
or docs.

Jan and Cherry Studio are **open-source interaction/UI references**, not source
to transplant. Linear, Raycast, and Things 3 are **proprietary behavioral
inspiration** only. Apple HIG is **guidance**, not a competitor and not a
library. Coding-agent competitors live in
[competitor research](competitor-research.md); renderer libraries in
[library research](library-research.md).

## Evidence matrix

| Reference | Status / delivery | What official material shows | Transferable lesson (hypothesis) | Caution |
| --- | --- | --- | --- | --- |
| **Jan** | OSS **Apache-2.0**; `src-tauri/tauri.conf.json` verifies **Tauri**. Linux deb/AppImage x64 releases documented. | Product page: friendly, low-information hero. Repo: local/cloud models, assistants, local API, MCP. | First-run should make ready vs needs-setup legible before asking for work. | Source reuse needs license/dependency review. Do not copy branding. Keyboard, focus, menus, reduced-motion **unverified**. |
| **Cherry Studio** | OSS Community Edition **AGPL-3.0**; `electron-builder.yml` verifies **Electron**. Linux desktop documented. | Official screenshots: dark workspace, left nav, feature strip, chat canvas, provider/model controls. Docs: light/dark, message styles, sidebar ordering, composer `+` / `/` palette. | Progressive disclosure at the composer; user-controlled transcript density. | AGPL is not a ban on learning from behavior; **source reuse needs separate review**. Broad “AI workstation” IA is not AVA’s. a11y unverified. |
| **Linear** | **Proprietary**. Web, macOS, Windows, iOS, Android listed. Desktop framework **unverified**. | Sparse sidebar; dense-but-scannable lists in official imagery. Docs: command menu, searchable shortcut help, `/` search, create, Space “Peek.” | Keyboard should accelerate visible work; contextual preview can avoid extra permanent panels. | Behavioral inspiration only. Not Linux-qualified. Not OSS. |
| **Raycast** | **Proprietary**. Site calls it “Native”; macOS and Windows (beta). **No Linux delivery evidenced.** Framework/source unavailable. | Compact search-first launcher. Manual: focus on open, fuzzy search, Enter primary action, action panel, Escape/back. | One default action, searchable secondary actions, visible shortcut hints, reversible navigation. | Do not copy a global launcher or macOS symbols into AVA/Linux. |
| **Things 3** | **Proprietary** Apple-platform product (Mac/iPad/iPhone/Vision in support docs). Framework unverified. | Official product/help: calm lists; Quick Entry; type-to-search; hideable sidebar/toolbar; Move dialog with search. | Content over decoration; a small verb set is fast when selection, search, move, and back are consistent. | Behavioral inspiration only. No Linux app or source. |
| **Apple HIG** | Official Apple guidance. **Not** a Linux UI kit and **not** Apple certification. | Typography hierarchy, layouts that accommodate text-size changes, sized controls, stable focus, Reduce Motion. | Craft criteria: readability, predictability, purposeful feedback, respect for user preference. | Do not present AVA as macOS, copy window-control chrome or SF Symbols, or ship Apple fonts. |
| **Zeron** | User-preferred reference. [Deep dive](zeron-inspiration.md). Pinned commit `1c83cda221bd18a3b55ea49e5b25ec2c148955bc`. Native Rust GPUI, not Electron. | Published screenshot: continuous dark canvas, open assistant prose, subdued user bubbles, quiet tool summaries, one bottom composer, optional adjacent diff pane. | Content-first hierarchy and conversation beside inspectable results. Hypothesis only. | Not a layout, stack, or feature commitment. Screenshot is not current-UI certification. No app was run. Zeron's daemon, sync, browser, and worktree lifecycle are not AVA commitments; see [approved AVA scope](README.md#agreed-desktop-v1-scope) for optional worktrees. |

## User-preferred reference

**Zeron** is the design the user specifically likes. It is not grouped with
the license-reviewed notes below: licensing was excluded for this pass, and
this page states no license. The dated deep dive is
[Zeron inspiration](zeron-inspiration.md) (2026-10-02 local / 2026-10-03 UTC,
commit `1c83cda221bd18a3b55ea49e5b25ec2c148955bc`). A comparative checkout
remains at `docs/reference-code/zeron/` and is outside this catalog. Native
GPUI there is a framework fact, not an AVA stack choice. Electron remains the
accepted host direction. Zeron's daemon, remote sync, harnesses, embedded
browser, and specific worktree architecture/lifecycle are not adopted from the
inspiration. Parallel chats and optional user-enabled Git worktrees were
separately approved; see [canonical AVA scope](README.md#agreed-desktop-v1-scope).

## T3 Code project and thread organization

**User-requested workflow reference; retrieved 2026-10-04.**
[Theo's site](https://t3.gg/) links both T3 Code and T3 Chat.
[T3 Code](https://t3.codes/) is the closest verified match to the described
coding-project/chat organization; this does not assert the user's exact app
identity or that T3 Chat ships the same organization.

The [README at commit `4ee6bfd50ef4a089440d5c3662db2298da9cc50e`](https://github.com/pingdotgg/t3code/blob/4ee6bfd50ef4a089440d5c3662db2298da9cc50e/README.md)
identifies desktop, web, and mobile clients. Current public source supports:

- Project-scoped threads with an **All projects / selected project** filter.
  Logical project groups can contain multiple project references; this is not
  evidence for an arbitrary nested folder tree.
- Distinct conversation rows, pinning, and search; lifecycle shelves include
  active, snoozed, and settled threads, with an optional Working shelf.
- Thread-row state for working, waiting, approval, and input, plus a working
  duration. Background attention can remain discoverable when another chat
  is selected.

Sources: pinned [Sidebar.tsx](https://github.com/pingdotgg/t3code/blob/4ee6bfd50ef4a089440d5c3662db2298da9cc50e/apps/web/src/components/Sidebar.tsx)
and [Sidebar.logic.ts](https://github.com/pingdotgg/t3code/blob/4ee6bfd50ef4a089440d5c3662db2298da9cc50e/apps/web/src/components/Sidebar.logic.ts).
This is source evidence, not a running-app or accessibility certification.
Exact inactive-thread stop/cancel behavior was not established. No account,
app, installer, or provider was used; direct T3 Chat retrieval was rate-limited.

**For later AVA review:** project grouping and thread retrieval are separate
from independent run execution. Parallel chats are an
[agreed v1 requirement](README.md#agreed-desktop-v1-scope); the decided
same-project shared-copy and optional user-enabled worktree policy is recorded
in that canonical scope. Exact hierarchy, project identity, and optional
pin/search/archive features remain open. The per-process
[RPC admission boundary](architecture.md#one-active-run-and-interleaving)
still applies. No T3 server, sync, harness, or source-control architecture is
selected for AVA, and no new visual design is specified here.

## Open-source interaction references

### Jan

[janhq/jan](https://github.com/janhq/jan)
[LICENSE](https://github.com/janhq/jan/blob/main/LICENSE) Apache-2.0.
[src-tauri/tauri.conf.json](https://github.com/janhq/jan/blob/main/src-tauri/tauri.conf.json)
verifies the desktop shell is Tauri. [Releases](https://github.com/janhq/jan/releases)
document Linux amd64 deb and AppImage. Site: [jan.ai](https://jan.ai/).

**Claimed.** Open-source ChatGPT alternative: local models, cloud providers,
assistants, localhost OpenAI-compatible API, MCP. Current build instructions
require Rust for Tauri. The landing page uses a single large proposition, a
primary download action, substantial empty space, and limited top navigation.

**Interpretation.** Spatial restraint on the **marketing** page is not an AVA
aesthetic brief, and the page is playful/high-chroma — not recommended as look.
The useful hypothesis is onboarding posture: explain what is ready and what
needs setup. Retrieved source and pages did **not** establish Jan’s keyboard
map, focus management, native menus, dark/light behavior, or reduced-motion
support.

### Cherry Studio

[CherryHQ/cherry-studio](https://github.com/CherryHQ/cherry-studio)
[LICENSE](https://github.com/CherryHQ/cherry-studio/blob/main/LICENSE) AGPL-3.0.
[electron-builder.yml](https://github.com/CherryHQ/cherry-studio/blob/main/electron-builder.yml)
verifies Electron packaging. Docs: [docs.cherry-ai.com](https://docs.cherry-ai.com/docs/en-us/);
chat preview: [preview/chat](https://docs.cherry-ai.com/cherry-studio/preview/chat).

**Claimed.** Cross-platform desktop client. Docs describe custom providers,
assistant/topic grouping, global search, configurable sidebar, light/dark,
transcript bubble/simple layouts, collapsible code/thinking content. Composer
model in docs: a few common tools shown; others behind `+`; `/` opens the same
panel; arrows navigate; Tab/Enter confirm; Escape closes. Official screenshots
place navigation at the periphery with a large central work area.

**Interpretation.** Strongest sourced pattern is **capability at point of need**.
Transcript collapse acknowledges generated-material density. Warning: a broad
product accumulates modes; AVA should make backend/provider **status**
discoverable without granting the renderer authority over the C++ JSONL-RPC
backend ([architecture](architecture.md#process-ownership)). Screen-reader
semantics, keyboard focus quality, and reduced-motion were **not** verified.
AGPL does not forbid studying behavior; copying or linking Cherry source needs
its own legal review.

Source licenses and proprietary branding are separate. Apache-2.0 / AGPL-3.0
govern **source**. Product names, screenshots, and trademarks are not a font or
asset kit.

## Proprietary behavioral references

### Linear

[Download](https://linear.app/download). Docs:
[joining](https://linear.app/docs/joining-your-team-on-linear);
[keyboard shortcuts help](https://linear.app/changelog/2021-03-25-keyboard-shortcuts-help).

**Claimed.** Streamlined, command menu (`Cmd/Ctrl-K`), searchable shortcut
help, Space preview. Changelog records accessibility fixes (button semantics,
assistive-technology labels). That is maintenance attention, **not** a claim
of complete accessibility, and this research did not run Linear.

**Hypothesis.** A command palette should **supplement** visible controls, not
become the only way to discover critical actions (permission, cancel, session).

### Raycast

[raycast.com](https://www.raycast.com/). Manual:
[search bar](https://manual.raycast.com/search-bar),
[action panel](https://manual.raycast.com/action-panel),
[keyboard shortcuts](https://manual.raycast.com/keyboard-shortcuts).

**Claimed.** Focus search on open; arrows move selection; Enter primary action;
action panel for contextual verbs; typing filters them; Escape closes/backtracks;
destructive actions can be labeled. Compact mode: input-only when idle.

**Hypothesis.** Not “make a launcher.” Define one keyboard contract for lists,
menus, dialogs, and transient panes, then expose it. Linux shortcut collisions
and compositor behavior need later research on AVA’s qualified Linux x64
desktops. No Linux Raycast delivery was evidenced.

### Things 3

[culturedcode.com/things](https://culturedcode.com/things/);
support [keyboard](https://culturedcode.com/things/support/articles/2785159/).

**Claimed.** Quick Entry, keyboard creation/movement, type-to-search, searchable
Move dialog, new windows, sidebar/toolbar visibility toggles. Official material
is content-forward.

**Hypothesis.** A selected item should make its next action obvious; infrequent
configuration stays nearby but quiet. Aesthetic reputation is interpretation,
not measured running behavior.

### Apple HIG (guidance)

[Typography](https://developer.apple.com/design/human-interface-guidelines/typography);
[Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility);
[Focus and selection](https://developer.apple.com/design/human-interface-guidelines/focus-and-selection).

Apple calls for legible hierarchy, layouts that accommodate text-size changes,
adequately sized controls, focus that does not move without user interaction,
and reduced automatic/repetitive animation under Reduce Motion.

These are **craft criteria**, not a Linux widget kit and not a claim that AVA
is Apple-certified. “Apple-inspired” here means coherent hierarchy, quiet
surfaces, predictable focus, restrained motion, and fast keyboard operation —
not imitation macOS chrome.

Do **not** redistribute or embed **SF Pro, SF Mono, or SF Symbols**. Apple’s
font license as reviewed for this research restricts SF files to mock-ups of
Apple OS UIs by registered Apple Developers, with no embed in programs and no
mock-ups for non-Apple OS. AVA on Linux should plan on system or separately
licensed type and original iconography. That is a prudent alternative from the
reviewed license text, not a broader legal opinion.

## Candidate patterns (hypotheses only)

Not decided UX. Each would need later design review after this research is
read, then validation on Linux x64 — not a layout.

1. **Status as information.** Compact, persistent connection/readiness,
   execution context, and recoverable failure wording that stays truthful to
   RPC lifecycle ([architecture](architecture.md)). Renderer displays and
   requests; backend remains authority.
2. **One keyboard grammar.** Enter primary; Escape closes/returns; arrows move
   selection; a discoverable action menu for secondary verbs. Must not hide
   permission, cancel, or question behind an undiscoverable palette.
3. **Progressive composer/transcript detail.** Common actions inspectable;
   attachments, tool/diagnostic detail, and advanced choices disclosed
   contextually and collapsible without hiding the settled result. Maps to
   existing tool timeline / reasoning events, not a new event protocol.
4. **Adaptive concentration.** Collapsible panes and remembered local
   preferences may help small Linux displays if keyboard access and minimum
   content space remain. Not a mockup.
5. **Readable long-form output.** Role/time/status hierarchy; code/structured
   output with copy/expand; no reliance on bubble decoration alone.
6. **Linux without faux macOS.** Let the window manager own normal chrome
   unless a later platform study shows a compelling, accessible alternative.
   Freely licensed or system type; original icons.

## Open questions

- Which Electron window, menu, and shortcut behaviors are consistent on AVA’s
  qualified Linux x64 desktops without global-shortcut conflicts?
- Which JSONL-RPC lifecycle, error, and cancellation states must stay visible
  so status wording cannot contradict
  [RPC](../rpc-protocol.md) or fail-closed resolvers?
- What minimum renderer accessibility baseline can later be **verified**
  (keyboard-only, semantics, visible focus, zoom/text scaling, contrast,
  `prefers-reduced-motion`, dark/light) without claiming HIG or WCAG
  certification here?
- Which transcript/tool-result structures are session-persistent (RPC/session
  truth) versus ephemeral UI disclosure?
- What proprietary-free icon/font strategy stays legible on Linux without SF
  assets?

Design waits until the user reviews this research. No layout or visual spec is
implied.

## Source ledger

Retrieved 2026-10-02 local / 2026-10-03 UTC.

1. https://github.com/janhq/jan
2. https://github.com/janhq/jan/blob/main/LICENSE
3. https://github.com/janhq/jan/blob/main/src-tauri/tauri.conf.json
4. https://github.com/janhq/jan/releases
5. https://jan.ai/
6. https://github.com/CherryHQ/cherry-studio
7. https://github.com/CherryHQ/cherry-studio/blob/main/LICENSE
8. https://github.com/CherryHQ/cherry-studio/blob/main/electron-builder.yml
9. https://github.com/CherryHQ/cherry-studio/releases
10. https://docs.cherry-ai.com/docs/en-us/
11. https://docs.cherry-ai.com/cherry-studio/preview/chat
12. https://linear.app/download
13. https://linear.app/docs/joining-your-team-on-linear
14. https://linear.app/changelog/2021-03-25-keyboard-shortcuts-help
15. https://www.raycast.com/
16. https://manual.raycast.com/search-bar
17. https://manual.raycast.com/action-panel
18. https://manual.raycast.com/keyboard-shortcuts
19. https://culturedcode.com/things/
20. https://culturedcode.com/things/support/articles/2785159/
21. https://developer.apple.com/design/human-interface-guidelines/typography
22. https://developer.apple.com/design/human-interface-guidelines/accessibility
23. https://developer.apple.com/design/human-interface-guidelines/focus-and-selection
24. https://developer.apple.com/fonts/
