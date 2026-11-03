# 06/01 — Source-verified state and control-flow inventory

Baseline: plan branch `06-TuiFrontEnd_run_refactor`; runtime sources equal develop commit d1b6db7b. This document records source inspection, not behavioral validation. Line references refer to this baseline. Required inputs: 06/handoff.md and this node's description, read together with repository rules. The supplied handoff is confirmed below and extended by the idle-dispatch and active-run inventories.

## Run-lifetime declarations and aliases

Source: `src/ava/tui/TuiFrontEnd.cpp:134–565`.

| Declaration | Lifetime, readers/writers and proposed ownership |
| --- | --- |
| before_shutdown | First constructed RAII guard invokes frontend shutdown idempotently on every exit; session lifecycle, must outlive all dependent owners. |
| application, signals_manager, terminal_context | Borrowed singleton/application services. Application outlives TUI thread. Preserve thread assertions, signal activation, ncurses ownership and return-to-application cleanup. |
| graphics_cleanup | Clears composer terminal graphics on exit while Context is alive. Lifecycle resource, not a feature-state accessor. |
| initial_status_line | Initialized by keybindings/display loading, consumed by RuntimePresentationState constructor. Initialization local only. |
| terminal_background_detection | Resets response handler, detected appearance and startup FIFO on construction/destruction. Startup probe is scoped before initial presentation; no reprobe on resume. |
| presentation_state | Owns snapshot/sidebar, image attachments and command session grants. RuntimePresentationState already has backend hydration operations but publishes writable fields; distinguish screen projection from backend authority. |
| snapshot, sidebar, command_session_grants | Reference aliases to presentation_state fields; no new storage. Read by drawing, menus, navigation; mutated by feature publication, backend event application and coordinated transition. |
| plugin_ui; plugin_ui_shutdown | Coordinator plus local pointer-bag guard; guard calls shutdown(snapshot) before presentation destruction. Prefer scoped coordinator/lifetime owner retaining same teardown order. |
| terminal_write_failed, terminal_signal_received | Run outcomes written by idle dispatch and submit adapter; session result. Do not conflate SIGTERM/SIGINT policy or paint failures. |
| draft_state | Persistent editor/history/selection/palette state. Proposed Composer owner, sensitive text excluded from automatic debug printing. |
| input_history, history_index, draft_input, draft, jump_mode, selected_slash_command_index, slash_palette_suppressed, path_completion_force_active, draft_scroll_offset, draft_selection_anchor, draft_selection_cursor, pending_escape_clear | Aliases to draft_state. RuntimeDraftState also owns mouse_selecting, used through member operations. Group into editor invariants rather than one setter per scalar. |
| renderer | Terminal drawing, caches, viewport and frame scheduling currently mixed. Drawing remains renderer-owned; transcript interaction must gain operations, not mutable cache getters. |
| navigation | Borrows frontend, snapshot/sidebar/draft/renderer, mixes completion and transcript navigation. Evolve these into composer and transcript collaborators; immutable frontend reads stay const. |
| transcript_scroll_offset, completion_cache | Aliases to renderer public state; mutable cache edges are not read-only accesses. |
| active_select_list | Scalar tracks the kind independently of snapshot.select_list; SelectorHost must own the kind/view transition invariant. |
| prompt_stash | Focused existing controller borrowing presentation/draft/renderer/selector/keybindings; retain through narrow collaborators. |
| branch_summary_ui | Persistent active/exit_requested/generation/phase plus prior view/index/source restoration checkpoint. BranchSummaryOperation. Generation rejects stale snapshots; cancellation request and settlement are separate. |
| branch_summary_exit_ready | Separate session outcome currently set during settle; replace with explicit operation outcome. |
| settings_nav, settings_session_open | Navigation section/root checkpoint and display preview transaction plus independent open flag; SettingsDialog should own both. |
| clear_theme_preview_on_exit | RAII cleanup; SettingsDialog destruction should cancel overlay without backend writes. |
| transcript_search | Existing focused search collaborator; retain, with transcript-layout change API and selector lifecycle cooperation. |
| session_archive_confirmation | Optional exact backend session identity/action checkpoint; SessionSelector owns confirmation, backend retains permission and archive authority. |
| prompt_coordinator | Owns synchronized pending permission/question request queues. Resolver closures may run on worker threads; presenter operations remain TUI-thread-only. |
| permission_resolver, question_resolver | Retained callback values produced by coordinator, unused in outer loop. Audit assignment path before removal: frontend/active submission must not retain a target after coordinator lifetime. |
| mermaid_presentation | Existing focused renderer-service controller; its service lambda also owns viewport anchoring orchestration currently. |
| action_controller | Existing backend-facing UI action adapter; no backend authority transfer. |
| subagent_workspace | Existing path-free coordinator snapshot/inspector presentation owner; periodic polling and deadlines matter to later scheduling. |
| active_run_controller | Borrows all active collaborators; run() blocks outer dispatcher in a second loop. See active inventory below. |
| submit_controller | Submission parsing/routing and synchronous active_run_controller.run call; preserve local command semantics while turning async submission into a serviced operation. |

## Outer orchestration lambda read/write/call matrix

All outer lambdas capture by reference; actual dependencies, not capture syntax, determine ownership.

| Lambda(s) / source | Reads | Writes / external calls | Proposed destination |
| --- | --- | --- | --- |
| clear_settings_preview 235 | settings preview | cancel; apply_image_overlay(snapshot) | SettingsDialog cancellation |
| begin_settings_preview_baseline 239 | snapshot image/cursor settings | preview.begin(authoritative copy) | SettingsDialog open |
| ensure_settings_session 243 | open flag | navigation.reset, preview baseline, open flag | SettingsDialog invariant |
| rebuild_settings_view 250 | snapshot/sidebar, bindings, preview baseline | transient theme clear/restage; view publication, kind, section, overlay | SettingsDialog, SelectorHost publication |
| apply_settings_highlight_preview 274 | kind/view/index/action, display section | preview update/cancel, snapshot image overlay | SettingsDialog highlight |
| close_settings 297 | preview/navigation | view reset, kind None, preview/navigation reset, open=false | SettingsDialog close via SelectorHost |
| render 309 | renderer | renderer.render | Delete forwarding adapter |
| restore_branch_summary_selector 311 | prior view/source/index | replace snapshot.select_list and kind | BranchSummaryOperation + SelectorHost |
| present_branch_summary_status 316 | optional view | snapshot.status and subtitle | BranchSummaryOperation status publication |
| settle_branch_summary 321 | operation refresh/phase, prior selector, requested exit | on_branch_summary_refresh_catalog; restore; active/phase; exit flag or request_render | BranchSummaryOperation settlement result |
| apply_branch_summary_snapshot 356 | active/generation/operation | phase, terminal settlement or operation view/kind/render request | BranchSummaryOperation consume |
| poll_branch_summary 367 | active | frontend snapshot; synthesize failed operation on exception; settle/status | BranchSummaryOperation service |
| begin_branch_summary 385 | source/view/index | frontend prepare; generation/phase/checkpoint/active; apply snapshot | BranchSummaryOperation start |
| cancel_branch_summary 409 | active/generation | accumulated exit request; frontend cancel; status; poll | BranchSummaryOperation cancel |
| confirm_branch_summary 431 | active/phase/generation | frontend confirm; status; poll | BranchSummaryOperation confirm |
| service_mermaid_presentation 448 | snapshot sizes/generations; renderer detached status and scroll | layout-cache fill, Mermaid snapshot mutation, search refresh, detached anchor synchronize, full invalidation | Mermaid owner with TranscriptView layout-change operations |
| maybe_reload_display_settings 473 | action reload result, settings checkpoint/view/overlay | backend-local display hydration; preview rebase/rebuild/reselect/highlight; paint | Session display delivery + SettingsDialog rebase |
| clear_draft_for_interrupt, open_external_editor, suspend_to_background, paste_clipboard_image, cycle_reasoning, toggle_thinking_visibility, open_model_selector, open_reasoning_selector, open_scoped_model_selector, open_session_selector, cycle_model 519–536 | action controller | forward only | Direct calls; no duplicate methods |
| toggle_startup_overview 529 | settings session | close settings then action | Explicit competing-modal transition |
| slash_palette_active, file_reference_palette_active, path_completion_palette_active, completion_match_count, clamp_completion, previous_completion, next_completion, selected_completion_disabled_reason, selected_completion_text 538–546 | navigation and its cache | some cache population | Direct owner calls; completion cache owned by composer/render layout collaboration |
| scroll_up/down, toggle_tool_details_at, toggle_thinking_at, handle_sidebar_drawer_input, jump_to_bottom, scroll_to_message_boundary 548–556 | navigation state | transcript/sidebar interaction mutation | Transcript owner operations |
| handle_submit 560 | submit controller | submit result to two session flags; ASSERT signal implies BreakLoop | Session accepts explicit outcome |

## Read-only versus mutable sharing: concrete examples

* `TuiFrontEnd.cpp:287` binds `auto const& item = snapshot.select_list->items[index]`: pure read with event-local lifetime. Pass const view/item, do not duplicate the entire snapshot merely to access an action token.
* `:252–257` copies snapshot and overlays authoritative preview values to compute settings rows. This copy has deliberate projection semantics: preview must not rewrite authoritative candidate labels. A const projected input can replace copying only if the same distinction is retained.
* `:267–272` changes view, kind, section and preview publication together. Independent mutable getters would recreate the broken transaction boundary.
* `:453–460` calls `composer_max_transcript_scroll_offset_cached` with `renderer.transcript_layout_cache` and `completion_cache`. Although returning a number, this populates caches. `capture_transcript_viewport_anchor` then purely reads the populated layout. Keep the pure calculation free, put cache preparation in its owner.
* `:462–466` refreshes search after Mermaid transcript mutation, defers detached anchoring and synchronizes layout before invalidation. This is coordinated mutation across owners; expose a layout-change transaction instead of writable cache references.
* `:499–514` reloads authoritative display state before reapplying preview and reselecting by exact action identity. Equality of overlaid values cannot substitute for the explicit Applied signal.
* RuntimeDraftState operations mutate draft plus selection/history/palette invariants; currently consumers also edit public fields. Composer encapsulation must migrate those consumers, not merely put the old state bag behind a mutable getter.
* RuntimePresentationState::read_boundary_data is deliberately outside UI locks; apply_boundary_data only applies previously read transport data. Preserve this authority/locking split.

## Construction / cleanup order and lifecycle constraints

Before-shutdown guard is first. Terminal context belongs to Application and remains external. Graphics/startup detection guards precede presentation. Plugin shutdown guard is after presentation and plugin coordinator, so both are alive during cleanup. Renderer follows draft/presentation; navigation, stash, search and prompts follow their dependencies. Actions/workspace precede active-run; submit is last. Reverse destruction must retire the submission/future/event sinks and pending prompts before destroying presentation, terminal resources or shutdown guard. Normal tail explicitly clears Mermaid/plugin resources and invokes before_shutdown; guards cover early returns and exceptions. Preserve idempotency rather than deleting duplicate cleanup blindly.

All new owners should be TUI-thread-confined. Worker callbacks publish queued data only. Mutable access remains justified only for independently encapsulated collaborators whose operations maintain their own invariants. Exact class API and cycle resolution belongs to goal02; no architecture is claimed approved here.

## Baseline executable evidence

`source /workspace/ava-tools/activate.sh; cd /workspace/AVA; scripts/build.sh` exited 0 on the plan branch (log `/workspace/ava-tools/goal01-build.log`). Existing dev CMake cache: BetaTest, EnableDebug=ON, tests=ON, GCC16.2, Ninja, CMAKE_CXX_SCAN_FOR_MODULES=OFF. Compilation succeeded before this analysis; no runtime sources changed for goal01. No CTest executed, as requested. Earlier onboarding CLI help/version succeeded but does not validate TUI behavior.
# Goal 06/01: outer-loop dispatch inventory

Source baseline: `src/ava/tui/TuiFrontEnd.cpp` before any runtime refactor; line anchors below refer to the plan branch's unchanged implementation. This is analysis only. No build, CTest, runtime verification, Git mutation, or tracked-file edit was performed by this analysis.

## Scheduling and modal precedence

The outer `while (true)` starts at 573. It has one timed input read but also calls submission synchronously, which suspends this loop while the active controller runs its own loop. The two loops must eventually be integrated; changing only the wait at 606 is insufficient.

Before the loop, a received terminal signal returns 130 (568–569); failing initial Mermaid service or initial paint returns 1 (570–571). Each iteration services Mermaid (576), branch-summary polling (581), deferred branch-summary exit (586), due renderer work (588), and subagent workspace polling (594), in this order. The input wait starts at 250 ms, shortened by workspace and pending-render deadlines (599–605), then `read_curses_input_with_timeout` runs (606). On no decoded input, workspace poll, due render flush, and display-settings reload occur (607–619); this path does **not** reach the signal check at 622. `runtime_input.cpp:292–304` consumes the startup FIFO before invoking the terminal reader with an RAII timeout; an empty decoded event becomes `nullopt`. Thus signal handling cannot be assumed to occur on every outer iteration. Preserve or explicitly resolve the distinction when designing the event loop; do not silently change signal/thread policy.

For a decoded input, routing is strictly ordered:

1. Pending SIGTERM/SIGINT (622–679). Claim SIGTERM first. While a branch summary is active, cancel it and defer exit for SIGTERM; SIGINT requests cancellation without exit. With no branch summary, SIGTERM exits regardless of draft; SIGINT exits with an empty draft, otherwise clears the draft and paints. Signal exit records `terminal_signal_received`.
2. Resize (680–690): reset wheel governor, refresh transcript search, paint, consume input.
3. Wheel burst suppression (691–692): rejected wheel event is consumed before any modal handler.
4. Command-output modal only when neither permission nor question prompt exists (693–729). Dismiss/reset status or update output scroll and request a frame. Character, Space, or CtrlU dismisses then falls through to ordinary routing; other unhandled events are swallowed, including pointer input.
5. Active subagent workspace (730–741): handle, optional beep, request render if changed, consume every event.
6. Active branch-summary operation (742–767): classify Exit/Cancel/Confirm/Block, invoke operation or beep, consume every event. The switch's `break`s are not event-loop exits.
7. Clear reasoning/local-command feedback (768; writes both optional fields in `runtime.cpp:14–17`). Transcript search gets first refusal (769–777).
8. Prompt-stash selector gets next refusal (778–786).
9. Any generic selector (787–1587), including settings/session/scoped models and other selectors. It always consumes the event after the common render request, whether or not a specialized action matched.
10. Sidebar drawer gets next refusal (1588–1597).
11. Composer jump mode (1689–1725): toggle jump cancels, printable input seeks the target, other input clears jump state then falls through. Printable/jump-key input is consumed after render.
12. Ordinary action chain (1748–2458; priority details below).
13. Publish selected completion index to snapshot and request frame (2459–2464).

Permission/question prompts have no direct handler here; they are serviced in the active-run path. The command-output modal guard is still significant and must remain explicit.

### Selector subrouting

At 789–797, settings starts a preview session; replacement by another selector clears preview/navigation/open state. Input classification at 798–854 performs mouse hit testing first (Settings clicks only highlight; other enabled rows resolve), scoped-model key overrides second, and pure `handle_select_list_input` last.

Actions run in this order: redraw (912), settings cancel (920), settings resolve (946), scoped-model toggle (1108), enable all (1130), clear all (1143), provider toggle (1156), reorder (1179), save (1202), session summarize parent (1215), sort (1237), named filter (1241), path display (1245), archived filter (1249), label timestamp (1253), rename-to-draft (1257), labels-to-draft (1279), parent/child navigation (1301), archive/unarchive confirmation (1332), generic resolve/cancel (1381). The final render request and continue are at 1581–1586.

Settings resolve includes section entry with a saved root frame (955), models/scoped/reasoning selector replacement (965–978), hotkeys/edit/reload commands (980–1013), drafting feature commands (1015–1044), unavailable-row handling (1046), local display-setting confirmation (1051), and backend settings confirmation (1071). Cancel returns to the saved root query/selection from a section, or closes at root (920–944). Preview is cleared before authoritative confirmation and rebased on result; failed persistence remains open with current authoritative baseline (1054–1068, 1075–1104).

The generic resolve/cancel branch snapshots `resolved_list`, closes the visible selector, resets active kind and archive confirmation, then dispatches cancel, settings special cases, hotkeys drafting, model switch, reasoning selection, session open, user-turn fork/copy, settings action, or generic close (1385–1579). Some settings branches repeat operations already handled by the earlier settings-specific branch. Keep this overlap visible during migration rather than inferring those paths are unreachable without proving it.

### Composer/action-chain precedence

The chain is ordered, not an unordered key map; binding collisions obey first match:

- CopyLatestAssistant; PromptStash; Character insertion; MessageFollowUp submission; NewLine; ExternalEditor; Suspend; ClipboardPasteImage (1748–1807).
- JumpForward/Backward; DeleteBackward, DeleteForward, DeleteWordBackward/Forward, DeleteToLineStart/End; copy draft selection before transcript selection; ClearInput (1809–1909). CtrlD only becomes forward-delete for a nonempty draft (1726–1727).
- AutocompleteAccept: slash, file-reference, path, then forced path lookup (1910–1928).
- ModeToggle; Interrupt; Exit; VariantCycle; ReasoningSelect; ThinkingToggle; OverviewToggle; ModelSelect; ModelCycleForward/Backward; SessionResume/Tree; SessionNew/Fork; MessageDequeue (1929–2029).
- Half-page/page transcript scroll; message boundary; live tail; wheel scroll (2030–2065).
- Pointer dispatch: completion palettes first; transcript selection second; composer cursor/selection last (2066–2146). Completion hit tests order slash/file/path. Ctrl/Shift selection extension then CtrlHome/CtrlEnd and left/right/line/word cursor actions (2147–2204).
- PalettePrev slash/file/path before HistoryPrev, raw ArrowUp, and CursorUp (2205–2276). PaletteNext slash/file/path before HistoryNext, raw ArrowDown, and CursorDown (2277–2348). A failed history movement falls back to transcript scrolling; raw ArrowUp/Down precede configurable CursorUp/Down checks.
- Undo, Redo, Yank, YankPop; DetailsToggle; unavailable PromptAllow/Deny status; Cancel (2349–2437).
- Submit first handles backslash-Enter newline; otherwise synchronous submit disposition controls flow (2438–2454). Space insertion is last (2455–2458).

Cancel has its own precedence (2389–2436): draft selection, transcript selection, any completion palette, nonempty draft (first Escape arms clear, second clears), otherwise ignore. Neither terminal Interrupt nor key action Interrupt should be conflated with this.

## Per-event lambdas and captured-state edges

All the following are created during one dispatch; they are not persistent state. Read/write refers to their transitive behavior, not just explicit assignment. Where captures are `[&]`, the table lists actual dependencies rather than claiming the entire stack is used.

| Lambda / anchors | Reads | Mutates / calls | Natural ownership evidence |
| --- | --- | --- | --- |
| `input_result` initializer 798–854 | input, snapshot selector/items/query/dimensions, active kind, keybindings | Returns a value only; mouse geometry helper and pure selector-input helper | Host input classification; current event stays a parameter |
| `scoped_action` 815 | keybindings, input key, action parameter | Pure key matching | Delete adapter or keep pure per-event query |
| `preserve_session_selector_state` 855–878 | input result, current selector selected value | Clears archive confirmation, rebuilds next-view query/selection by value, clamps, replaces snapshot selector/status | Session selection owns confirmation; generic host may own view preservation algorithm. Also used for scoped-model views, so name is misleading |
| `selected_select_list_item` 879–883 | selector and event-selected index | Returns borrowed const item pointer | Read-only host lookup, valid only until selector mutation |
| `visible_select_list_values` 884–899 | selector plus current query/index | Copies view, filters, returns owned enabled nonempty values | Pure projection; no mutation of source snapshot |
| `settle_opened_session` 900–906 | frontend committed-session ID, status parameter | Broad transition/reset-and-refresh over presentation, draft, renderer, search, workspace, active kind; settles command status | Session-level coordination; backend ownership is not transferred |
| `synchronize_committed_session` 907–911 | frontend committed-session ID | Same multi-owner transition without backend refresh; returns whether ID changed | Session-level coordination on both success and error |
| branch authority callback 1314–1317 | `input_result.action`, `selected_value`, frontend | Calls backend parent/child operation synchronously | Frontend/backend authority boundary, narrow method call |
| model/reasoning/session callbacks 1455, 1476, 1490 | selected value or owned reasoning level, frontend | Calls backend selection; reasoning moves local level | Genuine callback boundary, synchronous today; do not retain per-event captures |
| copy-user-turn reader 1538 | `this`, entry ID parameter | Calls `on_read_user_turn_text` through evaluate-copy function; clipboard callback separately supplied | Frontend authority stays external |
| `is_action` 1598 | keybindings/current event key | Pure key matching | Event-local calculation |
| `select_slash_command` 1599–1616 | draft, slash catalog, selection | Clamp index; disabled status/beep or selection clear/draft replace; reset completion/scroll/history/saved draft; set status | Composer operation with explicit read-only catalog and status/beep result |
| `select_file_reference` 1617–1634 | completion model/draft/selection | Completion queries refresh cache/snapshot; disabled status/beep or selection clear/draft replace; reset state/status | Composer plus completion cache owner; queries are currently not const |
| `select_path_completion` 1635–1652 | as file-reference selection | Same writes, different status | Same owner; avoid duplicated mutable-ref adapter |
| `force_path_completion` 1653–1688 | suppression, completion count/model | Temporarily enables path mode, refreshes cache, restores flags on zero matches; clears Escape/history/saved draft/index; accepts single match or leaves palette; status/beep | Composer invariant: zero-match attempt restores suppression |
| `insert_input_text` 1729–1747 | current decoded text/character/bracketed-paste bit, draft selection | Clears Escape/history/saved draft/completion index/suppression/scroll; paste deletes selection then inserts safely; ordinary text replaces selection or inserts; optional status | Composer edit operation; decoded event stays a parameter |

Additional callbacks supplied at 2103 (`toggle_tool_details_at`, `toggle_thinking_at`) are outer-scope forwarding lambdas passed into renderer transcript mouse handling. They cross renderer/navigation ownership and should be considered with transcript extraction, not mistaken for per-event persistent objects.

## Snapshot/renderer read versus mutation evidence

- Read-only selection: `select_list_selection_for_screen_position(ComposerSnapshot const&,...)` (`composer.cpp:2315`), `clamp_select_list_selection(SelectListView const&,...)` (`composer_select_list.cpp:366`), and `handle_select_list_input(SelectListView const&,...)` (`composer_select_list.cpp:419,614`) compute values. They do not require a mutable snapshot owner merely to share data. The caller commits result at `TuiFrontEnd.cpp:914–918`.
- Selector lifecycle is currently split: `snapshot.select_list.reset()` and `active_select_list = None` occur together at 1263–1264, 1285–1286, 1392–1395, but `dispatch_tui_selector_authority` (`runtime.cpp:20–34`) resets **only** the snapshot selector, writes status, renders, then synchronously invokes backend callback. It does not know active kind. A host operation must own this consistency without moving backend policy into the host.
- `preserve_session_selector_state` writes selector view/status and archive confirmation; it is also used by scoped-model actions at 1121/1135/1148/1170/1193. Confirmation clearing belongs to session-selection behavior; generic preservation should not depend on archive state.
- Completion-looking queries are not read-only today. `RuntimeNavigationController::refresh_completion_cache` (`runtime_navigation_internal.cpp:24–31`) publishes draft text/cursor/palette flags into snapshot and mutates `renderer.completion_cache`. `file_reference_palette_active`, `path_completion_palette_active`, count/clamp/previous/next/reason/text call it (39–84). Replacing these with const accessors without separating publication/cache preparation would lose effects.
- Mouse completion hit tests at 2082–2092 consume a const-looking snapshot but take `completion_cache` mutably. Read-only view access alone does not capture this cache dependency.
- Scrolling mutates multiple domains: `RuntimeNavigationController::scroll_up/down` (`runtime_navigation_internal.cpp:87–121`) clear draft Escape state, set snapshot dimensions, synchronize renderer layout, refresh layout/completion caches, change scroll, show timed indicator, and retain/release detached sidebar snapshot. `transcript_page_size` (124–133) also mutates snapshot/cache despite returning a size. A transcript operation should own viewport/cache invariants; clearing composer Escape is explicit cross-owner dispatch policy.
- Tool/thinking toggles (`runtime_navigation_internal.cpp:136–216`) capture viewport anchor, mutate transcript item expansion and generation, refresh caches, restore scroll, and publish status. Treat the anchor/mutation/restore sequence atomically on the TUI thread. A pure anchor calculation can remain free.
- Renderer selection/copy state is mutated directly by mouse and selection handling at 2076/2085/2094/2103/2110/2149; draft and transcript selection are mutually coordinated at these call sites. Pointer operations need explicit outcomes or coordinator operations, not a mutable reference to both selection states.
- DetailsToggle at 2380–2382 synchronizes detached layout before changing snapshot's global tool-presentation mode and status. That order is part of the interaction/layout invariant.
- Session transition helper (`runtime_state.cpp:278–341`) resets grants, attachments, search, workspace, draft, renderer, transcript, tool indexes, prompt/queue/sidebar/input projection fields, selector, feedback, and committed identity, then optional refresh. This is a true multi-owner operation and belongs at composition level. Error paths at 1326/1497/1529 intentionally reconcile committed ID because a backend operation can commit before ancillary failure.
- Command-output handling reads dimensions/presentation at 695–696, writes scroll at 710, and resets command-output/status at 699–700 and 722–723. The modal's pointer swallowing is behavior, not a renderer cache concern.
- Settings writes span preview state and snapshot image overlay, local display settings, selectors, draft, and status. Examples: 1054–1062 and 1101–1103. Immutable display settings/callback outcomes can be shared; preview transaction owns rollback/rebase, while composer drafting should be an explicit operation.

## Complete outer-loop control-flow classification

Anchors below are exact `break`/`continue` statements in `TuiFrontEnd.cpp` for the pre-refactor baseline. Except for the explicitly identified nested statements, each break exits the outer event loop.

### Breaks

| Lines | Reason |
| --- | --- |
| 579, 584 | Mermaid service / branch-summary poll reports failure; sets write-failure outcome |
| 587 | Branch-summary deferred exit ready |
| 591, 597, 612, 617 | Due frame/workspace render/display-reload failure; sets write-failure |
| 648 | Signal-triggered summary cancel/presentation failure |
| 657 | Claimed SIGTERM without active summary (signal flag already set) |
| 666 | Claimed SIGINT with empty draft; sets signal flag |
| 674, 687 | Interrupt-clear paint / resize paint failure |
| 704, 714, 738 | Command-output dismiss/scroll render or workspace render failure |
| 750, 753, 756, 759 | **Nested switch only**: branch-summary Exit/Cancel/Confirm/Block dispatch completes |
| 764, 774, 783 | Branch-summary input, transcript-search, or prompt-stash handled failure |
| 871 | **Nested item-search loop only** inside preservation lambda; matching value found |
| 1233 | Starting branch-summary operation failed |
| 1463, 1517 | Post-model reasoning selector / pre-fork immediate paint failure |
| 1584, 1594 | Common selector render / sidebar handler failure |
| 1721, 1753, 1762 | Jump render / copy-assistant / prompt stash failure |
| 1774 | MessageFollowUp submit returns BreakLoop |
| 1787, 1796, 1805 | External editor / suspend / clipboard-image operation reports failure |
| 1953 | Interrupt-clear immediate render failure |
| 1957, 1961 | Empty-draft Interrupt action / Exit action; no terminal-signal flag is set solely by these key actions |
| 1974, 1988, 1996, 2014 | Reasoning / overview / model / session selector opening failure |
| 2021 | New/Fork submit returns BreakLoop |
| 2445 | Backslash-Enter render failure |
| 2451 | Ordinary submit returns BreakLoop |
| 2463 | Common tail render request failure |

`handle_submit` determines signal/write flags for its BreakLoop outcomes; do not turn every submit break into a generic normal exit. Every failure row except explicit control exits sets `terminal_write_failed = true` immediately before the break.

### Continues

- 619: no decoded input after idle service.
- 650: signal-driven summary cancellation serviced.
- 676: SIGINT cleared nonempty draft.
- 689: resize serviced.
- 692: wheel governor rejected event.
- 706,716: command-output dismissal/redraw serviced; 727: modal swallowed another event.
- 740: active workspace always consumed event.
- 766: active branch summary always consumed event.
- 776,785: search / stash handler consumed event.
- 1586: generic selector consumed event (even `None` action).
- 1596: sidebar handler consumed event.
- 1723: jump mode consumed printable/jump key.
- 1755,1764: copy-assistant / prompt-stash trigger consumed event.
- 1776: MessageFollowUp returned ContinueLoop.
- 1789,1798,1807: editor/suspend/clipboard operation consumed event.
- 1955: key Interrupt cleared draft.
- 2023: New/Fork returned ContinueLoop.
- 2447: backslash-Enter became newline.
- 2453: ordinary submit returned ContinueLoop.

There are no other continues inside this outer-loop source span. Ordinary action fallthrough reaches common snapshot index publication and frame request at 2459–2464.

### Returns and cleanup

The only enclosing-function returns in the inspected span are pre-loop 569 (130), 571 (1), and final 2471. All returns at 810–909, 1315, 1455, 1476, 1490, 1538, 1598, 1605, 1623, 1641, and 1662/1675/1683/1687 belong to lambdas/callbacks and must not become session exits during extraction.

After any outer-loop break: Mermaid shutdown (2467), plugin UI shutdown (2468), explicit guarded before-shutdown callback (2469), then return 130 if terminal signal recorded, otherwise 1 for write/shutdown failure, otherwise 0 (2471). Stack guards still unwind. Early pre-loop returns bypass the explicit tail but retain RAII cleanup. Exceptional unwinding is also distinct from ordinary loop outcome.

## Event-local state and lifetime decisions

Keep the following event-local: poll timestamp/delay/optional input, decoded input/event, claimed SIGTERM flag, command-output geometry/result, handled-optionals, branch-summary input success flag, selector classification/result, selected item pointer/value, current query/index, copied next-view/current-view, root-query/root-selection, backend result/status/receipt tokens, archive/noninvasive flags, current selection iterator/index, copied visible-value vector, completion result/match count/disabled reason, jump target/direction, CtrlD classification booleans, pointer phase/palette-claimed flag, cursor positions, submit disposition. They are recomputed from a particular event and should become parameters/locals, not persistent members.

Borrowed pointers returned from `selected_select_list_item` cease to be valid when snapshot.select_list is reset or replaced. Existing branches copy `selected_value` before authority helpers close the selector (1313, 1389); preserve that lifetime. Similarly event-capturing authority callbacks are synchronous under `runtime.cpp:31`; asynchronous migration must copy their input values instead of retaining references.

## Decisions needing goal 02 or goal 07 resolution

1. Where is the authoritative selector kind/view pair stored? Existing split reset in authority dispatch demonstrates why independent writable fields are unsafe. Rendering can borrow a const view, while a host commits lifecycle changes.
2. Who owns completion caches versus composer state? Existing query methods both publish draft projection and mutate renderer cache. Choose an explicit preparation/query split or a cohesive completion owner; do not describe these methods as const today.
3. How does transcript interaction request layout and preserve detached anchors without obtaining renderer's raw caches/scroll fields? Tool/thinking expansion, Mermaid layout changes, pointer selection, and live append all need the same invariant.
4. Preserve session transition ordering, including error-after-commit reconciliation; a SessionSelector must request backend work and let composition coordinate reset/refresh.
5. How is prompt precedence unified between idle and active mode? There is no permission/question input handler in this outer path; source evidence does not justify moving active behavior into a lower-priority generic composer handler.
6. Signal delivery without decoded input needs explicit scheduling analysis; current timeout path skips the signal check. Preserve the terminal ownership/assertion contracts while changing how the loop wakes.
7. Authority selectors, external editor, suspend and synchronous submit can block this loop. Their contracts must be recorded; success compilation does not prove fairness or shutdown behavior.
## Active submission and submit-path source inventory (goal 06/01)

Scope: read-only analysis of the plan branch before any runtime refactor. Line references below are baseline paths under `src/ava/tui/`. `A` denotes `runtime_active_run_internal.cpp`, `S` denotes `runtime_submit_internal.cpp`, `Q` denotes `runtime_state.cpp`, and `AS` denotes `runtime_active_run_state_internal.h`. Full A/S and their headers were read; this section also inspects the queue implementation and prompt/active-input boundaries. No build or tests were run by this analysis worker.

### Call and lifetime boundary

`RuntimeSubmitController::submit` calls `active_run_controller_.run(std::move(submitted))` synchronously at S597, then translates its outcome at S598–600. Thus the outer `TuiFrontEnd::run` loop cannot service anything until this call returns. `run` owns a local state at A545 and a future launched at A630; normal and loop-break paths join via `submit_future.get()` at A864. The worker calls frontend/backend services, never terminal operations directly. The current future, state, callback captures, prompt coordinator, plugin coordinator and borrowed controllers have tightly coupled lifetimes: a serviced replacement must preserve all of them until worker settlement, rather than return from `start()` with references to its temporary stack.

### Controller members and true operation state

`RuntimeActiveRunController` currently owns no run future or per-run state. Its header privately stores borrowed references to `frontend_`, `presentation_state_`, `draft_state_`, `renderer_`, `prompt_coordinator_`, `prompt_stash_`, `plugin_ui_`, `navigation_`, `action_controller_`, `transcript_search_`, `subagent_workspace_`, and one `service_mermaid_presentation_` callback (header 169–180). All must outlive an active submission. Read-only frontend access is not sufficient for operations: it creates queues and submits requests. An eventual ActiveRun operation is the candidate owner of the following per-run values, rather than additional session-wide scalar fields:

| Actual state | Readers/writers and invariant | Current lifetime / proposed treatment |
| --- | --- | --- |
| `submitted`, immutable `is_command_submission`, `supports_active_queue` (AS36–38) | Submission worker, event settlement, active-command routing; classification at A538/544 | Entire operation; owned by ActiveRun. Submitted text is sensitive (`NoLogString`) and must not become generated debug text. |
| `command_events_released`, `event_projection_pending`, `initial_request_id`, `ordinary_turn_request_ids` (AS39–42) | Queue draining buffers command transcript until authoritative completion receipts arrive (A432–446, 865–868); reset on session change | ActiveRun owns authority projection state; backend receipt authority remains external. |
| `submitted_transcript`, `turn_snapshot_leading_evictions` (AS43–44) | Copied at A548, capped transcript merge updates A474–477, settlement reads A949; reset A298–300 | ActiveRun baseline; not a second persistent session transcript owner. |
| `submit_image_attachments` (AS45) | Moved from pending attachments A550–554, worker reads A646 | ActiveRun owns consumed attachment references; presentation removes pending labels together. |
| `event_queue` (AS46) | Three thread-safe producer forms; TUI drains/discards; received flag used by fallback settlement | ActiveRun queue, lifetime spans all publishers and worker join. |
| Aggregate `event_state`; `initial_command_event_state`; `unattributed_local_event_state`; `request_event_states` (AS49–52) | `apply_queued_event` updates, authority projection and settlement read; `discard_for_session_transition` resets | ActiveRun owns these accumulators. Their text/authority content justifies print opt-out. |
| Atomic `run_cancel_requested` (AS53) | TUI stop/close/failure sets; backend predicate and finish observe | ActiveRun cancellation state; atomic because worker reads concurrently. |
| `close_after_submit` (AS54) | TUI stop/close paths set; input loop and final outcome read | ActiveRun outcome, distinct from cancellation alone. |
| Optional `active_queues` (AS55) | Created by frontend A574; worker extracts callbacks A631–634; TUI queue commands and final finish use | Backend-owned capabilities retained by ActiveRun, not terminal authority moved to worker. |
| `event_context_mutex`, `current_event_context` (AS56–57) | Follow-up request callback writes under mutex A566–570; runtime sink snapshots under same mutex A595–600 | ActiveRun context transport; do not treat const snapshot access as thread synchronization. |
| `event_sink`, `turn_started_at` (AS58–59) | Sink also installed as prompt audit target A607, cleared A923/928; start time read in projection/meta | ActiveRun callback/time state. |
| `active_run_outcome` (A507), `result` (A624), `session_changed` (A625), `render_failed` (A649) | Failure/termination flags, future result, transition and fallback settlement | ActiveRun state/result. Default outcome has `break_loop=true`; success clears it only at A1048. |
| `permission_resolver`, `question_resolver` (A613–614, 627–628) | Created from coordinator, copied into worker submit context | Retained callbacks valid until worker joins; candidate ActiveRun members or values inside a retained worker request. |
| `submit_request_id`, `control_sink`, `plugin_ui_endpoint` (A565,571,591) | Used to prepare queues/submission and captured by worker | Initialization data that must remain valid while a worker borrows it; favor owned worker request values when removing stack lifetime. |
| `request_key_bindings` (A629) | Deliberately copied into worker capture | Request snapshot, not mutable session-global binding alias. |
| `submit_future` (A630), `cadence` (A729) | Readiness loop/get and frame/spinner deadlines | ActiveRun future; scheduling deadlines later belong to event loop/member timers. |

`RuntimeActiveRunState::discard_for_session_transition` at A296–308 clears transcript baselines, receipts, queue and event accumulators after joining the old-session worker. It does not reset all operation members: the old request and cancellation state remain until operation destruction. Preserve the distinction when extracting session transitions.

### Alias inventory (no duplicate ownership)

A508–515 aliases presentation `snapshot`, `sidebar`, `pending_image_attachments`; draft `input_history`, `draft`, `selected_slash_command_index`; renderer `transcript_scroll_offset`, `ui_mutex`. A546–564 aliases state `submitted`, `submitted_transcript`, `submit_image_attachments`, `event_queue`, `event_state`, `run_cancel_requested`, `close_after_submit`, `active_queues`, `event_context_mutex`, `current_event_context`. A603 aliases `event_sink`; A609 copies `turn_started_at` read-only. None is a new independently owned field.

`drain_events` A394–407 creates aliases for queue, event state, baseline transcript, eviction count, cancellation, snapshot/sidebar, renderer scroll offset, detached output count, completion/layout caches, detached sidebar and UI mutex. The time value A399 is a read-only copy.

S67–81 aliases presentation snapshot/sidebar; draft input history/index, saved input, draft, jump mode, selected completion index, palette suppression/force, scroll and pending Escape; renderer transcript scroll, detached output count and detached sidebar. These are the mutable-reference bundle to reduce through domain operations, not names to copy into the submit object.

### All capturing lambdas in run and their data flow

| Lambda/source | Captures actually used, reads and writes | Ownership implication |
| --- | --- | --- |
| `refresh_plugin_surface_fit` A516–522 | `snapshot`, `this`; queries kernel size; writes snapshot width/height; calls plugin surface cancellation | Plugin geometry operation with renderer geometry source, not a pure read. |
| `render` A523–526 | Calls fit lambda and renderer render | More than trivial forwarding today; split explicit geometry fit then renderer operation. |
| `maybe_reload_display_settings` A527–535 | Action controller, `render`; applied reload renders, terminal failure propagates | Session/display service coordinating renderer; no settings-preview state exists here. |
| `clear_draft_for_interrupt` A536 | `this` only; forwards to action controller | Delete forwarding adapter. |
| `set_current_request_id` A566–570 | Mutex/context; writes both request/correlation IDs under lock | ActiveRun event-context member operation. |
| `mark_follow_up_started` A579–584 | By-value prior backend callback; by-reference context setter | Genuine async boundary: publish context first, invoke original capability. Callback target must survive queued follow-ups. |
| `cancel_requested` A592 | Atomic by reference, `this` for plugin deadline | Genuine worker predicate; expose cancellation/deadline transport without terminal access. |
| `runtime_event_queue_sink` factory A593–602 and returned sink A594 | Factory builds a reference-capturing sink; returned sink snapshots context under lock and enqueues | Factory is redundant; retained sink needs lifetime-safe owner and publish-before-notify contract later. |
| `settle_turn_activity`, `request_stop`, `request_close_after_submit` A610–612 | `state`, `this`; forward corresponding members | Delete forwarding adapters after owned state exists. |
| Async submit A630–648 | `[&, request_key_bindings]`: frontend, active queues, submitted text/request ID, resolvers, event sink, cancel predicate, queue, plugin endpoint, attachments; bindings copied | Real thread callback. Everything borrowed must outlive worker; move request data to retained request/operation members. |
| `fail_active_run` A650–656 | Outcome, render_failed, atomic, plugin and prompt coordinator; sets failure/cancel and unblocks pending work | ActiveRun failure transition; cleanup ordering matters. |
| `service_pending_prompt` A657–667 | Coordinator, cancel/stop callbacks, state and nested before-prompt callback; drains events after modal completion | ActiveRun prompt servicing; currently blocking nested modal path. |
| Before-prompt callback A658–662 | Search, presentation snapshot, selector ref from action controller | Session modal precedence operation closing search and overview before prompt. |
| `plugin_host_modal_conflict` A668–674 | Snapshot const reads and transcript search open state, pure palette visibility calls | A const projection predicate can remain a free computation with explicit inputs. |
| `poll_plugin_ui` A675–681 | Fit/conflict helpers, plugin, snapshot, stop callback and renderer | Plugin service operation with explicit changed/deadline outcome. |
| `dispatch_retained_input` A682–728 | Input parameter; snapshot/plugin/renderer/search/coordinator/state and service/failure/stop callbacks | Dispatcher operation; should not retain per-event input. |
| Search input forwarding callback A704 | `this`, current input | Inline synchronous boundary; replace with operation binding only if callback contract remains. |
| Before-prompt callback A705–708 | Same close-search/overview side effects as A658–662 | Shared session precedence operation. |
| Ordinary input forwarding callback A721 | `this`, state, current input | Synchronous adapter for prompt-before-input; no state ownership. |
| Modified-file `any_of` predicates A964,972 | Current file/path read, no writes | Per-element pure predicates; remain local/free. |
| Truncated bash predicate A1012 | No capture | Pure predicate. |

No lambdas occur inside `RuntimeSubmitController::submit`; it contains many branch-local values and owner mutations instead.

### Other stateful helper edges and exact mutation evidence

* `request_event_state` A183–190 searches then inserts into `state.request_event_states`; `command_event_state_for_request` A193–200 returns writable authority-segment state. This is stateful lookup/creation, a candidate ActiveRun method, not a mathematical helper.
* `apply_queued_event` A208–255 visits the event variant. Its capturing visitor writes aggregate state, request-segment state and snapshot tool index. Backend receipts still decide conversation authority. Non-command events call `record_tui_tool` A223 and `update_indexed_tui_tool` A225. A read-only snapshot argument would be wrong.
* `authorized_command_event_transcript` A257–271 reads state and builds a return vector; local `append` captures only that result vector. `authorized_command_events_received` A274–287 reads state. These are projection computations; no need to force them onto an unrelated object.
* `merge_unauthorized_command_event_tools` A113–163 reads state and mutates only its by-value result vector. Local `identity`, `settled`, `merge`, nested matching predicate and `collect` do not orchestrate retained session state. Keeping this as a value transformation is reasonable.
* `drain_events` A408–426's `apply_non_transcript_event_authority` copies queued messages/activity/todos, inserts missing modified files, caps list at 50 and may upsert stopping activity. This is an operation of the active-presentation owner.
* Exact layout mutation: A463 calls `composer_max_transcript_scroll_offset_cached(snapshot, ..., completion_cache, ..., transcript_layout_cache, ...)`: snapshot is input, but both caches can mutate. A465 captures anchor. A467–468 snapshots detached sidebar. A474 caps transcript and updates baseline/evictions; A475–481 carries expanded tools/thinking and refreshes search; A485–493 updates deferred layout, detached output count and scroll. Those changes form one viewport-preserving transcript operation, not a const renderer read.
* Exact snapshot-only read examples: A669–673 checks popup/palette state to choose plugin conflict; A931 chooses event-received condition; A992 reads transcript size before fallback. They can use const projections with explicit operation lifetime.
* A876 invokes `apply_committed_session_transition` with presentation, draft, renderer, search, workspace, selector and backend-committed ID. This is the session-coordinator seam, not a reason to move session authority into ActiveRun or selector.
* A949 `settle_tui_submission` receives baseline/event snapshots and produces a settlement value. The later application A951–1035 mutates transcript/sidebar/status/processing and related search/selection state. Keep value calculation separate from owner mutation.

### Current queue and producer contract

`RuntimeEventQueue` (`runtime_state_internal.h` 38–54) owns private mutex, vector and monotonic-since-discard `received` flag. `envelope_sink` Q100–107 and `subagent_launch_sink` Q110–116 return `[this]` callbacks, lock, append, set received. `enqueue` Q119–124 does the same for contextual runtime events. There is no eventfd, condition variable, notification callback, wait method or queue-size bound. `drain` Q127–132 swaps all events under lock; `discard` Q135–140 clears and resets received. `received_any` Q142–145 does not mean currently nonempty. Existing polling is therefore essential until a real wakeup path is wired.

Producers visible from this boundary: worker runtime events through event sink (A594–600), backend active-queue control envelopes (A574), backend subagent launch notifications (A644), prompt audit via same runtime sink (A607), follow-up request context changes (A579–584), worker completion via future, cancellation predicate observations. Prompt request publication is a separate mutex-protected queue: `runtime_prompts_internal.cpp` 742/770 appends and worker waits on per-request condition variables at 745/773. Neither publication wakes the current TUI loop. Plugin UI has its own endpoint; its publication mechanism requires separate inspection. Do not claim all producers already notify.

### Active loop control-flow map

The only explicit loop in `run` is A730–860; helper/settlement `for` loops iterate values, not scheduling loops. Condition `submit_future.wait_for(0) != ready` is a readiness poll. After any break, execution still reaches plugin finish then blocking `future.get()`.

| Location | Effect/reason |
| --- | --- |
| A732–766 | Claim SIGTERM first, otherwise SIGINT. Termination or SIGINT with empty draft cancels/closes, records terminal signal and breaks at 753. SIGINT with draft clears it and renders; render failure breaks 764, successful clear continues 766. Unclaimed signal falls through. |
| A769–776 | Service pending permission/question before all ordinary input. Service failure cancels/fails and breaks 773. Serviced modal continues 776. |
| A777–780 | Plugin poll/deadline handling; failure cancels and breaks. |
| A782–795 | Poll ncurses once, compute whether buffered input or frame deadline takes precedence. Buffered input dispatch failure breaks 791; close-after-submit breaks 794; otherwise continue 795. |
| A797–807 | Wait only on future for minimum of cadence, pending render and workspace deadline. Future ready breaks 807; event/prompt/input producers cannot wake this wait. |
| A810–823 | Advance cadence and spinner; poll workspace. Workspace render-request failure breaks 816. Spinner updates processing frame under UI lock. |
| A825–839 | Drain events, service Mermaid, reload display when live viewport/no workspace, request spinner footer render. Failure breaks 829,834,839 respectively. |
| A841–849 | Dispatch retained input after servicing due frame work; failure breaks 846, close-after-submit breaks 849. |
| A851–859 | Refresh plugin surface geometry and flush pending render; failure breaks 856; completed render resets cadence frame deadline. |
| A863–868 | Finish plugin submission before future.get; then accept ordinary-turn receipt IDs and release authorized buffered command events. |
| A872–898 | Read committed backend session and boundary transports before UI lock; transition/apply; finish active queues; on changed session discard all old-session emission and finish receipts. |
| A901–925 | Final drain, merge unauthorized command tools into local result, record local tools, clear audit sink; return default break outcome if render_failed. |
| A927–930 | Idempotent plugin finish and audit clear again; terminal signal returns break outcome. |
| A931–1035 | Compute settlement and apply fallback/local output, sidebar, selection/search and processing state. |
| A1036–1044 | Mermaid/render failure sets terminal_write_failed and returns; close-after-submit or result.quit returns break outcome. |
| A1046–1049 | Assert signal/break relationship, set break_loop=false, return successful continue outcome. |

Early return A622 occurs before worker launch if initial render fails: plugin submission finishes, but audit sink was already installed at A607 and is not cleared on that branch. The sink captures local state that then dies. This is a concrete lifetime hazard to record, not silently repair in analysis. Exceptions from `future.get()` likewise bypass the explicit audit clearing. A proper RAII operation lifetime should remove this dangling callback risk when implementation goals own the cleanup.

### Input precedence reached by the loop

A682–728 routes plugin modal first (Ctrl+C stop semantics; resize immediate render), then transcript search with pending-prompt precedence, then pending prompts before general active input. `runtime_active_run_input_internal.cpp` 430–523 further routes: command output overlay (unless permission/question), subagent workspace, transcript search, wheel governor/resize, prompt stash, preemptive input, completion acceptance, active commands, composer edit, navigation, next-navigation, mouse, restricted toggles. Every per-event `input`, geometry, decision and callback result remains local; none justifies a persistent member.

### Submit-path declaration and return inventory

S65 `forced_submission` is an invocation value. `immediate_slash_submission` S83 and `submitted` S188 are preparation values; transferring submitted into the active operation ends their need. Palette `matches`, disabled reasons, selected item/text and replacement selection (S96–175) are per-call computations. Branch values (`stash_argument`, `success`, query/target/reload arguments/results/status, settings snapshot, imported image/label, indexed tool/transcript index, diff text/title, copy target/text/status flags, thinking item index) remain branch-local. No new persistent state is needed for them.

The path first suppresses pending Escape, resolves forced/exact/palette submission, resets draft and selection, then dispatches local commands in this order: stash; jobs; model; scoped models; sessions; fork-from; search; reload local targets; local display settings; hotkeys; sidebar; settings; overview; attach; tool; diff; copy; details; thinking; otherwise ActiveRun (S200–597). Empty submitted returns NoAction at S602. Exact local branch terminal-render failures return BreakLoop with `terminal_write_failed=true`; successful local handling returns ContinueLoop. These return pairs are at S106/108,129/131,159/161,184/186,225/227,234/236,243/245,252/254,261/263,271/273,309/311,329/330,341/343,356/358,376/378,385/387,398/400,409/411,416/418,443/445,463/465,477/479,547/549,565/567,593/595. Disabled file/path references return ContinueLoop without render at S145/170. Stash S217 maps `success` to both disposition and terminal failure (existing FIXME notes this semantic ambiguity). ActiveRun outcome fields are copied at S598–600.

Exact cross-owner writes include S118–126 draft replacement + history/selection reset + snapshot selected index; S188–197 draft/transcript selection reset; S335–338 select-list + kind + status + scroll publication; S348–353 sidebar drawer + detached viewport reset; S363–373 startup overview/trust + settings view/kind + status + scroll; S554–562 tool presentation. These should become composer/selector/transcript operations while backend calls (`on_attach_image`, reload/display actions, eventual `on_submit`) retain backend authority.

### Review questions carried forward

1. Prompt servicing is not merely a nonblocking method: it calls `service_claimed_request` at `runtime_prompts_internal.cpp` 702, which enters permission/question loops at 294/481 via 686/689. A final single loop must service these modals too, or explicitly state any permitted handoff; replacing only A730 leaves nested terminal event loops.
2. Preserve command request receipt segmentation and old-session event discard. Exposing unrestricted state for convenience risks leaking command output into ordinary conversation history.
3. Own the audit sink registration with cleanup covering initial-render failure, normal settlement and exceptions; current explicit clearing is incomplete.
4. Backend `on_submit` and active-queue callback capabilities must outlive the operation's worker. Which request data will be captured by value versus retained in ActiveRun should be decided explicitly in architecture/contracts.
5. Renderer mutation is wider than rendering: layout caches, detached sidebar, scroll, selection and deferred transcript updates need coordinated ownership. Const access is correct only for read examples identified above.

Additional producer evidence: `runtime_plugin_ui_internal.cpp` 256–285 `present_request` validates the generation/binding, creates a pending modal, and appends under `state->mutex`; only a test-only `after_enqueue_for_test` hook runs afterward. Its loop at 287 waits on the worker-side modal condition variable until reply/cancellation/deadline (20 ms bounded waits at 295). This is a producer-side wait, not another TUI input/render loop. It should remain distinguishable from the permission/question TUI loops when consolidating scheduling.
