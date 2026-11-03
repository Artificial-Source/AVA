# Issue93 — completed refactor and validation handoff

Repository: Artificial-Source/AVA. Work branch: `astra/06-TuiFrontEnd_run_refactor`, based on plan branch `06-TuiFrontEnd_run_refactor` at `ec4143e98e8f23cfa6a30852b3d4871204ed07a4`. Only the work branch is pushed; no PR is opened. The numbered plan is executed in order. User authorization permits agent-selected decisions at02/07; no personal maintainer approval is claimed. Full runtime tests were deliberately deferred until after11 was committed and pushed; actual results are recorded below.

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
|11 final ownership audit/cleanup | `d0a18fdf` |

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

Goal11 staged-header generation exited0 with normal CMake regeneration; the subsequent full configured `scripts/build.sh` exited0 after58 scheduled steps, including ava and ava_tests linkage, with no compiler warnings/errors. Logs: `/workspace/ava-tools/goal11-generate.log` and `goal11-build.log`. All final source changes were included. Formatting uses clang-format22 and diff-check. No CTest ran before the11 checkpoint was pushed. Compiled regression coverage includes timer cancellation/generation/rearm, wake coalescing/HUP, pending-request retirement, prompt Escape ordering, plugin deadline cancellation, receipt boundaries, attached painting and display timer discovery; compilation alone does not validate those assertions.

The cloud setup provides an unprivileged signed Debian tool cache and activation/install instructions under `/workspace/ava-tools`; the saved cloud environment configuration is an unpublished review draft (`cecfgdraft_7b7bce3a8e288199bbe265f478f08f07`). Fresh-task restoration was not tested. This environment completed the builds; no new production dependency was added.

## Runtime/test results

The required full run executed exactly once after `d0a18fdf` was pushed:

```sh
source /workspace/ava-tools/activate.sh
scripts/run-tests.sh
```

Result: exit8,233 configured tests,114 passed,88 failed,31 skipped,63.01s wall time, four jobs. Log: `/workspace/ava-tools/refactor-full-tests.log`. These are the original full-run counts; they are not rewritten after the focused correction below. CTest's displayed62% includes skipped tests; this guide separates them explicitly.

The new `ava_tests.tui_event_loop` (4.01s) and `ava_tests.tui_pending_requests` (0.16s) passed. Generated-printing coverage/format, ASSERT comments, module/frontend state boundaries and the other source guards passed except the stale Markdown target below. The complete original outcome ledger follows, so passed checks, failed checks and skipped behavior can be distinguished without access to the workspace log.

### Clear refactor-caused failure and correction

`ava_tests.markdown_links_source` found `docs/engineering/backend-current-state.md` linking the removed `runtime_active_run_internal.cpp`. Updated that link to `ActiveRun.cpp` and reconciled the nearby cancellation-owner/prompt wording. This is the only failure clearly and simply attributable to this refactor from the available evidence. No runtime assertion or test expectation was weakened.

Focused verification only:

```sh
scripts/run-tests.sh -R '^ava_tests\.markdown_links_source$'
```

Passed1/1, exit0,0.41s (`/workspace/ava-tools/refactor-link-check.log`). The full suite was not repeated. The final docs-only `scripts/build.sh` also exited0 (`/workspace/ava-tools/refactor-final-build.log`); the final record check after all environment follow-ups exited0 with no work required (`refactor-final-record-build.log`). The separate terminal setup corrections below clear40 more original failures. Across the focused follow-ups,41 original failures have a passing result;47 remain unresolved/unverified. No new full-suite total is claimed.

### Other failures and evidence limits

- **Sanitized subprocess loader failures.** The unprivileged tool cache works through activation's LD_LIBRARY_PATH, but security-preserving child environments omit that variable. Full logs explicitly report missing `libdw.so.1` for ava/benchmark helpers and exit127 for fake helpers. Read-only `env -u LD_LIBRARY_PATH ldd` confirms fake MCP/plugin/curl/clipboard executables cannot resolve libdw; their RUNPATH contains cached libcwd only. Sanitized CMake also cannot resolve `librhash.so.1`/`libuv.so.1`. The environment setup therefore supports compilation but is incomplete for these runtime launches. Do not weaken child environment policy to make tests pass.
- **Missing procfs capability.** CLI/benchmark ownership harnesses fail before exercising AVA because `/proc/self/task/<pid>/children` does not exist. Independent fresh-process inspection confirms both that path and `/proc/thread-self/children` are absent despite matching PID/task entries. User-space package installation cannot supply this kernel/procfs capability. This affects many CLI/process/benchmark checks; their cleanup assertions/timeouts are not evidence of a TUI regression by themselves.
- **Terminal initialization failures were resolved through setup corrections.**40 original terminal/TUI checks aborted or segfaulted during initialization, often at unchanged `BasicScreen.cxx:28` or with allocator corruption. The tool cache's dangling libtinfo shared target caused static tinfo to be embedded while shared ncurses also loaded shared tinfo. Actual generated link commands did not reference libtinfo.a directly, so restoring the matching shared targets and relinking corrected resolution without changing CMake configuration.18 remaining fixtures required missing xterm-direct/xterm-16color terminfo; authenticated supplemental data resolved those failures. The focused evidence is below. No terminal/thread assertion or repository runtime source changed.
- `app_command_registry` explicitly reports fake MCP exit127; `app_interactive` fails MCP list/get counts before the helper can run. `app_runtime` misses a gated fake-plugin hook; its helper has the same loader dependency, but that assertion omits stderr, so this attribution remains an inference. LSP/process cleanup and other failures remain listed below rather than silently classified as passing or all proven pre-existing.

The only post-run repository correction is the simple documentation fix. The workspace tool setup was corrected separately as described below. No unrelated backend/process repair, child-environment policy relaxation, CMake reconfiguration or full-suite rerun was performed. Compilation and the named deterministic terminal fixtures are verified; full live-loop runtime equivalence is not. Default-loader runtime availability and a procfs-capable environment are still needed for the remaining validation. The saved onboarding draft records these limitations and is not proof of fresh-task restoration or all subprocess tests.

### Focused terminal follow-up after workspace setup repair

The setup cache contained ncurses development files but lacked their shared-library targets. Read-only inspection verified installed libtinfo6/libncursesw6 and retained libncurses-dev all have version6.5+20250216-2. Restored workspace `libtinfo.so.6` and `libncursesw.so.6` links to those matching system libraries. The generated link commands use the ncurses linker script and contain no explicit libtinfo.a input; no cache/archive workaround was used. Removed only generated `build/ava` and `build/tests/ava_tests`, then `scripts/build.sh` successfully relinked both in two steps. `nm -D` no longer shows the embedded static `_nc_*`/`cur_term` definitions. Log: `/workspace/ava-tools/refactor-ncurses-relink.log`.

First focused command:

```sh
scripts/run-tests.sh -R '^ava_tests\.(terminal_|tui_terminal_|tui_composer)'
```

Result:23/41 passed,18 aborted, exit8,1.67s (`refactor-terminal-focused.log`). The whole composer suite, all nine composer-rendering cases, virtual terminal smokes and geometry/layout checks passed. The18 failures all requested xterm-direct or xterm-16color, which were absent; they were not ignored.

Added pinned `ncurses-term 6.5+20250216-2` to the retained workspace package manifest and extracted its data locally. Debian InRelease signature and package-index hash were verified; package SHA256 is `bd6104ab13bd008b4bd5aaf8740fd9f5168ca5199a72fe1c3e984fe49286fbb6`. Activation now includes the local terminfo directory before existing system directories. No repository dependency declaration or global system package changed. Installation instructions prevent both missing shared targets and missing supplemental terminfo, and the saved onboarding draft was updated successfully (still unpublished).

Reran only the18 remaining fixtures:

```sh
scripts/run-tests.sh -R '^ava_tests\.terminal_(keyboard_input_mode\.|mouse_input_mode\.|color\.(srgb_round_trip|osc4_protocol|mutable_palette|portable_pairs)$)'
```

Passed18/18, exit0,0.53s (`/workspace/ava-tools/refactor-terminfo-focused.log`). Across these two focused runs, all41 selected terminal cases have a passing result, clearing all40 original terminal failures. Together with the documentation correction,47 original failures remain without a passing follow-up. The original full-run ledger below remains unchanged and the full suite was executed only once.

### Skips and future checks

The31 actual skips comprise23 tmux scenarios, four opt-in image/lifecycle/OSC8 checks, live-provider, real-clangd, ACP SDK interop and debug/libcwd-incompatible Linux packaging. Opt-in flags/dependencies were not changed. These are skips, not passes. Future terminal checks should use the existing harness after environment correction: simultaneous buffered input/event floods; HUP during prompt/finalization; signal without characters; editor/suspend restoration; detached selection/autoscroll; preview across reload; branch cancellation and Mermaid queue contention. No blocking design decision remains; the unresolved decision is which compatible environment to use for the remaining runtime validation.

### Complete original full-run outcome ledger

Original failures are retained; the follow-up column distinguishes verified corrections from the 47 unresolved failures. The commands and counts for each focused run are recorded above.

| Test | Original outcome | Follow-up |
| --- | --- | --- |
| `ava_tests.threadsafe_custom_access_lock` | Passed | — |
| `ava_tests.core_mode` | Passed | — |
| `ava_tests.core_utf8` | Passed | — |
| `ava_tests.core_fd_helpers` | Passed | — |
| `ava_tests.test_harness` | Passed | — |
| `ava_tests.diagnostics` | Passed | — |
| `ava_tests.acp` | Failed | Unresolved |
| `ava_tests.session` | Passed | — |
| `ava_tests.session_run_controller` | Passed | — |
| `ava_tests.session_title_coordinator` | Passed | — |
| `ava_tests.branch_summary_coordinator` | Passed | — |
| `ava_tests.subagent_coordinator` | Passed | — |
| `ava_tests.subagent_delivery_manager` | Passed | — |
| `ava_tests.core_json_permission` | Passed | — |
| `ava_tests.curl_transport_process` | Failed | Unresolved |
| `ava_tests.clipboard_image_process` | Failed | Unresolved |
| `ava_tests.app_command_classification` | Passed | — |
| `ava_tests.app_command_registry` | Failed | Unresolved |
| `ava_tests.tools` | Failed | Unresolved |
| `ava_tests.config_context_auth_oauth` | Passed | — |
| `ava_tests.provider_config` | Passed | — |
| `ava_tests.provider_user_catalog` | Passed | — |
| `ava_tests.provider_builtin_generic` | Passed | — |
| `ava_tests.command` | Passed | — |
| `ava_tests.app_compaction` | Passed | — |
| `ava_tests.app_interactive` | Failed | Unresolved |
| `ava_tests.app_print` | Passed | — |
| `ava_tests.app_event_serialization` | Passed | — |
| `ava_tests.app_event_bus` | Passed | — |
| `ava_tests.app_rpc_queue` | Passed | — |
| `ava_tests.app_rpc_resolver` | Passed | — |
| `ava_tests.app_rpc` | Failed | Unresolved |
| `ava_tests.app_runtime` | Failed | Unresolved |
| `ava_tests.provider_openai` | Passed | — |
| `ava_tests.provider_anthropic` | Passed | — |
| `ava_tests.provider_gemini` | Passed | — |
| `ava_tests.provider_live_smoke` | Skipped | — |
| `ava_tests.agent_loop_resilience` | Passed | — |
| `ava_tests.agent_loop` | Passed | — |
| `ava_tests.agent_tool_dispatcher` | Passed | — |
| `ava_tests.agent_todo` | Passed | — |
| `ava_tests.tool_scheduler` | Passed | — |
| `ava_tests.lsp` | Timeout | Unresolved |
| `ava_tests.plugin` | Failed | Unresolved |
| `ava_tests.plugin_runner_process` | Failed | Unresolved |
| `ava_tests.plugin_ui` | Passed | — |
| `ava_tests.process_adoption_posix` | Passed | — |
| `ava_tests.process_capability_posix` | Passed | — |
| `ava_tests.process_deadline` | Passed | — |
| `ava_tests.process_environment` | Passed | — |
| `ava_tests.process_launch_protocol_posix` | Passed | — |
| `ava_tests.process_monitor` | Passed | — |
| `ava_tests.process_readiness` | Passed | — |
| `ava_tests.process_supervisor` | Passed | — |
| `ava_tests.process_supervisor_posix` | Passed | — |
| `ava_tests.mcp` | Failed | Unresolved |
| `ava_tests.mermaid_render` | Failed | Unresolved |
| `ava_tests.permission_rules` | Passed | — |
| `ava_tests.tui_composer` | Aborted | Passed (focused) |
| `ava_tests.tui_event_loop` | Passed | — |
| `ava_tests.tui_pending_requests` | Passed | — |
| `ava_tests.terminal_horizontal_layout` | Aborted | Passed (focused) |
| `ava_tests.terminal_paragraph` | Aborted | Passed (focused) |
| `ava_tests.terminal_window` | Aborted | Passed (focused) |
| `ava_tests.tui_terminal_geometry` | Aborted | Passed (focused) |
| `ava_tests.tui_terminal_mouse_sgr` | Aborted | Passed (focused) |
| `ava_tests.run_observer` | Passed | — |
| `ava_tests.runtime_diagnostics` | Passed | — |
| `ava_tests.containment` | Passed | — |
| `ava_tests.terminal_keyboard_input_mode.escape_delay_default` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.escape_delay_configured` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.kitty_reply` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.fallback_reply` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.modify_other_keys_reply` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.unrelated_input` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.startup_input_replay` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.split_utf8_replay` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.invalid_utf8` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.handoff_input` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.idempotent_stop` | Aborted | Passed (focused) |
| `ava_tests.terminal_keyboard_input_mode.destructor_stop` | Aborted | Passed (focused) |
| `ava_tests.terminal_mouse_input_mode.context_lifecycle` | Aborted | Passed (focused) |
| `ava_tests.terminal_mouse_input_mode.handoff_lifecycle` | Aborted | Passed (focused) |
| `ava_tests.terminal_color.srgb_round_trip` | Aborted | Passed (focused) |
| `ava_tests.terminal_color.osc4_protocol` | Aborted | Passed (focused) |
| `ava_tests.terminal_color.mutable_palette` | Aborted | Passed (focused) |
| `ava_tests.terminal_color.xterm_indexed` | Aborted | Passed (focused) |
| `ava_tests.terminal_color.portable_pairs` | Aborted | Passed (focused) |
| `ava_tests.tui_terminal_virtual_smoke.baseline` | Aborted | Passed (focused) |
| `ava_tests.tui_terminal_virtual_smoke.no_color` | Aborted | Passed (focused) |
| `ava_tests.tui_terminal_virtual_smoke.screen` | Segfault | Passed (focused) |
| `ava_tests.tui_terminal_virtual_smoke.tmux` | Aborted | Passed (focused) |
| `ava_tests.tui_terminal_virtual_smoke.kitty` | Aborted | Passed (focused) |
| `ava_tests.tui_terminal_virtual_smoke.wezterm` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.search_tail` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.changed_session` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.session_transition` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.atomic_search` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.message_navigation` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.detached_completion` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.message_navigation_empty` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.display_reload_poll` | Aborted | Passed (focused) |
| `ava_tests.tui_composer_rendering.display_reload_overview` | Aborted | Passed (focused) |
| `ava_tests.lsp_real_clangd_smoke` | Skipped | — |
| `ava_cli.rpc_broken_pipe_subprocess` | Passed | — |
| `ava_cli.acp_sdk_interop` | Skipped | — |
| `ava_cli.headless_rpc_validate_session` | Passed | — |
| `ava_cli.headless_rpc_contract` | Passed | — |
| `ava_cli.headless_thinking_alias` | Passed | — |
| `ava_cli.headless_rpc_context_export` | Passed | — |
| `ava_cli.headless_print_prompt_overrides` | Failed | Unresolved |
| `ava_cli.headless_print_positional_prompt` | Failed | Unresolved |
| `ava_cli.headless_print_no_session` | Failed | Unresolved |
| `ava_cli.headless_print_session_startup_options` | Failed | Unresolved |
| `ava_cli.headless_print_file_args` | Failed | Unresolved |
| `ava_cli.offline_print_prompt` | Passed | — |
| `ava_cli.package_manager_deferred` | Passed | — |
| `ava_cli.isolated_env` | Failed | Unresolved |
| `ava_cli.headless_mode_json_alias` | Failed | Unresolved |
| `ava_cli.headless_tool_visibility` | Failed | Unresolved |
| `ava_cli.headless_rpc_compact_success` | Failed | Unresolved |
| `ava_cli.headless_rpc_active_run_rejection` | Failed | Unresolved |
| `ava_cli.headless_rpc_permission_reply` | Failed | Unresolved |
| `ava_cli.headless_rpc_permission_grant` | Failed | Unresolved |
| `ava_cli.headless_rpc_permission_grant_lifecycle` | Failed | Unresolved |
| `ava_cli.headless_rpc_question_reply` | Failed | Unresolved |
| `ava_cli.headless_rpc_question_reply_multi` | Failed | Unresolved |
| `ava_cli.headless_rpc_provider_error` | Failed | Unresolved |
| `ava_cli.headless_rpc_tool_failure` | Failed | Unresolved |
| `ava_cli.headless_performance_smoke` | Failed | Unresolved |
| `ava_cli.headless_e2e_model_smoke` | Failed | Unresolved |
| `ava_cli.headless_rpc_bash_process_cleanup` | Failed | Unresolved |
| `ava_cli.headless_rpc_cancel` | Passed | — |
| `ava_cli.headless_rpc_error_recovery` | Passed | — |
| `ava_cli.headless_rpc_mcp_commands` | Passed | — |
| `ava_cli.headless_rpc_command_registry` | Failed | Unresolved |
| `ava_cli.headless_rpc_plugin_commands` | Passed | — |
| `ava_cli.headless_rpc_sample_plugin` | Passed | — |
| `ava_cli.headless_rpc_mcp_config_errors` | Passed | — |
| `ava_build.ctags_json_detection` | Passed | — |
| `ava_tests.process_gate` | Passed | — |
| `ava_tests.test_timing_trace` | Passed | — |
| `ava_tests.fake_provider_owner` | Passed | — |
| `ava_cli.headless_parallel_read_search_tools` | Failed | Unresolved |
| `ava_tests.startup_overview_close_wait` | Passed | — |
| `ava_tests.sidebar_drawer_reflow_wait` | Passed | — |
| `ava_tests.model_selector_wait` | Passed | — |
| `ava_tests.command_output_viewport_wait` | Passed | — |
| `ava_tests.context_report_wait` | Passed | — |
| `ava_tests.terminal_lifecycle_cursor` | Passed | Passed (focused) |
| `ava_tests.sequencing_wait` | Passed | — |
| `ava_benchmark.harness_self_test` | Passed | — |
| `ava_benchmark.smoke` | Failed | Unresolved |
| `ava_benchmark.process_harness_self_test` | Passed | — |
| `ava_tests.benchmark_driver` | Timeout | Unresolved |
| `ava_benchmark.live_git_self_test` | Passed | — |
| `ava_benchmark.historical_artifact` | Passed | — |
| `ava_benchmark.process_smoke` | Failed | Unresolved |
| `ava_debug.generate_print_members` | Passed | — |
| `ava_tests.markdown_link_verifier` | Passed | — |
| `ava_tests.markdown_links_source` | Failed | Passed (focused) |
| `ava_tests.documentation_structure_checker` | Passed | — |
| `ava_tests.documentation_structure_source` | Passed | — |
| `ava_tests.assert_comments_checker` | Passed | — |
| `ava_tests.assert_comments_source` | Passed | — |
| `ava_tests.module_dependency_rules` | Passed | — |
| `ava_tests.module_dependency_rules_self_test` | Passed | — |
| `ava_tests.frontend_state_boundary` | Passed | — |
| `ava_tests.frontend_state_boundary_self_test` | Passed | — |
| `ava_tests.test_timeout_policy` | Passed | — |
| `ava_tests.junit_summary` | Passed | — |
| `ava_build.no_cxx_module_flags_guard` | Passed | — |
| `ava_tests.curl_transport_static_contract` | Passed | — |
| `ava_tests.clipboard_image_static_contract` | Passed | — |
| `ava_tests.curl_transport_static_contract_negative` | Passed | — |
| `ava_tests.clipboard_image_static_contract_negative` | Passed | — |
| `ava_tests.plugin_runner_static_contract` | Passed | — |
| `ava_tests.plugin_runner_static_contract_negative` | Passed | — |
| `ava_tests.mcp_stdio_static_contract` | Passed | — |
| `ava_tests.mcp_stdio_static_contract_negative` | Passed | — |
| `ava_cli.doctor_support` | Failed | Unresolved |
| `ava_cli.runtime_diagnostics` | Failed | Unresolved |
| `ava_build.parallel_test_runner` | Failed | Unresolved |
| `ava_tests.session_read_authority_inventory` | Passed | — |
| `ava_tests.plugin_ui_authority_inventory` | Passed | — |
| `ava_tests.session_reference_inventory` | Passed | — |
| `ava_tests.acp_interop_manifest` | Passed | — |
| `ava_tests.live_acp_dogfood_launcher` | Failed | Unresolved |
| `ava_tests.live_dogfood_launchers` | Failed | Unresolved |
| `ava_tests.acp_support_manifest` | Passed | — |
| `ava_cli.acp_subprocess` | Failed | Unresolved |
| `ava_cli.rpc_parser_subprocess` | Passed | — |
| `ava_cli.interactive_startup` | Failed | Unresolved |
| `ava_cli.interactive_pty` | Failed | Unresolved |
| `ava_tests.rpc_contract_manifest` | Passed | — |
| `ava_tests.rpc_python_client` | Passed | — |
| `ava_cli.rpc_python_client_e2e` | Passed | — |
| `ava_release.provenance` | Passed | — |
| `ava_release.install_component` | Passed | — |
| `ava_release.candidate_contract` | Passed | — |
| `ava_release.publication_dry_run` | Passed | — |
| `ava_release.package_linux` | Skipped | — |
| `ava_release.package_timeout_cleanup` | Failed | Unresolved |
| `ava_tui.tmux_smoke_suspend_resume` | Skipped | — |
| `ava_tui.tmux_smoke_keybind_conflict` | Skipped | — |
| `ava_tui.tmux_smoke_theme_env` | Skipped | — |
| `ava_tui.tmux_smoke_theme_persisted` | Skipped | — |
| `ava_tui.tmux_smoke_nested_settings_preview` | Skipped | — |
| `ava_tui.tmux_smoke_startup_overview` | Skipped | — |
| `ava_tui.tmux_smoke_active_run` | Skipped | — |
| `ava_tui.tmux_smoke_restore_followup` | Skipped | — |
| `ava_tui.tmux_smoke_streaming_scroll` | Skipped | — |
| `ava_tui.tmux_smoke_mermaid` | Skipped | — |
| `ava_tui.tmux_smoke_transcript_search` | Skipped | — |
| `ava_tui.tmux_smoke_transcript_selection` | Skipped | — |
| `ava_tui.tmux_smoke_subagent_workspace` | Skipped | — |
| `ava_tui.tmux_smoke_branch_summary` | Skipped | — |
| `ava_tui.tmux_smoke_main_startup_trust_keybinds` | Skipped | — |
| `ava_tui.tmux_smoke_main_models_selectors` | Skipped | — |
| `ava_tui.tmux_smoke_main_editor_input` | Skipped | — |
| `ava_tui.tmux_smoke_main_slash_completions` | Skipped | — |
| `ava_tui.tmux_smoke_main_permission_flow` | Skipped | — |
| `ava_tui.tmux_smoke_main_question_flow` | Skipped | — |
| `ava_tui.tmux_smoke_main_session_mgmt` | Skipped | — |
| `ava_tui.tmux_smoke_main_paste_scrollback_attach` | Skipped | — |
| `ava_tui.tmux_smoke_plugin_ui` | Skipped | — |
| `ava_tui.kitty_image_smoke` | Skipped | — |
| `ava_tui.iterm2_image_smoke` | Skipped | — |
| `ava_tui.terminal_lifecycle_smoke` | Skipped | — |
| `ava_tui.osc8_smoke` | Skipped | — |
| `ava_debug.print_members_coverage` | Passed | — |
| `ava_debug.print_members_format` | Passed | — |
| `ava_debug.libcwd_output_routing` | Passed | — |
