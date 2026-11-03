# 06 migration ledger

This is the shared handoff for ordered implementation goals. Required inputs read for goal03: 06/handoff.md, 06/01/inventory.md, 06/02/architecture.md and the goal03 description. Goals01/02 were committed and pushed first. Source paths below are relative to the repository. Compilation is the intermediate gate; no CTest or TUI behavior checks have run during this refactor.

## 06/03 — Explicit session composition

`src/ava/tui/TuiSession.h` and `.cpp` introduce a noncopyable, nonmovable run-lifetime owner. `TuiFrontEnd::run` retains terminal eligibility, terminal/signal startup policy, graphics/background-detection guards, keybinding/display loading and local `initial_status_line`, then constructs the session. The startup status is consumed by presentation construction; no status reference is retained. The existing backend/frontend callbacks are called through the borrowed frontend, enabled by one explicit `friend class TuiSession`; protected authority methods have not been made public. No RPC or terminal-thread assertion code changed.

Session declaration order is now explicit: borrowed frontend/context; presentation and plugin; exit flags; draft; renderer; navigation; selector kind; stash; temporary branch-summary/settings fields; search; archive confirmation; prompt coordinator; Mermaid; actions; workspace; active-run controller; submit controller. Every member reference targets an earlier member or the outer frontend/context. Session is nonmovable because existing controller callbacks bind its address. The active controller's Mermaid callback uses `std::bind_front(&TuiSession::service_mermaid_presentation, this)`, not references to constructor locals.

The former aliases are not separate stored fields: code addresses the actual private owner (for example `draft_state_.draft`, `presentation_state_.snapshot`, `renderer_.completion_cache`). There is no public mutable getter. This is the explicitly permitted intermediate composition, not the final feature ownership model.

### Implemented operations

Settings preview/open/rebuild/highlight/close and display reload are session methods pending SettingsDialog extraction. Branch-summary start/poll/consume/confirm/cancel/settle and restoration are methods pending BranchSummaryOperation. Composer completion acceptance, forced completion and decoded-text insertion are methods with explicit per-event parameters. Selector classification, state preservation, selected-item/visible-value projection, committed-session settlement and synchronization are methods. Per-event `RuntimeInput`, selection result, backend result, queries and status text remain locals/parameters.

All outer stateful orchestration lambdas were removed. Trivial action/navigation/render adapters were deleted and replaced with direct calls; genuine callback parameters use explicit member bindings. Pure action-key matching is a direct free-function calculation. The few remaining inline lambdas bind synchronous backend authority callbacks or the read-user-turn boundary; they do not own run-lifetime state. Their per-event value lifetime remains the existing synchronous call lifetime.

The two unused outer resolver values were removed. The coordinator initializes `accept_prompt_requests_` true, and each active submission obtains its real resolver callbacks and reopens admission itself; the original outer factory calls only redundantly set that initial flag and returned unused closures. No worker callback now targets a constructor-local resolver.

### Cleanup and result contracts

`TuiSession::run(std::function<bool()> const& before_shutdown)` borrows a bound outer guard invocation only for the duration of the call and never stores it. Normal loop exit shuts down Mermaid/plugin presentation, invokes that guard while all collaborators and terminal context remain alive, and returns signal 130 before write/shutdown failure 1 before success 0. Early pre-loop returns and exceptions do not invoke the normal-tail callback; session destruction runs before the outer shutdown guard, retaining the early-exit RAII path. Destructor shutdown is idempotent and also clears the theme preview while the presentation owner still exists. The plugin's own destructor still cancels its synchronized state even if a later session member constructor fails before the session is fully constructed; no plugin submission starts in construction.

Architecture wording was reconciled to preserve normal frontend shutdown before graphics-guard destruction, while distinguishing early-return/exception unwinding. No signal or terminal ownership policy change is intended. Behavioral equivalence remains unverified.

### Printing and file conventions

TuiSession deliberately uses `AVA_DEBUG_PRINT_MEMBERS_OPT_OUT`: its intermediate composition owns arbitrary draft/message bodies and mutable presentation/authority aggregates. Automatically printing that object would expose content. This is the specific sensitive-data exception approved by architecture, not an anonymous-namespace workaround. New files match the class name; cpp starts with sys.h. Existing anonymous startup guards retain their internal-linkage exceptions. Feature extractions must choose their own printing policy and default to ON for non-sensitive state.

### Remaining work and removal goals

| Temporary coupling / known gap | Owning next goal |
| --- | --- |
| Session holds selector kind separately from snapshot view; direct kind/view writes and existing controller reference bindings remain | 04/01 establishes SelectorHost; consumer migrations through04/04 and05; final check11 |
| Settings navigation/preview/open and branch actions remain in session methods | 04/02 SettingsDialog |
| Session-selector archive confirmation and preservation mixed with generic/scoped selectors; transition helpers still receive existing reference bundle internally | 04/03 feature owner and coordinated per-owner transitions |
| Branch-summary state temporarily flattened into private session members; exit-ready flag still separate | 04/04 BranchSummaryOperation explicit result |
| Large input dispatch chain and direct draft-state writes remain; extracted edit/completion methods are temporary session operations | 05/01 Composer/dispatcher |
| Renderer still exposes its legacy caches/viewport; navigation retains mixed completion/transcript behavior | 05/02 TranscriptView/renderer |
| Mermaid viewport service and plugin/theme shutdown still coordinated by session; no separate feature-lifetime owner yet | 05/03 |
| Active submission still blocks in its original nested loop; permission/question modal loops unchanged | 09–10 serviced operations/single loop |
| Active-run prompt audit sink captures stack state on initial-render failure/exception (baseline inventory finding); not modified by03 | 09 RAII operation/sink lifetime |
| Idle polling/deadline calculations, buffered-input and signal-without-input behavior unchanged | Fresh analysis06, contracts07, implementation08–10 |

No empty implementation stubs, no disabled behavior, and no fabricated passing checks were introduced. Each listed coupling is an intended intermediate step; goal11 must reconcile it against actual implementation rather than assume extraction occurred.

### Build evidence

- Existing configured `build/`: CMAKE_BUILD_TYPE=BetaTest; EnableDebug=ON; OptionEnableLibcwd=ON; EnableAvaBuildTests=ON; GCC16.2.0; Ninja; CMAKE_CXX_SCAN_FOR_MODULES=OFF. No manual reconfigure was performed.
- Command: `source /workspace/ava-tools/activate.sh; cd /workspace/AVA; scripts/build.sh` (four jobs supplied by activation). Adding TuiSession.cpp causes the build system's normal CMake regeneration; it succeeded.
- First compilation completed through linking ava and ava_tests, but the wrapper exited2 because a sandbox-created Git zombie kept its worker process-group liveness check true. This run is not reported as a passed build. Recovery proved runnerPID10240 absent and group10245 contained only PID1-owned zombie10255; the stale lock was archived outside the checkout. No build/test wrapper code changed.
- Repeated full build uses the same command with cloud execution access enabled. Final exit/result is recorded below after completion.
- Formatting: repository clang-format configuration, using clang-format22 (clang-format19 cannot parse SpaceInEmptyBraces). `git --no-pager diff --check` is run before commit. No CTest run.

- Final full build: exit0; all44 scheduled steps completed, including ava_tests linkage (`/workspace/ava-tools/goal03-build-final.log`). Compilation is verified; no runtime/TUI/test behavior is claimed.

## 06/04/01 — Encapsulated selector host

Inputs read: 06/handoff.md, 06/01/inventory.md, 06/02/architecture.md, this ledger and the parent04/child01 descriptions. This entry supersedes goal03's separately owned selector kind and writable snapshot selector references. Parent04 remains not-achieved until all four feature children are complete.

`src/ava/tui/SelectorHost.h` and `.cpp` own the private active kind and optional view, including current filter and selection. `open(kind, view)` installs the pair together; `replace(view)` retains the active kind; `close()` clears both. `update_selection(index, query)` and `set_subtitle(text)` publish narrow changes. `replace_preserving_selection(view, index, query)` preserves the prior row's value identity when available and applies the existing selection clamp. `kind()`, `view()` and `selected_item(index)` expose immutable reads; item pointers expire on mutation. `visible_values(query)` preserves the previous exclusion of disabled/empty-value rows. No writable kind/view getter exists.

The host borrows the session's earlier-constructed presentation snapshot and publishes a copy after each operation. The authoritative view never comes from that snapshot. The host is noncopyable and outlives stash, search, prompts, actions, workspace, active run and submission consumers. Actions expose the shared encapsulated host to the existing active-run overview/session-transition helpers; this replaces the old writable enum accessor without exposing host fields. The frontend/backend authority callbacks retain their access and behavior.

### Integrated consumers and projection exceptions

- Session settings/branch-summary/generic selector paths, action selectors, slash-command settings/hotkeys, stash and transcript search now call host operations. The old session selected-item and visible-values forwarding methods were deleted; selection identity preservation is implemented by the host. Common event classification and feature policy remain in their existing owners pending04/02–04 and05/01.
- Startup overview helpers, committed-session transition helpers and prompt preemption use the same host. Prompt preemption retains its existing Overview/PromptStash-only policy. `dispatch_tui_selector_authority` now closes the host before painting pending status and invoking synchronous authority, so a hidden hosted view cannot survive that boundary.
- Subagent workspace uses the same host and the explicit `SubagentWorkspace` kind. Its active query/selection is read from the host. The private `selector_` remains a rebuild/return checkpoint for workspace inspection and cycling, refreshed from the host before use while selector mode is active. Closed workspace cleanup closes only its own hosted selector, leaving unrelated selector cleanup to its owning feature or the committed-session coordinator. This prevents workspace reset from erasing the Overview kind before overview-specific cleanup.
- Transcript search builds or copies a local view, updates rows with the existing incremental diagnostics, then publishes it in one host operation. Opening search cannot reuse another selector's chrome merely because a projection exists. The search query/projection cache remains feature data for05/02; the host owns the currently displayed query/index.
- The existing `ScopedTranscriptSearchModalProjection` temporarily suppresses/restores only the rendering projection while computing underlying transcript layout. It never closes or mutates the host. Composer layout helpers also modify local snapshot copies only. These are documented rendering projections, not independently writable selector authority;05/02 will reconcile the renderer seam and11 audits remaining writes.

No unused wrapper, stub implementation, optional workspace host, or legacy enum constructor was added. Test source consumers are adapted to construct one shared host per fixture, seed it through `open` and inspect `kind`; assertions retain their original intent. This is compilation compatibility, not a test run.

### Printing, remaining work and checks

SelectorHost deliberately uses `AVA_DEBUG_PRINT_MEMBERS_OPT_OUT`: hosted rows may contain stashed prompt bodies or transcript excerpts, and the borrowed rendering snapshot contains arbitrary messages. The default ON marker was attempted, but its generated printer tries to stream those deliberately non-printable aggregates. Regenerating did not supply a safe printer; content printing was not added to work around that boundary. This is the specific sensitive-data exception, recorded in the header. All host fields are private, the cpp starts with sys.h, and accepted API-contract assertions explain the caller correction.

| Remaining coupling | Removal/integration goal |
| --- | --- |
| Settings navigation, preview and open lifetime still in session | 04/02 |
| Session selection/archive policy still in session/actions; host only owns generic selection identity | 04/03 |
| Branch-summary operation state still in session, including prior selector checkpoint | 04/04 |
| Generic selector input classification and dispatch chain remain in session; no mutable selector aliases remain | 05/01 |
| Search modal projection suppression and query/projection cache remain in existing search/renderer seams | 05/02 |
| Workspace rebuild/return checkpoint and presentation lifecycle remain in existing workspace controller | 05/03 |
| Existing actions-to-host collaborator accessor used by active-run transition helpers | 09–10 integration, final ownership audit11 |

Full configured build, formatting and diff-check evidence is recorded below before commit. No CTest, UI smoke, manual reconfiguration or unrelated configuration workaround is part of this goal.


Build sequence for04/01:

- `source /workspace/ava-tools/activate.sh; scripts/build.sh` completed with exit0, including ava_tests linkage. Normal CMake regeneration for SelectorHost.cpp succeeded; no manual reconfiguration. This run exposed GCC16 warnings from whole-optional projection assignment; publication now explicitly copies a present view or resets an absent projection.
- The new header was staged before `scripts/build.sh --target generate-print-members` so the tracked-source generator included it. The subsequent full build failed in the newly generated ON printer because ComposerSnapshot is deliberately non-streamable. Per repository instructions, the generate-print-members target was retried first and succeeded; the sensitive-content exception above was then applied and printers regenerated successfully. No compiler/configuration workaround was introduced.
- Final full build uses the same activation/script with cloud execution access enabled, logged in `/workspace/ava-tools/goal0401-build-verified.log`. Its result is recorded below after wrapper completion.
- Production C++ was formatted with clang-format22; test API adaptations were formatted on changed ranges with the same formatter. `git diff --check` passed. No tests or runtime/TUI smoke were executed.

- Final full build exit0, all34 scheduled steps completed including ava_tests linkage; no warnings/errors in the final log. Compilation and projection-interface integration are verified; runtime behavior remains untested until the planned final gate.


## 06/04/02 — Settings dialog lifetime and preview ownership

Inputs fully read: handoff, inventory, architecture, current migration ledger including04/01 interfaces, and04/02 description. `SettingsDialog.h/.cpp` now own private navigation/root checkpoint, preview baseline/overlay and open lifetime. Session no longer retains those fields or their settings algorithms. The dialog borrows earlier-lived frontend, presentation, SelectorHost and terminal Context. One explicit TuiFrontEnd friendship grants existing protected settings callbacks; backend persistence/trust authority stays external.

`open()` reads popup-local trust, begins one baseline and builds root rows. `highlight(index, query)` updates the host and reversible preview without persistence. `cancel()` restores root query/index from a section (also when the root checkpoint is absent), or closes at root. `resolve(input)` owns section navigation and authoritative confirmation: it copies the selected action before any close/rebuild, clears preview before exactly one callback, rebases after success/failure, preserves the local display section on save, and keeps errors open with transient status. Cross-feature actions return `variant<SettingsDialogRequest, string>`: next-selector requests or an owned draft command. Session coordinates actions/composer mutations without giving the dialog a mutable draft/action reference bundle. `/settings` submission now calls this same owner.

`close()` closes only a Settings host view, then retires preview/navigation. `synchronize_selector()` retires an old preview after another feature replaced the view without closing the replacement. Destructor cleanup restores the baseline and clears global theme preview while presentation/context remain alive; cleanup is idempotent. The former session-level theme cleanup moved into this owner. Settings is declared after SelectorHost and before consumers; its references survive destruction.

Display reload remains explicit orchestration: action-controller Applied result -> `rebase_after_display_reload()` -> one renderer paint. Rebase captures query/index/exact action/staged action, reapplies hydrated authority, rebuilds rows from authoritative image/cursor values with theme overlay temporarily suppressed, restores action identity, and reapplies highlight. Value equality never substitutes for Applied. Existing last-known-good custom-theme fallback is retained in the preview calculation helper.

The duplicate generic Settings Resolve arms were removed: inside the enclosing hosted-view gate, the earlier Settings Cancel/Resolve branches consume those same inputs before generic Resolve/Cancel can run. No distinct reachable behavior was removed. The older redundant in-display check inside the non-display success arm remains for final obsolete-glue review11; it is not new feature state.

Printing is a specific sensitive-data exception: the dialog retains the full presentation and selector aggregates containing arbitrary transcript/stash content, which deliberately have no safe generated content printer. It uses documented OPT_OUT rather than exposing that content. New fields are private, files match the class and cpp begins with sys.h. No empty stub or mutable state getter was added. The sole submit test fixture was adapted to construct a shared dialog against the Application-owned Context; assertions remain unchanged, and no tests were run.

Remaining cross-feature draft application/selector dispatch is composition work for05/01; generic preview value helpers and their existing test seams remain for final11 audit. Session-selector and branch-summary extraction remain04/03–04. No terminal-thread, signal or RPC policy changed.

Build: staged new headers, then activation + `scripts/build.sh --target generate-print-members` succeeded. Full `scripts/build.sh` (configured BetaTest/EnableDebugON/LibcwdON/testsON, GCC16.2, four jobs, cloud execution access) is recorded below after completion. No manual reconfigure or tests. Changed C++ formatted with clang-format22; diff-check is the commit gate.

- First full compile caught an unused submit sidebar alias after settings extraction; it was removed. Final full build exit0, all34 scheduled steps including ava_tests linkage, log `/workspace/ava-tools/goal0402-build-final.log`. No warnings/errors in the final log. Diff-check passed; no behavioral checks run.


## 06/04/03 — Session selector and committed transition coordination

Inputs consumed: the fully read handoff/inventory/architecture, current ledger including04/01–02, and04/03 description. `SessionSelector.h/.cpp` privately own archive/unarchive confirmation and the session-browsing operations. `open()` publishes the catalog, resets confirmation and requests rendering; `browse(input)` preserves query/selected value through sort/filter/display rebuilds; `archive(input)` retains exact session/action matching, protects current-session archive, handles unavailable rows and preserves existing feedback. `cancel_confirmation()` is the explicit competing-input/selector operation replacing optional-state references. `draft_command(input)` returns an owning rename/labels command; session composition applies it to the composer.

`resolve(input)` and `navigate_branch(input)` copy selected identity before host close, paint pending authority state, and call existing frontend callbacks synchronously using bound owned values. They return callback results; invalid branch selection yields no attempted operation. Session composition separately reads the frontend's committed identity on both success and error, refreshing retained data only on the existing success path. A result string is never treated as session authority. SessionSelector has no transition-owner or mutable draft bundle. Its explicit TuiFrontEnd friendship preserves protected backend callbacks without widening them. The old action-controller open-session forwarder was removed; submission and session hotkeys call this feature directly, preserving Escape reset and render-failure handling.

Architecture was explicitly reconciled to introduce `SessionTransitionCoordinator.h/.cpp` as a focused composition owner, held by TuiSession after workspace/search/renderer and before active-run/submit. Its private references target longer-lived collaborators; consumers pass only committed identity to `apply(id)` or frontend+identity to `apply_and_refresh(frontend,id)`. It has no state accessors. This avoids duplicate transition sequencing and a permanent legacy test adapter. Both old free transition helpers were deleted, and fixture composition uses the same coordinator.

Transition order is preserved through private presentation phases, accessible only to the coordinator: compare identity and clear grants/attachments; reset search/workspace/draft/renderer; reset transcript/tool-index/prompt/sidebar/input/feedback projection and popup overview when appropriate; close selector; clear sidebar records and commit local identity. Same-ID apply returns false before any reset; apply-and-refresh still refreshes retained data for the same identity. ActiveRun continues reading committed ID and boundary transports after worker join and before the UI lock, then calls `apply` under that existing lock and applies already-read transports. Old-session event/receipt discard and quit semantics are unchanged.

SessionSelector and coordinator use specific documented OPT_OUT boundaries: retained presentation/catalog/draft/renderer aggregates contain arbitrary message text, attachment and permission data with no safe generated content printer. No unsafe content printers were added. All new members are private, cpp files start with sys.h, and frontend/context/collaborator lifetimes are explicit. New headers were staged before debug metadata generation. Test constructor/helper adaptation retains all assertions; no tests were run.

Remaining planned work: branch-summary checkpoint/phase/exit ownership04/04; composer draft application and generic/scoped selector dispatch05/01; renderer scroll/caches and search/workspace resets05/02–03; active-run service lifetime09–10. No stub or unused wrapper was introduced. Existing action-to-host accessor remains only for prompt/overview collaboration pending09–11, not transition bundles.

Checks: activation + configured `scripts/build.sh --target generate-print-members` passed with normal CMake regeneration. Full four-job BetaTest/debug/tests-enabled build result follows below. Changed C++ formatted with clang-format22 and diff-check passed. No manual reconfigure, tests, terminal-policy or RPC changes.

- Full build exit0, all60 scheduled steps including ava_tests linkage (`/workspace/ava-tools/goal0403-build.log`); no warnings/errors. Compilation verified, behavior untested.
