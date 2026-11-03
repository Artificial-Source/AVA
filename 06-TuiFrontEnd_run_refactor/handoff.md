# TuiFrontEnd::run refactor — standalone handoff

## Authority and desired outcome

This objective was requested by an experienced C++ maintainer who wants understandable object-oriented code, not a cosmetic decomposition of a giant function. No conversation context is required to execute this plan. The numbered tree is the execution order; this document supplies rationale and preliminary evidence, not a competing task list.

### Artifact paths and handoffs

Paths such as /06/01/inventory.md use plan refpaths, not OS-root paths: resolve /06/01 to its actual node directory beneath $PLANROOT/ObjectiveTree, then append inventory.md. Every subgoal description explicitly names its relevant input artifacts and output/update destinations; nested goals do not inherit an implicit reading list. Planned artifacts do not exist until their producing goal writes them; a missing prerequisite is a handoff gap to resolve, not permission to guess its decisions.

| Artifact | Producer and consumers |
| --- | --- |
| /06/handoff.md | Planner-supplied requirements and preliminary evidence; input to every subgoal |
| /06/01/inventory.md | Written by /06/01; input to /06/02 onward, including nested coder goals |
| /06/02/architecture.md | Written by /06/02; input to /06/03 onward, including nested coder goals; reconcile explicitly if later decisions change it |
| /06/03/migration-ledger.md | Created by /06/03; read and maintained throughout later implementation/integration, with ordered entries naming each producing goal and superseded contracts; input to later analyses/decisions and final review |
| /06/06/scheduling-analysis.md | Written by /06/06; input to /06/07–11 |
| /06/07/event-contracts.md | Written by /06/07; input to /06/08–11 |
| /06/11/refactor-handoff.md | Written by final review; input to any subsequent runtime-validation or maintenance objective |

Index each artifact in the contents file of its containing node. The shared migration ledger records non-code outputs from coder steps (actual APIs, invariants, lifetime/producer contracts, stubs and build evidence) so subsequent goals can consume them without conversation history. Keep entries attributable and current; do not hide approved architectural changes solely in the ledger. If additional .md outputs become necessary, update the producing and all consuming goal descriptions to name their exact plan-relative paths.

The present TUI thread body is an implicit object represented by a large stack frame, reference aliases, capturing lambdas, state bags with public members, and free functions receiving mutable references and callbacks. Group correlated persistent state into clearly named objects with private members and operations maintaining invariants. Objects can still live on the stack. Ownership should follow both actual usage correlations and domain meaning; Mermaid state must not be grouped with unrelated state merely because both access the screen snapshot.

Replace capturing orchestration lambdas with member functions of the appropriate state owner. Remove trivial forwarding lambdas instead of adding forwarding methods everywhere. Replace stateful free-function orchestration with owner operations. Independent computations/value transformations (the user's example is sin(x)) may remain free functions, including anonymous-namespace helpers; do not make them static class members merely to attach them to an unrelated class. Analyze even lambdas declared inside event branches, not just function-scope ones. Genuine asynchronous callback boundaries need explicit target/lifetime contracts rather than indiscriminate reference captures.

An intermediate large session owner is acceptable: it makes the existing implicit object explicit and provides a place from which to extract smaller owners. Do not stop at a god object, a class full of public fields, mutable-reference accessors for every field, or a dispatcher taking the original fifteen references. Existing useful owners should be evolved, not blindly wrapped or renamed. Proposed class names below are hypotheses awaiting the architecture decision goal.

**initial_status_line is initialization-local, not persistent state.** It is consumed when constructing RuntimePresentationState and is not used after that construction. Do not promote it to a member. Likewise keep per-event temporaries local and distinguish reference aliases from actual stored state.

The intended scheduling endpoint is one TUI-thread loop, inspired by utils/threading/EventLoop.h's example near line 59, with separate operations for termination, time-based work, application updates, and terminal input. Both input and notifications can be ready and both must be serviced before another blocking wait. Keyboard and mouse are decoded from terminal input; resize and signals need explicit treatment. Feature objects do not calculate wait_for/wait_until durations. Instead an object such as Spinner registers a member callback to run every 250 ms; the loop owner reconciles competing deadlines internally.

## Execution and acceptance policy

### Maintainer design requirements (apply throughout this objective)

Use common sense for genuine exceptions, but document their concrete reason rather than silently ignoring these requirements.

1. All new class data members are private, or protected where justified for a base-class design. Read access goes through member accessors. Prefer state-changing member operations that maintain invariants over mutable-reference accessors. A mutable accessor to a custom-class member is acceptable when that member encapsulates its own invariants and its operations do not require coordinated changes to other owner state: avoid mechanically wrapping every method of an independently encapsulated subobject. Even in that case the member stays non-public and access is through a member function. This exception is not permission to expose writable scalar flags, containers of raw state, or public-field state bags.
2. Use AVA_DEBUG_PRINT_MEMBERS_ON by default, not AVA_DEBUG_PRINT_MEMBERS_OPT_OUT. Opting out requires a specific good reason, such as a required sensitive-data boundary or the repository's anonymous-namespace/internal-linkage limitation; record it. Do not put the new domain classes in anonymous namespaces merely to avoid providing generated printing. Follow repository include/marker rules and do not expose secrets or message bodies through generated debug output.
3. Class names are CamelCase. Each new class is declared in a correspondingly named header, e.g. SettingsDialog.h; substantial definitions belong in SettingsDialog.cpp. Clearly short inline functions such as simple one-line accessors may remain in the header. Do not add new domain classes to generic runtime_*_internal state-bag headers simply because the old code lives there. Record any justified special-case exception in the architecture decision.
4. Classify shared snapshot/renderer dependencies precisely. Read-only use generally needs a const reference with an explicit lifetime, not a new mediator or duplicated state. Shared read access is not evidence of common ownership. Non-const access needs exact source examples and analysis of the fields/operations changed, cache side effects, and cross-object invariants. Bring problematic mutation boundaries to the maintainer for review instead of assuming every reference must disappear or every read needs a wrapper. Constness alone does not establish cross-thread synchronization; these collaborations remain TUI-thread-owned unless explicitly designed otherwise.

These requirements refine the ownership proposals below. Goal 01 must distinguish reads from writes with concrete call-site evidence; goal 02 must record accessor/const-reference contracts, class/file names, and justified printing or visibility exceptions; goal 11 must review those decisions against the resulting code. Compilation remains the executable gate, not a substitute for this design review.

- Work begins with analysis and a user-reviewed ownership decision, then object refactors, followed by a second analysis/decision cycle for scheduling. Child descriptions label analyst/planner/coder roles; they do not require unsupported plan metadata.
- **Each intermediate code step must compile. Running the application or passing tests is not an acceptance requirement.** This is explicit user permission for incomplete intermediate designs, not permission to claim unverified behavior works.
- API-first development is welcome: a class may initially exist unused, or callers may be written against a designed interface backed by compiling empty stubs. Record every temporary stub, missing wiring, replaced behavior, and intended removal goal. Track the ledger as a regular artifact with a contents entry under goal 03; maintain it throughout subsequent goals. Avoid leaving silent placeholders in the finished objective without explicit user agreement.
- Analysis/decision-only goals must not break the compiling tree. Establish the initial build result and actual configuration rather than assuming it from a directory name. Do not silently repair unrelated existing failures; report a blocker and seek direction.
- Use the configured build via aap-build (or the prescribed cmake --build equivalent). Do not reconfigure just to validate an extraction. Follow environment rules: configure errors stop work. Record the command, actual build type/debug options, and result for code goals. BetaTest can have EnableDebug/libcwd enabled; directory names and CWDEBUG are not equivalent to assertion availability.
- Do not gate intermediate goals on full tests, PTYs, benchmarks, runtime behavior, or broad repository cleanup. Optional focused checks may inform a decision, but failures are evidence to record, not an excuse to expand scope. Formatting/checks should be local; preserve existing aligned comments and all #if 0 blocks. git diff --check is useful but does not prove behavior.
- Preserve user edits and other objectives, especially /08 terminal-thread safety and /10 RPC work. Do not change thread assertions, terminal ownership policy, or backend authority merely to make a smoke test pass.
- Every analysis/decision result is an artifact in its plan node with a contents entry. Include current symbol/file references and explicit unresolved questions. Analysis completion means evidence recorded, not just an intention to inspect later.
- Future testing suggestions belong in the final handoff, explicitly marked unrun. The final refactor should have actual implementations/wiring, not be declared complete simply because empty stubs compile.

## Preliminary source map (refresh at goal 01)

Source references were observed during the discussion and are navigation hints, not immutable line numbers. Locate definitions with findsymbol and read current files; repository contents may have changed.

- src/ava/tui/TuiFrontEnd.cpp: run at roughly 134–2472; initial frame 567–571; outer loop 573–2465.
- src/ava/tui/TuiFrontEnd.h: frontend callbacks/service boundary.
- src/ava/app/tui/interactive_tui.cpp: run_tui creates/joins the dedicated TUI thread; lifetime boundary is relevant but should not grow feature logic.
- src/ava/tui/runtime_state_internal.h: RuntimePresentationState, ActiveSelectList, runtime event queue, committed-session transition helper declarations.
- src/ava/tui/runtime_draft_internal.h: RuntimeDraftState and its public interaction fields.
- src/ava/tui/runtime_render_internal.h: RuntimeRenderer state/caches/viewport/render scheduling.
- src/ava/tui/runtime_navigation_internal.h: RuntimeNavigationController, currently sharing draft/presentation/renderer state.
- src/ava/tui/runtime_views_internal.h: SettingsNavigationState and settings preview structures.
- src/ava/tui/runtime_actions_internal.h: RuntimeActionController.
- src/ava/tui/runtime_prompts_internal.h: RuntimePromptCoordinator and resolver callback ownership.
- src/ava/tui/runtime_subagent_workspace_internal.{h,cpp}: workspace publication/polling/deadlines.
- src/ava/tui/runtime_submit_internal.cpp: RuntimeSubmitController::submit, calling active_run_controller_.run near 597–600.
- src/ava/tui/runtime_active_run_internal.{h,cpp}: second event/input/render loop hidden in RuntimeActiveRunController::run.
- src/ava/tui/runtime_input.cpp: startup-buffer replay and timed/nonblocking ncurses reading near 277–305.
- src/ava/tui/terminal/{Context,KeyboardInputMode,MouseInputMode,ScopedTimeout}.*: terminal ownership, buffering, startup negotiation, input timing, and protocols. Inspect actual decoder dependencies rather than assuming one read gives a complete event.
- utils/threading/EventLoop.h: example and notification/termination/watch-registration contracts; inspect implementation before designing integration.

## Preliminary inventory of run-lifetime state

The initial analysis covered outer run scope; goal 01 must also inventory nested active-run state and branch-local capturing lambdas. For every item record owner/lifetime, readers, writers, cleanup, and whether it is truly persistent. Do not count aliases as duplicate members.

| Current state | Existing owner / interpretation |
| --- | --- |
| before_shutdown | RuntimeBeforeShutdownGuard; idempotent frontend shutdown callback |
| application, signals_manager, terminal_context | Borrowed application services, not ownership transfers |
| graphics_cleanup, terminal_background_detection | Terminal/protocol/startup-input cleanup lifetimes |
| initial_status_line | Initialization-only local; NOT a member candidate |
| presentation_state | RuntimePresentationState |
| snapshot, sidebar, command_session_grants | Aliases into presentation_state |
| plugin_ui, plugin_ui_shutdown | Coordinator plus local guard with pointers to coordinator/snapshot |
| terminal_write_failed, terminal_signal_received | Session outcome flags |
| draft_state | RuntimeDraftState |
| input_history, history_index, draft_input, draft | Aliases into draft_state |
| jump_mode, selected_slash_command_index, slash_palette_suppressed, path_completion_force_active | Aliases into draft_state |
| draft_scroll_offset, draft_selection_anchor, draft_selection_cursor, pending_escape_clear | Aliases into draft_state |
| renderer, navigation | Existing renderer and navigation controller sharing state |
| transcript_scroll_offset, completion_cache | Aliases into renderer |
| active_select_list | Independent selector-kind state, coupled to snapshot.select_list |
| prompt_stash | Existing RuntimePromptStashController |
| branch_summary_ui | Local BranchSummaryUiState: active, exit_requested, generation, phase, prior_session_view, prior_selected_index, selected_source_value |
| branch_summary_exit_ready | Separate operation-to-session exit flag |
| settings_nav, settings_session_open, clear_theme_preview_on_exit | Settings state, separate open flag, cleanup guard |
| transcript_search | Existing TranscriptSearchController |
| session_archive_confirmation | Optional PendingSessionArchiveAction |
| prompt_coordinator, permission_resolver, question_resolver | Prompt owner and retained resolver callback values; determine exact lifetime needs |
| mermaid_presentation | Existing presentation controller |
| action_controller | Existing RuntimeActionController |
| subagent_workspace | Existing RuntimeSubagentWorkspaceController |
| active_run_controller | Existing controller which enters another loop |
| submit_controller | Existing RuntimeSubmitController |

Local cleanup/state types requiring explicit lifetime decisions: RuntimeBeforeShutdownGuard, ComposerTerminalGraphicsGuard, TerminalBackgroundDetectionGuard (anonymous namespace), and run-local PluginUiShutdownGuard, BranchSummaryUiState, ClearThemePreviewOnExit. Do not eliminate RAII simply to reduce the count of objects.

## Lambda correlation inventory

Goal 01 should turn this preliminary grouping into a read/write/call matrix: actual captured state, mutations, external callback arguments, proposed owner, and dependencies that cross owners. A shared snapshot reference does not by itself justify combining otherwise independent domains.

### Settings (roughly 235–303)

- clear_settings_preview: settings_nav and snapshot.
- begin_settings_preview_baseline: settings_nav and snapshot.
- ensure_settings_session: settings_session_open and preview/reset helpers.
- rebuild_settings_view: settings state, snapshot/sidebar, active_select_list, keybindings.
- apply_settings_highlight_preview: settings preview plus selected view/item.
- close_settings: selector/view state, preview and session-open lifetime.
- Settings portion of maybe_reload_display_settings (473–518): authoritative reload, preview rebase, selection preservation, view rebuild and paint.

### Branch summary (311–444)

restore_branch_summary_selector, present_branch_summary_status, settle_branch_summary, apply_branch_summary_snapshot, poll_branch_summary, begin_branch_summary, cancel_branch_summary, confirm_branch_summary. These share branch_summary_ui and its phase/generation/cancellation/restore invariants, selector/view publication, frontend operation callbacks, rendering requests, and branch_summary_exit_ready. Strong candidate for one operation object with explicit results.

### Rendering, Mermaid, actions and navigation (309; 447–565)

- render is just a forwarding adapter to renderer.render.
- service_mermaid_presentation combines Mermaid servicing, transcript scroll/cache/viewport anchor handling, snapshot updates and search refresh. Extract the feature behavior but put general viewport anchoring behind the transcript owner's API.
- maybe_reload_display_settings spans authoritative reload and settings-specific preview operations; split responsibilities rather than give one object unrestricted access to everything.
- Action adapters: clear_draft_for_interrupt, open_external_editor, suspend_to_background, paste_clipboard_image, cycle_reasoning, toggle_thinking_visibility, open_model_selector, open_reasoning_selector, open_scoped_model_selector, open_session_selector, cycle_model. Most should disappear in favor of direct calls. toggle_startup_overview additionally closes settings and needs explicit coordination.
- Completion/navigation adapters: slash_palette_active, file_reference_palette_active, path_completion_palette_active, completion_match_count, clamp_completion, previous_completion, next_completion, selected_completion_disabled_reason, selected_completion_text.
- Transcript/navigation adapters: scroll_up, scroll_down, toggle_tool_details_at, toggle_thinking_at, handle_sidebar_drawer_input, jump_to_bottom, scroll_to_message_boundary.
- handle_submit forwards to submit_controller, updates outcome flags, and checks the BreakLoop/signal relationship. Replace implicit writes with a session-level explicit result.

### Lambdas created during selector input (798–911)

input_result initializer lambda and nested scoped_action: current input/keybindings/selector projection. preserve_session_selector_state: result/view/confirmation state. selected_select_list_item and visible_select_list_values: current selection/filter result. settle_opened_session and synchronize_committed_session: backend commit result plus presentation, draft, renderer, search, workspace and selector transition. These are not persistent objects just because they capture persistent state; make operations with explicit per-event parameters.

### Lambdas created during composer input (1598–1747)

is_action: event/keybinding query. select_slash_command, select_file_reference, select_path_completion, force_path_completion: correlated draft/completion/selection state, status and optional terminal beep. insert_input_text: event plus draft mutation. The actual stateful behavior naturally belongs to the composer; notification/beep/presentation publication are explicit collaborators or outcomes.

## Free-function and mutable-reference audit seeds

Inspect definitions and callers; this is not a blanket instruction to convert every function named here. Some are pure projection/selection calculations and should remain free functions. Document the distinction, including externally mutated caches and callbacks.

- Settings: reapply_settings_preview_after_display_reload, settings_select_list_view_for_section, clamp_select_list_selection, restore_branch_summary_session_view, branch_summary_operation_view, reselect_settings_display_row_after_rebuild.
- Session transitions: apply_committed_session_transition_and_refresh and apply_committed_session_transition currently coordinate many mutable owners. Replace the reference bundle with session-coordinator operations and per-owner transition methods, not backend authority inside a selector.
- Transcript/cache: capture_transcript_viewport_anchor, composer_max_transcript_scroll_offset_cached, file_reference_palette_selection_for_screen_position_cached, path_completion_palette_selection_for_screen_position_cached, runtime_wheel_input_accepted.
- Selector/input policy: branch_summary_terminal/status/input_intent/callback_failure_status, select_list_selection_for_screen_position, handle_select_list_input, filter_select_list_items, hotkeys_select_list_view. Retain pure computations where appropriate.
- Composer: replace_composer_draft, insert_composer_paste_text, insert_composer_draft_text, apply_composer_draft_action, jump_composer_draft_to_character, clamp_composer_draft_cursor_to_atomic_boundary, browse_composer_input_history, clamp_slash_palette_selection, slash_command_selection_disabled_reason/text, filter_slash_commands, previous_slash_palette_selection, next_slash_palette_selection.
- Authority boundaries: on_session_selector_archive/unarchive and branch/sort/filter callbacks, dispatch_tui_selector_authority. These request backend changes; refactoring must not move backend policy or session authority into TUI objects.
- Stateful I/O: runtime_input::read_curses_input_with_timeout, drain_startup_probe_input, beeps, signal claims. Determine ownership/service boundaries rather than treating them as mathematical helpers.

## Candidate architecture — not yet approved

| Candidate | Private state and behavior | Existing structure to evolve |
| --- | --- | --- |
| TuiSession | Composition, startup/shutdown, outcome, coordinated session transitions; not editing/menu algorithms | run stack frame and guards |
| Composer | Draft/cursor/selection/scroll/history/completion/jump/Escape state and editing operations | RuntimeDraftState plus relevant navigation/input helpers |
| SelectorHost | Active selector kind and view lifecycle, common selection/filter/input routing, consistent open/replace/close | active_select_list and snapshot.select_list |
| SettingsDialog | Navigation and preview transaction, baseline/highlight/rebase/cancel, open/close/rebuild | settings_nav/open flag/cleanup plus settings lambdas |
| SessionSelector | Session browsing, preservation and archive/unarchive confirmation | session_archive_confirmation and selector branches |
| BranchSummaryOperation | Generation/phase, prior selector state, start/poll/confirm/cancel/settle, deferred exit outcome | branch_summary_ui, related lambdas and exit flag |
| TranscriptView | Viewport, selection, scrolling, anchoring and interaction | parts of renderer/navigation; retain focused search collaborator |
| MermaidPresentation | Mermaid servicing and feature-specific publication, asking TranscriptView to preserve its viewport on layout changes | existing Mermaid controller and servicing lambda |
| ScreenRenderer | Terminal drawing resources, render/layout caches, invalidation and frame deadlines | RuntimeRenderer minus feature-owned interaction state |
| TuiInputDispatcher | Modal/focus precedence and decoded-event routing; little persistent state | outer-loop input branches |
| ActiveRun | Submission future/queue/completion/cancellation and active presentation state, serviced not blocking | RuntimeActiveRunController's nested loop/state |
| TuiEventLoop | One wait/dispatch mechanism and timer registrations | composition or specialization of utils::threading::EventLoop; decision pending |

Existing prompt coordinator/stash, plugin UI and subagent workspace remain focused collaborators unless evidence suggests a better boundary. Spinner is a small candidate active-presentation object using member-callback timer registration; it does not calculate global waits.

Questions for architecture approval: exact class names/count; who owns authoritative frontend display data versus a rendering snapshot; read-only projections versus copies; selector-host/feature ownership and lifetime; cache owner versus interaction owner; minimal references needed by each collaborator; how session transitions coordinate without cycles. A private monolithic snapshot with unrestricted mutable accessors is not encapsulation. Backend messages, permissions, sessions and process authority stay backend-owned.

## Existing scheduling and control-flow evidence

There are TWO important loops. Do not replace the outer wait and declare streaming migrated.

### Idle/editor outer loop in TuiFrontEnd.cpp

- Around 567–571: pre-loop signal check, Mermaid service and initial render.
- 575–598: Mermaid service, branch-summary polling, deferred-exit check, due render flush, subagent workspace poll.
- 599–605: choose input wait from 250 ms idle cadence, shortened by workspace and pending-render deadlines.
- 606: read_curses_input_with_timeout; runtime_input.cpp first consumes startup input, otherwise applies Context::timeout and performs ncurses input reading.
- 607–620: no input causes more workspace polling, due render flushing and settings reload checks, then continue.
- 622–679: signal handling; SIGTERM versus SIGINT differ; active branch summaries request cancellation/deferred exit, nonempty drafts can clear rather than terminate.
- 680–690: resize handling/redraw; 691–692 wheel governor filtering.
- 693–2465: modal/selector/navigation/composer/mouse/submit dispatch interspersed with render requests and immediate renders. A continue usually means one input was consumed; break can mean exit or failure. Goal 01 must classify every loop-exit path and distinguish nested-loop break statements.
- After loop: Mermaid/plugin cleanup, before-shutdown callback, exit code 130 for terminal signal, 1 for terminal-write/shutdown failure, otherwise 0 (verify current policy).

### Active submission loop

RuntimeSubmitController::submit synchronously calls active_run_controller_.run near runtime_submit_internal.cpp:597–600. RuntimeActiveRunController::run begins near runtime_active_run_internal.cpp:503, installs an event sink around 593–606 and launches frontend_.on_submit asynchronously around 624–648. It loops near 730 until the future is ready. Around 769–860 it services prompts/plugin UI, polls ncurses input, handles signals, advances a spinner, drains backend events, polls workspace/settings and renders. Around 797–807 it waits via submit_future.wait_for using cadence/deadline calculations. The event queue in runtime_state_internal.h was a mutex-protected queue without its own notification wait.

Therefore the outer loop is suspended while active submission consumes input and updates. Today's apparent multiplexing is timed polling, not a common epoll/eventfd wait. Goal 06 must enumerate every producer and nested wait, including prompt/plugin and external-editor/suspend handoffs, rather than assume the two main loops are the only blocking paths.

## Event/timer design requirements and decisions still needed

Use an event loop owner to hide global timeout calculations. Composition with EventLoop was suggested, not decided; the user also explicitly permits a specialized derived class or ownership in TuiFrontEnd. Do not invent a generic framework beyond actual clients.

- Proposed client meaning: a Spinner registers its elapsed member every 250 ms; rendering can register a one-shot flush; input decoding may register a partial-sequence deadline. Select API by writing real client call sites, with compileable temporary stubs if useful.
- Registrations need explicit cancellation and target-lifetime rules, preferably RAII. Define callback thread, cancellation during callback/dispatch, rescheduling, destruction order and repeating-timer missed-tick policy. Spinner should normally skip missed ticks rather than burst catch-up calls; other timer policies must be deliberate.
- Internally compute the earliest absolute deadline. Service due timers even if notifications/input keep arriving, not only on a timed_out result. Define fair input/notification work budgets and how leftover work prevents sleeping.
- Notifications are wakeups, not the authoritative payload. Publish queue/state first, then notify. Coalescing must not lose data. Identify event arrival, prompt/question publication, plugin UI requests, operation completion, workspace/settings changes and Mermaid completion producers, and their shutdown order. Do not pretend periodic external discovery has notifications without identifying its source.
- Dispatch application updates and terminal input when both are ready. Both may change presentation; request invalidation and paint a coherent frame after servicing sources, subject to urgent-paint/deadline policy. This is a proposed ordering policy to approve, not an established behavior.
- fd readability does not imply all decoded events are available only at that moment. Ncurses and startup/keyboard buffers may hold input after the fd becomes non-readable. Drain/service buffered input before blocking and determine whether ncurses decoding can itself wait for escape/mouse/multibyte sequences. Coordinate with the separate InputDecoder discussion rather than silently requiring its full implementation.
- Determine EOF/hangup/error and resize handling explicitly. Signals must wake or be handled by the loop even without terminal characters; do not infer a signal is equivalent to a stop request. Avoid unsafe calls to general notification APIs from a signal handler without verifying signal-safety.
- EventLoop.h currently says watch registration occurs before waiting, notify accepts only watch_event events, notifications coalesce and remain pending across waits, and terminate blocks until acknowledged and MUST NOT be called from the waiting thread. TUI-local exit needs a local stop outcome rather than calling blocking terminate on itself. Stop/join publishers before destroying the loop. Re-read current contracts before implementing.
- Preserve the terminal thread boundary and backend safety boundaries. This objective restructures scheduling and ownership; it is not an excuse to remove assertions, reassign terminal ownership, add locks indiscriminately, or change ncurses-threading scope rules from objective /08.

## Output expected from the analysis goals

Goal 01 inventory should include a complete declaration/alias inventory for both loops, a lambda/member/helper state read-write matrix, state-mutating free-function edges, cleanup/construction order, event dispatch precedence, and a loop control-flow map with wait/continue/break/return reasons. Every persistent item must have a proposed owner or an explicit reason to remain local/external.

Goal 02 architecture should include selected names, private member lists, meaningful operations/invariants, collaboration/lifetime diagram, and ownership of projections/caches/selector state. Record disagreements and obtain user decisions where real tradeoffs remain.

Goal 06 scheduling analysis should be refreshed AFTER the object extraction, identifying remaining implicit state and actual producers/deadlines/buffers. Goal 07 records approved callback and waiting contracts. Final handoff must distinguish compilation evidence from behavioral evidence; suggest deterministic owner/dispatcher/timer checks and PTY lifecycle checks for later work without claiming they ran or requiring them as this objective's gate.
