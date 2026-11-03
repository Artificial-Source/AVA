# 06/02 — Ownership decisions

Inputs read: 06/handoff.md and the source-verified 06/01/inventory.md. Baseline runtime remains the plan branch. These are agent-selected decisions under the user's instruction to continue without waiting for review; they are reviewable, not a claim that the maintainer has personally approved them. No unresolved question blocks implementation. Scheduling contracts are deliberately reserved for goal07 after goal06's fresh audit.

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
| TranscriptView | Scroll/detached state, selection, anchors, new-output indicator interaction | Scroll, select/copy/toggle, apply transcript/layout change, reset on committed session. Preserve detached anchor around any mutation affecting layout. |
| RuntimeRenderer (evolved; ScreenRenderer rename optional only if useful) | Drawing resources, completion/layout/screen caches, invalidation/frame state | Prepare layout, request/flush paint, geometry query, coherent frame publication. Does not own feature policies. |
| RuntimeMermaidPresentationController (evolved) | Existing request identity/config/result records; transcript/search collaborators as needed | Service and publish layout changes through transcript operations, shutdown/cancel. No broad service lambda in session. |
| TuiInputDispatcher | Ordered callbacks bound to longer-lived feature/composition targets | Route a decoded event under explicit precedence; Pass continues, Handled consumes, Exit/Failure stop dispatch. No mutable context bag or feature-reference bundle. Registrations are owned and destroyed before their nonmoving targets. |
| RuntimeActiveRunController (evolved into serviced ActiveRun; matching ActiveRun files if renamed) | Owned request/future, event accumulation/queue/context, cancellation/close intent, completion outcome | Start/consume/update/input/complete/cancel. Never run a terminal loop in the final state. Holds worker data through join and retires sinks on all exits. |
| TuiEventLoop | Waiter registrations, member timers, local stop state, notification readiness | One wait/dispatch owner. Goal07 chooses detailed API; clients register callbacks rather than compute wait durations. |
| Spinner | Frame phase and timer registration | Start/tick/stop; member callback every 250ms, skip missed ticks. Renderer owns drawing, spinner owns phase. |

Focused prompt coordinator/stash, plugin UI, transcript search and subagent workspace remain collaborators. Prompt UI must eventually become serviced modal operations too: inventory found permission/question terminal loops in addition to active-run. Worker condition-variable waits are permitted; a second TUI input loop is not. External editor/suspend are explicit terminal handoffs, not competing terminal ownership.

## Composition and lifetimes

Application and its terminal/signal services outlive TuiFrontEnd and its TUI thread. TuiFrontEnd initializes terminal policy and startup configuration, keeping initial_status_line local, then constructs/runs TuiSession. A scoped terminal/bootstrap owner may encapsulate guards while preserving their initialization order; do not change thread assertions or signal-handler policy. TuiSession is noncopyable and its lifetime is one run.

Construction dependency graph:

```
Application/Context + TuiFrontEnd (borrowed, outer lifetime)
  -> shutdown/terminal guards
  -> presentation + selector + composer
  -> renderer/transcript + navigation/search
  -> settings/session selector/branch summary + actions/stash
  -> prompts/plugin/workspace/Mermaid
  -> loop registrations + active operation + submission router
```

Goal05/01 evolves the former public `RuntimeDraftState` into `Composer.h/.cpp`, matching the selected owner. Its editor, history checkpoint, selection, completion, jump, Escape and scroll fields are private; immutable draft/history reads do not allow callers to bypass transitions. Editing, history browsing, completion acceptance, pointer selection and submission/reset operations coordinate related fields. `ComposerEditContext` and `ComposerCompletionOrigin` preserve the baseline's distinct idle/active and keyboard/pointer reset semantics rather than silently broadening resets. Composer publishes input projection explicitly; renderer clamping and completion-only publication remain separate operations. Prompt stash restoration invokes its existing storage operation against Composer's private editor through a focused Composer operation. No retained callback or mutable state-bag accessor is introduced. Arbitrary draft/history and undo/kill text require the documented generated-printing opt-out.

Goal05/01 installs the dispatcher last in TuiSession, after every callback target. It owns an ordered callback vector, with an adapter for existing event handlers returning optional render-success. The exact precedence and conditional fallthrough remain explicit in session composition; focused handlers coordinate owners through operations. SelectorInputController owns the former common/scoped selector algorithms and backend callback boundaries; typed owned results request draft application, reasoning selection, or committed-session reconciliation without borrowing those owners. SelectorHost remains the single private kind/view/publication authority. TuiFrontEnd grants only the focused selector controller the same existing protected callbacks.

Specific declaration order follows actual constructor dependencies and must be recorded in migration ledger. Avoid cycles by passing a narrow operation callback/interface for a genuine cross-owner request, with owner lifetime documented; do not create a generic context object exposing every field. Event-loop notification lifetime must exceed all publishers. During shutdown: stop admission, request cancellation/unblock pending prompts and plugin requests, join submission, unregister sinks and timers, cancel presentation work, invoke frontend shutdown while collaborators and terminal context remain alive, then unwind terminal graphics guards. Early returns and exceptions use the outer shutdown guard after session unwinding, preserving the baseline RAII path. Normal and early-return cleanup use RAII; cleanup must be idempotent. In particular remove the baseline audit sink dangling-capture hazard documented in inventory.

Existing callbacks that invoke backend authority remain backend callbacks. They do not confer session/provider/permission/process ownership on a TUI object. A TuiSession friendship declaration in TuiFrontEnd is acceptable for composition/bootstrap access; focused backend-facing feature friendships may be used deliberately instead of widening protected callbacks to public. Document exact friends and never grant backend data access merely to avoid defining an operation.

## Projection and reference contracts

RuntimePresentationState remains the owner of frontend display projection and committed identity tracking. Rendering snapshot is a projection, not source of backend truth. Feature owners publish only their part of that projection. SelectorHost owns actual kind/view; snapshot.select_list is a published rendering value, not a second independently edited authority. Until all consumers migrate, a narrow explicitly recorded compatibility bridge may exist; remove writable kind/view aliases by goal11.

Read-only sharing uses const references with owner lifetime. Examples from inventory: selected item/action, sidebar display facts, branch-summary snapshot, settings row builders, plugin conflict predicates. A borrowed selected-item pointer expires at selector replacement; copy action/identity before invoking authority that may close the view. Do not store string_view/span into mutable view/draft.

Mutation is explicit: settings updates preview+view together; composer updates draft/history/selection/completion together; transcript applies mutation with anchor capture/restore; renderer prepares caches before pure queries. Do not label current completion/page-size queries const while they still publish snapshot fields or fill caches. Cache results may be read through const views after owner preparation. Pure filter/index/geometry/status formatting calculations stay free functions.

Committed session transitions remain composition-level operations. Goal04/03 makes their ordering an explicit `SessionTransitionCoordinator` owned by TuiSession, rather than duplicating it in idle and active consumers or retaining a free function with a mutable reference bundle. Its constructor borrows the earlier-lived presentation, draft, renderer, search, workspace and selector owners. Consumers pass only the committed ID to `apply`, or frontend plus ID to `apply_and_refresh`; fixture composition uses the same coordinator. RuntimePresentationState grants only this coordinator access to private transition phases: clear grants/attachments, reset projection, and finalize sidebar/identity. The coordinator inserts the other owners' reset operations between those phases, preserving the prior sequence. `apply_and_refresh` must be called without the UI lock because it performs backend reads; active completion continues to read identity/boundary data before its existing UI lock, then calls `apply` and applies the already-read data. Read committed ID/backend transports outside the UI lock, then reset each owner and apply retained data under existing TUI synchronization. Reconcile on error-after-commit as well as success. Reset grants, attachments, transcript/search, workspace, composer, selection and selector as one coordinated transition. Same-identity refresh preserves session-scoped state. Never put this reference bundle into a new dispatcher or SessionSelector constructor as the final design.

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
