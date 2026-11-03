# Review of `astra/06-TuiFrontEnd_run_refactor` at `ab5189c`

Reviewed against the plan in `06/` (`handoff.md`, especially "Maintainer design requirements"), `AGENTS.md` and `docs/development/cpp-safety-rules.md`. Baseline for comparison: `ec4143e` (plan branch head, equal to `develop` plus the plan files). Paths and line numbers below refer to the astra tree at `ab5189c`.

The review is read-only: nothing on the astra branch was changed. Each finding has an ID (B = behavior, D = design, C = comments, T = tests, X = docs) so follow-up work can refer to it.

## Summary

- **Builds cleanly** here with GCC 13.3 (`cmake --preset dev`, `scripts/build.sh`): no errors, no warnings.
- **Test results:** see "Test results" below.
- **Key routing order is preserved.** The idle input precedence matches the old else-if chain, and composer resets, the signal table, exit codes (130 > 1 > 0) and the session-transition reset order all match the baseline.
- **No deadlock, use-after-free or worker-thread terminal access was found** in the active-run, prompt and plugin code. Some baseline bugs were fixed: an exception escaping a prompt no longer leaves the worker blocked, and the audit sink no longer dangles.
- **The maintainer's design goals are only partly met.** The giant function is gone, but a writable screen-state accessor is used 214 times, two god objects remain (`TuiSession`, `ActiveRun` with 15 constructor references), friendship is widened, and the code is barely commented.
- **There are a few real behavior changes and loop inefficiencies** (B1–B7).

## B. Behavior and correctness (fix first)

**B1. Plugin dialogs are cancelled while a permission/question prompt is open.** Confirmed.
- **Where:** `ActiveRun::consume` calls `poll_plugin_ui()` on every Running turn (`ActiveRun.cpp:400`), and `plugin_host_modal_conflict()` (`ActiveRun.cpp:335-343`) counts `permission_prompt`/`question_prompt` as a conflict. `runtime_plugin_ui_internal.cpp:507-519` then cancels a visible plugin modal, and `:584-590` answers Cancel to new modal requests.
- **Baseline:** the prompt ran its own blocking loop, and `service_pending_prompt()` returned `Serviced` → `continue` before `poll_plugin_ui()` (`base runtime_active_run_internal.cpp:769-781`). Plugin requests therefore stayed queued and appeared after the prompt closed.
- **Proposal:** do not poll plugin UI while `prompt_coordinator_.has_active()`, so requests stay queued as before. Add a regression test.

**B2. A decoded-but-discarded input ends the input batch, so buffered input waits up to 100 ms.** Likely.
- **Where:** `poll_curses_input` (`runtime_input.cpp:272-289`) returns `nullopt` both for "nothing there" and for a decoded `Key::Unknown`: a handled OSC 11 reply, an unknown escape sequence, or a failed `getmouse`. `TuiSession::service_input_batch` (`TuiSession.cpp:~1197-1203`) treats `nullopt` as "buffer empty", so the loop then blocks.
- **Why it matters:** bytes can still be waiting where epoll cannot see them: the `Context` keyboard replay buffer and the ncurses FIFO. They are then only read on the next 100 ms control tick. The baseline `continue`d straight into another read. This contradicts the handoff's claim "polls startup/ncurses buffered input before blocking".
- **Proposal:** return a three-way result (Input / Discarded / Empty) and keep draining on Discarded. Expose `Context::has_buffered_input()` (replay buffer plus startup queue) and include it in the "runnable" check before `wait(Block)`.

**B3. The main loop runs an extra non-blocking turn after almost every paint, and its order is hard to read.** Confirmed.
- **Where:** `TuiSession::run` (`TuiSession.cpp:1228-1271`) captures `timer_work = loop_.has_due_work()` before the second `service_due()`, which then handles exactly that work. `runnable` still includes `timer_work`, so `wait(ReadyOnly)` follows and a whole extra turn runs.
- **Why it happens:** paint readiness itself comes from a frame-timer callback (`runtime_render_internal.cpp:~173-178`), so each keypress or spinner frame costs two turns.
- **Proposal:** have `paint_ready_frame` paint when "frame requested and its deadline has passed", so the frame timer only supplies the wake deadline. Then remove the second `service_due()`, the second `service_presentations()` and `timer_work`. `run()` should read exactly: control → due timers → application updates → input → paint → wait.

**B4. Signals and resize never wake the loop; a 100 ms control timer polls for them.** Confirmed mechanism. This needs a maintainer decision (see "Decisions").
- **Mechanism:** `utils::threading::EventLoop::wait_impl` retries on `EINTR` (`utils/threading/EventLoop.cxx:283-286`), and `core::Signals::signal_handler` only sets an atomic bit. Signals are seen at the top of a turn or by `control_timer_` (100 ms, `TuiSession.cpp:33,81`).
- **Compared with baseline:**
  - Idle is better than before (the old idle wait was 250 ms).
  - During an active run, latency goes from ≤16 ms to ≤100 ms.
  - The timer adds a permanent 10 Hz idle wakeup.
- **Requirement not met:** the handoff asks that "signals must wake or be handled by the loop even without terminal characters".

**B5. Idle wakeups rose from about 4/s to about 12/s, and Mermaid polls at 60/s while anything is pending.** Confirmed.
- **Where:** the 100 ms control timer and the 500 ms display-reload timer are always armed. Mermaid repeats every 16 ms whenever a request is outstanding or deferred (`runtime_mermaid_internal.cpp:~98-104`). Each wakeup runs a full turn.
- **Proposal:**
  - After B3/B4, lengthen or drop the control timer.
  - Make Mermaid bridge completion publish its result and then call `TuiWakeup`, with a back-off (for example 100→250 ms) only for Busy retries.

**B6. `service_control` doubles as the control timer's callback, which breaks the loop's own callback contract.** Confirmed.
- **Where:** it runs at the top of every turn and again as a timer callback (`TuiSession.cpp:73,1236`). As a callback it cancels the active run, completes worker prompt requests, cancels the branch summary and renders. `TuiEventLoop.cpp:122` says callbacks "may only publish state or invalidate a frame".
- **Proposal:** make the control timer callback a no-op wake (or remove it, see B4) and handle signals only in the control stage.

**B7. A failed prompt paint is no longer reported to the worker as an I/O error.** Confirmed.
- **Where:** an attached `RuntimeRenderer::render()` (`runtime_render_internal.cpp:~381-393`) only invalidates, and returns false only after an earlier failure. The prompt paths at `runtime_prompts_internal.cpp:~161-163,316-318` therefore no longer return a paint failure to the worker. This contradicts `event-contracts.md` ("Modal Io errors are returned to the worker as before").
- **Proposal:** propagate the end-of-batch paint failure to the claimed prompt, or document the new policy and test it.

**B8. `ActiveRun::shutdown() noexcept` can call `std::terminate`, and it silently swallows errors.**
- **Where:** `ActiveRun.cpp:589-626`. Some calls are wrapped in `catch (...) {}` with no logging (`:603, :613, :621`). Others are not wrapped: `fail_pending_requests()`, `plugin_ui_.finish_submission()` (which can allocate) and `retire_operation()`.
- **Proposal:** wrap every cleanup step the same way, and record the failure (diagnostic or `Dout`) instead of silently ignoring it.

**B9. Fragile "interrupt" of a claimed prompt request.** Confirmed mechanism; not reachable today.
- **Where:** `fail_pending_requests()` interrupts the claimed request (`runtime_prompts_internal.cpp:~798-804`). Because `complete()` stores only the first result, a later real answer from the UI is silently dropped. It is correct today only because `cancel()` calls `cancel_active()` first; `request_stop()` and `request_close_after_submit()` do not.
- **Proposal:** remove `claimed_request_`, `interrupt_claimed_request` and `retire_claimed_request`. `claimed_request_` always equals `active_request_`, and `cancellation_pending_` plus `service()` already complete the claim.

**B10. Smaller behavior items.**
- **Settings preview survives Ctrl+D inside Settings.** This is a baseline bug, but `SettingsDialog` now claims to own this invariant.
  - Path: Archive → `SessionSelector::archive` closes a non-Session list (`TuiSession.cpp:~411-414`, `SessionSelector.cpp:112-117`) without telling `SettingsDialog`.
  - Fix: route Archive to the active kind's cancel. Better, have `SelectorHost` notify the previous owner whenever its view is replaced or closed.
- **Active run: input retained after a claimed signal is still dispatched** (`TuiSession.cpp:~1204-1205`). The idle path drops it. Possible double Interrupt.
- **Selector lists are deep-copied into the snapshot on every arrow key** (`SelectorHost::publish`, `SelectorHost.cpp:102`). Transcript search also copies the view out and back in (`runtime_transcript_search_internal.cpp:749,783`). The baseline mutated in place. Proposal: let the renderer read the host's view through a const reference.
- **New behavior to confirm as intended:**
  - The spinner and event rendering continue while a prompt is open; the baseline froze both.
  - After a stop, keys are decoded and dropped until the worker ends.
  - HUP during a permission prompt emits a "selected deny" audit.

## D. Design requirements not met

**D1. `RuntimePresentationState::screen()` / `sidebar()` return mutable references to public-field structs.** Confirmed.
- **Where:** `runtime_state_internal.h:137-143`. There are 214 `screen()` uses, about 88 of them direct field writes (TuiSession 39, SelectorInputController 16, SettingsDialog 13, SessionSelector 10, ActiveRun 7, BranchSummaryOperation 3).
- **Composer's projection is written from outside Composer:** `TuiSession` writes `screen().selected_slash_command_index = draft_state_.completion_index()` at `TuiSession.cpp:193,492,671,1112`, and `ActiveRun.cpp:383` does the same.
- **Rule conflict:** Astra filed this as a "deliberate exception", but handoff rule 1 says the exception "is not permission to expose writable scalar flags, containers of raw state, or public-field state bags". Handoff line 180 also says "A private monolithic snapshot with unrestricted mutable accessors is not encapsulation."
- **Proposal:**
  - Give each feature named publish operations (status text, command output, tool presentation, prompt projection, etc.).
  - Move `selected_slash_command_index` into `Composer::project`.
  - Expose only a const view to everyone else.

**D2. `RuntimeRenderer::transcript()` returns a mutable `TranscriptView&`** (`runtime_render_internal.h:126`).
- **Problem:** 32 mutating calls go through it. They skip the `lock_ui()` and `synchronize_timers()` that the renderer's own wrappers perform (`runtime_render_internal.cpp:333-352`). Callers must also call `prepare_transcript_layout()` first and pass its result in (`runtime_navigation_internal.cpp:97-100`).
- **Proposal:** add the few operations callers need (scroll, live tail, selection) to `RuntimeRenderer`, and make `transcript()` const.

**D3. `ActiveRun` is a second god object.**
- **Size:** its constructor takes 15 references (`ActiveRun.h:64-68`), the "original fifteen references" the handoff forbids. It has about 47 methods and about 33 data members, 9 of them bool flags.
- **Rule 3 violation:** its definitions are spread over `ActiveRun.cpp` (801 lines) and four `runtime_active_run_input_*_internal.cpp` files (about 1050 lines).
- **Extra complexity:**
  - Completion takes two loop turns plus a `Finalizing` phase (`ActiveRun.cpp:484-562`). It needs `final_projection_serviced_`, special cases in `has_pending_updates`/`accepts_input`, and input buffering in TuiSession.
  - The stop-request plumbing is dead: `service()`/`start_claimed_request()` can never return `RequestRunStop` (`ActiveRun.cpp:314,318-319,445-451`, `runtime_prompts_internal.cpp:834-835,849`).
- **Proposal:**
  - Extract active-run input routing into its own class (CamelCase files), reusing `TuiInputDispatcher` stages.
  - Group per-run state in a private `std::optional<Operation>` struct instead of resetting 8 bools by hand in `start()`.
  - Drain the frozen backlog and settle in one `complete()` call, removing the `Finalizing` phase.
  - Delete the dead `RequestRunStop` branches.
  - Replace the positional-bool `ActiveRunOutcome(bool,bool,bool)` with named factories.
  - Inject the terminal context instead of `Application::instance().terminal_context()` (`ActiveRun.cpp:253`).

**D4. `TuiSession` still contains feature algorithms.**
- **Size:** 1284 lines. The biggest handlers: `handle_composer_navigation_input` 183 lines, `handle_selector_input` 158, `handle_session_input` 106, `handle_transcript_input` 105. `architecture.md:11` itself says TuiSession should own "ordering and cross-owner transitions, not menu/edit algorithms".
- **Duplication:**
  - The palette previous/next plus "command N/M" status block is copy-pasted six times (`TuiSession.cpp:~894-1003`) and again in `runtime_active_run_input_edit_internal.cpp`.
  - There are 78 copies of `key_matches_action(frontend_.key_bindings(), TuiAction::X, event.key)`.
- **Structure:** the handlers are slices of the old chain cut by position, not by owner. The order dependencies between slices (ClearInput before Interrupt, Space after Submit) are only implied by the registration order (`TuiSession.cpp:63-72`), with no comment.
- **Proposal:**
  - Add a private `is_action(TuiAction, InputEvent const&) const`.
  - Add a single `Composer` key handler returning `{handled, status, beep}`, with the original order in one documented function.
  - Add `Composer::step_completion(direction)` returning the status text.
  - Let `SelectorHost` dispatch to the active feature (SettingsDialog / SessionSelector).
  - Give command output its own small owner.
  - Have `SettingsDialog::resolve` return a typed result instead of `TuiSession` comparing `*command == "/keybindings set "` (`TuiSession.cpp:335`).
  - Document the dispatcher stage order.

**D5. Five new `friend` classes on `TuiFrontEnd`** (`TuiFrontEnd.h:18-22`: TuiSession, SettingsDialog, SessionSelector, BranchSummaryOperation, SelectorInputController).
- **Problem:** they expose all protected members, including the data fields `key_bindings_` and `options_`. There is also `friend class SessionTransitionCoordinator` in `runtime_state_internal.h:149`.
- **Proposal:** a narrow authority interface (the callbacks actually used) passed to those classes.

**D6. A public-field state bag survives:** `RuntimePluginUiCoordinatorState` (`runtime_plugin_ui_internal.cpp:217-239`, 16 public fields, mutated by free functions such as `queue_clear_locked`). The refactor even added a field to it. Proposal: give it private members and member operations.

**D7. Generated-printing opt-outs.**
- **Reasonable:** the content owners (Composer, ActiveRunEvents, ActiveRunInbox, PendingAttachments, the Pending*Request classes, TranscriptView, SelectorHost, BranchSummaryOperation).
- **Not justified:**
  - `TuiWakeup`: `weak_ptr` needs no locking to print; add a `weak_ptr` printer.
  - `TuiWaitState`: an atomic bool prints fine elsewhere, e.g. `GitBranch.h`. The real blocker is that `utils::threading::EventLoop` has no printer. Say so, or add one.
- **Weak:** classes holding only references (SessionTransitionCoordinator, SelectorInputController, mostly SessionSelector/SettingsDialog). Make reference members printable as type@address and switch them to ON.
- **Also:**
  - `TuiSession`'s stated reason should be the real one: its members' types opt out.
  - The new anonymous-namespace `AcceptedIdentity` (`runtime_mermaid_internal.cpp:16-23`) has no reason comment.
  - `RuntimeSubmitController` has its marker in the private section (`runtime_submit_internal.h:67`).

**D8. File and type layout.**
- `TranscriptView.h` also declares `TranscriptPositionIndicatorState` and a public-field struct `DeferredDetachedViewport` (rule 3).
- `Composer` is not `final` and is copyable.
- `PendingPermissionRequest` and `PendingQuestionRequest` are identical apart from the result type and interrupt value; use one template.

**D9. Dead code, an over-wide API, and test-only code in production.**
- **Unused:** `Composer::escape_pending()` and `TranscriptView::selection_range()`.
- **Should be private:** `Composer::replace_selection` / `delete_selection` / `selected_text`, and `TranscriptView::discard_deferred_update` / `has_pointer_interaction`.
- **Mutable access to a borrowed object:** `RuntimeActionController::selectors()` returns a mutable `SelectorHost&` and has one caller (`ActiveRun.cpp:308`).
- **Test-only modes in production classes:** `RuntimeRenderer::flush_pending_render` / `service_auxiliary_timers`, workspace `poll` / `next_poll_`, and `RuntimeActionController::loop_attached_`. AGENTS.md asks to move these out of production interfaces.
- **`lock_ui()` gives no protection:** `ui_mutex_` is only ever taken on the TUI thread, and the new code locks it in some places and not others. Either document it as TUI-only re-entrancy protection or remove it.
- **Redundant state in the active-run classes:**
  - `ActiveRunInbox` double-locks (`mutex_` plus the queue's own mutex).
  - `terminal_published_` duplicates `terminal_`.
  - `ActiveRunEvents::received_events_` is never read.
- **Two-way coupling:** `Composer::force_path_completion` / `select_file_reference` take `RuntimeNavigationController&`, while navigation already borrows `Composer&`.

**D10. `TuiWaitState::disable()` and `enabled_` guarantee nothing.**
- **Why:** a producer can pass the `enabled_` check, then `disable()` returns, then the producer's `notify()` still runs. That is harmless only because the producer's `weak_ptr` lock keeps the waiter alive. The header text "disables admission before releasing its share" suggests a lifetime guarantee that does not exist.
- **Proposal:** either delete it, or document it honestly as a syscall-saving shortcut that gives no lifetime guarantee.

## C. Comments (the maintainer's main complaint)

The new classes are close to uncommented. Where comments exist, they are mostly one-line lifetime statements ("X outlives Y") rather than explanations of how the mechanism works.

Approximate counts ("commented" means a meaningful comment directly on the declaration):

| Class | Class comment | Methods commented | Data members commented | .cpp comment lines |
|---|---|---|---|---|
| TuiSession | yes | 1/28 | 4/29 | 39 / 1284 lines |
| ActiveRun | yes | 3/47 | 1/33 | 29 / 801 |
| ActiveRunEvents | yes | 0/16 | 0/13 | 0 / 263 |
| ActiveRunInbox | yes | 3/19 | 0/8 | 0 / 156 |
| ActiveRunOutcome | no | 0/4 | 0/3 | – |
| BranchSummaryOperation | no | 1/13 | 0/14 | 2 |
| Composer | no | 4/64 | 0/13 | 0 / 616 |
| PendingAttachments | yes | 0/4 | 0/2 | 0 |
| PendingPermission/QuestionRequest | yes | 1/5 each | 0/5 each | 0 |
| SelectorHost | yes | 1/11 | 0/3 | 3 |
| SelectorInputController | yes | 1/4 | 0/5 | 6 |
| SessionSelector | no | 1/8 | 0/6 | 0 |
| SessionTransitionCoordinator | yes | 2/2 | 0/8 | 0 |
| SettingsDialog | no | 0/14 | 0/6 | 17 |
| Spinner | yes | 0/5 | 1/2 | 1 |
| TranscriptUpdate | no | 0/2 | 0/2 | – |
| TranscriptView | yes | 4/40 | 0/10 | 3 |
| TuiEventLoop | yes | 1/18 | 1/5 | 5 |
| TuiInputDispatcher | yes | 1/2 | 0/1 | 0 |
| TuiTimer | yes | 0/7 | 0/2 | 0 |
| TuiTimerRegistration | yes | 0/8 | 0/4 | 5 |
| TuiWaitState | yes | 0/6 | 0/7 | 0 |
| TuiWakeup | yes | 0/1 | 0/1 | 0 |

**What the maintainer wants:** a comment that explains the high-level effect, not what the code literally does. For every member, say:
- **why it exists** and which part of the mechanism it is;
- **which thread(s)** may call it or touch it;
- **ordering and lifetime:** what must have happened before the call, and what the caller can rely on afterwards — above all, which objects must outlive which, and when it becomes safe to destroy something;
- **for data members:** what the value means, who sets or clears it, and on which thread.

A comment that restates the code, such as "sets `enabled_` to false", does not count.

Worked example (verify against the code before reusing it; see D10):

```cpp
// The OS wait resources (epoll fd + eventfd, via utils::threading::EventLoop)
// behind TuiEventLoop. Owned by TuiEventLoop through a shared_ptr; producer
// threads only ever hold a TuiWakeup (a weak_ptr to this object). Because a
// producer briefly locks that weak_ptr inside notify(), this object can outlive
// TuiEventLoop for the duration of an in-flight notify(), and its destructor
// may then run on that producer thread.
class TuiWaitState final
{
 public:
  // Any thread. Wakes the TUI thread's wait() if it is blocked, or makes the
  // next wait() return immediately. Carries no data: the producer must publish
  // its payload (queue entry, result) BEFORE calling this, or the TUI thread
  // can wake, find nothing, and go back to sleep.
  void notify() noexcept;

  // TUI thread only, called once from ~TuiEventLoop. After it returns, newly
  // started notify() calls do nothing, but one already in progress may still
  // complete; that is harmless because it only touches this object, which it
  // keeps alive. disable() does NOT make it safe to destroy anything a
  // producer uses: owners must stop/join their producer threads before the
  // objects those threads reference are destroyed.
  void disable() noexcept;

 private:
  std::atomic_bool enabled_{true};   // Cleared by disable(); notify() checks it to skip pointless wakeups after the loop is gone.
};
```

More examples of the expected level:
- **`TuiEventLoop::wait`:** "Called once per `run()` turn, on the TUI thread, after every source has been serviced. Block sleeps until the earliest armed timer deadline, input-fd readiness or a `TuiWakeup`; ReadyOnly only collects readiness. It consumes notification bits, so every mailbox must be re-examined each turn. It returns immediately once stop, hangup, error or notifier failure is latched. Signals do not interrupt it (EventLoop retries EINTR)."
- **`TuiTimerRegistration::generation_`:** "Bumped by every arm/cancel/repeat and by `prepare_dispatch`. `service_due` snapshots (id, generation) before running any callback, so a registration re-armed or cancelled by an earlier callback in the same pass is skipped as stale."
- **`Composer::pending_escape_clear_`:** "Armed by the first Esc on a non-empty draft. A second Esc clears the draft. Any edit, navigation or scroll disarms it, so the clear needs two consecutive Escapes."
- **`ActiveRunInbox::publish_terminal`:** "Worker thread, exactly once, as the last action after every backend callback scope has been destroyed. The TUI may join right after taking this payload."

**Comments that are currently wrong:**
- `ActiveRun.h:76` says `service_control` is "called once … before timers"; it has three callers.
- `ActiveRun.h:123` names the wrong dispatch function.
- The comment at `ActiveRun.cpp:510-511` belongs above `retire()` at `:504`.
- The docs say "50 ms Escape", but `config.h:18` is 100 ms, plus ncurses `ESCDELAY`.

## T. Tests

**Weakening check.** No assertion was weakened or deleted to make a test pass. The number of `expect` checks per file is equal or higher. Two coverage losses and one weak spot:
- **T1. Lost branch coverage.** `test_unauthorized_command_event_tool_merge` (`tests/tui_command_output_tests.cpp`) no longer reaches the settled-pending-tool merge branch (`ActiveRunEvents.cpp:77-81`). The canceled call now arrives as a `ToolResultEvent`, which always goes to the transcript. Restore a case that seeds a settled, Canceled pending tool.
- **T2. Removed without replacement.** The baseline check "a buffered input at the frame deadline services provider updates before input drainage" (`ActiveRunCadence`) was removed, and nothing tests the equivalent ordering in `TuiSession::run`.
- **T3. Timing-dependent test.** The new retained-input/prompt-race case in `test_active_run_session_transition_discards_prior_session_events` spins until the question is published. Make it deterministic.

**Untested.**
- **T4.** Nothing tests `TuiSession::run` itself: the 256/32 budgets, the ReadyOnly vs Block choice, the closing-drain path or signal observation. The descriptor-error path and `notification_failed` are untested too.
- **T5.** These classes have no direct tests: ActiveRunInbox, TuiInputDispatcher, BranchSummaryOperation, SelectorInputController, TranscriptView and TuiWaitState. Add focused unit tests for the loop turn order (B3), buffered-input draining (B2), plugin-during-prompt (B1) and timer-callback contract (B6).

## Test results

(See below; filled in from the full-suite runs on both trees.)

## Decisions for the maintainer

1. **Signal wakeup (B4).** Options:
   - (a) Add an opt-in "interrupted" result to `utils::threading::EventLoop::wait` and use `epoll_pwait2`'s sigmask so SIGINT/SIGTERM/SIGWINCH are unblocked only inside the wait. This changes the `utils` submodule.
   - (b) Have the signal handler also write to an eventfd that the loop watches. `write(2)` is async-signal-safe. This changes `core::Signals`.
   - (c) Keep the 100 ms poll and document it as the accepted latency.

   Options (a) and (b) touch code outside the TUI, and the plan says not to change signal-handler policy without approval.
2. **Screen state sharing (D1).** Is a shared writable `ComposerSnapshot` acceptable on the TUI thread, or must it become per-feature publish operations? The handoff text says the latter; this review recommends the latter.
3. **`Runtime*` names.** Astra kept old names (`RuntimeRenderer`, `RuntimePromptCoordinator`, …) for evolved owners, while the plan's naming rule is CamelCase files per class. Keep them, or rename them into `CamelCase.h` files?
