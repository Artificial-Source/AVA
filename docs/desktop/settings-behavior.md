# Settings behavior and local previews

## Current browser-only scope — revision 15

The normal app at **`http://127.0.0.1:5182/`** has five working Settings bodies:
**General, Shortcuts, Conversation behavior, Notifications & sounds, Context
management**. Navigation has **16 individually searchable pages**. Providers stays
top-level; Resources stays four separate pages; Connections stays MCP/LSP only.
[Appearance](appearance-customization.md) and
[provider/resource/tool refinement](provider-resource-chat-refinement.md) remain
available. Earlier “current” counts and entry-size records are preserved history.

Astra MAX delivered the implementation and named native-test correction. One
integrated **GPT-6.1 Sol MAX** source/actual-image gate is complete:
**No material findings.** Changes apply immediately and survive Back/reopen for
this visit. Refresh resets preferences and local edits; nothing is persisted or
written to AVA configuration.

### Working controls

| Page | Actual behavior |
| --- | --- |
| **General** | Enter/Native newline policy, with Shift+Enter, Ctrl/Cmd+Enter and composition handling. Future-chat Build/Plan, Current/supplied project, Remember/Agents/Changes/Context/Closed inspector, and optional first-message naming. Naming is off by default, snapshotted at creation, one-shot, whitespace-normalized and bounded to 60 Unicode code points. Fixtures/existing chats are not renamed. Model defaults remain separately owned. |
| **Shortcuts** | Ten grouped searchable real actions; effective/default/custom/unassigned bindings, press recording, Escape/cancel focus, reserved/conflict explanations, unassign and row/all reset. Recorded keys reach Settings, New chat, parent model/composer/tools, inspector tabs/visibility or explicitly bound Stop of the viewed Working mock. General owns Enter. Native editors, composition/AltGraph/repeats, menus/dialogs, hidden/stale/departed targets and read-only child restrictions remain protected. |
| **Conversation behavior** | Initial collapsed/expanded tool defaults; explicit group/global choices win. Same-ID replay retains choices; removed/reintroduced groups inherit current defaults. Successful output text can be hidden without hiding errors, lifecycle or file-change facts. Code wrapping defaults off; diff wrapping defaults on, including supplied inline diffs and independent Changes. Actual local Send can scroll to the new message or preserve reading position. The sample is illustrative, not a run. |
| **Notifications & sounds** | Local policy/test cards only: completion/input/failure channels, master mute, background-only simulation, None/Soft tone/Chime and 0–100% volume. Test event is silent. Only an accepted trusted Play click constructs Web Audio: one sine voice, 0.36s, maximum gain 0.04, with stop/disconnect/close and stale/late/navigation cleanup. Absent/blocked APIs are explicit. **No OS notifications, permission requests, autoplay, live subscriptions or network audio.** |
| **Context management** | Policy preview only: automatic eligibility, 1–95% trigger, 0–20 recent turns and 0–100,000 token retention budget. Projection uses supplied matching/current/safe context facts: zero remains zero; unknown/mismatched/stale usage stays unknown. Aster’s 80% trigger is 171,200 against 173,120 supplied tokens. Invalid drafts explain bounds while retaining the last accepted projection. **No Compact now, context/history removal, cost reconstruction or config/provider action.** |

App bindings deliberately use a conservative browser/OS-safe set:
**Ctrl/Cmd+Shift** with **1, 2, 6–9, 0, comma or period**. Letters, ordinary typing,
unshifted tab numbers, Alt/AltGraph, bare Cmd-comma and screenshot keys 3–5 stay
reserved. Defaults: Settings/comma, New chat/9, Models/6, Composer/7, Tools/8,
Agents/1, Changes/2, Context/0. Stop and inspector visibility start unassigned.
These are frontend bindings, not AVA’s TUI keybinding configuration.

Each page has its own immediate reset. **Reset appearance** leaves behavior
preferences alone. Confirmed **Reset preferences** restores all defaults and the
future-chat model while preserving existing chats/models/messages/drafts/references,
pins/project collapse and user definition metadata. Standalone screen review owns
independent values and resets. Mounted Settings retains tool expansion; inspecting
a child still unmounts the parent’s local disclosure state, as before this task.

### Ownership and bounded loading

Optional readonly preference bundles are visit-local presentation data, not backend
policy. AppShell/reducer own actual new-chat/first-send effects. Changes, recording,
tests and resets retain current view/root/overlay/latest authority. Scoped inherited
code/diff attributes change presentation, not supplied facts, syntax/status colors
or accounting. Global/token styles, Composer CSS, protected modal F03, provider
assets/notices, dependencies/configuration, backend/legacy and history are unchanged.

The complete optional **AppCommands** owner loads on keyboard activation. Captured
focus/view identity is rechecked after loading; import failure leaves visible
controls available and late resolution cannot target a departed chat. Activation,
failure and late-target regressions cover this boundary. No arbitrary widget
splitting, `manualChunks`, dependency/config or cap change. Entry:
**499,991 raw / 153,028 Node-default gzip bytes**, **9 below** the unchanged
500,000-byte cap. Future root changes must remeasure this very small headroom.

## Independent verification — 2026-10-11

Main independently passed:

- **159 nearest tests / 13 files**, typecheck, **605 full tests / 44 files**, four
  workers; warning-free **493-module build**. No timeout/config/guard weakening.
- **210 native browser checks**: 24 keyboard, 31 behavior/context/reset, 17 audio,
  105 layout/native reset, 27 navigation/preservation/Tab and six request/catalog.
  **138 scenarios / 14 families** remain independently reachable.
- All five requested pages at **1440×960, 850×820, 520×850, 1440×560, 3440×1440**.
  All 25 page/layout combinations had Reset fully visible, enabled, hit-tested and
  natively clicked. Chat’s 44px-wide rail starts at y=0; sidebar/inspector and
  ≤780px reading/composer caps remain. Back/reset stay ordinary-flow and
  scroll-reachable; no sticky footer or global focus trap.
- Trusted native Play/Stop clicks scheduled a real bounded Web Audio oscillator
  and closed its context, respecting policy silence. A controlled deferred-resume
  probe held a real native resource, navigated away, then released the promise:
  no late voice or departed-page mutation. **Hearing/audio quality is not claimed.**
- **18 finite painted-text states**, minimum **5.405:1**: five pages, conflict/
  reserved/invalid/zero/suppressed states, palettes, higher contrast and narrow
  chat. Sampling compares normal and transparent-text QA paint at glyph pixels;
  decorative native focus-outline pixels inside a typographic range box are not
  mislabeled as text background. Icons/native values/placeholders, hidden/clipped/
  disabled and unsampled states excluded; no all-state WCAG/AT/GPU claim.
- Empty ordinary console/page errors; **410 requests / 51 unique URLs**, zero
  external HTTP. Fonts/logos and preview resources remain local/loopback.

**SETTINGS15-F01 fixed:** correcting invalid 96 back to accepted 90 initially
consumed a guard claim without changing its JSON scope, blocking later edits.
A no-op now neither claims nor forwards. Two regressions fail before/pass after;
native correction followed by zero-turn/zero-token edits updates the actual
projection. Invalid-default reset remains editable too. No numeric/controller/
guard-hook rewrite; closed older F03/M01/provider-hover findings stay protected.

Sol inspected all **42 actual changed files** and **27 actual PNGs** (one before,
26 fresh), independently verified by native model/image-tool metadata. Exact
verdict: **No material findings.** Its supplied native evidence covered the initial
183 checks; Main’s additional 27 navigation/preservation/Tab checks passed without
source changes. MAX was commissioned; transcripts expose native model/tool facts,
not wire-level reasoning effort. No further broad review wave was added.

## Evidence and preservation

[26 current captures](concepts/react-settings-behavior/) include
[General](concepts/react-settings-behavior/ava-react-general-body-1440x960.png),
[shortcut conflicts](concepts/react-settings-behavior/ava-react-shortcuts-conflict-current-1440.png),
[narrow reading controls](concepts/react-settings-behavior/ava-react-conversation-reset-visible-520x850.png),
[finished sound preview](concepts/react-settings-behavior/ava-react-notifications-sound-finished-1440.png),
[fixed zero retention](concepts/react-settings-behavior/ava-react-context-correction-zero-current-1440.png)
and [unknown usage](concepts/react-settings-behavior/ava-react-context-unknown-orion-1440.png).
Only reviewed current images are published; before/finding/diagnostic images stay
comparison evidence. Older revision folders remain unchanged.

Acceptance artifacts: `/tmp/ava-settings-behavior-baseline-cpb4q27b/` contains the
reviewed map/API evidence, 1,880-file baseline/142 before copies/Git snapshots,
source/worker reports, named F01 fail-before evidence, Main tests/native/audio/
layout/contrast/network, model/image metadata, findings ledger and final audits.

Final preservation: **1,880 → 1,928 files**, **26 modified / 48 added / zero deleted**,
**1,854 unchanged**, **1,748 frozen hashes verified**. Additions are 21 source/test
files, this document and 26 PNGs. Full older documentation suffixes, backend/legacy,
dependencies/config/global styles/highlighter/Composer CSS/F03/assets/licenses/
notices and HEAD/staged/unrelated tracked-unstaged changes are preserved.
Documentation links/structure/verifier tests and whitespace pass. No source copied
from competitors; no install/stage/commit/reset/stash/worktree or unrelated change.

Persistence, unrestricted/host keymaps, actual notifications/subscriptions,
compaction, provider/backend/native-host integration, production accessibility,
packaging/performance/legal and independent-chat routing remain separately scoped
work, not implied by this browser-preview acceptance.
