# Window and Conversation Navigation Research

**Retrieved 2026-10-04. Initial product approval 2026-10-04; later prototype
feedback supersedes the horizontal-tab choice with sidebar-only Pinned/Recent
navigation. Not shipped behavior or full-client implementation approval.**
This compares project/session navigation, conversational tabs, and optional
windows. It is not a mockup, layout, stack selection, implementation plan, or
claim that these features exist in AVA. Official docs and pinned public source
were inspected; no competitor app was installed or run and no account/provider
was used. Documentation claims and source observations are not runtime,
accessibility, or performance certification.

The [agreed brief](README.md#agreed-desktop-v1-scope) requires independent
parallel chats, including the same project, with a shared working copy by
default and explicitly user-enabled Git worktrees. The approved direction below
adds multi-project navigation, optional windows, and sidebar-only Pinned/Recent
shortcuts. The original horizontal working-set proposal is superseded.

## Comparison

'Not established' means insufficient evidence in the reviewed material, not
that a product lacks the feature. Chat tabs are distinct from product-mode,
file, diff, browser, terminal, and split-pane tabs.

| Reference | Verified navigation pattern | Conversational tabs | Additional windows |
| --- | --- | --- | --- |
| **Goose** — official docs [1] | Multiple active chats in one window; the sidebar switches among recent sessions; directory switcher selects the working directory. | `Ctrl/Cmd+T` means a new session in the same window, not proof of a browser-style chat-tab strip. | Explicitly documents sessions in new windows. Strongest direct example of both workflows. |
| **T3 Code** — pinned docs/source [3,4] | All-projects/selected-project scope and thread rows; pinning and working/approval/input state. Start in background opens another draft while the submitted thread runs. | A separate conversational-tab strip was not established; sidebar navigation is verified. | A first-class project-window workflow was not established. |
| **Zeron** — pinned source [5,6] | Sidebar session list; configurable project/device/one-list organization and project scope. | `shell/tabs.rs` explicitly says the horizontal session-tab strip is gone and the sidebar is the session list. Right-pane surface tabs are different. | A native multi-project-window workflow was not established; no daemon architecture is selected for AVA. |
| **Claude Code Desktop** — official docs [2] | Parallel sidebar sessions; two sessions can be viewed in separate panes; unviewed completion produces notifications. | Chat/Cowork/Code product modes and transient side chats are not a persistent conversational-tab strip. | Panes can pop out into windows; this does not establish independent project/session windows. |
| **Codex / ChatGPT desktop** — official docs [7,8] | Projects organize related chats; pin/search/archive; navigation includes the next chat needing attention. | 'Previous chat or tab' and 'close current tab or window' do not clearly establish a persistent conversational-tab strip. | Independent project-window workflow not established by these docs. |
| **OpenCode Desktop v2** — pinned source [9] | This narrow window-source check does not certify full project/session navigation. | Not established in this check; TUI Tab mode-switching is not chat-tab evidence. | `window.new` dispatches to `createWindow`; verifies a menu-action path, not run/session ownership across windows. |

## Approved navigation direction

**One default window, not a one-window restriction.**

1. **Default to a multi-project window.** Do not force a new window or app
   instance for every project. Keep project/chat identity clear when switching.
2. **Use the sidebar as the complete library.** Organize projects and their
   chats, including inactive and attention-required work. This is a display of
   backend session state, not a new transcript authority.
3. **Use sidebar-only chat navigation; no horizontal chat tabs.** After viewing
   the React prototype, the user removed the previously approved working-set
   strip. Optional **Pinned** and **Recent** chat sections belong below Projects
   in the sidebar and may span projects. Pins are explicit user choices;
   recency must not silently pin every viewed or saved chat. All shortcut views
   refer to the same existing chat, not new sessions/runs. Exact restoration and
   recency policy remain full-client presentation decisions. Zeron's removed
   horizontal strip is comparison evidence, not architecture adoption.
4. **Offer an optional additional window for focus or another monitor.** It
   could focus a project without requiring a separate uncoordinated app
   instance. Viewing an existing chat in another window should not duplicate
   its agent/run. Optional windows are approved for v1; exact ownership,
   routing, and lifecycle require design and validation.
5. **Separate view navigation from execution.** Switching chats or removing a
   sidebar pin/recent shortcut should neither cancel work nor delete/archive its
   session. Stop
   remains explicit. Window close and application quit are separate lifecycle
   questions; do not silently promise a background daemon or unattended work
   after the app exits.

This fits the focused coding-agent brief without adopting editor tabs,
docking/split workspaces, bundled terminals, cloud sync, or competitor servers.
For more UI observations see [T3 Code](desktop-inspiration.md#t3-code-project-and-thread-organization)
and [Zeron](zeron-inspiration.md).

## Boundaries and remaining decisions

- The multi-project default, optional windows, and sidebar-only Pinned/Recent
  navigation are approved product direction. The mock-data React prototype is
  separately authorized; full-client implementation is not.
- Project identity/grouping, session routing, duplicate-chat views across
  windows, sidebar pin/recent restoration, and active-work behavior when the last
  window closes or the app quits remain open. Worktree
  creation/selection/lifecycle ownership also remains open.
- Competitor hide/archive/close behavior is not generally proven to keep a
  live run working. T3's documented start-in-background and Claude's unviewed
  completion notification are narrower positive evidence, not a universal
  close-window guarantee.
- Preserve [public RPC admission](architecture.md#one-active-run-and-interleaving):
  one active top-level run per backend process. Window/view counts do not grant
  concurrent admission on that process. Backend lifecycle/routing is still to
  be designed and validated; permissions, resolver ownership, session leases,
  and renderer sandbox remain unchanged. Only Linux x64 backend is qualified;
  competitor Windows/macOS shortcuts are not AVA support claims.

## Primary sources

Retrieved 2026-10-04; source pins are comparison inputs, not adopted code or architecture.

1. [Goose session management](https://goose-docs.ai/docs/guides/sessions/session-management/) — multiple active sessions in one window; sessions in new windows; shortcut distinctions.
2. [Claude desktop](https://code.claude.com/docs/en/desktop) — Manage sessions, Arrange workspace, side-chat persistence, pop-out panes, unviewed notifications.
3. [T3 Code Sidebar.tsx](https://github.com/pingdotgg/t3code/blob/4ee6bfd50ef4a089440d5c3662db2298da9cc50e/apps/web/src/components/Sidebar.tsx) — project scope and thread navigation/status.
4. [T3 Code working with threads](https://github.com/pingdotgg/t3code/blob/4ee6bfd50ef4a089440d5c3662db2298da9cc50e/docs/user/thread-sidebar.md) — Start in the background, pinning, Working section.
5. [Zeron shell/tabs.rs](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/shell/tabs.rs) — removed horizontal session strip; sidebar navigation.
6. [Zeron settings.rs](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/settings.rs) — SidebarOrganization and space_filter; observed default InOneList / all spaces.
7. [OpenAI projects and chats](https://developers.openai.com/codex/projects) — local project folders and distinct-outcome chats; pin/search/archive.
8. [OpenAI desktop commands](https://developers.openai.com/codex/reference/commands) — previous/next chat or tab, attention jump, close tab/window; mixed product surface, not proof all tabs are conversations.
9. [OpenCode menu-actions.ts](https://github.com/anomalyco/opencode/blob/740072694de71fae23f35131591e7c32a9713014/packages/desktop/src/main/native/menu-actions.ts) — window.new invokes optional createWindow handler.
