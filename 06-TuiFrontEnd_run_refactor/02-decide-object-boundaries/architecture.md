# 06/02 — Ownership decisions

Inputs read: 06/handoff.md and the source-verified 06/01/inventory.md. Baseline runtime remains the plan branch. These are agent-selected decisions under the user's instruction to continue without waiting for review; they are reviewable, not a claim that the maintainer has personally approved them. No unresolved question blocks implementation. Scheduling contracts are selected in07 after06’s fresh audit; see06/07/event-contracts.md.

## Selected owners and files

New domain classes have CamelCase names in matching headers/implementation files under src/ava/tui. Existing useful controllers may retain their names while their responsibilities are narrowed; do not rename an entire subsystem solely for appearance. The following are ownership commitments, not a demand for an otherwise unused wrapper for every row.

| Owner | Private state | Meaningful operations / invariant |
| --- | --- | --- |
| TuiSession | Borrowed frontend/terminal services, owned collaborators, session exit/failure outcome | Compose, service, dispatch, reconcile committed session, close. Own ordering and cross-owner transitions, not menu/edit algorithms in final state. |
| SessionTransitionCoordinator | Borrowed presentation, composer/draft, renderer, search, workspace and selector owners | Apply one committed-identity transition in the required reset order; refresh retained data when requested. Owned by TuiSession; shared by idle/active consumers, with identical fixture composition. |
| SelectorHost | Active kind and authoritative optional SelectListView; selected/query state belongs to view | Open/replace/close, select/filter, preserve selection identity, publish render projection. Kind=None iff no hosted view. One transaction replaces both. |
| SelectorInputController | Borrowed frontend, presentation, selector host, renderer and Context | Classify common/scoped selector input, apply catalog actions, return owned draft/committed-session requests for composition. No composer/transition/settings/branch reference bundle. |
| SettingsDialog | Navigation section/root checkpoint, preview baseline/overlay, open lifetime | Open/rebuild/highlight/rebase/confirm/cancel/close. Highlight never persists configuration. Reload explicitly rebases authoritative baseline before overlay publication. |
| SessionSelector | Archive confirmation, selection preservation checkpoint as needed | Browse, preserve query/identity, request archive/unarchive/parent/child/open via frontend, yield committed-session outcome to coordinator. Backend keeps exact identity validation and policy. |
| BranchSummaryOperation | Active/generation/phase, cancellation and deferred-exit intent, prior selector checkpoint | Start/consume/confirm/cancel/settle; reject stale generations; yield explicit pending/settled/exit/failure outcome. No separate shared exit flag. |
| Composer | Draft/cursor/selection/mouse-select/history/saved draft, jump/Escape and completion interaction | Insert/paste/edit/move/select/history/accept completion/reset. Coordinate all draft-related reset invariants in operations. Keep decoded event and derived query temporaries local. |
| TranscriptView | Private viewport/count/frozen sidebar, selection, deferred anchors, pending item shift and position-indicator lifetime | Owned by renderer with no renderer back-reference. Scroll/navigate/restore, complete updates, select/copy, publish frame phases and reset; consumes immutable prepared layout authority. |
| RuntimeRenderer (evolved) | Private drawing/completion/layout/screen caches, wheel admission, UI mutex, frame scheduler and owned TranscriptView | Prepare layout/completion/geometry, lock coordinated updates, request/flush paint and publish coherent frames. Const diagnostic views expire at the next cache mutation. |
| RuntimeMermaidPresentationController (evolved) | Private request identity/config/result records and borrowed snapshot/renderer/search | Reconcile bridge results, preserve viewport, refresh search, request a frame, return Unchanged/Changed/RenderFailed, shutdown/cancel. Direct idle/active collaborator. |
| TuiInputDispatcher | Ordered callbacks bound to longer-lived feature/composition targets | Route a decoded event under explicit precedence; Pass continues, Handled consumes, Exit/Failure stop dispatch. No mutable context bag or feature-reference bundle. Registrations are owned and destroyed before their nonmoving targets. |
| RuntimePluginUiCoordinator | Shared synchronized endpoint mailbox/token and borrowed snapshot | Admit a submission, publish input/poll outcomes, cancel worker waits, finish/shutdown its own ephemeral projection. Worker cancellation never mutates projection. |
| RuntimePromptCoordinator + PendingPermissionRequest/PendingQuestionRequest | Admission, queued/claimed shared request ownership, audit sink; request-private prompt/result/mutex/wait/retirement | Open admission explicitly, claim/service/complete, close admission and cancel safely, shutdown. A claimed waiter retains borrowed callback state until UI resolution retires. |
| RuntimeSubagentWorkspaceController | Private mode, selector return checkpoint, workspace/job/generation and poll deadline | Open/refresh/navigate/close its presentation; cleanup closes only its own host and does not cancel backend jobs. |
| ActiveRun | Owned submission/future, backend queue capabilities, completion phase and outcome, Spinner; borrowed earlier-lived UI collaborators | Start, bounded consume, one-event input, complete, cancel and shutdown. No terminal reads or nested loop. Finalizing freezes producer admission and drains accepted receipts before settlement. |
| ActiveRunInbox | Synchronized event queue/context, stop flag, terminal result/exception and producer admission | Shared endpoints publish before waking and safely reject after retirement; payload readiness precedes join. No projection/terminal access. |
| ActiveRunEvents | TUI-only aggregate, baseline/evictions, ordered request partitions and receipt authority | Apply events, authorize exact receipts, return immutable/value projections, cap baseline updates, merge unauthorized settled tools, discard old-session state. |
| TuiEventLoop | Waiter registrations, member timers, local stop state, notification readiness | One wait/dispatch owner. Goal07 chooses detailed API; clients register callbacks rather than compute wait durations. |
| Spinner | Frame phase and timer registration | Start/tick/stop; member callback every120ms (actual composer cadence), skip missed ticks. Renderer owns drawing, spinner owns phase. |

Focused prompt coordinator/stash, plugin UI, transcript search and subagent workspace remain collaborators. Prompt UI must eventually become serviced modal operations too: inventory found permission/question terminal loops in addition to active-run. Worker condition-variable waits are permitted; a second TUI input loop is not. External editor/suspend are explicit terminal handoffs, not competing terminal ownership.

## Composition and lifetimes

Application and its terminal/signal services outlive TuiFrontEnd and its TUI thread. TuiFrontEnd initializes terminal policy and startup configuration, keeping initial_status_line local, then constructs/runs TuiSession. A scoped terminal/bootstrap owner may encapsulate guards while preserving their initialization order; do not change thread assertions or signal-handler policy. TuiSession is noncopyable and its lifetime is one run.

Construction dependency graph:

```
Application/Context + TuiFrontEnd (borrowed, outer lifetime)
  -> shutdown/terminal guards
  -> loop + presentation/attachments/grants/plugin
  -> composer + renderer/transcript + navigation/selectors
  -> settings/session selector/branch summary + stash/search
  -> prompts/Mermaid/actions/workspace/transitions
  -> active operation + submission router/dispatcher/control registration
```

Goal05/01 evolves the former public `RuntimeDraftState` into `Composer.h/.cpp`, matching the selected owner. Its editor, history checkpoint, selection, completion, jump, Escape and scroll fields are private; immutable draft/history reads do not allow callers to bypass transitions. Editing, history browsing, completion acceptance, pointer selection and submission/reset operations coordinate related fields. `ComposerEditContext` and `ComposerCompletionOrigin` preserve the baseline's distinct idle/active and keyboard/pointer reset semantics rather than silently broadening resets. Composer publishes input projection explicitly; renderer clamping and completion-only publication remain separate operations. Prompt stash restoration invokes its existing storage operation against Composer's private editor through a focused Composer operation. No retained callback or mutable state-bag accessor is introduced. Arbitrary draft/history and undo/kill text require the documented generated-printing opt-out.

Goal05/01 installs the dispatcher last in TuiSession, after every callback target. It owns an ordered callback vector, with an adapter for existing event handlers returning optional render-success. The exact precedence and conditional fallthrough remain explicit in session composition; focused handlers coordinate owners through operations. SelectorInputController owns the former common/scoped selector algorithms and backend callback boundaries; typed owned results request draft application, reasoning selection, or committed-session reconciliation without borrowing those owners. SelectorHost remains the single private kind/view/publication authority. TuiFrontEnd grants only the focused selector controller the same existing protected callbacks.

Goal05/02 makes TranscriptView a private RuntimeRenderer member borrowing the already-lived presentation/sidebar/Composer; it has no renderer back-reference, retained callback or construction-time service. Renderer exposes the independently encapsulated view through transcript(), whose methods coordinate viewport/count/sidebar/selection invariants, and keeps cache mutation behind preparation/paint operations. Read-only layout/completion/frame views are borrows valid until the next corresponding preparation, render or reset. RuntimeNavigationController remains a focused input/query collaborator, publishes the original completion-input fields before renderer queries and no longer borrows sidebar storage. A private immutable TranscriptUpdate token carries pre-mutation detached/anchor decisions through active event draining without retaining input or iterator references. Existing UI lock scopes use an owning lock guard returned by renderer.

Goal05/03 moves Mermaid orchestration into the existing owner, borrowing already-constructed snapshot/renderer/search and returning explicit service outcomes. ActiveRun holds that owner directly, replacing the optional callback/no-op seam. Plugin UI now borrows its earlier-lived projection and owns idempotent finish/shutdown cleanup; worker endpoints still retain only synchronized mailbox state, never the projection. Prompt admission opens once before submission resolver creation, and pending request owners synchronize cancellation/result/retirement privately. A claimed prompt cannot release its worker while auto_resolve may still borrow worker-local state; cancellation of the wait completes after UI resolution retires. Workspace cleanup is its own close operation, also used by transitions and destruction. Nested prompt/active loops and complete active-operation exception/sink lifetime remain explicitly assigned to09–10.

Specific declaration order follows actual constructor dependencies and must be recorded in migration ledger. Avoid cycles by passing a narrow operation callback/interface for a genuine cross-owner request, with owner lifetime documented; do not create a generic context object exposing every field. Event-loop notification lifetime must exceed all publishers. During shutdown: stop admission, request cancellation/unblock pending prompts and plugin requests, join submission, unregister sinks and timers, cancel presentation work, invoke frontend shutdown while collaborators and terminal context remain alive, then unwind terminal graphics guards. Early returns and exceptions use the outer shutdown guard after session unwinding, preserving the baseline RAII path. Normal and early-return cleanup use RAII; cleanup must be idempotent. In particular remove the baseline audit sink dangling-capture hazard documented in inventory.

Existing callbacks that invoke backend authority remain backend callbacks. They do not confer session/provider/permission/process ownership on a TUI object. A TuiSession friendship declaration in TuiFrontEnd is acceptable for composition/bootstrap access; focused backend-facing feature friendships may be used deliberately instead of widening protected callbacks to public. Document exact friends and never grant backend data access merely to avoid defining an operation.

Goal09 replaces the nested controller with `ActiveRun.h/.cpp`, retaining its useful input decomposition while giving the operation actual request/future/completion ownership. Its shared `ActiveRunInbox` is the sole production event/context/terminal endpoint owner; its private `ActiveRunEvents` owns the receipt-sensitive accumulator. Immutable aggregate/baseline borrows expire at the next owner mutation. No public per-run state bag or mutable getter remains. Worker submission/context scopes retire before terminal payload publication; ordinary completion consumes that payload before joining. Backend identity/transports are still read outside the UI lock, queue finish may publish final receipts, then inbox admission closes before any old-session discard. Remaining accepted events drain in bounded turns before settlement.

The session now constructs the loop before all notification/timer clients. Prompts require that loop in construction; plugin UI attaches explicitly before admission; ActiveRun owns Spinner. Prompt UI is persistent start/service/input/cancel state with no terminal read. A question's borrowed auto callback remains reachable only while its request resolution lease is held. Permission Escape returns a stop effect with that lease retained; ActiveRun publishes stop first, then retires the permission after the input callback returns. This preserves stop-before-worker-release without reentrant modal completion. Shutdown returns the recorded immutable active outcome, so signal130 survives a concurrent render failure. Explicit shutdown retires prompt callbacks and plugin waits before joining, and safe inbox retirement covers copied audit endpoints as well as the installed registration.

At its checkpoint, goal09 retained the outer timed input wait and immediate renderer as a recorded intermediate seam. Goal10 removed that seam and integrated the final control/application/input/paint/resource-wait batches and all remaining discovery/render timers. No nested active/prompt input loop or synchronous run compatibility adapter remains.

## Projection and reference contracts

RuntimePresentationState owns private frontend display projection and committed identity/catalog tracking. Its explicit screen()/sidebar() boundary shares the existing render DTOs on the TUI thread; it is not backend authority. Feature owners publish only their part. SelectorHost owns actual kind/view; snapshot.select_list is a published rendering value. No writable kind/view alias remains. Goal11 moves staged attachment references/badge cardinality into PendingAttachments and the existing grant registry into TuiSession, with narrow injections to their actual consumers. The remaining mutable DTO exception and exact publication/cache examples are documented in06/11/refactor-handoff.md; metadata and domain state remain private.

Read-only sharing uses const references with owner lifetime. Examples from inventory: selected item/action, sidebar display facts, branch-summary snapshot, settings row builders, plugin conflict predicates. A borrowed selected-item pointer expires at selector replacement; copy action/identity before invoking authority that may close the view. Do not store string_view/span into mutable view/draft.

Mutation is explicit: settings updates preview+view together; composer updates draft/history/selection/completion together; transcript applies mutation with anchor capture/restore; renderer prepares caches before pure queries. Do not label current completion/page-size queries const while they still publish snapshot fields or fill caches. Cache results may be read through const views after owner preparation. Pure filter/index/geometry/status formatting calculations stay free functions.

Committed session transitions remain composition-level operations. Goal04/03 makes their ordering an explicit `SessionTransitionCoordinator` owned by TuiSession, rather than duplicating it in idle and active consumers or retaining a free function with a mutable reference bundle. Its constructor borrows the earlier-lived presentation, draft, renderer, search, workspace and selector owners. Consumers pass only the committed ID to `apply`, or frontend plus ID to `apply_and_refresh`; fixture composition uses the same coordinator. RuntimePresentationState grants only this coordinator access to private projection-reset and sidebar/identity-finalization phases. Goal11 injects the independently owned attachment/grant owners so the coordinator first clears them on changed identity, then inserts the other owners' reset operations before projection/finalization, preserving the prior sequence. `apply_and_refresh` must be called without the UI lock because it performs backend reads; active completion continues to read identity/boundary data before its existing UI lock, then calls `apply` and applies the already-read data. Read committed ID/backend transports outside the UI lock, then reset each owner and apply retained data under existing TUI synchronization. Reconcile on error-after-commit as well as success. Reset grants, attachments, transcript/search, workspace, composer, selection and selector as one coordinated transition. Same-identity refresh preserves session-scoped state. Never put this reference bundle into a new dispatcher or SessionSelector constructor as the final design.

## Encapsulation and printing

All new data members private, including borrowed collaborators. Constructors document who outlives whom. Prefer member operations. A mutable accessor to an independently encapsulated collaborator is acceptable only if it cannot bypass coordinated invariants; writable scalar/container/state-bag getters are not an endpoint.

Every new class ends its last public section with AVA_DEBUG_PRINT_MEMBERS_ON by default. Use NoLogString for sensitive text where practical. A class retaining arbitrary draft/message bodies, event authority aggregates or callbacks that expose such data may deliberately use OPT_OUT with a specific comment and ledger entry. SessionTransitionCoordinator uses this exception because its borrowed presentation/draft owners contain those deliberately non-printable aggregates; it does not add content printers merely to satisfy generated output. Do not use anonymous namespaces to avoid the printing requirement. Existing anonymous RAII helper types retain their documented internal-linkage opt-out until replaced. Each new cpp begins with sys.h. Preserve aligned comments and all #if 0 blocks.

## Migration strategy and temporary exceptions

Goal03 may introduce a private intermediate TuiSession state aggregate and move the outer orchestration into methods. It is a temporary way to expose the implicit stack lifetime, not completion. Its feature algorithms and mutable dependencies must be listed with removal destinations 04/01–04, 05/01–03 and09–10. Initialization-only locals and per-event values stay local. Delete trivial action/navigation forwarding lambdas rather than mechanically replacing them with forwarding methods. Genuine worker callbacks bind lifetime-safe target operations; pure calculations remain local/free.

Goals04/01–04 establish selector/features in order. Goals05/01–03 migrate composer/dispatch, transcript/rendering and presentation lifetimes. Goal06 audits the resulting actual code, not this proposed graph. Goals07–10 decide and integrate scheduling including prompt modals. Goal11 removes obsolete adapters, unsafe mutable access and unused stubs; every remaining deviation needs a concrete reason and maintainer-facing flag. Unused API-first classes can exist only in intermediate commits, never as a substitute for wiring.

## Alternatives rejected and maintainer review flags

* Rejected: one private monolithic snapshot with mutable getters. This changes syntax without ownership.
* Rejected: one class per lambda or forwarding methods mirroring each controller method. Domain state and operations must stay together.
* Rejected: moving permissions/session/process policies to selectors to avoid callbacks. Frontend methods remain the authority boundary.
* Rejected: making all reads copies or adding mediators solely because two features read snapshot. Const references suffice with clear lifetimes.
* Rejected: replacing only the outer polling wait. Active-run and permission/question nested loops must be serviced by the same TUI loop.
* Rejected: redesigning terminal decoding/thread-safety or RPC objectives. Preserve terminal ownership assertions and bounded decoder behavior; document any synchronous decoder subreads and use buffered reads before sleeping.
* Review flag: retain existing Runtime* class names where evolving a useful owner avoids churn; new classes always get matching CamelCase files. Reconcile any renames/deviations in ledger and final handoff.
* Review flag: compatibility adapters may be needed for source-level test consumers. Keep assertions meaningful; no test-fixing churn or CTest until final goal. API adaptation needed to keep the full build compiling is distinct from weakening tests.

## Evidence

No runtime changes in goal02. `source /workspace/ava-tools/activate.sh; scripts/build.sh` in /workspace/AVA passes with existing dev BetaTest/EnableDebug=ON/GCC16.2/Ninja build. No tests run. `git diff --check` is the pre-commit hygiene gate, not behavioral proof.

## Goal07 scheduling reconciliation

The actual integration contract is `06/07/event-contracts.md`, informed by the post-extraction audit. TuiEventLoop composes existing utils::threading::EventLoop with fixed terminal/application watches, private absolute-deadline member timers and local stop. Move-only timer handles are canceled before stable callback targets and the earlier-lived loop die. Weak shared waiter-resource capabilities publish wake hints without referencing session/presentation; authoritative queues/results publish first. Active submission and both prompt modals become serviced operations, including scoped sink/worker retirement.

Actual Spinner cadence is120ms, not the earlier illustrative250ms; late repeating callbacks skip missed ticks. Renderer batches paint after application/input service and owns its16ms deadline, with explicit flush exceptions for synchronous terminal handoffs. Signal/resize observation uses a100ms control member timer without changing handlers/thread policy. Workspace150ms, display500ms, branch250ms and Mermaid16ms retry/discovery timers are deliberate because their current source contracts lack complete frontend notifications. Mermaid retryable/unsubmitted work counts as pending. Plugin and question auto-resolution own120s and100ms timers respectively. None of these feature clients compute a global wait duration.

Application batches are bounded at256 events and terminal batches at32 events, with retained backlog defeating blocking and due timers serviced despite continuous readiness. Buffered ncurses/startup input is checked before sleep. Terminal HUP/error has an explicit close/failure policy; signals preserve130 outcome precedence. Existing decoder subreads and synchronous backend/editor/suspend work remain documented latency limits. These agent-selected decisions proceed under user authorization; no claim of personal maintainer approval is made.


## Goal10 implemented wiring

TuiSession now runs the single control → due callbacks → bounded application updates → bounded buffered input → due callbacks/coherent paint → resource wait sequence. Production attaches renderer, display, branch, workspace and Mermaid clients before dispatch. Frame/indicator/selection timers belong to the renderer; discovery timers belong to their feature owners. Timer callbacks publish readiness/outcomes, never paint. Explicit synchronous paint remains only at initial/terminal/backend authority handoffs. Published completion holds input until bounded final receipt settlement. Known terminal closure disables new feature discovery and drains final buffered bytes before owned cancellation/join and frontend shutdown. Details and remaining final-audit obligations are in06/03/migration-ledger.md.


## Goal11 final audit

The implemented ownership/lifetime guide is `06/11/refactor-handoff.md`. PendingAttachments owns reference/badge pairing and transfer with exception rollback; TuiSession owns the existing grant registry before prompt/transition borrowers. RuntimePresentationState has no public fields; its screen/sidebar DTO publication exception is explicit rather than a claim of complete immutable projection. TuiSession retains event precedence and cross-feature coordination, while editor, selector, branch, viewport, request and receipt state live in their respective owners. The final audit removes raw queue callback factories, unused rendering APIs, remaining active mouse forwarding lambdas and obsolete frontend includes. All temporal integration rows are closed; no deferred stub remains. Runtime validation follows only after the compiled11 checkpoint is pushed.

## Final R13 editor dispatch

`Composer::handle_key` dispatches to private phase methods. One ordered action
table owns deletion, clearing, horizontal movement and undo/yank precedence.
`ComposerEditContext` preserves the existing idle and active reset rules.
`ActiveRunInput` calls those phases at its existing feature boundaries and applies
returned scroll/selection requests. Active cancellation, local commands and queue
submission remain with their existing owners. Clipboard failure consumes an
already-selected copy binding, so it cannot fall through into another edit.

The test-only `ComposerPolicyFixture` keeps its owned editor, catalogs, bindings
and prepared completion values public so each case can arrange a collision
without adding getters and setters that repeat the fixture's fields. This is a
visibility and file-layout exception for an anonymous, synchronous test fixture;
no production feature receives it. Its comment identifies the test-thread and
lifetime limits, and its debug marker excludes user-text examples.

## Final R14 frontend interfaces

The five formerly friended features borrow `TuiSessionActions`,
`SettingsDialogActions`, `SessionSelectorActions`, `BranchSummaryActions` and
`SelectorInputActions`. These abstract interfaces have no fields or workers.
`TuiFrontEnd` implements them through its existing application overrides and
keeps the 28 concrete backend-action declarations protected. Public inheritance
allows an explicit interface cast; these interfaces narrow dependencies without
claiming to enforce backend security. There are no forwarding action methods.

Settings copies provider, model and reasoning labels through its interface only
when building the Model section. That preserves when the three refresh flags
are consumed. Presentation uses its already-retained frontend for backend-action
refreshes, local bindings and local display settings. Callers no longer pass the
same frontend through several features. `apply_key_bindings` replaces the stored
map's value; its address remains valid for existing const borrows. Presentation
updates key hints immediately after applying that map.

## Final R15 diagnostic boundaries

`SelectorHost` owns only a kind and a presentation reference, so its generated
printer is enabled. `RuntimePluginUiCoordinator` prints its presentation address,
opaque transport/token identities and timer metadata. Its request state remains
private in the implementation file with no content printer; diagnostics never
inspect that state or acquire its mutex.

`ActiveRun::Operation` stays nested because only its lifecycle owner uses it.
This is a file-layout exception for private implementation state, not a shared
result or another feature API. Its handwritten printer shows only the six
lifecycle flags. Submitted text, queues, future, reducer and inbox are excluded.
That bounded printer permits generated printing of the containing `ActiveRun`.

`ComposerInputResult::status` and both `DraftCommand` strings directly spell
`NoLogString`, which the generator omits. Their remaining metadata can print.
`MemberReference` now lives in its matching header and preserves address-only
reference handling through the existing helper include. Selector result text is
also annotated. Its saved-view alternative has a handwritten row-index printer;
the copied `SelectListView` never enters diagnostics.


## Final R16 result layout

`ComposerInputResult`, `DraftCommand`, `InputPollResult` and
`SelectorDispatchResult` have matching headers. `RuntimeInput` also has its own
header so polling results do not depend on the reader-function declarations.
The enum associated with each result stays beside it.

`SelectorDispatchResult` is a variant: payload-free requests use an enum, while
five records own the fields needed for draft completion, command drafting,
session success, session error or parent summary. These five records share the
result header as a deliberate layout exception: they form one result contract,
with one producer (`SelectorHost`) and one consumer (`TuiSession`). None is a
new feature owner. A summary always carries its saved view, so a summary request
without restoration data cannot be constructed through the old field mismatch.
Session reconciliation still precedes success/error feedback; draft completion
and command drafting retain their different reset rules.

These result records also retain public fields as a visibility exception. Each
is a short-lived, owned value assembled once by its producer and consumed by its
caller on the same thread. They own no registration, worker or coordinated
mutable state. Public aggregate initialization makes each handoff explicit;
private fields with matching setters would add no invariant. Feature owners
remain private and expose operations instead of writable state.


## Final R16 loop construction

Renderer and plugin UI timers register during construction with a required
`TuiEventLoop&`. Their test-only unattached modes and `loop_attached_` flags are
gone. All timer handles retire before their loop and callback targets.
Plugin transport receives only the loop's weak wake handle; an endpoint still
cannot retain a screen owner. Poll no longer expires a request by an injected
clock; tests drive the same expiry callback through the real scheduler.

`TuiEventLoopFixture` is test infrastructure in its matching support header. Its
private pipe and loop keep production lifetime rules executable in fixtures
whose ncurses input is a regular file. Its generated-printing exception is that
test-support headers are outside production printer discovery. Existing local
fixture classes remain local; no new production adapter or test mode is added.
The close/poll race keeps poll on the TUI thread and invalidates the endpoint on
a joined worker, with a deadline on both the race gate and teardown.


## Final R16 test boundaries and remaining layout decision

Production no longer exposes saved draft, forced completion, frozen sidebar,
new-output count, indicator visibility or painted-cache getters for tests.
Editor tests observe published completion fields and actual forward history
restoration. Transcript tests re-enter detached history before a frame to expose
stale count/sidebar state; a live-tail paint cannot silently repair the reset
being checked. Tests still check that the count waits for a paint.

A test-only `ScopedScreenDrawObserver` wraps the real full-screen draw call,
then owns a copy of the resulting cache and return value. It neither requests a
frame nor computes substitute output. This preserves prompt-before-answer and
frame-coalescing assertions while removing `RuntimeRenderer::painted_frame`.
Its scoped thread-local registration and direct wrapper friendship are test
instrumentation exceptions; no production owner exposes new mutable state.
The copied frame contains user text, so this test class deliberately opts out
of generated printing. It observes full-screen calls from other translation
units under GNU linker wrapping; footer-only drawing is outside its scope.

Legacy input readers used only by tests are removed. Startup probes retain the
native-terminal reader. Tests call the same `poll_curses_input` used by Session;
PTY waits use a real loop timer, and drainage distinguishes `Discarded` from
`Empty`. `InputReadDeadline` stays in this test translation unit because only
its local wait helper uses it; this is a file-layout exception for test support.
Its private flag changes only through the timer callback, and tests read it
through a const observer. Its anonymous linkage requires the printer opt-out.

The finished-tool merge helper is private to `ActiveRunEvents.cpp`. Tests through the owning reducer retain duplicate replacement, canceled/error cards, unattributed and
unauthorized groups, and authorized/running exclusion. The removed direct
helper case manufactured a canceled card still in `pending_tools`; current
`ToolResultEvent` producers instead move canceled cards into the transcript.
The defensive internal pending-card merge remains. No state injection API
replaces the export.

`Composer::scroll_to` was the sole producer of a nonzero draft scroll offset,
and only a test called it. The assertion that injected a nonzero offset is removed;
the independently reachable jump-reset case remains. This does not add or
remove user scrolling behavior. Pointer completion now returns owned feedback
to Session; even a disabled clicked row consumes the pointer event before
transcript or draft selection. No shared local-command router is introduced.

The `Composer` and `composer` filenames remain distinct only by case, as the
maintainer explicitly requested. Case-insensitive checkout support still needs
a separate filename decision.

## Final R20 test observations

The session integration test keeps its local `Turn` observations and
`SessionProbe` arrangement fields public. These are test instrumentation and
file-layout exceptions: cases install callbacks and inspect owned measurements
without adding production hooks or a second API of test-only setters. Only the
TUI thread accesses the scoped probe. Copied audit callbacks use independently
synchronized captures that survive worker shutdown. Both anonymous types opt
out of printing; observations can contain input examples.

The control-timer case wraps real registration, callback preparation and waiting.
It checks the selected absolute control deadline, a real OS timeout without
another event, and actual execution of the copied callback on the next turn.
The borrowed registration node is read only while Session is alive. Late setup
may first require ready-only turns; the assertions begin with the first actual
blocking wait and do not impose a tight elapsed-time limit.

The session-reset test seeds `session_entry_count` through an explicit const cast
of a known non-const, single-threaded fixture. This is a narrow test-only exception
because the field currently has no production writer; no public setter is added.
The test checks the populated live owner before transition and its absence after.
Workspace discovery uses a far-future synthetic time with headroom, so real paint
time cannot conceal an erroneous redraw deadline.
