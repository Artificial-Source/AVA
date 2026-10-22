# Critique refinement

Revision 21 · browser-only, visit-local UI prototype.

## Scope and user decisions

An Impeccable critique scored every screen and the whole product; a separate
technical audit covered accessibility, performance and responsive behavior. This
revision fixes all **P1 and P2 findings** according to the user's choices:

- **Trust moments first:** make failures, permission decisions, stopped work,
  destructive actions and recovery understandable before decorative polish.
- **Keep the familiar monochrome look:** refine the accepted compact interface,
  its hierarchy and shared controls rather than introduce a new visual identity.
- **One preview banner for disclosures:** put the invented-data/no-save/no-send/
  no-run boundary and Run preview controls together, instead of repeating caveats
  across normal product surfaces. A quiet Preview chip can reopen the banner.

The work remains confined to the browser mock. It adds no real backend, provider
connection, authentication, host execution or durable authority. Provider choices
and permission decisions persist only for the visit. Existing escaping,
supplied-data ownership, draft/selection protection and reduced-motion behavior
remain part of the contract.

### Independent critique scores

Independent reviewers scored each checkpoint. All six final design scores are
in the **Good (28–35)** band; design scores are out of 40.

| Surface | Baseline | After round 1 | Final |
| --- | --- | --- | --- |
| Main chat | 24 | 28 | 30 |
| In-chat widgets | 24 | 28 | 33 |
| Settings | 22 | 30 | 33 |
| Models & providers | 22 | 26 | 32 |
| Finder & dialogs | 22 | 29 | 34 |
| Whole product | 27 | 28 | 30 |

The technical audit's baseline is **14/20**; it was **not re-run**. The per-area
summary below includes round 2 corrections from the first re-score and round 3
corrections from the final re-score. No further score after those corrections is
implied.

## What changed

### Main chat, sidebar and composer

- The conversation title aligns with the reading column and has one shared **⋯**
  actions menu. Rename and Archive operate on visit-local chats; Archive offers
  Undo, and unarchiving restores the chat to the library.
- The inspector starts closed below 1280px and becomes an overlay sheet at
  **≤1100px**, preserving transcript space. The sidebar can collapse to a **56px
  rail** with New chat and Settings. Inspector sheet Escape returns focus to its
  rail control; Escape in a child conversation returns to its row in Agents.
- New chat focuses the composer. Send/Stop shares one stable button element;
  Run preview/Stop/Retry likewise keeps its button stable so action changes do not
  strand keyboard focus.
- Sidebar rows keep the status glyph beside the title and move the status word
  to the meta line. Collapsed projects show attention/failed/working rollups;
  the open chat is marked once rather than repeated across its shortcuts.
- Pin and **⋯** actions appear on row hover or keyboard focus; filled pins remain
  visible in Projects. The same row menu opens on right-click, F2 starts Rename,
  and each project has a New chat action. Archived chats leave sidebar lists.
- The attachment action is **Add context**. Build/Plan has a description of each
  intended mode. The context meter leads with **81% used** from 80%, adds the
  Attention color and warning glyph from 90%, and keeps unknown usage neutral.
- Context now leads with a window-usage summary and grouped facts. The Agents
  inspector heading and rail badge agree about working children. Empty chats use
  the AVA mark without redundant instruction copy.
- **Round 2:** skip links and F6/Shift+F6 region cycling make the main regions
  reachable without tabbing through every control. Panel shortcuts move focus;
  Escape from a child returns to its Agents row. Stop defaults to **Mod+Shift+.**
- **Round 3:** the sidebar exposes **Find a chat** and a **Commands** button;
  inspectors have a Widen/Narrow toggle. At ≤1100px, moving focus into the
  conversation closes the inspector sheet so no focused control is hidden behind
  it, addressing the WCAG 2.4.11 focus-obscuring issue.

### In-chat widgets and transcript

- One change ledger and one unified diff renderer serve tool details and
  **Changes**. **Review in Changes** takes a supplied file change to the inspector;
  no change is inferred from tool output or success.
- Status sits beside the action. Tool groups summarize actionable facts, such as
  **3 tools · 1 failed · Show details**, rather than repeat every completed state.
  A single expand/collapse toggle and the **Tool details** preset label replace
  competing controls. Expanding details no longer scrolls their row out of view.
- Queued tools use neutral gray. After a stopped run, queued tools read **Not run**
  and tools that were running read **Not finished**. Neither label invents a
  per-tool cancellation or a successful result.
- Failed tools lead with their cause. An explicitly supplied image preview stays
  a bounded 160px thumbnail rather than overwhelming the failure explanation;
  missing or failed media remains honest about its availability.
- The Attention chat includes an inline permission request and a receipt naming
  the chosen decision. Decisions are stored for the visit and clear Attention
  everywhere. The question widget has a visible **Other…** choice; validation uses
  error tone. Run notices pair status with the cause and Retry where available.
- Markdown renders progressively, including framed code blocks for unterminated
  fences. Fenced code uses the shared CodeBlock renderer, idle Shiki highlighting
  and CSS-variable syntax colors. Formatting is preloaded at boot rather than
  waiting for each stored assistant turn to request it.
- **Round 2:** failed turns/tools offer **Ask AVA to retry**, question-load errors
  offer Retry, and the tool-detail preset shows **Custom** when manual expansion
  diverges. Changes deduplicates paths and adds a summary, file list and per-file
  collapse. Permission decisions now update the shared chat state rather than
  leaving the sidebar's Attention status behind.

### Settings

- Navigation is reduced to **11 sections**, grouped under App, Agent and About.
  Search matches individual setting rows and takes the user to the relevant row,
  rather than merely filtering page titles. Navigation supports roving keyboard
  focus; at ≤1100px a sticky section picker replaces the desktop navigation.
- **Extensions** brings Skills, Prompts, Plugins, MCP servers and language servers
  together. Subagents retain inline create/edit/delete actions rather than a
  separate detour for each edit.
- Shortcut recording shows keycaps and an explicit **Move from…** conflict action.
  Notification rows have their own tone previews, with one live region for sound
  feedback rather than competing announcements.
- **Reset all preferences…** lists the real changes before confirmation and offers
  Undo. Escape clears an active Settings search, otherwise closes Settings.
- Sidebar material is controlled only in Settings → Appearance; the ambiguous
  half-circle Opaque toggle is removed from the AVA brand row.
- **Round 2:** trust copy names the current project and the **`/trust project`**
  command; Extensions links to that guidance. Context management has explicit
  units and consistent explanatory copy.
- **Round 3:** Shortcuts has a **Navigation** section for F6, F2, Escape and
  Ctrl/⌘+Enter, plus a visible hint explaining how app shortcuts can be remapped.

### Models & providers

- The model picker focuses search on opening, supports typing and Enter selection,
  and uses scope-specific titles and commit buttons. A Recent group shortens the
  common path; provider links lead directly to the relevant connection flow.
- Providers form one grouped list with explicit actions instead of a wall of
  repeated cards. Connection steps use stable footer bars, named errors and a
  readable device-code layout. A successful local connection scene offers
  **Choose a model** as the next action.
- Disconnect uses the shared destructive tone. Provider monograms provide an
  intentional fallback mark instead of a generic or missing logo.
- **Round 2:** local provider connection state survives navigation for the visit;
  Aster Lab and Orion Lab start connected, and models from unconnected providers
  say so. Connection flows no longer expose preview-only advance controls:
  API keys of at least 20 characters without spaces pass the local verification,
  and browser/device steps settle on their own. No real authentication occurs.
- **Round 3:** the provider directory has a filter, API-key entry has explicit
  guidance, and providers with only one connection method skip the method list.

### Finder & dialogs

- **Mod+K** opens the command palette and **Mod+P** the chat finder, including from
  the composer. Both use top-anchored, grouped keyboard pickers with combobox/
  listbox semantics and shortcut chips.
- Rename, Branch, Archive, Workspace and Providers screens are reachable in the
  app, not only the review harness. Rename selects the current title and shows
  validation in error tone; Archive explains its consequences.
- Branch defaults to the latest message, is safe to submit with Enter and creates
  a real visit-local chat from the chosen message. Permission dialogs initially
  focus **Deny**, not an Allow action.
- DialogFrame has a visible header, deliberate first-field focus and top anchoring.
  Workspace actions and the ScreenReview/catalog fixtures follow the same controls;
  the obsolete **Export session** fixture action is removed.
- **Round 2:** dialogs anchor to the top and their footers never clip content.
  Finder searches message text and shows a matching snippet; the project dialog
  supports keyboard navigation. Branch selection now creates a local chat rather
  than ending at a preview-only chooser.
- **Round 3:** Branch shows the full selected message, and archive guidance links
  to **Find a chat** on its Archived view.

### Whole product and shared foundation

- Shared type, spacing, radius and color tokens replace scattered values. Type is
  rem-based with an 11px floor; the input-edge token is raised to the 3:1 non-text
  contrast target. Presets reach composer controls, buttons, badges and inputs,
  rather than stopping at Settings samples.
- ChatStatus supports queued state and separate glyph/label parts. ListNotice and
  FormField share an error tone; IconButton has a quiet variant. These primitives
  keep status, feedback and action hierarchy consistent across screens.
- The final consistency sweep shares quiet **Back to …** buttons/links across
  Settings, its loading state, child conversations and the review/catalog entry
  points. Back uses a left arrow, View conversation a right arrow, and close
  controls use the same quiet 15px X. Composer tools, inspector close and sidebar
  collapse use IconButton rather than the obsolete global icon-button class.
- Sidebar row/project actions hide until hover or focus only on hover-capable
  pointers; touch users keep visible controls. Run preview replies/tool copy use
  ordinary product wording, while unused child notes, image labels and screen
  styles are removed.
- Repeated per-surface preview disclaimers and the separate Run preview row are
  replaced by the single banner. Product controls keep their ordinary action names
  while the preview boundary remains explicit and reachable.

## Deliberate decisions and deviations

- **Shortcut grammar:** Mod+K for the palette and Mod+P for the finder are deliberate
  exceptions to the earlier Mod+Shift-only app-shortcut grammar. The familiar
  shortcuts work in the composer, not just when focus is outside a text field.
- **Destructive tone:** red text and border, never a filled red button. It marks the
  consequence without replacing the monochrome interface with warning-colored
  primary actions.
- **Stopped tools:** Not finished means work was active when local delivery stopped;
  Not run means it was still queued. Supplied facts are preserved without claiming
  a real tool cancellation protocol.
- **Settings consolidation:** Extensions replaces five separate resource pages;
  Sidebar material remains in Settings only. These are clean navigation cutovers,
  not duplicated old and new controls.
- **Branching:** a branch is a real in-memory chat created from the selected
  message, not a backend fork. Refresh still resets it; no session export or
  execution is implied.

## Recorded verification

Final implementation verification supplied for this revision is:

| Check | Result |
| --- | --- |
| Typecheck | Passed |
| Full test suite | **68 test files / 988 tests passed**, with `--maxWorkers=6` |
| Production build | Passed |
| Entry budget | **427.38 kB raw / 130.39 kB gzip**, below the unchanged **500,000-byte raw cap** |
| Text-floor detector | No sub-11px text left in the app |
| Browser checks | **1440×900 and 1024×700**, including the palette from the composer, inspector overlay sheet/focus return and fenced code rendering |

Under heavy unrelated machine load, some lazy-load tests time out; every test
file passes in isolation. The six-worker limit and load sensitivity are part of
the recorded conditions, not a claim that every parallel/load configuration passes.
These facts record the supplied implementation checks; this documentation pass
runs no builds, tests, detector or browser checks. Review scores are recorded
separately above and do not imply full accessibility or native-host qualification.

## Known remaining limits

- No real backend, provider or authentication is connected. Tool activity,
  permissions, connection flows and context measurements are browser-local
  examples; Send adds a local message only. Provider state and branches survive
  navigation, not refresh. There is no host file access, upload, model execution,
  durable persistence or Electron integration.
- A light theme remains future work. The current palettes stay dark.
- Geist is not bundled; selecting it depends on a local installation and otherwise
  uses the configured fallback. No font download was added.
- Help is limited to Settings descriptions and the Shortcuts page; there is no
  separate help center.
- Screen-reader behavior was checked through the accessibility tree, not with
  NVDA or VoiceOver.
- The test suite is sensitive to heavy machine load. On the loaded 20-core
  machine, lazy-load tests could time out even though every file passes in
  isolation; the recorded full-suite pass uses **`--maxWorkers=6`**.

## Revision 22 — Help & first-time use

This follow-up adds in-app help and a complete browser-local first-run path. It
supersedes revision 21's limited-help note; the scores and acceptance above remain
revision 21 history. The preview stays visit-local, with no real backend, provider
authentication, host execution or durable persistence.

### Help and keyboard references

- `screens/HelpDialog.tsx` and `helpTopics.ts` provide one searchable Help dialog.
  Entry points are fixed **F1** (also over Settings), the sidebar footer's **Help**
  button beside Settings, palette **Help** and **Keyboard shortcuts** commands,
  Welcome's **Help F1** link, and contextual **About permissions**, **About context**
  and **About changes** links.
- Search matches word starts across topic text and current shortcut rows. When
  nothing matches, it can offer **Search Settings for “…”** if Settings has a
  matching row and **Open Commands** where that destination is available. Escape
  clears the search before closing Help.
- **Getting started** shows the live Connect a provider, Open a project and Ask
  AVA for a change checklist. In Help it folds to one line after setup is complete;
  Welcome keeps the checklist visible.
- **How AVA works** covers Chats and projects, Build and Plan, Permissions, Changes,
  Subagents, Context, Tool details, Providers and API keys, Extensions and
  Appearance. **Go to** links appear only where a real destination exists. There
  is no About modes link beside the native Build/Plan select; its explanation is
  available in Help without replacing the native control.
- **Keyboard shortcuts** reflects current bindings, including **Unassigned**, the
  user's Enter setting and fixed navigation keys such as F1. **Change shortcuts**
  opens Settings rather than maintaining another shortcut editor.

### First-time use and honest Send states

- The Preview banner's **Start fresh / Restore sample data** switches between an
  empty first-run workspace and the supplied examples. Welcome uses the AVA mark,
  a one-line purpose and the three-step Getting started checklist; the sidebar
  has a matching empty state.
- New chat with no projects opens the project picker. Opening a project creates
  one empty, logo-only chat, not a second setup screen inside the transcript.
  If the default model's provider is disconnected, new chats choose a model from
  a connected provider.
- A chat whose model's provider is disconnected cannot Send. The action explains
  **Connect [provider] to send with [model]**; a visible composer line says
  **[provider] isn’t connected, so this chat can’t send.** and offers **Choose a
  model**. Each send attempt re-announces the line, and Enter does not insert a
  newline while blocked by that connection state. Connected Send still adds only
  a local user message; it does not call a model.
- Empty chats' context meter reads **No usage yet**, distinguishing an unused chat
  from a model whose usage is unknown.

### Follow-up fixes found during verification

- The lost-typing issue after closing dialogs was reproduced in headed Chromium.
  Focus is now re-seated on the next frame so typing intended for the message box
  lands there after dialog close.
- The shell no longer shrinks behind dialogs. Startup no longer loads the
  Appearance Settings page merely for its defaults; those defaults live in
  `behaviorPreferences.ts`.
- Disconnect consequences explicitly say that chats using the provider's models
  cannot send until reconnection. **Done** leads after connecting during setup.
- An empty Find a chat offers **New chat** when a project is open, otherwise
  **Open a project**, so the empty state has a usable next action.

### Independent re-score

These scores were recorded after the Help/first-run round and **before** the
follow-up fixes above; no further re-score of those fixes is claimed.

| Surface | Score |
| --- | --- |
| Help & shortcuts | **29/40** |
| First-time use | **28/40** |
| Whole product | **31/40**, previously 30/40 |

The **Help and documentation** heuristic improved **2 → 3**.

### Recorded verification

| Check | Result |
| --- | --- |
| Typecheck | Passed |
| Full test suite | **73 test files / 1,064 tests passed**, with `--maxWorkers=6` |
| Production build | Passed |
| Entry budget | **463.91 kB raw / 141.57 kB gzip**, below the unchanged **500,000-byte raw cap** |
| Browser checks | Headed and headless Chromium at **1440×900 and 1024×700**: full first-run journey, Help entry points, topic deep links, typing after dialog close and the blocked-Send line |

These are the supplied implementation checks, not checks run by this
documentation pass. No real screen-reader session or live provider behavior is
claimed.

### Open items and deliberate limits

- Getting started is not inserted into empty chats: the brief keeps their
  transcript logo-only. Welcome and Help own setup guidance.
- Which providers to suggest to first-time users remains a product decision.
- Restore sample data has no confirmation; it is an explicit preview control.
- Following Help's **Go to** into Settings and then pressing Escape returns focus
  to the sidebar Settings button, not the original Help opener.
- Recent can repeat a project's only chat as a shortcut.
- Help does not describe undo/revert behavior because its real behavior is not
  defined.
- There was no real screen-reader session; the earlier accessibility-tree checks
  are not NVDA or VoiceOver evidence.
- The existing no-real-backend/provider/auth, dark-only, unbundled-Geist and
  machine-load-sensitive test limits continue to apply.
