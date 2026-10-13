# Sidebar activity and composer research

**Researched 2026-10-05. This record describes research, not backend implementation.**

**Subsequent authorization, 2026-10-05:** after reviewing these recommendations,
the user requested **Astra at max reasoning** to implement them **in the mock**.
That separate UI-only iteration is tracked in the [React prototype](react-prototype.md).
Follow-up OpenChamber header references clarified one shared top bar and a compact
inspector toggle/picker at its top-right, with the panel/rail beneath. A subsequent
user correction moved show/hide above Agents on the right rail and replaced the
header's generic picker with useful local parent-context/change/activity details.
Layout only: no reference colors, usage percentages, account/avatar, or unsupported
controls. Later user feedback separately authorized **status-only accents and a
slow Working breathing highlight**: green Working, bright-white Completed, red
Stopped/Failed, yellow Attention, gray Unavailable, with static reduced-motion
fallback. This is a scoped exception to the earlier monochrome/static direction,
not adoption of screenshot colors or continuous full-row shimmer. The user's next
clarification specifically requests **a cycling Working icon and glowing Working
text**. That study is now historical: the user subsequently rejected the highlight,
retaining **only the cycling icon**, and requested premium minimal sidebar/panel
opening/closing motion, larger header details and ultrawide responsiveness. The
[frontend component-system comparison](frontend-component-system.md) tracks reusable
chat-widget foundations for later integration. Non-Working statuses stay static;
reduced motion preserves readable status without animation. The latest separately
approved mock revision removes the workspace top bar: parent title/project, Context
and change/activity controls move into a flat main right sidebar. A quiet inline
identity/Back cue remains when collapsed or inspecting a child. Revision 8 has
independent mock evidence in the prototype record: 178 tests, 70 browser checks,
seven layouts and the fixed narrow-containment finding. The user's next correction
requests title only at the chat canvas's top-left, no project subtitle and no repeated
sidebar heading; Context/module controls stay in the sidebar. Revision 9 has
independent mock evidence: 179 tests, 50 browser checks and seven layouts, with no
material source findings. Subsequent feedback rejects the repeated Context/actions
row. Revision 10 instead has one Context page, a selected-profile composer meter and
logo-only empty chats: **213 tests, 63 browser checks and seven layouts**, with no
material source findings. The earlier shared-header/sidebar-title/shared-row studies
remain history; none adds a backend capability.
The research evidence below is preserved; it does not qualify the new UI or
approve live backend/RPC, Git/GitHub, extensions, images, or production adoption.

## Context information follow-on (2026-10-06)

The OpenCode screenshots prompted a separately authorized **mock-only** Context
page and composer meter. Read-only pinned v2 inspection at
`740072694de71fae23f35131591e7c32a9713014` shows the
[Context tab](https://github.com/anomalyco/opencode/blob/740072694de71fae23f35131591e7c32a9713014/packages/app/src/session/files/session-context-tab.tsx#L114-L139)
uses **last assistant** input/output/reasoning/cache totals and that assistant's
model context window, not composer selection or cumulative session accounting.
Its System Prompt is a stored instructions delta; Raw messages are serialized
stored-session UI objects, **not the exact request sent to a provider**. AVA's
user-selected-model meter deliberately differs; reference behavior is not copied.

AVA's public model catalog has optional configured `context_window_tokens` and
`max_output_tokens` (`src/ava/app/rpc/serialization_models.cpp:43–84`); cumulative
usage/cost is separately optional in `get_session_stats`
(`src/ava/app/rpc/serialization.cpp:1027–1100`). Missing is not zero; reasoning/cache
can be subsets, and cost completeness matters. The TUI estimates active context
plus current system prompt (`src/ava/app/interactive_tui_support.cpp:340–363`), but
**that estimate has no published RPC field**. Neither a full resolved system-prompt
export nor the current raw provider payload is public. No effective-input-budget
field or output-reserved denominator is invented.

The mock supplies explicitly fictional model/provider-window-matched active estimates,
independent cumulative counters/cost, first-party prompt text and an escaped local
committed-message projection. Real estimate/prompt/payload wiring requires separately
scoped public API and privacy work; no private JSONL/auth/path shortcut. Actual UI
and bounded evidence are in the [prototype](react-prototype.md#revision-10-independent-validation).

The user liked the monochrome React revision and asked to research a right sidebar
for subagents, then supplied layout references for chat statuses, a right icon
rail, and a richer composer with a glyph beside the model name. The user explicitly
clarified: **layout, not the reference colors**. AVA's black/white/gray palette,
connected shell, opaque reading canvas, inset composer, and no horizontal chat
tabs remain the baseline. This does not authorize a full IDE or full-client work.

## Findings

A secondary inspector is a good fit, but **“most coding agents have a right
sidebar for active subagents” is not established**. In this bounded sample,
secondary surfaces often contain review/files/browser/terminal tools. Actual
child-agent visibility varies: tasks panes, popovers, child conversations, and
read-only transcript surfaces. Those are not interchangeable with a left list
of independent top-level chats.

| Product / evidence | Secondary surface and actual child-agent pattern |
| --- | --- |
| **Claude Code desktop**, current official [desktop docs](https://code.claude.com/docs/en/desktop) | Arrangeable chat/diff/browser/terminal/file/plan/tasks/subagent panes. **Tasks** shows subagents, background shells, and workflows inside the current session; clicking an entry opens output in the subagent pane or allows stopping it. Views opens the pane. A permanently open right roster is not established. Other sessions remain in the left sidebar. |
| **Codex app**, official [troubleshooting](https://developers.openai.com/codex/reference/troubleshooting) | The side review pane reflects project Git state, staged/unstaged changes, branch comparisons, or **Last turn**. It can include changes Codex did not make. These docs do not establish a child-agent roster in that pane. |
| **Cursor**, official [Agents Window](https://cursor.com/docs/agent/agents-window) | Agent-first multi-workspace/parallel-agent interface with diffs and local/cloud handoff, including cloud-subagent capability. This page does **not** establish where a child-agent inspector lives or its default visibility; parallel agents alone do not prove a right child roster. |
| **OpenCode**, source pin `740072694de71fae23f35131591e7c32a9713014` | A right review/files surface and a separate background-work popover. Its list distinguishes shell/subagent entries; selecting a subagent navigates to the child session. This is not a default right subagent list. [Background source](https://github.com/anomalyco/opencode/blob/740072694de71fae23f35131591e7c32a9713014/packages/app/src/session/summary/background.tsx). |
| **Zeron**, source pin `1c83cda221bd18a3b55ea49e5b25ec2c148955bc` | Optional right surface host includes File/Browser/Diff/Terminal/**Subagent**/SideChat. A child row opens a **read-only** child transcript surface, with lifecycle glyph and relative time. Child agents and side chats are distinct. [Row source](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/files/sections.rs), [surface source](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/shell.rs). |
| **T3 Code**, source pin `c138163c8cc39d4a98b2d3d796bcef183013bac5` | Right surfaces include diffs/files/browser/device/terminal/PRs. This pin explicitly **removed** the former Agents panel. Child conversations remain inspectable: provider-native child views replace the composer with model/status and **Open parent**. [Panel store](https://github.com/pingdotgg/t3code/blob/c138163c8cc39d4a98b2d3d796bcef183013bac5/apps/web/src/rightPanelStore.ts), [child bar](https://github.com/pingdotgg/t3code/blob/c138163c8cc39d4a98b2d3d796bcef183013bac5/apps/web/src/components/chat/ProviderSubagentBar.tsx). |

Official documentation was fetched on the research date. Source behavior is
qualified to each pin, not asserted as the current shipped release. Default-open
behavior is not assumed where unverified. Reference code is comparative input,
not an architecture or source-code adoption.

## User references: what to borrow

Three supplied stills were visually inspected. Their exact release/product
identity is not assumed beyond the visible **T3 Code ALPHA** label in the first.

1. **Left chat rows:** a small status before a truncated title, trailing relative
   time, grouped projects, and subtle selected-row fill. Some idle rows have no
   status. This describes **chat/parent status**, not a list of child subagents.
2. **Right edge:** a narrow vertical icon strip adjoining one inspector, with an
   active marker. The visible pane is Git. This suggests a **module switcher**, not
   that AVA should implement every icon or Git operation shown.
3. **Composer:** compact icon cluster, model glyph immediately beside its name,
   mode and Send controls; a changed-file summary above the input. Unidentified
   icons do not establish permission, extension, or tool semantics. The screenshot's
   `@ / ! #` helper syntax is not AVA's contract.

The references' colors, logos/assets, window controls, commit/sync/revert behavior,
microphone, and perpetual animation are **not** approved adoption. Do not restore
the rejected full-width footer, helper row, or bottom sidebar utility area.

### T3 status and animation: version matters

The supplied ALPHA still proves placement, not motion. At the captured T3 pin:

- The older `resolveThreadStatusPill` returns **Completed** only for an **unseen
  completion** (`completedAt > lastVisitedAt`), not as a permanent status for every
  idle chat. It returns null for ordinary idle rows. Working/Connecting have a
  `pulse` flag; that alone does not prove the animation in the supplied still.
- The newer inbox row uses **Working/Waiting/Goal** and **Done** for an unread
  completion. Its source explicitly says: **“No shimmer: a label that animates
  forever is noise in a sidebar full of them.”**
- Receding working rows use `opacity-70`, hover/focus restoration, and
  `motion-reduce:transition-none`. This verifies reduced-motion adaptation for
  that **opacity transition**, not every historical pulse animation.
- A stale sidebar comment still mentions a parent's “Agents surface,” while
  `rightPanelStore.ts` explicitly removes `agents` in storage version 14. Reading
  that comment alone would give the wrong current-at-pin panel recommendation.

Sources: [Sidebar logic](https://github.com/pingdotgg/t3code/blob/c138163c8cc39d4a98b2d3d796bcef183013bac5/apps/web/src/components/Sidebar.logic.ts)
and [Sidebar rendering](https://github.com/pingdotgg/t3code/blob/c138163c8cc39d4a98b2d3d796bcef183013bac5/apps/web/src/components/Sidebar.tsx).

For AVA, prefer a quiet **grayscale glyph + status label before title**. Active,
completed, failed, stopped, and attention states should remain distinguishable by
shape/text, not color. A brief state-change emphasis could be explored later;
continuous row shimmer is not the research recommendation. Reduced motion must
retain static status information. Exact unread-completion lifetime remains a
product decision, not something to copy implicitly from T3.

## Recommended AVA layout hypothesis

At research completion these were recommendations, not implemented features or
final design. The user subsequently authorized their **mock-only** implementation;
Astra at max reasoning owns that prototype code/design. Historical recommendations
below do not themselves prove implementation or browser acceptance.

### One rail, one attached inspector

- A slim **right-edge icon rail** switches one attached inspector. No detached
  cards, exterior gutters, nested glass, or second chat-tab strip. The opaque
  transcript and bounded composer remain the primary area.
- First useful modules: **Agents** and the existing read-only **Changes** view.
  The panel can close while the rail retains a small, accessible activity count.
  Actual default-open behavior and narrow-window adaptation need visual testing.
- Git/GitHub, image context, and extensions are potential later modules, not
  decorative dead buttons or a request for a full IDE. Add them when their purpose
  and underlying capabilities are approved. Changes and future Git should not
  become two redundant panels.
- Keep list scope explicit: **children of the selected parent chat**. A global
  cross-chat roster would require separate ownership/aggregation design. Left
  Projects/Pinned/Recent remains navigation for independent chats.

### Click a child, inspect its conversation

- Rows put a lifecycle glyph/label before a short task identity, with bounded
  secondary metadata. Working first; terminal states remain distinguishable.
  Avoid reducing every non-running outcome to ambiguous “inactive.”
- Clicking a row can open a **child conversation view**, with unmistakable parent
  identity and **Back to parent**. Entering it is inspection—not new execution,
  session takeover, cancellation, or an automatically created sidebar chat.
- Prefer **read-only inspection first**. Preserve the parent's draft/model/mode
  and run ownership; do not silently retarget the ordinary Send/Stop buttons.
  Child messaging, steering, restart, and permission replies are separate controls
  requiring later backend/contract decisions.
- In a mock iteration, use invented child data and truthful unavailable/failure
  states. Do not claim real progress percentages, test success, private thoughts,
  token-by-token child streaming, or persistence after app exit.

### A richer, still compact composer

Keep the 780px maximum reading-column enclosure, not a new full-width toolbar.
A sensible first layout exploration is:

- **Add/context** button with a small menu instead of a permanent row of helpers.
- **Expand** for longer drafting; a local presentation action.
- **Monochrome provider/model glyph + model name**, together in one picker. For
  invented preview models, first-party fictional marks avoid borrowed branding.
  Real provider artwork needs a separately verified asset/brand mapping; catalog
  strings do not supply icons. No new icon dependency is selected.
- Existing **Build/Plan** and Send/explicit Stop, without stacking multiple
  unexplained policy switches. Model reasoning controls are optional only where
  supported; a preset icon must not mean “bypass permissions.”
- An optional, understated **changes/context summary above** the enclosure when
  meaningful, linking to Changes rather than asserting ownership of every dirty
  file. No microphone, shell/snippet grammar, or image generation is implied.

## What AVA can actually support today

The first-party capability investigation inspected source and existing tests;
it did **not** run them or launch a backend. Public RPC v1 remains authoritative.
The browser prototype is still mock-data only.

| Surface | Current evidence | Desktop gap / caution |
| --- | --- | --- |
| Child-job identity/state | Public `list_jobs`, `get_job`, `wait_job` expose parent/child IDs, foreground/background, execution/delivery states, timestamps/counters and bounded metadata. | Owner-scoped process-local jobs, not arbitrary top-level chats. No percent/current-step field or restart recovery. |
| Child result/control | `get_job_result` provides bounded safe terminal content; `cancel_job` is cooperative; `promote_job` exists. These are active-safe. | List/status deliberately omit terminal content. “Cancel requested” is not already canceled. No public job resume/restart. |
| Child title/type/model | Internal snapshots have bounded interactive-only display fields. | **Explicitly excluded from public RPC/model-tool/persisted schemas.** Do not serialize them incidentally to build a nicer row. |
| Child conversation | TUI uses an owner-bound, path-free `SubagentInspectorFrame` with capped committed User/Assistant messages, generations, terminal/freeze/unavailable flags. | **No inspector RPC command/serializer.** No tools/reasoning/live-token stream in this frame; do not promise that ordinary child setup text is always absent. Clicking into a live child chat is not already desktop-ready. |
| Child steering | Exists in coordinator/model job tool. | Public `steer` is for the active top-level prompt/follow-up, **not a selected child**. |
| Model identity/selection | `get_state`, `list_models`, `set_model`, reasoning commands, and session events. | No provider-logo assets in the catalog. Model mutations are not all active-safe. |
| Images/file context | `prompt.attachments`/`images`: up to 16 validated PNG/JPEG/WebP/GIF inputs. TUI has file-reference completion. | No generic file-picker/completion endpoint or permission-free renderer filesystem access; don't import the reference's whole `@ / ! #` grammar. No image-generation promise. |
| Commands/plugins/MCP | Unified `list_commands`/`invoke_command`; local plugin discovery/inspection/enablement and permissioned local install/remove; MCP discovery/inspection. | Not an extension marketplace, remote GitHub integration, microphone, or permission bypass. |

The live-child inspector bridge is therefore a **real future integration gap**,
not simply rendering an already-public TUI object. Independent parallel chats
still require separately designed host/backend routing: one active top-level run
per RPC process remains the constraint. Inspection/navigation must preserve
session leases, owning run/job IDs, resolver routing, backend permissions, and
fail-closed cancellation/disconnect behavior. No renderer reads private session
JSONL or receives generic Node/IPC/RPC access.

### First-party context map

| Owner | Evidence / eventual relevance |
| --- | --- |
| [RPC protocol](../rpc-protocol.md#subagent-job-snapshot) and [desktop architecture](architecture.md) | Public snapshots, command/admission/resolver boundaries; this research adds no wire fields. |
| [`subagent_job.h`](../../src/ava/agent/subagent_job.h) | Canonical states/identity/counters and explicit interactive-display exclusion. |
| [`subagent_coordinator.h`](../../src/ava/agent/subagent_coordinator.h) | Parent-owned inspect/control authority, separate from UI. |
| [`subagent_inspector.h`](../../src/ava/agent/subagent_inspector.h) | Bounded path-free committed transcript frame and unavailable/freeze semantics. |
| [`rpc/session_commands.cpp`](../../src/ava/app/rpc/session_commands.cpp) | Actual public list/status/wait/result/cancel/promote dispatch. |
| [`subagent_coordinator_tests.cpp`](../../tests/subagent_coordinator_tests.cpp), [`app_rpc_session_tests.cpp`](../../tests/app_rpc_session_tests.cpp) | Existing owner-isolation, redaction, generation/freeze and active-safe control coverage; inspected, not rerun for this research. |
| [Current React prototype](react-prototype.md) | Later mock-only seam: shell/attached Changes, sidebar rows, per-chat reducer, composer, focus and disclosure tests. No files in `apps/` changed by this research. |

## Research verification

Primary-source spot checks covered the official pane/task descriptions, pinned
OpenCode/Zeron child-view behavior, T3 status/rendering/panel-store/child-bar
files, and AVA's public job dispatch and internal frame. One integrated read-only
documentation review has **no open material findings**:

| Finding | Disposition | Evidence |
| --- | --- | --- |
| `SIDEBAR-F01`: alleged T3 pin/generation conflation | **Rejected and closed** after primary-source recheck | Both `resolveSidebarThreadStatus` and the older `resolveThreadStatusPill` coexist at the cited `c138163c` pin; the document separately describes the new inbox renderer. The same reviewer confirmed closure. |

Source-link/structure gates, all **16** documentation-verifier tests, whitespace,
and the **1,458-file** preservation baseline passed. Only this new document and
the two desktop status/index documents changed. No backend/TUI tests or live
provider checks were rerun for documentation research.

## Decisions recorded before the prototype authorization

The user subsequently approved proceeding with the recommended mock layout.
Astra may settle its bounded presentation choices in that implementation; live
capabilities and production behavior remain separate decisions.

1. Approve or adjust **right rail + Agents/Changes inspector**, and choose its
   default visibility; future module icons need not be shown now.
2. Decide whether child inspection occupies the central canvas or remains within
   the inspector. In either case, retain a clear parent return path and no
   implicit writable child composer.
3. Choose which compact composer controls merit initial fixtures; keep the
   monochrome model glyph/name adjacency and exclude fake unsupported actions.
4. Decide completion/unread persistence and any brief grayscale transition.
   Status layout inspiration does not select T3's current inbox architecture.

No implementation, dependency install, new visual mockup, backend/provider run,
or public contract change was performed **during this research**. The subsequent
mock iteration is separate and must supply its own checks. Existing approved prototype artifacts,
legacy client, source, package/lock/config/notices, HEAD and staged work are
preserved. Production UI/accessibility, real RPC integration, logos/assets,
Git/GitHub, extension lifecycle, and light/system design require later scope.
