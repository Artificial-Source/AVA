# Second review: `astra/06-TuiFrontEnd_run_refactor` at `d48292c`

This review checks the fixes for the first review (`review-astra-ab5189c.md`), which Astra made in the 24 commits `ab5189c..d48292c`. It is read-only: nothing on the astra branch was changed.

Paths and line numbers refer to `d48292c`. New finding IDs start with **R** (round 2). Where an R finding continues a first-round finding, the old ID is in brackets.

## Summary

Most of the first review is now properly fixed:
- the main loop reads clearly;
- the screen state is no longer writable from outside;
- the god objects are smaller;
- the friend declarations are gone;
- every member has a comment.

What is left:
- **One new bug:** a busy-loop introduced by the B10 fix (R1).
- **Comments that are now complete and mostly true, but hard to read** (R2–R3).
- **Design fixes that only partly landed** (R12–R16).
- **A few test gaps** (R17–R20).

## Test results

Same machine as the first review: Ubuntu 24.04 container, GCC 13.3, `cmake --preset dev`, `LANG=C.UTF-8`.

| Check | Baseline `ec4143e` | Astra `d48292c` |
| --- | --- | --- |
| Build | OK, no warnings | OK, no warnings (after `--target generate-print-members`, as AGENTS.md prescribes for an incremental build) |
| Full suite | 224/231 pass, 7 fail | 235/242 pass, 7 fail; all 11 new tests pass |
| Failing tests | `tools`, `command`, `app_rpc`, `lsp`, `containment`, `live_git_self_test`, `live_dogfood_launchers` | the same 7 (container-specific) |
| tmux TUI scenarios, all 23 | 23/23 pass | 23/23 pass |
| New and changed TUI tests repeated 20× (`ctest --repeat until-fail:20`) | — | 23/23 pass, no flakes |

Astra's own environment could not run the tmux suite: `libdw.so.1` was missing. The table above is therefore the first end-to-end TUI evidence for this version.

## Status of first-review findings

| ID | Status | Notes |
| --- | --- | --- |
| B1 | Fixed | Small leftover delay, R4 |
| B2 | Fixed | |
| B3 | Fixed | One pass per turn; no extra turn after paint |
| B4 | Fixed as decided | 100 ms poll kept and documented honestly |
| B5 | Partly | Mermaid fixed; idle wake rate unchanged (R6) |
| B6 | Fixed | One remaining timer callback does real work (R7) |
| B7 | Fixed | Hardening suggestion R8 |
| B8 | Fixed, with caveats | R9 |
| B9 | Fixed | The API still allows the hazard (R10) |
| B10 | Fixed, but introduced R1 | Settings route untested (R17) |
| D1 | Partly | `screen()`/`sidebar()` are const; uses dropped from 214 to 124 (all reads). Many publish operations are one-line setters (R12) |
| D2 | Fixed | `transcript()` is const; mutations stay inside the renderer |
| D3 | Partly | `Operation`, named outcomes and single-call completion are done. `ActiveRunInput` duplicates Composer key policy (R13) |
| D4 | Partly | `TuiSession.cpp` 1284 → 863 lines; `key_matches_action` copies 78 → 1; palette copies gone. The logic moved into a 271-line `Composer::handle_key` (R13) |
| D5 | Partly | Friends were removed by making 28 protected frontend methods public (R14) |
| D6 | Fixed | |
| D7 | Mostly | R15 |
| D8 | Fixed | One leftover, see R16 |
| D9 | Mostly | New test-only code, R16 |
| D10 | Fixed | |
| C | Coverage fixed; readability not | R2, R3 |
| T1–T5 | Mostly | R17–R20 |

## Behavior and correctness

**R1. Busy-loop at 100% CPU after an empty-draft Ctrl+C or Ctrl+D (or SIGINT/SIGTERM) during a run, if any key arrives before the worker stops.** Medium; introduced in `2babd33` and kept by `7ad1c02`. Confirmed by code reading in three independent traces; not reproduced at runtime.
- **Why it happens:**
  - `ActiveRun::accepts_input()` returns false once `close_after_submit` is set (`ActiveRun.cpp:465-468`).
  - `TuiSession::service_input_batch` then returns without reading (`TuiSession.cpp:772-773`).
  - With no pending updates, the loop calls `wait(Block)` (`:845-846`). But the terminal fd is level-triggered (`EPOLLIN`, no `EPOLLET`), so unread bytes make the wait return at once, every turn.
  - Each turn runs control, timers, consume, plugin poll and paint, until the worker exits, which can take seconds.
- **Before:** at `ab5189c` keys were decoded and dropped; the baseline blocked in `future.get()`.
- **Fix:** in the close-after-submit state, decode and discard input as `ab5189c` did, or sleep on a deadline instead of fd readiness while input is deliberately left unread.
- **Regression test:** a session test that writes bytes after close-after-submit while the worker is held, and asserts that the number of loop turns stays bounded.

**R4 [B1]. After a prompt is answered, a deferred plugin dialog and the spinner restart wait up to 100 ms.**
- `complete_permission`/`complete_question` (`runtime_prompts_internal.cpp:569-583`) do not wake the loop, and `ActiveRun::has_pending_updates` (`ActiveRun.cpp:470-473`) does not count queued plugin requests.
- Fix: notify in `complete_*`, or count queued plugin requests when no prompt is active.

**R6 [B5]. The idle UI still wakes about 10–12 times per second** (baseline about 4).
- The 100 ms control timer and the 500 ms display-reload timer are armed out of phase.
- Fix: run display reload on every fifth control tick, or align their phases. That gives a steady 10/s, which is acceptable under the B4 decision.

**R7 [B6]. `BranchSummaryOperation::discover_progress` calls the frontend (`branch_summary_snapshot()`) and can settle the operation from inside a timer callback** (`BranchSummaryOperation.cpp:35-42`). That breaks the rule at `TuiEventLoop.cpp:122` ("callbacks only publish state or invalidate a frame"). Fix: the callback only marks "discovery due", and the work runs in `service_presentations`.

**R8 [B7, hardening].** Other failure exits (a failed handoff paint, `RenderFailed` from consume) still abandon the claimed prompt as "interrupted". Fix: after the loop, call `fail_active_render()` whenever `terminal_write_failed_` is set.

**R9 [B8].**
- **Failures are silent in release builds.** Shutdown reports cleanup failures only through `Dout` (`ActiveRun.cpp:603`), which compiles to nothing without CWDEBUG. Fix: latch a `cleanup_failed` bit into the returned outcome (non-zero exit) or use the sanitized diagnostics layer.
- **Shutdown can now hang.** If a release step throws (`:619-620`), the worker may stay blocked in `PendingRequest::await()`, and the join at `:622-625` then hangs forever. Before, the process terminated. Fix: decide (terminate after logging, or a documented hang) and say so in a comment.

**R10 [B9]. `PendingRequest::interrupt()` still stores a result on a claimed request** (`PendingRequest.h:65-74`). A later real answer would then be dropped silently, which is the B9 bug; only caller discipline prevents it. Fix: rename it to `interrupt_queued()`, add `ASSERT(!resolving_)` with the required comment, and correct "either thread may cancel" (only the TUI calls it).

**R11. Smaller items.**
- A Mermaid bridge without `set_completion_notifier` never discovers completions. Production always sets one; add a slow fallback retry or an `ASSERT`.
- Mermaid completions and display-reload still apply while a host prompt is open; the baseline froze them. Confirm whether that is intended.

## Comments and documentation (the maintainer's main complaint)

**R2. Comment coverage is now 100%, and most comments are true.** About 45 claims were checked against the code. These are false or misleading:
- **`ActiveRun.h:85` (`cancel`), "completion still owns worker join".** False for `Close`: the hangup path never calls `complete()`; `shutdown()` joins the worker after the loop. Also state what the bool return means.
- **`ActiveRun.cpp:392-393`, "Its worker may borrow the current view".** Contradicts `runtime_prompts_internal.h:220` ("workers never access prompt views"). What is borrowed is worker-stack data used by TUI-thread callbacks (e.g. `QuestionPrompt::auto_resolve`); say that.
- **`TuiSession.cpp:95`, "Attach timers only after that point".** `Spinner`, `RuntimePromptCoordinator` and `control_timer_` register timers in the initializer list. The true rule is that nothing dispatches before `run()` calls `service_due`.
- **`PendingRequest.h:97`** says "both shared owners"; there are three (resolver, queue, TUI claim).
- **`ActiveRunInbox.h:54` (`set_request_id`)** says "TUI before launch or producer thread"; it is only called on the TUI thread.
- **`config.h:17`** says the value is ignored when `ESCDELAY` is set. Only `set_escdelay` respects `ESCDELAY`; the escape-continuation read always uses the 100 ms constant.
- **`docs/interfaces/themes-keybindings.md:149-150`** still says "50 ms" for Escape assembly; the real value is 100 ms.
- **Undocumented return values:** `ActiveRun::handle_modal_input` (`optional<bool>`: what does `false` mean?), `ActiveRun::cancel` and `request_stop`.

**R3. The comments are hard to read.** They meet the coverage target but not the readability target that was set.
- **Undefined jargon.** In the main TUI files, "authority" appears about 60 times, "receipt" 47 (defined nowhere), and "chrome" 20 (defined nowhere). "Admission", "claim", "projection", "publication" and "settle" are also used without definition.
- **"terminal" has two meanings:** the TTY, and the worker's final result (`ActiveRunTerminal`, `publish_terminal`, "another terminal key"). Rename the result meaning to "final result" or "outcome".
- **Filler:** every deleted copy/move operator has its own comment (e.g. four in `TuiEventLoop.h:47-54`). One line such as "Not copyable or movable: callbacks hold its address" is enough.
- **Stacked duplicate comments** left over from successive edits, in about 12 places: `TuiSession.h:156-157, 161-162, 209-210, 214-218`; `Composer.h:164-166, 176-178, 248-251`; `TranscriptView.h:59-62, 73-76, 87-90, 142-145`.
- **Examples of the needed rewrite:**
  - `ActiveRunEvents.h:20`, "Exact receipt identity is the sole command-event conversation authority." → "For a slash command, an event enters the conversation transcript only if its request ID is one the backend returned in `ordinary_turn_request_ids`, meaning a real model turn."
  - `ActiveRun.h:164-165`, "…so worker prompt authority owns the visible surface and its input." → "Stop the spinner and close search, so the permission/question prompt is the only thing on screen receiving keys."
  - `ActiveRun.h:89`, "a published completion blocks another terminal key" → "Once the worker has published its final result, no more keys are handled until `complete()` has processed it."
  - `ActiveRunInbox.h:115`, "preserve first-publication wins after TUI take moves the payload out" → "Remembers that the TUI already took the result, so a second `publish_terminal()` is still rejected."
  - `TranscriptView.h:184`, "eviction debt applied before rebinding live selection" → "How many items the size cap removed from the front since the selection was last adjusted; added to the selection's indices before use."
  - `TranscriptView.h:130-131`, "copy from supplied painted authority… invalid authority fails closed" → "Copy using the last painted layout. If it no longer matches the selection, copy nothing and report failure."
- **`docs/engineering/tui-architecture.md` is a good map,** and 8/8 spot-checked claims are true. Three problems:
  - It never defines its core vocabulary. Add a short glossary and use only those terms in code comments.
  - The "add a key binding / modal" sections are checklists of questions, not steps. Name the actual places: the `ActiveSelectList` kind, the `SelectorHost::dispatch` branch, and the two handler lists (`TuiSession::input_dispatcher_` and `ActiveRunInput::dispatcher_`).
  - It refers to "issue #93" and "the review handoff", which a newcomer cannot find from `docs/`. Link them or remove the references.

## Design (partly fixed)

**R12 [D1]. Screen state.**
- **Many trivial publishers.** `RuntimePresentationState` has about 71 operations, 28 of them one-line setters (`publish_status`, `publish_processing(bool)`, `publish_spinner`, `publish_position_indicator(bool)`, …). 21 classes hold it mutably and any of them can publish any field; `publish_status` alone has 213 calls in 17 files. `publish_composer_feedback` duplicates `publish_status`.
- **Publishing is not tied to mutation.** `RuntimeActionController::clear_draft_for_interrupt` changes the draft, but all four callers must remember to call `publish_completion` afterwards (`TuiSession.cpp:187,245,477`, `ActiveRunInput.cpp:428`). `publish_composer(Composer&)` also mutates its argument, because `project()` clamps the cursor (`Composer.cpp:732`).
- **Fix:**
  - Group the setters into per-feature publications (e.g. one `publish_run_frame(...)` for ActiveRun) and delete `publish_composer_feedback`.
  - Operations that change the draft publish it themselves.
  - Clamp the cursor at mutation time, so `project()` can be const.

**R13 [D3/D4]. Duplicated key policy, and one oversized function.**
- `ActiveRunInput` (880 lines, 12 references) re-implements `Composer::handle_key`'s jump, edit, history and palette phases, including identical status strings ("jump cancelled", "history previous"). Compare `ActiveRunInput.cpp:372-387, 586-596, 646-700` with `Composer.cpp:24-42, 62-89, 175-212`.
- `Composer::handle_key` (`Composer.cpp:16-286`) is 271 lines with a capturing `is_action` lambda.
- **Fix:** split `handle_key` into per-phase private methods plus an edit-action table, give it a `ComposerEditContext`, and have `ActiveRunInput` call it (mapping scroll requests to its navigation calls).
- **Optional (maintainer call; the duplication predates the refactor):** `RuntimeSubmitController::submit` is 505 lines, and its local commands (/details, /sidebar, /stash, /jobs, /search, display) are duplicated in `ActiveRunInput::run_active_command`. One shared local-command router would remove that.

**R14 [D5]. The friends were replaced by widening the public interface.**
- `TuiFrontEnd` now has 28 previously protected backend-action methods in its public section (`TuiFrontEnd.h:45`), so any code holding a `TuiFrontEnd&` can call `on_session_selector_*`, `on_branch_summary_*`, `on_toggle_mode`, and so on.
- Fix: one small interface per feature (or one `TuiFeatureActions`) that `TuiFrontEnd` implements and passes to the five features, keeping the methods themselves protected. Also replace the mutable `TuiKeyBindings& key_bindings()` with `apply_key_bindings(TuiKeyBindings)`.

**R15 [D7]. Debug printing.**
- `SelectorHost` opts out with a stale reason. Its only members are a reference and an enum; switch it to ON.
- Stale "borrowed projection" reasons on `ActiveRun` (`ActiveRun.h:116`) and `RuntimePluginUiCoordinator` (`runtime_plugin_ui_internal.h:104`). For ActiveRun the real blocker is `Operation`; give it a small printer that shows only its flags.
- `ComposerInputResult`, `DraftCommand` and `SelectorDispatchResult` can use `NoLogString` (as `PendingSessionArchiveAction` does) and switch to ON.
- Raw pointer members are printed via `utils::print_pointer`, which dereferences them. Make `print_member` print raw pointers as an address only, or document the hazard.
- `MemberReference` lives in `print_reference.h` (rule 3); rename the file or record an exception.

**R16 [D8/D9]. Layout and test-only code.**
- **Result structs in another class's header:** `ComposerInputResult` (`Composer.h:91`), `SelectorDispatchResult` (`SelectorHost.h:38`), `DraftCommand` (`SettingsDialog.h:39`) and `InputPollResult` (`runtime_input_internal.h:66`). Either move them to their own headers or record the exception. `SelectorDispatchResult::text` means different things per `request`; use a `std::variant`.
- **Name collision on case-insensitive filesystems:** `Composer.{h,cpp}` and the existing `composer.{h,cpp}` differ only in case (from `c8e551e`, missed by the first review). Rename one of them (maintainer decision).
- **New test-only code:**
  - `detail::merge_settled_tools` was exported for T1 (`ActiveRunEvents.h:28`).
  - `RuntimeRenderer::loop_attached_` and `RuntimePluginUiCoordinator::loop_attached_` keep fixture-only modes.
  - New accessors have no production caller: `TranscriptView::frozen_sidebar/new_output_count/position_indicator_visible`, `RuntimeRenderer::painted_frame`, `Composer::saved_draft/path_completion_forced/scroll_to`.
  - Production no longer calls `runtime_input::read_curses_input_with_timeout`/`read_curses_input`; only tests do.

  Fix: make tests attach a real `TuiEventLoop` (the session tests already do), delete those modes and readers, test `merge_settled_tools` through `ActiveRunEvents`, and either remove the test-only accessors or justify them.
- **Leftovers:**
  - unreachable `return render_input();` at `TuiSession.cpp:449`;
  - six unused using-declarations at `ActiveRun.cpp:50-55`;
  - the `apply_feedback` lambda (`runtime_navigation_internal.cpp:81`) duplicates `ActiveRunInput::apply_composer_result`.

## Tests

No test was weakened to make code pass; all `expect` messages from the baseline are still present or strengthened. Gaps:
- **R17 [B10].** `test_settings_close_restores_preview_and_selector_storage` calls `settings.close()` directly, so it would pass without the fix. Drive Ctrl+D (Archive) through `SelectorHost::dispatch` with a nested Settings page open, and assert that the preview is restored.
- **R18 [T4].** Signals are only tested with no active run, and HUP with an active run is not tested. Add:
  - an active-run control/post-decode signal case (exit 130, retained key not dispatched);
  - HUP during a permission prompt (asserting an "interrupted" audit);
  - the R1 close-after-submit case.
- **R19. Timing risks.**
  - `turn_order` (:458) and `frame_deadline` (:490) assert `wait == Block`, but the 100 ms control timer or the 120 ms spinner can make it ReadyOnly on a slow machine (e.g. the CI sanitizer job). Push those deadlines out in the fixture, or accept ReadyOnly when a due timer explains it.
  - Four 2 s `yield()` spins remain (composer tests :785, :857, :901, :1058, plus `complete_active_run_for_test`); use the T3 loop-wait pattern.
  - Possible hangs: `wait(Block)` without a fallback timer at composer :260, and an unbounded `endpoint_future.get()` at :748.
- **R20. Weaker spots.**
  - `session_entry_count == std::nullopt` (composer :372) is always true now; seed it before the transition.
  - The selector workspace "no redraw" check `!has_due_work(time_zero+300ms)` (`tui_selector_tests.cpp:1957`) can pass wrongly on a slow machine.
  - The B6 test does not assert that turn 1 was the control-timer wake.
