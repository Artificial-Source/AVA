# Desktop Competitor Research

Dated snapshot of coding-agent and IDE surfaces that could inform a later
optional AVA desktop client. This is research, not a design, layout, stack
choice, or implementation plan.

**Retrieved:** 2026-10-02 (machine local); 2026-10-03 UTC. Method: official
docs, product pages, GitHub README/LICENSE files, and changelog pages from that
snapshot. **No competitor apps were installed or run.** No live model calls,
sign-ins, or subscriptions.

**Claimed** means vendor documentation or repository text. **Interpretation**
is synthesis for AVA as a local Linux x64 C++ agent that an optional Electron
host would drive only over public [RPC v1](../rpc-protocol.md). Star counts and
user-count marketing are not quality evidence.

Out of scope here: Jan, Cherry Studio, Linear, Raycast, Things, Apple HIG
([interaction references](desktop-inspiration.md)); renderer libraries
([library research](library-research.md)); visual design.

Integration constraints that this page must not weaken live in
[architecture](architecture.md), [product principles](../product/principles.md),
[containment](../security/containment.md), and
[sandboxing](../security/sandboxing.md).

## Comparison matrix

| Product | Form | Openness | Shell (only if source proves) | Claimed desktop status | Workflow relevance to AVA |
| --- | --- | --- | --- | --- | --- |
| OpenCode Desktop | CLI + desktop + IDE extension + web | OSS MIT ([`anomalyco/opencode`](https://github.com/anomalyco/opencode)) | **Electron** (`packages/desktop/README.md`) | Desktop called out as a beta-class surface in retrieved product/docs text | Closest OSS **local agent GUI**; not an AVA backend analog |
| Goose | Desktop + CLI + API | OSS Apache-2.0 ([`aaif-goose/goose`](https://github.com/aaif-goose/goose)) | **Electron** (`ui/desktop` + Electron Forge + React) | Packaged Linux desktop documented | Closest OSS **session-centric agent app**, not an IDE |
| ChatGPT desktop (Codex view) | Closed desktop + OSS CLI + IDE extension + cloud | **App closed**; CLI Apache-2.0 ([`openai/codex`](https://github.com/openai/codex)) | **Unverified** | Linux described as preview | Vendor **command-center** language (local / worktree / cloud) |
| Claude Desktop (Code tab) | Closed desktop hosting Chat / Cowork / Code; separate CLI | Closed app; CLI separate | **Unverified** | Official Linux page labeled **beta** | Parallel sessions, diffs, permission modes next to send |
| Cursor | Closed AI IDE + CLI + cloud agents | Proprietary; no OSS app repo retrieved | **Unverified officially** | Linux mentioned in docs | Full IDE — too large as a product analog |
| Zed | Native OSS editor + Agent Panel | Most editor source GPL-3.0; GPUI Apache-2.0 | **Native GPUI** (not Electron) | Linux editor documented | Editor-hosted **panel/thread** patterns only |
| Cline | VS Code / JetBrains extension + CLI + Kanban web | Apache-2.0; JetBrains plugin **not** OSS | Host IDE | N/A (extension) | HITL approve/diff in a sidebar |
| Continue | VS Code / JetBrains extension + CLI | Apache-2.0; **repo read-only / unmaintained** | Host IDE | N/A | Historical Chat/Plan/Agent sidebar grammar |

Separate these forms. Goose and OpenCode are **agent desktops**. ChatGPT
desktop’s Codex view and Claude’s Code tab are **closed command-center
desktops**. Cursor and Zed are **full editors**. Cline and Continue are
**host-editor extensions**, not standalone agent desktops.

Do not infer AVA backend architecture from another product’s UI libraries, HTTP
server, or copy. AVA’s public contract remains stdio JSONL RPC, not those
servers.

## Agent desktops

### OpenCode Desktop

**Exists** as terminal, desktop, IDE extension; v2 docs also list a web surface
via `opencode pair`. Repo [MIT](https://github.com/anomalyco/opencode/blob/dev/LICENSE).
Docs: [opencode.ai/docs](https://opencode.ai/docs),
[v2 docs](https://opencode.ai/v2/docs).

Current `packages/desktop/README.md` **expressly says Electron**. Older blobs
mention Tauri; treat Electron as current and Tauri as historical. Do not guess
OpenCode’s server or API design from that shell, and do not copy it into AVA.

**Claimed workflow.** TUI/docs: Tab switches **build** (full access) vs **plan**
(read-only; bash asks). `/init` writes `AGENTS.md`. `/undo` / `/redo`. Marketing
copy claims multiple agents in parallel. Permissions
([docs/permissions](https://opencode.ai/docs/permissions)): `allow` / `ask` /
`deny`; ask UI once / always / reject; `--auto` auto-approves non-denied. Tool
list is first-class (read/edit/bash/task/skill/webfetch). Desktop is documented
as a client of OpenCode’s own server/SDK — **not** a model for AVA RPC.

**Interpretation.** Useful as a local-agent GUI over a local runtime, with an
explicit plan/build split and session-scoped “always.” Weak: desktop maturity is
still claimed-beta; v1 and v2 docs coexist; Electron↔Tauri history. AVA already
has backend build/plan modes and allow/ask/deny; OpenCode’s more permissive
defaults are not an AVA policy target.

Published visuals: [opencode.ai](https://opencode.ai) and repository README
screenshot assets. Do not redistribute.

### Goose Desktop

**Exists** as desktop + CLI + API. Repo
[Apache-2.0](https://github.com/aaif-goose/goose). Docs:
[goose-docs.ai](https://goose-docs.ai/). AAIF / Linux Foundation. Core claimed
Rust; UI is Electron.

Current `ui/desktop/package.json` has **Electron Forge + React**. That is
Goose’s desktop shell, not an AVA stack decision.

**Claimed workflow.** Session list (search, pin, rename, export
JSON/YAML/Markdown); chat with collapsible tool output; Settings for provider,
extensions, theme, tool/file permissions. First-run: provider login or manual
keys, then enable extensions. `GOOSE_MODE`: `auto` / `approve` / `smart_approve`
/ `chat`. Per-tool Always / Ask / Never in approve modes. Subagents documented
(restricted tools, parallel research). Recipes as YAML. Keyboard docs: new
session / new chat / close / settings / search; Enter send; Esc cancel.
Conversation history is claimed shared between CLI and Desktop.

A prior OS-tool **Seatbelt** sandbox statement is **not** Electron renderer
sandbox, and Goose docs no longer present it as current. Never read that
history as a reason to drop AVA’s required sandboxed renderer or backend
containment ([architecture](architecture.md#process-ownership),
[containment](../security/containment.md)).

**Interpretation.** Strong session-library and chat-only mode. Weak: general
agent, not a code-review IDE; do not treat Electron+Node as given for AVA.

Visuals: [goose-docs.ai](https://goose-docs.ai/); desktop guide mirror
[block-goose.mintlify.app/guides/desktop-app](https://block-goose.mintlify.app/guides/desktop-app).

### ChatGPT desktop (Codex view)

**Exists, with a moving product surface.** Dated launch material described a
dedicated Codex macOS app. The retrieved hub
[developers.openai.com/codex/app](https://developers.openai.com/codex/app) is
titled **ChatGPT desktop app | ChatGPT Learn**. This page records that rename
as documented on the snapshot date; it does **not** claim a verified migration
chronology or a proven desktop shell engine.

CLI remains OSS [Apache-2.0](https://github.com/openai/codex). **CLI license is
not the desktop app.** The app is closed.

**Claimed workflow.** Dropdown → Codex. Run targets **Local** / **Worktree** /
**Cloud**. Projects + parallel chats. Composer permissions include ask, approve
for me, and full access, plus profiles. Disclosure: inline diff review, comment
on diffs, open in external editor, integrated terminal. Worktrees are vendor
documented at
[codex/environments/git-worktrees](https://developers.openai.com/codex/environments/git-worktrees)
(isolated checkouts, Local↔Worktree handoff). Those behaviors are **vendor
claims, not AVA commitments.** AVA has session fork/clone and a qualified Linux
x64 backend workspace; it does not currently own git-worktree lifecycle.

Linux desktop is described as **preview**. Shell engine: **not proven** from
official source. Do not write “Electron Codex app.”

**Interpretation.** Strongest published command-center language in this set.
Weak: closed; renamed; Linux preview; cloud and computer-use are not AVA
product. Do not merge ChatGPT chrome (non-coding tabs) into a coding client.

Visuals/demos: [introducing-the-codex-app](https://openai.com/index/introducing-the-codex-app/);
[codex/app](https://developers.openai.com/codex/app).

### Claude Desktop (Code tab)

**Exists** as one closed app: **Chat / Cowork / Code**. The Code tab is the
Claude Code GUI. CLI (`claude`) is separate; docs claim shared
`CLAUDE.md`/settings/MCP but **not** session lists.
[code.claude.com/docs/en/desktop](https://code.claude.com/docs/en/desktop);
Linux:
[desktop-linux](https://code.claude.com/docs/en/desktop-linux) (official page
**labeled beta**, includes Chat/Cowork/Code). Install: [claude.ai/download](https://claude.ai/download).

Framework: **unverified**. No AVA platform-support promise follows from Claude
Linux beta. AVA’s only qualified backend remains Linux x64
([product principles](../product/principles.md)).

**Claimed workflow.** Sidebar sessions; new session; session switcher. Git
worktree as a **session option**. Two sessions side by side. Permission modes
include Manual, Accept edits, Plan, Auto, and a dangerous Bypass (settings or
org policy). Diff indicator opens file list + hunks; line comments; Review
code. Transcript: Normal / Thinking / Verbose. Side chat is documented as **not
persisted**. Panes include chat, diff, browser, terminal, file, plan, tasks,
subagent. Computer use is a research preview and **not** claimed for Linux.
`/desktop` continues a CLI session in the app.

**Interpretation.** Permission mode next to Send, collapsed vs verbose tool
trace, and worktree-as-option are the transferable *questions*. Avoid
three-product tabs, cloud/SSH as table-stakes, computer-use, and becoming a
mini-IDE.

## Full IDEs

### Cursor

Closed AI IDE + CLI + cloud agents. Docs:
[agent overview](https://cursor.com/docs/agent/overview),
[run modes](https://cursor.com/docs/agent/security/run-modes),
[worktrees](https://cursor.com/docs/configuration/worktrees). No app
source/LICENSE retrieved.

**Claimed workflow.** Agent in a side pane. Modes include Agent / Ask / Plan /
Debug. Checkpoints (local, not Git). Queue vs steer. Side chats. Run modes:
Auto-review / Allowlist / Run Everything. Docs state a classifier is **not a
security boundary**. Worktrees are UI-native in an Agents Window; IDE uses
skills/commands.

**Interpretation.** Named run modes, checkpoints vs git, and queue vs interrupt
are review references. Cursor is too large as an AVA information architecture:
browser control, cloud VMs, and a full IDE are out of AVA’s terminal-first
shape. Do not treat Auto-review theater as a substitute for AVA’s existing
permission policy and containment.

### Zed AI

Open-source editor ([zed-industries/zed](https://github.com/zed-industries/zed)).
[Software overview](https://zed.dev/software-overview): **most source GPL-3.0**;
**GPUI Apache-2.0**. Published distributed app/services terms are separate. Do
not copy source, licenses, or trademarks.

**Claimed workflow.** Three harnesses: Zed Agent, External Agents (ACP),
Terminal Threads. Agent Panel: new thread, threads sidebar, switcher,
checkpoints per edit, follow-agent, Review Changes keep/reject. Parallel agents
docs ([zed.dev/docs/ai/parallel-agents](https://zed.dev/docs/ai/parallel-agents)):
threads grouped by project; optional worktree isolation.

**Interpretation.** Use for thread-list and opt-in isolation language, and for
ACP so the GUI is not the only agent. Zed is a full editor; AVA must not become
one. GPL copyleft is a source-reuse hazard, not a UI-learning ban.

Visuals: [parallel-agents blog](https://zed.dev/blog/parallel-agents);
[agent panel](https://zed.dev/docs/ai/agent-panel); [zed.dev/ai](https://zed.dev/ai).

## Host-editor extensions

### Cline

[cline/cline](https://github.com/cline/cline) Apache-2.0. Docs:
[docs.cline.bot](https://docs.cline.bot/). Not a standalone coding desktop.
JetBrains client is not open-sourced.

**Claimed workflow.** Every file edit and command needs approval unless Auto
Approve / YOLO. Plan explores; Act executes. Diffs in the host editor;
checkpoints. Auto Approve is per-category; YOLO auto-approves everything
including Plan→Act, with explicit deletion/push warnings. A separate web Kanban
uses per-card worktrees; that is not the VS Code sidebar.

**Interpretation.** Approve/reject on the proposed diff, Plan vs Act, and YOLO
as an explicit dangerous mode. Do not embed a browser-using agent or make YOLO
a default. AVA already fail-closes unknown permission state
([architecture](architecture.md#cancel-permission-and-question-states-fail-closed)).

### Continue

[continuedev/continue](https://github.com/continuedev/continue) Apache-2.0.
Docs: [docs.continue.dev](https://docs.continue.dev). Current `main` README:
repository is **read-only and no longer actively maintained** after a final
2.0.0. Treat as a **historical interaction reference**, not a candidate
dependency and not an active competitor.

**Claimed historical workflow.** One input: Chat (no tools) / Plan (read-only
tools) / Agent (all tools). Per-tool Continue / Cancel; policies Ask First /
Automatic / Excluded. `@` context.

Visual: [agent quick start](https://docs.continue.dev/ide-extensions/agent/quick-start)
(permission screenshot in docs).

## Cross-cutting patterns (interpretation)

Closest local-agent desktops: Goose and OpenCode. Closest closed command
centers: Codex-in-ChatGPT-desktop and Claude Code tab. Cursor/Zed are IDEs.
Cline/Continue are sidebars.

| Concern | Patterns in official docs | Hazard if copied blindly |
| --- | --- | --- |
| Session/project nav | Sidebar sessions (Claude, Goose, OpenCode); Codex projects; Zed threads-by-project | IDE project trees and multi-root workspaces |
| Isolation | Git worktrees as a **visible session option** (Claude, Codex, Cursor Agents Window, Zed) | Hidden extra checkouts; vendor cleanup rules are not AVA backend work |
| Permissions | Mode next to composer (Claude, Codex, Goose); allow/ask/deny (OpenCode, Cline) | Classifier-only “Auto” sold as a security boundary |
| Transcript | Collapsed tools + Verbose (Claude); collapsible tool output (Goose); checkpoints (Cursor, Zed, Cline) | Dumping every tool call as the only view |
| Composer | Model + permission + environment in prompt chrome | Three-tab super-app (Chat/Cowork/Code) |
| Multi-agent | Subagents inside a session vs parallel sessions vs ACP externals | Coordinator graphs and Kanban boards |
| Onboarding | Folder picker + first permission mode (Claude); provider wizard (Goose); `/init` (OpenCode) | Forced cloud login for a local RPC client |

Docs that *claim* polish: Claude’s permission/diff/verbose/shortcut table;
Codex Local/Worktree/Handoff language; Goose session library; Cline’s YOLO
warnings. Docs that *claim* unfinished: OpenCode desktop beta-class; Claude
Linux beta; Codex Linux preview; Goose OS-tool sandbox removed; Continue
unmaintained. None of that was observed in running builds on this snapshot.

## Open questions for later design

These are presentation questions over **existing** AVA capabilities and
approved backend work. They do not optionalize renderer sandbox, RPC
fail-closed permission/question/cancel, backend allow/ask/deny, build/plan
write policy, or Landlock/seccomp containment.

1. **Project grouping and lifecycle presentation.** The approved
   [navigation direction](window-navigation-research.md#approved-navigation-direction)
   and [canonical desktop scope](README.md#agreed-desktop-v1-scope) establish
   a multi-project default with optional windows and sidebar-only Pinned/Recent
   navigation; later prototype feedback removed the horizontal chat-tab proposal.
   Remaining questions concern project grouping and view/window lifecycle
   presentation, not whether those features are in the brief.
2. **Sessions.** How should `list_sessions`, `new_session` / `switch_session`,
   `fork_session` / `clone_session`, names/labels, and export appear without
   inventing a second transcript authority beside RPC results
   ([architecture](architecture.md#streaming-text-versus-committed-settlement))?
3. **Worktrees.** Vendors expose git worktrees as a session option. Optional,
   explicitly user-enabled Git worktrees are approved future-desktop scope
   ([canonical scope](README.md#agreed-desktop-v1-scope)); the backend does not
   own worktree lifecycle. Remaining design question: creation/selection and
   lifecycle ownership. No cleanup/merging policy or vendor architecture is
   selected.
4. **Plan / permission chrome.** AVA already has build/plan modes
   ([thinking modes](../core/thinking-modes.md)) and RPC `permission_reply`
   `allow` / `allow_session` / `deny` plus persistent rules. How should those
   existing states sit next to Send? Do not replace them with classifier Auto
   or Bypass.
5. **Transcript density.** RPC already emits tool timeline, diffs, reasoning
   events, and `get_messages`. TUI already collapses thinking. What is default
   vs verbose **presentation** of those events?
6. **Cancel and one active run.** How is cooperative `cancel`, fail-closed
   resolver release, `steer` / `follow_up`, and `active_run` admission made
   obvious? Behavior is already specified; UI cannot default-allow on cancel.
7. **Subagents.** AVA has `task` subagents, child sessions, and process-local
   jobs ([subagents](../core/subagents.md)). Broader task graphs remain
   deferred (`AVA-REL-215`). Desktop must not invent coordinator Kanban or
   imply live jobs survive process restart.
8. **Scratch vs session.** Claude’s unpersisted side chat vs AVA `fork_session`.
   Any extra pane is either ephemeral presentation or a real session fork —
   not a third history.
9. **Non-IDE chrome.** How to surface permission, question, diff preview, and
   job status **without** shipping a bundled terminal, browser, or file editor
   as table-stakes?
10. **Providers.** AVA is already multi-provider. Desktop is a client of that
    catalog (`list_models` / `set_model`), not a new provider layer and not a
    single-engine lock-in.

Not in scope to reopen: whether AVA “needs a sandbox”; dropping renderer
isolation; copying OpenCode/Goose servers; Windows/macOS AVA backends.

## Visual and demo sources

Link only; do not copy assets.

- OpenCode: [opencode.ai](https://opencode.ai); [v2 docs](https://opencode.ai/v2/docs); [releases](https://github.com/anomalyco/opencode/releases)
- Goose: [goose-docs.ai](https://goose-docs.ai/); [aaif-goose/goose](https://github.com/aaif-goose/goose)
- Codex / ChatGPT desktop: [introducing-the-codex-app](https://openai.com/index/introducing-the-codex-app/); [codex/app](https://developers.openai.com/codex/app)
- Claude Code desktop: [desktop](https://code.claude.com/docs/en/desktop); [desktop-linux](https://code.claude.com/docs/en/desktop-linux); [download](https://claude.ai/download)
- Cursor: [agent overview](https://cursor.com/docs/agent/overview); [run modes](https://cursor.com/docs/agent/security/run-modes)
- Zed: [parallel-agents](https://zed.dev/blog/parallel-agents); [agent panel](https://zed.dev/docs/ai/agent-panel)
- Cline: [github.com/cline/cline](https://github.com/cline/cline); [auto-approve](https://docs.cline.bot/features/auto-approve)
- Continue: [agent quick start](https://docs.continue.dev/ide-extensions/agent/quick-start)

## Source ledger

Retrieved 2026-10-02 local / 2026-10-03 UTC.

1. https://github.com/anomalyco/opencode
2. https://github.com/anomalyco/opencode/blob/dev/LICENSE
3. https://github.com/anomalyco/opencode/blob/dev/packages/desktop/README.md
4. https://opencode.ai/docs
5. https://opencode.ai/docs/permissions
6. https://opencode.ai/v2/docs
7. https://github.com/aaif-goose/goose
8. https://goose-docs.ai/
9. https://goose-docs.ai/docs/guides/managing-tools/goose-permissions
10. https://github.com/aaif-goose/goose/blob/main/ui/desktop/package.json
11. https://developers.openai.com/codex/app
12. https://developers.openai.com/codex/sandboxing
13. https://developers.openai.com/codex/environments/git-worktrees
14. https://openai.com/index/introducing-the-codex-app/
15. https://github.com/openai/codex
16. https://code.claude.com/docs/en/desktop
17. https://code.claude.com/docs/en/desktop-linux
18. https://code.claude.com/docs/en/desktop-quickstart
19. https://claude.ai/download
20. https://cursor.com/docs/agent/overview
21. https://cursor.com/docs/agent/security/run-modes
22. https://cursor.com/docs/configuration/worktrees
23. https://zed.dev/docs/ai/agent-panel
24. https://zed.dev/docs/ai/parallel-agents
25. https://zed.dev/software-overview
26. https://github.com/cline/cline
27. https://docs.cline.bot/features/auto-approve
28. https://github.com/continuedev/continue
29. https://docs.continue.dev/ide-extensions/agent/quick-start

**Limitations.** Claude and ChatGPT desktop frameworks unverified. Continue
beyond archived 2.0.0 unclear. OpenCode v1/v2 docs overlap. Feature lists are
documentation-claimed, not observed in running builds. Historical Electron-move
commentary is not a product spec.
