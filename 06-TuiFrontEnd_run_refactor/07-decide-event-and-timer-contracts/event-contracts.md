# 06/07 — Event and timer integration contracts

Required inputs read: handoff, inventory, architecture, migration ledger through06 and scheduling-analysis. These are agent-selected decisions under the user's explicit direction to proceed without waiting for review. They are not a claim of personal maintainer approval. No blocking question remains. Source behavior is still unverified; this goal changes no runtime code.

## One owner, one terminal loop

Compose `TuiEventLoop` with the existing `utils::threading::EventLoop`. Register the borrowed Context input descriptor and one application notification event before the first wait; never register a terminal descriptor as a notify event. Keep platform epoll/kqueue and the existing terminal/ncurses/thread assertions. The TUI loop owner supplies member timers, local stop state, due-work service and one wait operation. Do not add a second platform poller or use blocking utils terminate for a TUI-local exit.

TuiSession declares the loop before all timer and notification clients. It remains nonmoving. Its final run body reads as control/termination, due callbacks, application updates, terminal input, coherent paint, then wait. Active submission and permission/question modals are serviced operations inside that loop; they never read terminal input or own another dispatch loop. Worker CV waits, finite value iteration, startup probe collection and explicit external-editor/suspend handoffs remain distinct from live TUI scheduling.

The actual08 public interfaces must be recorded in the ledger before09 begins. Intended call sites are narrow:

```cpp
timer_ = loop.member_timer(*this, &Spinner::tick);
timer_.repeat(detail::kProcessingIndicatorFrameDelay);
flush_timer_.arm_at(next_frame);
timer_.cancel();
loop.service_due();
loop.wait(TuiWaitMode::Block); // or ReadyOnly when locally runnable work remains
```

Callbacks take the monotonic dispatch timestamp and return void; owner state records fallible results for the composition service to consume. No timer callback paints or waits. A feature requests invalidation or publishes its outcome. Read-only loop outcome queries may expose terminal hangup/error and notification failure; there is no mutable registry getter. New domain/resource classes use matching CamelCase headers, private fields and default ON printing, with any opaque/sensitive resource opt-out stated in the ledger.

## Timer registration, cancellation and missed ticks

Use a move-only `TuiTimer` registration handle that borrows its earlier-lived loop. The handle owns registration cancellation, not its callback target. The callback target stays at a stable address and outlives the handle. Owners with callbacks bound to this cannot move while registered. Explicit attach/detach is acceptable for existing standalone test fixtures; production composition must attach every client. Empty/default handles are inert until attached, not a production substitute for wiring.

`member_timer` binds a target member operation; a registration begins disarmed. `arm_at` replaces a one-shot deadline, `arm_after` computes an absolute deadline inside the loop, `repeat` schedules the first callback after one interval, and `cancel` disarms the registration. Handle destruction removes it. These names may be adjusted in08 if the equivalent actual API is documented. Invalid nonpositive repeat intervals are programmer-contract violations with actionable ASSERT comments and safe release handling.

The loop stores steady-clock absolute deadlines and computes only its own earliest wait. Features never combine `time_until_*` values or call wait_for/wait_until to schedule TUI work. Semantic comparisons for wheel admission, worker deadlines and authority validation remain permitted; they do not choose loop sleep duration.

Every service_due pass snapshots due registration IDs and generations. Cancellation/destruction/rearm before a callback suppresses its old generation. Never retain iterators across callbacks. Disarm a one-shot or provision the next repeat before invoking its callback; callback cancellation/rearm wins. Timers armed for now/past from a callback are eligible on the next loop turn, never recursive or repeatedly invoked in the same batch. A repeating timer skips missed intervals and fires once per due pass, preserving its original phase by moving its deadline beyond dispatch time. Spinner advances one visible phase per invocation, with no burst catch-up paints. This deliberately changes late-spinner catch-up behavior while retaining its real120ms cadence.

Timer callbacks run only on the TUI thread. Exceptions cannot leave a stale registration iterator or claim; unwind through the owner/session cleanup boundary. Cancel all handles before target destruction and before loop destruction. A timer is a scheduling mechanism, not a new owner of permission/backend authority.

## Notifications and asynchronous publication

Notifications are coalescing hints. Each application service pass examines every participating mailbox and operation, even when only one event bit caused the wake. Queue/context/result synchronization is authoritative. Producers append/update under their existing lock, release it, then notify. No worker receives Context, renderer, selector, or mutable presentation access.

Use a copyable wake capability backed by weak shared ownership of actual waiter resources. A successful weak lock pins resources for the notify call; a retired capability is inert. Do not protect a raw loop pointer with only an enabled atomic: that does not serialize destruction against an in-flight notify. The capability must never capture TuiSession, a timer target or projection. Unexpected notifier failure records a shared failure indication for the next control/service turn; it cannot silently discard data or throw through a backend callback. The100ms control timer provides a bounded opportunity to observe such failure, subject to synchronous work below.

RuntimeEventQueue, prompt request queues and plugin request/clear messages gain publication-before-notify wiring. RuntimeEventQueue's historical received_any remains settlement evidence, never readiness. Preserve cross-producer serialization and FIFO order under its mutex. A bounded drain leaves unread events queued or retains an explicitly runnable local backlog; it cannot swap everything then sleep with unprocessed records.

Submission completion publishes an owned terminal result or exception into an operation-owned synchronized mailbox/promise before notification. The publication is the last worker action that may use operation/backend state; only safe wake delivery and thread-wrapper epilogue remain afterward. Ordinary completion consumes only a published terminal payload and then retires/joins the worker. Distinguish payload readiness from the final OS-thread epilogue; a join after publication is acceptable, a speculative get on an unready old future is not. Shutdown may join after explicit cancellation and prompt/plugin retirement. Scope-exit notify inside the old result-returning async callable is rejected because it precedes future readiness.

Retained event/audit/context endpoints should own synchronized inbox lifetime or a checked weak capability, not capture run-stack references. Follow-up context changes happen before the original backend follow-up callback. Clearing the prompt audit registration alone does not retire an already copied sink; endpoints must remain safe through all producer joins. Preserve request-ID receipt authorization, missing-ID fail-closed behavior, initial todo seeding and old-session discard after backend queue finish receipts.

## Honest timer clients and source limits

| Client | Selected registration / result |
| --- | --- |
| Spinner | Repeating120ms while processing, stop on settlement; actual composer constant and tests supersede architecture's250ms example |
| Renderer | One-shot16ms coalesced frame deadline; urgent invalidation eligible at end of current service batch; callback marks paint readiness |
| Transcript indicator/selection | One-shot1s expiry and existing selection edge cadence, registered by their owning view/render collaboration; preserve frozen/drawn layout authority |
| Workspace | Repeating150ms while visible; refresh changes request a frame; close cancels presentation timer without canceling backend jobs |
| Display reload |500ms member discovery callback; initially eligible; preserve active-mode live-tail/no-workspace eligibility and explicit SettingsDialog rebase |
| Branch summary |250ms member discovery while active; immediate consume after start/confirm/cancel; stop at terminal settlement; generation checks unchanged |
| Mermaid |16ms member retry/discovery while in-flight OR retryable/unsubmitted work exists; service immediately after transcript/session/config mutation; stop only when neither remains |
| Plugin |120s invocation one-shot; expiry publishes cancellation and active-run stop effect; worker20ms cancellation observation remains worker policy |
| Question auto-resolution |100ms member check while auto_resolve exists; no fabricated producer notification; cancel before releasing borrowed callback scope |
| Prompt wheel | Existing40ms suppression remains input admission; member expiry if state reset needs scheduling, never a read timeout |
| Session control | Repeating100ms member operation checks pending signals and buffered resize/input opportunity; also checked on every normal loop turn |

Mermaid and branch use real discovery timers instead of extending backend notifications in this refactor. Their current neutral contracts are incomplete for notifier-only readiness: Mermaid try-lock/capacity retries and branch terminal fallback are documented in06. The chosen timers remove feature wait calculations without pretending those backends publish a complete frontend wake source. This is an implemented scheduling decision, not a stub. Workspace/theme files likewise require periodic discovery. A future producer subscription can replace each timer independently after covering every publication/retry path.

## Fairness, buffering and painting

Each turn checks control/stop, services due timers, consumes up to256 queued application events, processes up to32 decoded inputs, services due callbacks again if decoding took time, then paints one coherent frame. Prompt admission and application state must be examined before routing each retained input where the old precedence requires it. A full budget or explicit backlog makes the next wait nonblocking; both sources get another turn. These budgets limit item counts, not wall time of arbitrary callbacks. Timer due work runs even under continuous input/notification readiness.

Before any blocking wait, attempt startup/ncurses buffered input regardless of fd readiness. Never infer an empty decoder buffer from a non-readable fd. Preserve existing50ms Escape and1s bracketed-paste assembly and document that they bound only those subreads, not total input batch time. This objective does not implement the separate InputDecoder redesign. Per-event input/query/geometry stays local. If a prompt is claimed in the existing precedence race, consume the retained ordinary/search key; do not replay it into the newly opened prompt.

While integrated, renderer render/request calls enqueue full/footer invalidation; urgent calls request an end-of-batch frame. The loop performs actual painting after shared updates/input, with full dominating footer. Existing standalone renderer fixtures may retain immediate painting when no loop mode is attached. Record the actual activation API in10 and remove obsolete production flush/deadline polling. Explicit synchronous terminal handoffs may flush before relinquishing terminal ownership and after restoration; document these exceptions rather than leave arbitrary intermediate paints. Paint failure closes admission/cancels active work and produces terminal-write-failure outcome through the same cleanup path.

Terminal read, notification and due timer work may all be ready; service all within budgets before blocking again. Hangup is sticky. Drain already buffered final input in bounded turns, then request normal close; terminal error/invalid input descriptor yields failure close. Never repeatedly wait on a level-triggered HUP. Do not execute post-close new submissions while draining merely to empty the buffer; control state disables new admission. Existing signal exit130 outranks write failure1, which outranks normal0.

## Prompt and active-operation semantics

Active submission exposes start, consume/service, one-event input, cancel, completion/outcome and shutdown operations. RuntimeSubmitController returns after starting it; session routes later input to the active owner. No compatibility run wrapper may preserve a nested terminal loop. Encapsulate the current public RuntimeActiveRunState, preferably separating synchronized inbox/context from TUI-only receipt/event accumulation. Avoid a new mutable reference bag or blanket friend to let input edit fields. Queue/restore/active-command/cancel operations are semantic member calls.

Prompt coordinator owns one claimed operation. Start claims permission before question atomically with queue removal, performs before-prompt composition and initializes the projection. service checks automatic resolution and cancellation without reading input; handle_input consumes exactly one decoded event. Completion cancels timers/callback reachability before completing the pending request and retiring the claim. Every exceptional start/service/input path supplies fallback denial/interruption and retires the claim. Worker await remains result AND !resolving.

Preserve request-result distinctions: permission Escape requests run stop then denies; Ctrl+C/Ctrl+D deny without that extra stop callback. Question user cancellation does not request run stop. Question auto-resolution is checked before cancellation/input dispatch, retaining its precedence where callback readiness can be observed. Terminal signal interruption remains a distinct result/audit, and the outer loop retains actual signal claiming/outcome policy. Modal Io errors are returned to the worker as before; centralized actual paint failure also ends the session safely. Ignored modal keys are consumed. Search input and prompt claim remain atomically ordered; no callback is called under the coordinator mutex if it can reenter publication or block.

## Signals and shutdown

Do not change Signals handlers, masks, ncurses ownership, assertions or RPC. The100ms control timer compensates for EventLoop's internal EINTR retry and atomic-only signal publication. It is an observation cadence, not a hard latency promise across decoder subreads, synchronous authority work or editor/suspend. Check SIGTERM before SIGINT; preserve branch deferred exit, active cancellation/close and draft-clear distinctions. A local stop flag replaces self-terminate.

On any exit or exception: stop new submission admission; close prompt/plugin admission; request active cancellation; cancel/retire prompt auto/input callbacks and complete pending waiters; clear/cancel plugin waits; join active worker; finish queue capabilities and apply/discard final receipts under the existing authority rules; clear audit registration and retire endpoints; cancel timers; shut down Mermaid/presentation; invoke frontend shutdown while its collaborators and Context remain alive; unwind terminal guards. Active terminal outcomes may keep the loop servicing cancellation until settlement; a render failure can use the explicit blocking shutdown after unblocking prompts/plugins. All paths, including initial render failure and worker exception, must follow owned cleanup.

Backend committed identity and boundary transports are read outside the UI lock, then applied on the TUI thread through SessionTransitionCoordinator. No notification or timer changes backend permission/session/provider/process authority. Callback targets and loop resources outlive every publisher use; weak wake retirement is defense in depth, not a substitute for stopping/joining producers.

## Rejected alternatives, review flags and evidence

Rejected: replacing only the idle wait; future polling plus a decorative event loop; one global periodic callback that polls every feature; notification-before-result; timeout calculations in features; raw loop pointers behind enabled flags; invoking ordinary notify from signal handlers; catch-up timer storms; backend notifier expansion without bridge retry/fallback coverage; permanently retaining synchronous run/modal adapters for tests.

Maintainer review flags: control cadence100ms improves idle signal observation; skipped spinner catch-up and end-of-batch painting intentionally change scheduling; discovery timers remain for sources without complete notification contracts; existing synchronous decoder/authority/handoff latency remains. These are concrete implementation choices, not unapproved silent stubs. No maintainer decision is currently required to proceed. Runtime checks remain deferred until after11 as requested.

Existing configured dev build must compile before this commit; no CTest, terminal smoke or manual reconfiguration.08 may temporarily add unused loop/timer APIs, recorded in the ledger with10 as the integration deadline.09 may have an explicit intermediate integration seam only if recorded and removed by10/11. Final11 must not claim single-loop completion until all active and prompt nested input loops and feature wait calculations are absent from production flow.
