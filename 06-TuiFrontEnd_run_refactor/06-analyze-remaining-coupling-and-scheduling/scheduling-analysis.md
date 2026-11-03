# 06/06 — Scheduling evidence after extraction

Required inputs read: handoff, inventory, architecture and migration ledger through05/03. Source checkpoint: a219757c, after all05 children. This is analysis only; no runtime changes or behavioral verification. References below describe this checkpoint, not the older line numbers in01.

## Actual ownership and remaining implicit operations

The extracted Composer, SelectorHost, SettingsDialog, SessionSelector, BranchSummaryOperation, TranscriptView and presentation collaborators are real owners. TuiSession now composes them and owns the outer dispatcher. It still implements many cross-feature input stages; final11 must distinguish necessary composition from algorithms that belong to a feature. RuntimePresentationState still exposes its existing screen/sidebar/attachment/grant projections; that is not backend authority, but warrants final encapsulation review. RuntimeActiveRunState remains a public per-run state bag, and RuntimeActiveRunController::run still gives it stack lifetime. Encapsulating that operation is unfinished work, not covered by05's extraction.

`runtime_active_run_state_internal.h:30–68` retains submitted text/classification, baseline transcript and evictions, attachments, receipt-authorized command projections, event queue and aggregates, cancellation atomic, close intent, active queue capabilities, synchronized event context, sink and start time. `runtime_active_run_internal.cpp:480–1040` additionally owns result, session-change/failure outcomes, resolvers, request ID, endpoint, copied keybindings, future and cadence in a synchronous call. Move true per-run state into private operation ownership; request-local derived values and input remain locals. Pure receipt matching and value merging may remain free computations with explicit immutable arguments. Do not replace this bag with public fields behind another pointer.

The active call still contains stateful orchestration lambdas for plugin geometry/render, reload, follow-up context, event publication, cancellation/close/failure, prompt precedence, plugin polling and retained input. These must become owner operations or explicitly owned asynchronous bindings. The async worker currently captures its enclosing run frame by reference. A returning start operation cannot retain those captures. Backend queue capabilities, prompt resolvers and event sinks must survive until worker retirement.

## All TUI waits and input buffering

| Path | Current wait and control flow | Required change |
| --- | --- | --- |
| TuiSession::run, TuiSession.cpp:1094–1168 | 250ms terminal read, shortened by workspace and renderer waits; timeout path skips input-stage signal handling | One loop owner waits for fd, notifications and registered timers; control checks occur without characters |
| ActiveRun::run, runtime_active_run_internal.cpp:702–833 | Future readiness loop, 16ms cadence, separately polled terminal, prompts and plugin | start/consume/complete/cancel service operations; return to outer loop immediately |
| Permission UI, runtime_prompts_internal.cpp:298–416 | Blocking terminal read or100ms stop polling, shortened by wheel suppression | Persistent modal state and one-event input/service operations |
| Question UI, same:484–618 | Blocking or100ms auto/stop polling, wheel deadline, auto callback before/after read | Serviced modal; honest timer for auto-resolution source; no nested terminal read |
| PendingPermissionRequest/PendingQuestionRequest::await | Worker CV waits for result AND retired UI claim | Retain worker wait; it protects borrowed callback scope and is not a TUI loop |
| Plugin present_request, runtime_plugin_ui_internal.cpp:287–304 | Worker CV wait_until, capped20ms for cancellation predicate and invocation deadline | Retain worker policy unless separately changed; wake TUI on mailbox publication |
| External editor/suspend/clipboard and synchronous authority callbacks | Explicit terminal handoff or synchronous backend operation | Preserve ownership policy; no competing TUI loop. Their blocking duration remains outside dispatch fairness guarantees |

`runtime_input.cpp:277–305` consumes the64-entry startup FIFO before ncurses. `poll_curses_input()` uses timeout0, but ncurses and Context may already hold bytes when the fd is no longer readable. Always attempt buffered input before sleeping, and keep a bounded input batch with a nonblocking next turn when exhausted. Readiness alone is insufficient. Existing decoder subreads still allow50ms Escape assembly and1s bracketed-paste completion (`runtime_input_internal.h:45–49`); preserve them as bounded synchronous decoding, not an invented nonblocking decoder. Startup probe input collection remains initialization, outside the live event loop.

## Producer publication and wakeup audit

| Producer | Actual publication boundary / current notification | Integration requirement |
| --- | --- | --- |
| RuntimeEvent, envelope, subagent launch | runtime_state.cpp:96–120 appends under queue mutex and sets historical received flag; no TUI wake | Publish then notify after unlock. Drain bounded batches; queue nonempty, not received_any(), controls readiness |
| Follow-up request context | active.cpp:537–555 updates request/correlation under context mutex before backend callback | Preserve context-before-follow-up order; sink snapshots context safely; owned operation outlives copied callbacks |
| Submission terminal result/exception | core/thread.h:324–340 wraps std::async; future readiness currently discovered by polling | Publish an owned terminal result or promise before notifying. A notify inside the old async callable occurs BEFORE its returned future becomes ready and can lose the only completion wake |
| Permission/question request | prompts.cpp:739–766 pushes shared request under admission mutex then await blocks worker | Notify after queue publication/unlock. Close admission before teardown; claim is registered atomically with removal, permission before question |
| Prompt cancellation | prompts.cpp:635–674 swaps queues/copies claim under mutex; pending request interrupt wakes only worker CV | Wake TUI to retire an active claim. Worker cannot leave until no UI callback can borrow its stack |
| Plugin request/clear | plugin.cpp:266–283 enqueue under synchronized state; cancel_locked:240–254 queues Clear/completes modal | Publish then wake through mailbox-owned safe handle, preserving weak endpoint and generation checks; never capture projection in worker |
| Mermaid completion | backend mermaid_render_coordinator.cpp:688–692; worker744–756, immediate enqueue842/847/855, cancellation884/897 | Existing neutral bridge has enqueue/cancel/drain only. Internal pipe/CV wakes backend worker, not TUI. Requires explicit notifier extension OR documented member discovery timer |
| Branch summary | branch_summary_coordinator.h:179–186 offers on_snapshot; normal publication.cpp:1106–1153 and1698 calls it after unlock; app TuiFrontEnd.cpp:163 installs none | Either safely install/retire observer OR keep explicit operation timer. Observer-only replacement must also fix terminal allocation-failure fallback1155–1179, which only signals internal CV |
| Workspace | SubagentCoordinator.cpp:1383/1409 reads live source fingerprint/messages on inspect, then publishes generation1450–1455 | No general live-source notification exists. Existing terminal sink belongs to SubagentDeliveryManager and is not a live-update slot. Retain150ms visible-workspace discovery timer |
| Display files | display_reload.cpp:98–115 and tui_display_settings.cpp:1377–1416 discover document/theme changes via reads/fingerprints | No watcher descriptor/source notifier. Preserve500ms owner discovery timer (actions.cpp:34,106–115) |
| Question auto_resolve | Callback is checked by question UI, including OAuth-like external progress; no callback-change notification contract | Preserve bounded100ms member callback while needed; do not assume worker completion wakes a question that blocks that worker |

Mermaid has additional readiness traps: app bridge try_lock can return QueueFull on enqueue or an empty drain when busy (`app/tui/mermaid_presentation_bridge.cpp:80–82,125–137`). TUI service enqueues before draining (`runtime_mermaid_internal.cpp:326–328`) with32 pending maximum. Draining frees capacity while waiting records remain, potentially leaving no future producer. Any notifier-only design needs retained readiness/retry or drain-before-enqueue plus bounded rescheduling. A member timer is an honest alternative until the bridge has a complete notification contract. Do not label internal backend wake pipes as TUI notification sources.

## Deadlines and member timer clients

| Existing owner/state | Existing policy | Concrete requirement |
| --- | --- | --- |
| Renderer FrameScheduler | Full/Footer coalescing,16ms after last paint, permanent failure state | One-shot member callback marks frame ready; coherent paint after application/input service, failure propagated centrally |
| Transcript position indicator |1s visible lifetime, renewed on scroll | Registered expiry callback; cancellation/reset removes stale expiry |
| Transcript selection edge autoscroll | Selection-owned edge cadence and drawn-layout authority | Member timer services existing layout-aware operation; do not poll all deadlines from outer loop |
| Spinner |120ms phase advance (`composer_internal.h:73`); ActiveRunCadence mixes16ms frame and spinner clocks | Small owner/member callback; skip missed ticks rather than burst paints. Architecture’s250ms example disagrees with actual code/tests and must be reconciled in07 |
| Workspace |150ms while visible | Member discovery timer; close cancels registration, never backend jobs |
| Display reload |500ms, eligible immediately initially; active mode suppresses when workspace/detached transcript | Owner discovery timer plus explicit eligibility and settings-preview rebase |
| Branch summary | Discovered during idle250ms iterations, immediate poll after start/confirm/cancel | Operation timer while active, preserve generation checks and terminal-only deferred exit |
| Mermaid | Discovered every idle/active service, including async completion/retry | Member discovery/retry timer or fully wired notification contract, with immediate service after transcript/config changes |
| Plugin invocation |120s admission/deadline, currently tested during polling and worker predicate | One-shot owner deadline triggers cancel/settle; no wait-duration computation in feature |
| Prompt wheel suppression |40ms accepted-wheel interval; resize/idle resets governor | Semantic deadline may reject input without computing global wait. Timer resets suppression if needed |
| Prompt auto-resolution |100ms when callback present | Registered member callback checks auto progress; no nested input wait |
| Signal/resize discovery | Atomic signal bits, ncurses resize input; no safe wake API | Bound observation using loop-owned control timer unless signal policy is separately redesigned |

Timestamp comparisons used to reject a wheel burst, reject stale authority, or enforce a worker timeout are not global wait calculations. Remove feature `time_until_poll`, renderer/view minimum-wait APIs and active cadence waiting from final production dispatch. Pure clock-state test seams may remain only if genuinely used and their distinction is documented.

## Signals, wait primitive, fairness and shutdown

`Signals.cpp:34–38` only sets a lock-free atomic bit. `utils/threading/EventLoop.cxx:276` waits with a null signal mask, and283–286 retries EINTR internally. Therefore indefinite wait can strand SIGINT/SIGTERM or ncurses resize with no characters. General notify may throw and has no signal-safe contract; do not call it from the handler or alter terminal signal/thread policy in this objective. A loop-owned bounded control timer is a sensible permitted solution;07 must choose the bound explicitly. Keep signal precedence and draft/branch/active cancellation semantics. The old idle timeout-path omission should be corrected deliberately.

The existing utils loop already supplies epoll/kqueue, coalesced release/acquire notifications and fd read/hangup/error flags. Header21–34 requires a single waiter, watch registration before waiting, notify only watch_event handles and all producers retired before destruction. It has no member timers or callback registry. Result merges fd and notification bits; pending readiness takes precedence over timeout (`.cxx:358–367`). Service timers on every turn, including input floods, not only on timed_out. `terminate()` blocks for waiter acknowledgement; never use it for TUI-local exit. Destructor only closes internal descriptors and does not join producers.

07 must define: stable terminal/application watches; registration and cancellation handles; monotonic absolute deadlines; callback destruction/reschedule during dispatch; missed-tick policy; bounded input/event work; remaining-work rescheduling; processing both ready sources; coherent painting; EOF/hangup/error and buffered-final-input policy; signal checks without input; local stop outcomes; and explicit producer retirement. A copied wake capability must not dereference a destroyed session, timer target or presentation. Queue/state remains authority; notifications may coalesce.

Shutdown must stop new submission/prompt/plugin admission, request backend cancellation, interrupt and retire prompt callbacks, clear plugin waits, join the active worker, retire audit/event sinks and timers, and then destroy their targets. Preserve backend committed-session reads outside UI locks and receipt-gated old-session event discard. Current initial-render failure and exceptional future.get still leave a stack-capturing audit sink;09 must close this real lifetime gap. A terminal render failure must use the same pre-join cancellation path, not rely on late member destructors.

## Evidence and continuation

Independent source reviews covered active publication, prompt modal branches and wait implementation. Permission Escape requests run stop then denial; Ctrl+C/Ctrl+D deny without that callback. Question cancellation does not request run stop; auto-resolution currently wins before stop checks. Prompt input always consumes ignored keys. Preserve these distinctions or record an explicit07 decision. Search prompt-claim versus input dispatch currently shares the coordinator lock; a newly claimed prompt consumes that retained search key rather than feeding it into the modal.

Compilation remains the only executable gate. All code at this checkpoint compiled in05/03; this goal changes only artifacts. An incremental `source /workspace/ava-tools/activate.sh; scripts/build.sh` is recorded in the shared ledger before commit. No CTest or TUI smoke has run. Next:07 records decisions;08 builds actual loop/timer APIs;09 services active submission and prompts;10 wires the sole loop;11 audits every temporary seam.
