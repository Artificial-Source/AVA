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
