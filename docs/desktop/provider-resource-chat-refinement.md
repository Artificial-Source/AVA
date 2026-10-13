# Provider, resource and chat-component refinement

## Current chat display follow-up — revision 19

[Chat display presets](chat-display-presets.md) add Compact, Auto and Expanded
without provider/resource/backend or asset changes. The shared parent/Settings
control preserves supplied facts and manual inspection through incremental delivery.
745 tests, 132 Chrome checks and one actual-source/actual-PNG Sol MAX gate passed.
Earlier revision bodies and provenance below remain complete dated history.

## Current chat-component follow-up — revision 18

[Supplied streaming acceptance](chat-run-refinement.md) refines normal-chat tools
into compact native per-row disclosures with stable output/diffs and reading-aware
following. Explicit Run preview delivers finite supplied events, never real
provider/tool work or a response to the draft; ordinary Send remains local-only.
Astra MAX delivery passed **699 tests / 106 native checks** and the sole Sol 6.1 MAX
**38-file / 24-actual-PNG** gate returned **No material findings**.
[22 current captures](concepts/react-chat-run/). Provider/resource assets and
ownership remain unchanged. Earlier “current” sections and page counts below are
preserved dated history; current Settings still has 16 leaves.

## Current browser-only scope — revision 13

Accepted on **2026-10-11** after Astra MAX implementation, Main's independent
checks and one integrated **GPT-6.1 Sol MAX** source/actual-visual gate, completed
by a bounded recheck of its named hover finding. Open the normal app at
**`http://127.0.0.1:5182/`**. The screen/component catalogs remain optional reviews,
not the primary experience.

This supersedes the earlier provider-approval hold and combined Resources &
connections page in [revision-12 history](normal-app-refinement.md). Earlier
records, screenshots and model/sidebar corrections remain unchanged history.
Nothing here integrates the preview with Electron, AVA's backend, real provider
authentication, resource files, execution, persistence or live connections.

### Settings pages and ownership

Settings still hides/inerts the existing mounted chat sidebar and workspace;
Back restores their state and visible Settings-opener focus. It has 13 separate
leaf pages, nav-local search and remembered selection:

- **Top-level:** General, Appearance, Models, **Providers**, Shortcuts,
  Permissions & trust, About.
- **Resources:** **Subagents**, Skills, Prompts, Plugins, each its own page.
- **Connections:** **MCP and LSP only**, each its own page. Providers is not
  nested in Connections.

Subagents supports explicit create/save of **user-owned local definition
metadata**: name, description and model preference. Built-in `general`/`explore`
and project-managed examples are read-only. Names/descriptions are bounded and
validated; duplicate names, invalid/stale/closed/inactive/repeated actions fail
closed. Saved metadata survives Back/reopen for this visit through an opaque
AppShell snapshot; preference reset does not remove definitions. Standalone
review state/reset is independent. Refresh clears local edits.

**Model preference is not applied backend policy.** Current AVA definitions have
no per-definition model field or definition-write RPC; child runs inherit the
parent model. The preview permits inherit or its same three fictional profiles
as local preference metadata only. Editing this never changes the current chat,
new-chat default, historical attribution, running child or task definition file.
About and the selector's accessible description retain that limitation without
repeated per-row note slabs. No instruction bodies or private paths are inspected.

MCP separately shows supplied **configuration enabled/disabled**, connection
not-connected/unknown, global/project source, project-trust requirement and
one-shot process lifetime. Enabled configuration is not a live connected daemon.
No environment values, credentials, commands, remote URLs, OAuth, launch/restart
or config-write controls are added. LSP, Skills, Prompts and Plugins separately
show safe supplied metadata or unknown inventory; absence is not fabricated
installation, zero inventory, trust or connectivity.

### Provider identities and approved local assets

Providers now contains **15 read-only logo/name cards**, keyed on actual provider
IDs. Brand identity does not imply a configured account, authentication,
connectivity, model availability or a supported connection method. The existing
four fictional manual authentication rehearsals remain separately reachable in
an initially collapsed **Connection examples** disclosure. Only those exact
fixture identities/methods/sources can emit demo connection intents. Aster/Orion
are not renamed to OpenAI/Anthropic.

The shared decorative mark renders beside provider labels in the directory,
demo heading and Context provider row. Unknown/custom/compatible/prototype-key
and fictional providers get a neutral fallback; model-family glyphs are unchanged.
`xai` uses the xAI company mark, not Grok. `zai-coding-cn` shares Z.ai's mark only,
not its authentication/configuration/credential scope. Vercel stays unused
metadata; it is neither an adopted asset nor a selectable provider.

The user approved the bounded static-logo approach. Adopted source:

- [`@lobehub/icons-static-svg@1.95.1`](https://registry.npmjs.org/@lobehub/icons-static-svg/1.95.1),
  pinned git `49a2130df7bfa5eb1b088261bff20a37e2967789`.
- **14 unique original monochrome SVGs / 14,177 bytes**, serving 15 runtime IDs.
  Original archive/candidate bytes and SHA-256s are preserved. The harmless fixed
  root style is retained; SVGs are not rewritten or injected as HTML.
- Archive SHA-256:
  `fc2a063f9c5db18f6b3f27a62670c95a9e767cc45ba7bbe5d949f9f9af090fc3`;
  published SHA-512 integrity was also verified.
- Exact pinned upstream **MIT notice**, 1,064 bytes, SHA-256
  `add9d7531d1b21646317a8958e38fc727506fa39d24bdecb44154d943c82753a`.
  The archive lacked a LICENSE member, so the notice comes from the pinned
  upstream Git revision, not a guessed archive file.

[Original files, exact license and provenance manifest](../../apps/desktop-preview/public/provider-marks/)
are local. Rendering uses 24px current-color masks with adjacent text identity,
not arbitrary URLs. Bounded SVG inspection found no scripts, event attributes,
external references, images, foreign objects, animations or entity declarations.
The exact full notice is included in the preview's runtime-license text, and
[THIRD_PARTY_NOTICES](../../THIRD_PARTY_NOTICES.md) has an append-only section
explicitly outside the Linux C++ artifact. No logo package, React/Ant Design
transitives, dependency install, CDN or runtime logo fetching is introduced.
Source MIT permission is **not vendor endorsement, blanket trademark permission
or production distribution/SBOM certification**.

### Chat tools and supplied diffs

The mounted parent conversation has one **Expand all tools / Collapse all tools**
control affecting every current assistant tool group, plus optional local group
control. Initially collapsed; same-ID rerecords retain state, removed/reintroduced
or new groups start closed. Settings retains the mounted expansion; changing chats
starts collapsed. Hidden/inert/disconnected controls cannot change it. Multiple
assistant groups are tested deterministically; the normal fixture still has one
and no assistant turn was invented just to demonstrate batching.

A compact group header/status summary and **one activity list** replace duplicated
outcome/output lists. Lifecycle text remains readable while collapsed; failed
images keep a separate failure cue and human **Pasted image** label. Raw invented
paths/output are escaped opt-in details. Working remains an icon-only 2.4s linear
cycle; Completed white, Working green, Failed/Stopped red, Attention yellow and
Unavailable gray. No row-wide color, halo, fake progress or execution appears.

Explicit optional readonly `ToolView.changes` carries file identity/operation,
optional supplied finite nonnegative addition/deletion counts and supplied text,
binary or unavailable diff state. The expanded group shows all supplied file
sections together—no required per-file clicking. Zero is valid; unknown counts
are omitted. Missing changes invent nothing. Duplicate/invalid metadata fails
closed for changes without hiding ordinary output. Filenames/hunks are escaped;
truncation and the 200-line/20,000-character paint bound are explicit. There are
no file-open/apply/undo/copy/run actions. Diffs are **not inferred** from success,
tool names, arbitrary output, current Changes data or disk. The broader Changes
inspector remains independent and unchanged.

Output/raw-path/diff rendering has a genuine optional **ToolActivityDetails** lazy
owner requested on explicit expansion; late import resolution cannot reopen
closed content. No build threshold/chunking/config workaround is used. Heading,
activity and body spacing are tighter, with immutable supplied model/thinking
facts retained. The 15px/25.5px reading style, 780px maximum, composer CSS, rail,
markdown safety, authored notes and essential permission/security text remain.
No clipboard, regeneration, model invocation or fabricated Send response is added.

## Reference comparison and backend gaps

Read-only source comparison, not copied code, architecture, theme, fonts or assets:

| Reference / pin | Verified relevant behavior |
| --- | --- |
| OpenCode v2 `740072694de71fae23f35131591e7c32a9713014` | Generic agent configuration supports model/description, but no Settings subagent-definition editor was found. Extensions/MCP lists operate real lifecycle separately from config. Context tools group; file bodies defer mounting. Chat has no demonstrated global tool button—its Expand All is session-review diffs. |
| Original OpenCode `2406400f0aeb07b36d0495af4e05aaca49159832` | Config-backed agents, operational MCP picker and collapsed context-tool groups, not a demonstrated agent/server-definition editor or global chat expansion. |
| Pi `898ab804050730e9dcefb4443875d5a932aa6a32` | No native named-subagent/MCP Settings editor found. `ctrl+o` really toggles all current tool outputs in runtime state, unlike a per-tool click. Edit previews/results use supplied call/result fields. No browser shortcut was copied into AVA. |
| Zeron `1c83cda221bd18a3b55ea49e5b25ec2c148955bc` | Subagents are live child transcript chips, not editable definitions; Providers manages external harnesses. Ordinary work groups use compact summaries/individual details. Supplied inline diffs/stats and unavailable/truncated full payloads are distinct. |

Useful anchors: OpenCode v2 `packages/schema/src/config/agent.ts:11–22`,
`packages/app/src/settings/providers/extensions.tsx:74–114`,
`packages/session-ui/src/tools/tool-renderer.tsx:835–878,2009–2115`;
Pi `packages/coding-agent/src/modes/interactive/interactive-mode.ts:4397–4417`
and `core/tools/renderers/edit.ts:171–236`; Zeron
`crates/ui/src/transcript.rs:7990–8115`. These comparisons inform the original
AVA hierarchy, not equivalence or adoption of their backend behavior.

First-party constraints are
[`AgentDefinition`](../../src/ava/agent/agent_config.h),
[`inherited_child_options`](../../src/ava/agent/agent_turn_subagents.cpp),
[MCP's stdio/one-shot contract](../extensions/mcp.md) and
[public RPC](../rpc-protocol.md). There is no backend definition mutation or MCP
config-write adapter in this UI. Connecting resources, trust/source precedence,
real per-agent routing/model policy and persistence require separate backend work.

## Independent verification and named finding

Main ran **378 nearest tests/25 files**, typecheck, then **454 full tests/29 files**.
After the named CSS correction, final typecheck, all **454 tests** (14.31s) and a
warning-free **477-module build** pass. Entry is **499,990 raw bytes / 152,308
Node-default gzip**, only **10 bytes below the unchanged 500,000-byte cap**.
Future root additions must be remeasured; this is not capacity for another widget.

Native normally sandboxed Chrome on loopback passed:

- **50 normal-app checks**, all 13 pages, brand/demo separation, local
  create/edit/Back/reopen/reset ownership and preserved chat/model/draft state.
- Four additional **850×820, 520×850, 1440×560 and 3440×1440** tool/provider/editor/
  MCP layout variants alongside the 1440×960 baseline; actual narrow/short diffs
  are reachable by transcript scroll.
- **135 scenario observations / 14 families**, six explicit request/catalog
  checks, seven secondary checks, ten native Tab observations (no retained
  background control focused), all **12 actual prefixed diff projections**,
  and icon-only Working/Queued checks. Tool cases now number 19; Settings 25.
- **16 finite painted-text contrast states**, minimum **5.405**; narrow tools
  **5.631**, provider/editor/resource states **7.825**, MCP **7.130**. Native values,
  placeholders, glyphs, hidden text, disabled/unsampled states are excluded;
  this is not all-state WCAG/AT or GPU/performance qualification.
- All 14 local SVG URLs loaded, with no external HTTP. The pre-correction
  combined log observed 321 requests / 41 unique URLs; final scoped verification
  observed **374 requests / 51 unique URLs**, all 14 marks responding 200/304 and
  zero external HTTP. Console/page errors are empty.

**PROVIDER13-F01 is fixed and closed.** Sol found the new Create/Save primary
labels nearly white on white when hovered (~1.16:1), exposed by a pre-existing
shared hover rule. Exactly one editor-scoped enabled-primary hover override keeps
the dark foreground; frozen shared primitives/global/tokens are unchanged. Main
verified both native hovered controls at **18.423:1**, plus two painted hover
states (minimum **7.825**). The same Sol reviewer rechecked only that delta and
completed three initially missing catalog visuals. Final verdict:
**No material findings.** The shared pre-existing rule outside this new editor
was not broadened into an unrelated global button audit/fix.

Sol's first pass actually read 29 fresh images (native model metadata confirms
GPT-6.1 Sol); its bounded completion read five additional images, totaling 34.
MAX was commissioned for implementation and review; worker transcripts expose
model/image metadata but not wire-level effort. Protected historical F03/M01
findings remain closed; no timer, observer, global trap or dialog focus rewrite.

[Thirty current captures](concepts/react-provider-resources-tools/) include
[Providers](concepts/react-provider-resources-tools/ava-react-providers-1440.png),
[fixed Create hover](concepts/react-provider-resources-tools/ava-react-subagent-create-hover-fixed.png),
[fixed Save hover](concepts/react-provider-resources-tools/ava-react-subagent-save-hover-fixed.png),
[MCP](concepts/react-provider-resources-tools/ava-react-mcp-top-1440.png),
[grouped tools](concepts/react-provider-resources-tools/ava-react-chat-tools-collapsed-1440.png)
and [a visible narrow inline diff](concepts/react-provider-resources-tools/ava-react-inline-diff-visible-520x850.png).
The pre-fix capture remains temporary finding evidence, not a current published UI.

Acceptance artifacts are under `/tmp/ava-provider-settings-baseline-lblr7nbf/`:
reviewed maps, immutable before/hashes/Git snapshots, both worker reports,
source manifest, asset audit, Main logs/native observations, contrast/network,
review/recheck evidence, stable finding ledger and preservation report. Ordinary
repo checks and whole-file hash audits protect untracked prototype source as well
as tracked changes; backend/legacy/dependencies/config/ComposerCSS/F03/history
remain unchanged except the explicitly approved source-logo notice append.

Final preservation: **1,789 → 1,853 files**, **37 modified / 64 added / zero
deleted**, with **1,752 unchanged** and 1,505 protected existing files verified.
The 64 additions include 30 current PNGs and this new document. Full older README/
prototype acceptance suffixes, original asset bytes/license, HEAD/staged and
unrelated tracked-unstaged changes are preserved. Documentation passes: 153
Markdown files, 135 documents all reachable, 18 required indexes, 30 llms links,
16 verifier tests and whitespace checks. This is source/UI evidence, not a new
C++ or Electron build qualification.

Production/native accessibility, packaging, legal/SBOM, real configuration,
provider/auth/transport, file/tool execution and independent-chat/worktree routing
remain separate qualifications, not implied by this browser-only acceptance.
