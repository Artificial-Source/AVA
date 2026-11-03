# Issue93 — completed refactor and validation handoff

Repository: Artificial-Source/AVA. Work branch: `astra/06-TuiFrontEnd_run_refactor`, based on plan branch `06-TuiFrontEnd_run_refactor` at `ec4143e98e8f23cfa6a30852b3d4871204ed07a4`. Only the work branch is pushed; no PR is opened. The numbered plan is executed in order. User authorization permits agent-selected decisions at02/07; no personal maintainer approval is claimed. Full runtime tests are deliberately deferred until after11 is committed and pushed.

Required review inputs: `06/handoff.md`, `06/01/inventory.md`, `06/02/architecture.md`, `06/03/migration-ledger.md`, `06/06/scheduling-analysis.md`, `06/07/event-contracts.md`, and the actual08–10 implementations. Earlier inventory/ledger entries describe historical checkpoints; the final contracts below supersede their temporary coupling rows.

## Commit map

| Goal | Compiled checkpoint |
| --- | --- |
|01 inventory | `c5eb5e03` |
|02 ownership decisions | `5964c456` |
|03 session composition | `9464f875` |
|04/01 selector host | `5ebfa20c` |
|04/02 settings | `c7087a10` |
|04/03 sessions/transitions | `cf475b04` |
|04/04 branch summary | `06869446` |
|05/01 composer/dispatch | `c8e551e7` |
|05/02 transcript/renderer | `cca79e43` |
|05/03 presentation/request lifetimes | `a219757c` |
|06 scheduling analysis | `78c09cf0` |
|07 event contracts | `579a17fd` |
|08 loop/timers | `bda411c3` |
|09 serviced active run/prompts | `ac3b9342` |
|10 single loop | `6720d3ef` |
|11 final ownership audit/cleanup | This checkpoint; exact hash is added with post-checkpoint test results. |

Every row was built before commit and pushed before starting the next goal. Parent04/05 statuses follow their completed children. The final parent status follows11.

## Final ownership and operation boundaries

`TuiFrontEnd::run` retains terminal eligibility, signal activation, graphics/background-probe guards, configuration loading and initialization-local `initial_status_line`. It constructs one nonmoving `TuiSession`. Terminal ownership, thread assertions, signal handlers and RPC are unchanged; the refactor changes TUI code and its tests only.

| Owner | State and meaningful operations |
| --- | --- |
|TuiSession | Composition, outcomes, cross-feature input precedence, sole loop and shutdown. It holds no editor/selector/branch/active-run state bag. Its input stages route semantic owner operations and retain only event-local decisions. |
|Composer | Private draft/history/selection/scroll/completion/jump/Escape state; edits, acceptance, history, pointer and reset operations preserve their correlated invariants. |
|SelectorHost | Private authoritative kind and view; open/replace/close/selection operations publish a render copy together. |
|SettingsDialog | Navigation and preview transaction; baseline/highlight/confirm/cancel/rebase. Display reload explicitly rebases before final painting. |
|SessionSelector / SelectorInputController | Exact archive identity and session browsing; common/scoped selector classification and backend action results. Backend authority stays in frontend callbacks. |
|BranchSummaryOperation | Generation, phase, prior selector and deferred exit; only terminal settlement releases ExitReady. Discovery is private and timer-driven. |
|SessionTransitionCoordinator | Changed-ID reset ordering across existing owners; same-ID refresh preserves grants, attachments and interaction state. |
|PendingAttachments | Private staged references plus their visible badges; stage rolls back the reference if badge allocation throws, take transfers references and clears badges, clear settles both. No mutable reference-vector access. |
|TuiSessionGrantRegistry | Existing encapsulated registry now owned directly by TuiSession, borrowed by prompt coordination and committed-session transition only. It is no longer display storage. |
|RuntimePresentationState | Private screen/sidebar DTOs, frontend binding and catalog delivery metadata. Hydrates/applies frontend display facts; explicit shared projection boundary is detailed below. |
|RuntimeRenderer / TranscriptView | Private caches/frame scheduling/UI lock and private viewport/selection/frozen sidebar/deferred anchor state. Prepared cache borrows are const and expire at the next relevant preparation/paint. TranscriptView has no renderer back-reference. |
|ActiveRun / ActiveRunInbox / ActiveRunEvents | Private worker/request/phase ownership; synchronized producer mailbox; receipt-sensitive event accumulator. start/consume/input/complete/cancel/shutdown replace the nested active loop. |
|RuntimePromptCoordinator / PendingPermissionRequest / PendingQuestionRequest | One persistent claimed UI operation and private synchronized worker request result/retirement. No terminal input loop. |
|Plugin / Mermaid / Workspace | Existing focused owners retain synchronized endpoint, reconciliation or workspace projection lifetimes, with real cleanup and timer integration. |
|TuiInputDispatcher | Ordered member bindings; no fifteen-reference context, public field bag or duplicate retained event. |
|TuiEventLoop / TuiTimer / Spinner | One resource waiter and private timer registry, RAII registrations, and a small processing phase owner. |

New domain classes have matching CamelCase headers/implementations and private members. Existing useful Runtime* owners keep their names rather than receive empty wrappers. All new implementation files start with `sys.h`. Sensitive content owners have specific printing opt-outs; timers, loop, dispatcher, Spinner, immutable outcome/update values and the scalar-only FrameScheduler/WheelBurstGovernor use ON. TuiWakeup/TuiWaitState opt out for opaque concurrent waiter resources. Existing internal-linkage plugin/RAII helpers document their content/internal-linkage exclusions; no new domain class is hidden in an anonymous namespace to avoid printing.

## Shared mutable projection: deliberate boundary, not backend authority

RuntimePresentationState no longer exposes public storage. `screen()` and `sidebar()` return the existing render DTOs, with const overloads and lifetime documentation. This remains an intentional mutable sharing exception: the established renderer and feature publication contracts use ComposerSnapshot/SidebarSnapshot as a shared screen, while actual editor, selector, viewport, attachment and grant authority is encapsulated separately. Replacing every DTO field with a forwarding setter would obscure the same collaboration. No mutable accessors expose catalog-generation bookkeeping, attachment references, grants or renderer caches.

Concrete operations that justify and constrain this boundary:

- `SelectorHost::publish` writes `snapshot_.select_list` from its private authoritative view. Features use host operations; `ScopedTranscriptSearchModalProjection` temporarily suppresses/restores only the rendering copy to calculate underlying layout.
- `Composer::project` writes input/cursor/selection/completion projection together. Its draft and editing history remain private. RuntimeNavigationController publishes completion input before renderer cache preparation; those queries are intentionally not claimed const.
- `ActiveRun::drain_events` applies event authority under the existing UI lock, captures a TranscriptUpdate before mutation, updates transcript/count/search and completes viewport preservation through TranscriptView. `apply_non_transcript_event_authority` publishes queued messages and sidebar activity/todos/modified files. Worker endpoints never receive these DTOs.
- `SettingsDialog` applies a preview overlay, while `RuntimePresentationState::apply_local_display_settings` publishes authoritative local display settings and can clear image preview pixels without changing the attachment reference/badge pairing. `TuiSession::maybe_reload_display_settings` rebases preview before requesting paint.
- `PendingAttachments::stage/take/clear` are the only production structural writers of `screen.pending_attachments`; actions prepare the badge, ActiveRun takes references for submission, stash checks admission, and transitions clear both domains.
- `SessionTransitionCoordinator::apply` reads committed identity supplied by the caller, clears grants and attachments on change, resets search/workspace/composer/renderer, resets projection, closes the selector, then finalizes sidebar/identity. Backend identity and boundary transports are read outside the UI lock before this TUI-side application.

The DTO references last as long as the earlier-lived presentation owner; nested item/view/cache references expire on their corresponding mutation. This is TUI-thread sharing, not cross-thread synchronization. Renderer/TranscriptView borrow sidebar as const; ActiveRun and frontend hydration are its live publishers. Backend permission, committed session, provider and process authority remain external. This exception is explicit for maintainer review and is not a pending migration stub.

## Loop, deadlines and fairness

`TuiSession::run` is the only live TUI event loop: control → due member callbacks → up to256 application events → up to32 buffered decoded inputs → due callbacks again → coherent paint → resource wait. It polls startup/ncurses buffered input before blocking even when the descriptor is no longer readable. Full budgets, accepted backlog, published completion and newly due work select a nonblocking next turn. Both ready sources get service. Input remains buffered once completion publishes and through the finite finalizing receipt backlog, then ordinary dispatch resumes.

The loop composes existing `utils::threading::EventLoop` with fixed input and application watches. It does not add a platform poller, call blocking self-terminate, or change terminal/thread policy. Member timer callbacks take a steady-clock timestamp, publish readiness/outcomes and never paint. Stable registration IDs/generations and copied callbacks tolerate cancellation/destruction/rearm during dispatch. One-shots disarm and repeats advance before callbacks; missed repeats fire once and preserve phase. A handle removes its registration before the target/loop dies.

| Owner | Deadline / activation |
| --- | --- |
|Session control |100ms; observes existing atomic signal bits without requiring characters |
|Spinner |120ms while processing; actual baseline cadence supersedes plan's illustrative250ms |
|Renderer frame |16ms coalescing; urgent Full eligible at batch end, Full outranks Footer |
|Transcript indicator / edge autoscroll |1s expiry /50ms edge cadence via renderer-owned handles |
|Branch summary |250ms while active, immediate service on start/confirm/cancel |
|Workspace |150ms while visible |
|Display discovery |500ms, initially eligible; original active/live-tail/workspace restrictions retained |
|Mermaid |16ms while pending, retryable/unsubmitted, or canceled bridge identities still await drain |
|Question auto-resolution / wheel |100ms callback readiness /40ms suppression expiry |
|Plugin invocation |120s one-shot; worker cancellation observation remains bounded20ms policy |

Discovery timers are implemented choices: Mermaid try-lock/capacity retries, branch terminal fallback, external display files and workspace source inspection lack complete frontend notification contracts. Mermaid drains before enqueueing and again afterward, tracks accepted epoch/identity pairs beyond UI record removal, and drains canceled same-epoch work so backend completion capacity is not stranded. Authoritative epoch replacement resets obsolete tracking.

Attached render calls invalidate; `paint_ready_frame` paints after the batch and synchronizes direct view deadlines even when no frame is ready. Urgent Full thaw remains distinct from scheduled frozen layout. `paint_for_handoff` is deliberately synchronous at initial painting, editor/suspend restoration, clipboard preparation and selector/session backend authority boundaries. Unattached renderer/workspace/plugin fixtures retain explicit-clock or synchronous semantics; there is no alternate production loop.

Known HUP/error disables new submissions and discovery callbacks, drains/discards final buffered bytes in bounded batches and exits without waiting again on sticky readiness. HUP is normal close, descriptor error is failure, and signal130 outranks failure1. A signal handled during idle decoding still consumes that retained key. HUP uses immediate owned shutdown rather than waiting for branch presentation settlement; frontend shutdown retires its backend producer.

The100ms control period is not a hard latency guarantee: the existing50ms Escape and1s paste subreads, synchronous backend callbacks, external editor/suspend handoffs and arbitrary per-item cost remain. Budgets bound item counts, not wall time. These limits are preserved and documented rather than silently changing the separate input/terminal objectives.

## Publication and shutdown lifetimes

Construction order is dependency order: outer Application/Context/frontend and guards → loop → presentation/attachments/grants/plugin → composer/renderer/navigation/selectors/features → prompts/Mermaid/actions/workspace/transitions → ActiveRun/submission router/dispatcher/control registration. Destruction reverses it. TuiSession and callback targets cannot move. Timer handles are last owner members and retire first.

1. Producers publish payload under the relevant synchronized queue/context lock, unlock, then notify. A weak TuiWakeup pins actual waiter resources for an in-flight notify, never a raw session pointer. Coalescing is safe because queue/payload state determines readiness.
2. Worker submission/context callback scopes end before terminal result/exception publication. Only safe wake delivery/thread epilogue remains before join. Normal completion waits for that payload rather than polling a future or notifying before future readiness.
3. Completion closes prompt admission, retires claimed callbacks and cancels plugin waits before joining. Backend identity/boundary reads occur outside UI lock. Finishing queue capabilities can publish receipts; Inbox admission is retired before old-session discard. Accepted final receipts drain in bounded turns before settlement.
4. Pending request wait requires both a result and retired resolution claim. Question auto callbacks may borrow worker-local data, so interrupt records cancellation but cannot release the worker early. Permission Escape keeps its claim until ActiveRun publishes stop, then retires it; Ctrl+C/Ctrl+D denial and question cancellation keep their different semantics.
5. Error, signal, initial-render failure and exception cleanup use the same owned cancellation/unblock/join sequence. Copied event/audit endpoints own safe synchronized Inbox lifetime and reject late publication. Clearing a registration alone is not relied on for lifetime safety.
6. Explicit session shutdown cancels control, shuts down ActiveRun while all collaborators live, then Mermaid/plugin/prompts, invokes frontend shutdown with Context alive and unwinds terminal guards. Destructors repeat idempotent cleanup for early/exception paths. Signal outcomes survive later paint failure.

The remaining worker CV waits in pending requests/plugin transport are worker-side policy, not live TUI loops. Opaque pre-existing plugin mailbox records remain implementation-only synchronized transport with no external mutable API. Subagent foreground waits/interaction-gate retirement keep UI resolver borrowers within joined submission lifetime; backend cancellation remains backend-owned.

## Closed migration obligations and retained limits

All temporary ledger rows are resolved: separate selector kind/view, settings/branch state in session, public draft/active-run bags, public renderer caches, Mermaid forwarding/no-op seam, plugin shutdown pointer bag, dangling stack audit endpoints, active/prompt terminal loops, outer timed input wait and feature minimum-wait arithmetic are gone. Production attaches all timer clients. No empty stub, unused API-first owner, compatibility active run loop or deferred implementation remains.

Final cleanup removes raw-this RuntimeEventQueue callback factories (fixtures enqueue values directly), unused FrameScheduler pending-kind and renderer processing-frame adapters, remaining active mouse forwarding lambdas and obsolete TuiFrontEnd extraction includes. Branch discovery poll is private. Required marker includes and explicit printing exceptions are complete. Existing pure calculations and synchronous event-local predicates remain free/lambda computations; they do not retain orchestration state.

Review flags, with no blocking maintainer decision: the explicit screen/sidebar publication exception; retained Runtime* names; real120ms spinner/skipped catch-up; end-of-batch paint;100ms signal observation; honest discovery timers; unchanged synchronous decoder/backend latency. Runtime validation is separate from compilation. No permission, session, provider, process, RPC or terminal-thread policy is moved to the new owners.

## Build and environment evidence

Every goal uses the same configured `build/`: BetaTest, EnableDebug=ON, OptionEnableLibcwd=ON, EnableAvaBuildTests=ON, GCC16.2, Ninja and CMAKE_CXX_SCAN_FOR_MODULES=OFF. Activation is `source /workspace/ava-tools/activate.sh`; compile command is `scripts/build.sh` (four jobs). New headers are staged before `scripts/build.sh --target generate-print-members`. No manual reconfigure was used to check an extraction; source-list changes use normal successful CMake regeneration.

All committed checkpoints compile. Analysis-only01/02/06/07 incremental builds passed; all implementation checkpoints through10 passed full configured builds, including test executable compilation.10 completed48 steps with no warnings/errors. Detailed per-goal commands/results and intermediate compiler corrections are in `06/03/migration-ledger.md`; logs are under `/workspace/ava-tools/goal*-build*.log`. One03 sandbox/zombie runner-lock incident was diagnosed and rerun successfully without changing runner policy; final09 aggregate initializers were corrected under existing warnings-as-errors. No failure is hidden as a passing checkpoint.

Goal11 staged-header generation exited0 with normal CMake regeneration; the subsequent full configured `scripts/build.sh` exited0 after58 scheduled steps, including ava and ava_tests linkage, with no compiler warnings/errors. Logs: `/workspace/ava-tools/goal11-generate.log` and `goal11-build.log`. All final source changes were included. Formatting uses clang-format22 and diff-check. No CTest ran before11. Compiled regression coverage includes timer cancellation/generation/rearm, wake coalescing/HUP, pending-request retirement, prompt Escape ordering, plugin deadline cancellation, receipt boundaries, attached painting and display timer discovery; compilation alone does not validate those assertions.

The cloud setup provides an unprivileged signed Debian tool cache and activation/install instructions under `/workspace/ava-tools`; the saved cloud environment configuration is an unpublished review draft (`cecfgdraft_7b7bce3a8e288199bbe265f478f08f07`). Fresh-task restoration was not tested. This environment completed the builds; no new production dependency was added.

## Runtime/test results

No tests have run at this checkpoint. After11 is committed and pushed, run the full `scripts/run-tests.sh` once, record exact passed/failed/skipped counts and failures here, fix only obvious/simple refactor-caused failures, rebuild and report any focused verification separately. Do not infer behavior from successful compilation.

Configured opt-ins remain unchanged: tmux/PTY/graphics/live-provider/real-clangd/ACP SDK and packaging checks may skip for their documented flags/prerequisites. Record actual outcomes after the run rather than treating these as passes. Suggested future terminal checks, if not exercised by the configured suite: simultaneous buffered input/event floods; HUP during prompt/finalization; signal without characters; editor/suspend restore; detached transcript selection/autoscroll; settings preview across reload; branch cancellation and Mermaid queue contention. Use the existing opt-in harness rather than inventing a second terminal owner.
