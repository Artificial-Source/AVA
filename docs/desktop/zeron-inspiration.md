# Zeron Inspiration

User-selected visual and interaction reference for a later optional AVA
desktop client. The user specifically likes this design. Research only: not
an AVA layout, visual spec, token set, renderer choice, or implementation
plan. Electron remains the accepted host direction
([desktop index](README.md)). No runtime was installed and no GUI was run.

**Retrieved:** 2026-10-02 (machine local); 2026-10-03 UTC.
**Commit:** `1c83cda221bd18a3b55ea49e5b25ec2c148955bc`
([tree](https://github.com/zeronsh/zeron/tree/1c83cda221bd18a3b55ea49e5b25ec2c148955bc)).
The published screenshot is not current-UI certification. A comparative
checkout remains at `docs/reference-code/zeron/` and is outside the
documentation catalog, packages, and link graph. Do not copy source, assets,
fonts, or trademarks. Licensing was excluded; this note states no license.
Do not weaken [architecture](architecture.md), [RPC v1](../rpc-protocol.md),
or [product principles](../product/principles.md). See also
[desktop inspiration](desktop-inspiration.md).

## Observed published screenshot

The full published image (2560x1655) was visually inspected. That size is
reference evidence, not an AVA layout constant.
[README.md L1-L9](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/README.md#L1-L9)
calls it a live branch-diff sidebar. Link only, not an embedded asset:
[app-screenshot.jpg](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/apps/landing/public/assets/app-screenshot.jpg).
Historical Comet labels mean this is not a pixel-identical current-UI claim.

- Mostly continuous dark, tinted canvas. Conversation and review meet at a
  thin seam, not nested cards. Purple wallpaper outside the window is not an
  AVA background or branding choice. The image does not prove translucency.
- Compact title/context row: navigation, session title, muted workspace or
  device context. No large logo or heading competing with the content.
- Assistant output is open prose. User turns are subdued rounded bubbles.
  Role comes from placement and treatment, not a card around every paragraph.
- Tool activity is quiet, expandable-looking one-line rows, for example
  "Ran 8 commands · edited 1 file · read 5 files". The image does not prove
  expansion or focus. Reading rhythm is generous; links and inline code stay
  distinct but secondary.
- One bottom composer: agent/model/effort, attachment, and send. Checkout and
  branch metadata sit in a subdued footer, not a dashboard.
- Full-height adjacent diff pane: short toolbar, changed-file and count
  summaries, expandable-looking file rows, line numbers, red/green hunks.
  Denser than the transcript, and still beside it rather than a second app.
- In-app palette is restrained relative to the wallpaper. macOS traffic-light
  controls evidence the pictured host, not faux macOS chrome on Linux.

Image hypotheses only: content-first hierarchy, continuous surfaces, local
progressive disclosure, quiet metadata, and conversation next to results. Do
not prescribe geometry, colors, fonts, radii, controls, or features. Contrast,
keyboard/focus, reduced motion, scroll, Linux behavior, resize, and frame
time are not established here.

## Source substantiation

Source-described behavior only. Not measured smoothness, accessibility, or
performance, and not a reason to adopt Zeron's engine, row model, or event
authority. A renderer would display and request; the C++ backend remains
authority ([process ownership](architecture.md#process-ownership)).

- **Not Electron.** `crates/ui/Cargo.toml` depends on `gpui`, `gpui_platform`,
  `gpui_tokio`, and `gpui-base`
  ([L60-L64](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/Cargo.toml#L60-L64)).
  Framework fact only. Not a stack or license recommendation.
- **Optional Changes pane.** `shell.rs` describes a collapsible drag-resizable
  sidebar and an optional right Changes pane, hidden by default, with
  persistent width and collapsed state, and double-click handle reset
  ([L1-L12](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/shell.rs#L1-L12)).
  `right_pane_open` defaults false beside `right_pane_width`
  ([L1008-L1009](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/settings.rs#L1008-L1009)),
  so the screenshot's open pane is not the mandatory default. No width number
  is an AVA constant. `escape_stops_active_agent` defaults false
  ([L1013](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/settings.rs#L1013)):
  a stored preference, not an AVA cancel policy and not a safety finding.
- **Transcript.** `transcript.rs` describes virtualized block rows, a user
  bubble versus assistant markdown blocks, consecutive tool groups, and
  streaming row identity; wheel or touch reading breaks bottom pin
  ([L1-L26](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/transcript.rs#L1-L26)).
  Not a smoothness, scroll, or accessibility certification.
- **Tool summary.** `tool_group_summary` groups commands, changed files,
  reads, search, fetch, todo, other activity, and failure segments into one
  summary such as "Ran N commands · edited N files ..."
  ([L442-L516](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/proto/src/view.rs#L442-L516)).
  That is the shared summary, not proof of expansion, focus order, or
  keyboard disclosure. Do not reproduce the function or adopt its row model.
  Approval semantics were not established; this note does not call Zeron safe
  and does not assert that approvals are absent.
- **Composer and pickers.** `composer.rs` describes compact and expanded
  modes, Send/Queue/Stop morphing, send-failure recovery, per-chat drafts,
  and a question wizard that replaces the composer while input is awaited
  ([L1-L8](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/composer.rs#L1-L8)).
  Source description only; not permission-policy equivalence. `pickers.rs`
  describes model/harness and reasoning options, repo and branch pickers,
  staged draft selections, and skeleton or error retry
  ([L1-L12](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/pickers.rs#L1-L12)).
  Not a decision about AVA controls.
- **Changes viewer.** `changes.rs` describes file, hunk, and line grouping and
  preparing, clean, list, and error states that can keep the last content.
  Scopes include working tree, branch changes, and latest turn; views include
  unified and split
  ([L1-L28](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/changes.rs#L1-L28)).
  Not an API or architecture to copy.

Zeron's daemon, remote-device sync, harnesses, embedded browser, and specific
worktree architecture/lifecycle are not adopted from this inspiration. The user
separately approved parallel chats and optional user-enabled Git worktrees;
see [canonical AVA scope](README.md#agreed-desktop-v1-scope).

## Candidate patterns

Hypotheses for later review, not a layout or spec.

1. **Continuous surface.** A thin seam can separate conversation from review
   without card-wrapping every block. Do not freeze screenshot colors, type,
   radii, or window chrome.
2. **Quiet disclosure.** One-line summaries and a single composer can keep
   metadata nearby only if failure and question states stay truthful to RPC
   and keyboard-reachable. Expansion and focus are unverified.
3. **Optional results beside chat.** A remembered review pane need not be the
   default. Collapsing it must not hide permission, cancel, or question
   states, or move authority off the backend
   ([fail closed](architecture.md#cancel-permission-and-question-states-fail-closed)).

## Open questions

- What still holds on qualified Linux x64 once contrast, keyboard focus,
  reduced motion, resize, scroll stability, and frame time are measured?
- Which RPC session, permission, cancel, and question states must stay
  visible if a composer morphs or a review pane is collapsed?
- What evidence would separate this published screenshot from the current
  UI, given the historical Comet labels?
