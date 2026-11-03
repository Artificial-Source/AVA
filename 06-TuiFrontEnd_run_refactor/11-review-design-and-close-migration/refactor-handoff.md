# Issue #93 review handoff

This is the current implementation and validation handoff for
`astra/06-TuiFrontEnd_run_refactor`. It supersedes the pre-review handoff at
`ab5189c`. The comparison baseline is `ec4143e`. The review was copied unchanged
and committed first as `0ff46d60`; see
[the review](../../06-TuiFrontEnd_run_refactor/review-astra-ab5189c.md).
No pull request was opened.

For a newcomer, start with [TUI architecture](../../docs/engineering/tui-architecture.md).
That guide describes ownership, loop turns, threads, prompts, plugins and how to
add keys, selectors and timers. The earlier
[migration ledger](../03-introduce-session-composition/migration-ledger.md) separates current conclusions from
historical implementation checkpoints. Older 06 artifacts are history where
they disagree with current source or the review.

## Decisions and boundaries

The three maintainer decisions are implemented: keep Runtime class names,
replace writable screen/sidebar access with named feature publication and const
views, and retain the 100 ms control timer as a plain wake. No new decision is
needed to complete this review. The control interval is nominal; synchronous
callbacks and per-read decoding can delay observation. Escape continuation uses
100 ms by default, paste continuation one second. There is no 50 ms guarantee.

Terminal-thread assertions, ncurses thread ownership, signal-handler policy,
utils, RPC and backend permissions/session/provider/process authority are
unchanged. Mermaid gained completion notification plumbing, without moving its
worker or process authority into the TUI.

## Current ownership and ordering

`TuiFrontEnd::run` owns startup policy and outer terminal guards. It constructs
one stable `TuiSession`; frontend and Context outlive it. Session members are
ordered by dependency. Features and their timer handles are destroyed before
the collaborators they borrow. Session owns composition and transitions, while
Composer, selectors, SettingsDialog, CommandOutput and ActiveRunInput own their
feature algorithms. Existing frontend action callbacks form the public action
boundary; protected configuration storage is not exposed through friendship.

RuntimePresentationState owns screen/sidebar projections. Features publish
coherent changes through named operations; readers borrow const views only on
the TUI thread. SelectorHost owns its kind and view; selection-only changes do
not copy all rows. RuntimeRenderer owns mutable viewport, caches and scheduling;
its public transcript view is const. Completion matching is a const calculation
on prepared input and caches, with no Composer/navigation ownership cycle.

A normal turn is control → due timers → application updates → input → paint →
wait. Live updates have a 256-event budget; input has a 32-result budget including
discarded protocol replies. A full batch stays runnable with ReadyOnly; exhausted
work uses Block. Painting checks current time and consumes its frame deadline;
there is no second timer or application pass after painting. Handled signals
consume retained input in both idle and active paths. Known HUP/error closes
admission, drains buffered input without dispatch and exits without another wait.

ActiveRun owns an optional Operation with worker, inbox, reducer and request
capabilities. Ordinary keys belong to ActiveRunInput. A published terminal
result lets complete retire prompt/plugin callbacks, join, authorize receipts,
apply committed identity, finish queues, retire inbox admission, discard stale
session events and drain accepted final events before settlement in the same
call. Final draining uses chunks of 256 but can lengthen one turn. There is no
Finalizing phase or redundant terminal-published flag. Inbox owns the single
queue mutex; the TUI-only UI mutex has been removed.

A visible host prompt freezes spinner/provider projection and holds plugin UI
until its claim retires. A claim keeps worker-borrowing callbacks valid until
retirement, even if cancellation is already recorded. Failed deferred painting
answers the active prompt with Io before general cleanup. Shutdown continues
through independent guarded cleanup failures and records bounded diagnostics.

Timers never decode input, paint, claim signals or recursively service the loop.
The control callback is empty. Spinner repeats at 120 ms only while appropriate;
frames coalesce at 16 ms; branch, workspace and display discovery keep their
250/150/500 ms periods. Mermaid completion uses shared readiness plus weak wake;
only Busy work schedules a retry, starting at 100 ms and capped at 250 ms. A copied
notifier may survive unregistration, but captures no presentation owner.

TuiWakeup uses generated printing without locking its weak pointer. Reference
printers expose type/address, never borrowed contents. TuiWaitState opts out
because utils EventLoop has no printer. Its shared producer pin protects waiter
resources only; it neither joins workers nor protects other borrowed objects.
The ineffective disable flag is gone.

## Finding status

Every implementation group was built, formatted, checked, pushed and reviewed
by an independent agent. The latest test group passed independent review.
The documentation group also passed independent review. The validation update
below reports the executed final comparison.

| Finding | Status | Commit | Result |
| --- | --- | --- | --- |
| B1 | Fixed | `74f5209f` | Hold plugin UI while the host prompt owns input. |
| B2 | Fixed | `c5b53a93` | Charge discarded native input to the budget and keep draining. |
| B3 | Fixed | `ed44489e` | Use one six-stage turn; paint consumes its frame deadline. |
| B4 | Fixed | `996918eb` | Keep the maintainer-approved 100 ms wake and document latency. |
| B5 | Fixed | `cd62a2f1` | Use completion notifications and bounded Busy retries for Mermaid. |
| B6 | Fixed | `996918eb` | Make the control timer callback a plain wake. |
| B7 | Fixed | `489ef88c` | Complete a failed visible prompt with Io before shutdown. |
| B8 | Fixed | `89b25738` | Guard independent cleanup steps and report failures. |
| B9 | Fixed | `93ffdcb9` | Remove the duplicate claim and retire callbacks before worker release. |
| B10 | Fixed | `2babd336` | Restore preview cancellation, retained-key consumption, prompt freeze, buffered close input and interrupted audit behavior; avoid selector row copies. |
| D1 | Fixed | `1bc3c6b9` | Named feature publication plus const screen/sidebar views. |
| D2 | Fixed | `1bc3c6b9` | Const transcript view; renderer owns semantic mutations. |
| D3 | Fixed | `7ad1c02f` | Separate ActiveRunInput, private optional Operation and same-call completion. |
| D4 | Fixed | `1bc3c6b9`, `baced10b` | Move editing/selector/output algorithms into their owners; document dispatch order. |
| D5 | Fixed | `6051b1c3`, `1bc3c6b9` | Remove feature friendships; expose existing action callbacks and invariant transition operations. |
| D6 | Fixed | `519b78d7` | Encapsulate synchronized plugin transport state. |
| D7 | Fixed | `afecb8c7`, `1898d64d` | Safe reference/weak printing and honest remaining opt-outs. |
| D8 | Fixed | `6640df77` | Separate view helper files, final noncopyable Composer, shared PendingRequest template. |
| D9 | Fixed | `2f5b7b52` | Delete unused APIs, duplicate synchronization and test-only modes; remove Composer/navigation cycle. |
| D10 | Fixed | `325b618f` | Delete ineffective disable gate; explain actual shared-pin lifetime. |
| C | Fixed | `711f4a83` | Document files, classes, members, mechanisms and extension paths. |
| T1 | Fixed | `7d6c5990` | Seed a Canceled pending card through the production-used merge helper. |
| T2 | Fixed | `7d6c5990` | Verify real provider updates precede buffered input at a frame deadline. |
| T3 | Fixed | `7d6c5990` | Wait on publication predicates and real notifications, with a bounded timeout. |
| T4 | Fixed | `7d6c5990` | Exercise actual session budgets, wait policy, closing, signals and notification failure. |
| T5 | Fixed | `7d6c5990` | Direct owner tests plus loop, plugin/prompt and timer-contract coverage from earlier fixes. |

B4 is fixed according to the maintainer's explicit decision; no direct signal
wake was added. B10 includes intentional baseline corrections requested by the
review. Other behavior follows the baseline. No unresolved instruction conflict
required a different behavior choice.

## Member comment audit

Compared with ec4143e, the final source has 54 new classes/structs, including
nested values and test fixtures. The table counts declared functions, data,
nested types and enum values; it excludes debug marker macros and duplicate
out-of-line definitions. Each declaration has a mechanism, thread or lifetime
comment, interpreted with the enclosing class contract. Counts do not substitute
for review of whether the explanation is true. Substantially changed existing
owners were also documented and independently reviewed.

| New class or struct | Commented / total members | Use |
| --- | --- | --- |
| `MemberReference` | 3/3 | Production |
| `ActiveRun` | 54/54 | Production |
| `ActiveRun::Operation` | 12/12 | Production |
| `ActiveRunEvents` | 29/29 | Production |
| `ActiveRunInbox` | 31/31 | Production |
| `ActiveRunInput` | 35/35 | Production |
| `ActiveRunOutcome` | 11/11 | Production |
| `BranchSummaryOperation` | 30/30 | Production |
| `CommandOutput` | 3/3 | Production |
| `ComposerInputResult` | 4/4 | Production |
| `Composer` | 81/81 | Production |
| `PendingAttachments` | 9/9 | Production |
| `PendingRequest` | 16/16 | Production |
| `SelectorDispatchResult` | 5/5 | Production |
| `SelectorHost` | 17/17 | Production |
| `SelectorInputController` | 12/12 | Production |
| `SessionSelector` | 17/17 | Production |
| `SessionTransitionCoordinator` | 13/13 | Production |
| `DraftCommand` | 2/2 | Production |
| `SettingsDialog` | 24/24 | Production |
| `Spinner` | 12/12 | Production |
| `TranscriptUpdate` | 5/5 | Production |
| `TranscriptView` | 50/50 | Production |
| `TuiEventLoop` | 29/29 | Production |
| `TuiInputDispatcher` | 6/6 | Production |
| `TuiSession` | 65/65 | Production |
| `TuiTimer` | 16/16 | Production |
| `TuiTimerRegistration` | 15/15 | Production |
| `TuiWaitState` | 13/13 | Production |
| `TuiWakeup` | 4/4 | Production |
| `InputPollResult` | 2/2 | Production |
| `ComposerDispatchFixture` | 8/8 | Test fixture |
| `EventLoopFixture` | 5/5 | Test fixture |
| `PromptPublicationDeadline` | 2/2 | Test fixture |
| `EventLoopPipe` | 9/9 | Test fixture |
| `TimerRecorder` | 3/3 | Test fixture |
| `SpinnerFrames` | 3/3 | Test fixture |
| `MutatingTimerPair` | 12/12 | Test fixture |
| `RearmingTimer` | 9/9 | Test fixture |
| `FeatureOwnerFixture` | 17/17 | Test fixture |
| `B5BridgeState` | 7/7 | Test fixture |
| `B5AttachedMermaidFixture` | 12/12 | Test fixture |
| `RuntimeOwnerPipe` | 6/6 | Test fixture |
| `WakeDeadline` | 3/3 | Test fixture |
| `Turn` | 7/7 | Test fixture |
| `SessionProbe` | 19/19 | Test fixture |
| `ProbeScope` | 5/5 | Test fixture |
| `StreamRedirect` | 8/8 | Test fixture |
| `SessionInput` | 5/5 | Test fixture |
| `SessionSignals` | 3/3 | Test fixture |
| `WorkerGate` | 10/10 | Test fixture |
| `ReleaseWorker` | 3/3 | Test fixture |
| `TranscriptViewFixture` | 5/5 | Test fixture |
| `WaitStatePipe` | 11/11 | Test fixture |

## Validation

The existing development build uses BetaTest, EnableDebug=ON, GCC 16 and four
jobs. Every review commit compiles with `scripts/build.sh`. The baseline has
also been built in `/workspace/ava-base` with the same configuration. Tests use
`LANG=C.UTF-8 LC_ALL=C.UTF-8` and `/workspace/ava-tools/activate.sh`.

Focused tests passed after each group. The final T1–T5 group has passing results
for all 13 selected CTests, including nine isolated actual Session cases. Two
new fixture mistakes were corrected before commit: the canonical default
reasoning label is `unknown`, and the prompt-paint test must inject the checked
flush error because ncurses can consume a `/dev/full` write error before that
flush. Existing test expectations were not weakened.

The Session tests call real production stages through test-only GNU linker
wrappers. They observe ordering and use actual queues, input decoding and waits;
only scoped I/O faults and boundary synchronization are injected. These tests
are not a substitute for terminal smoke coverage. The direct WaitState tests
make no latency or wake-cause claim.

Documentation/printer checks passed in the C group (11 passes, one Linux package
skip because debug/libcwd builds are not distributable). Logs live under
`/workspace/ava-tools/review-*.log`; these workspace logs are evidence, not portable
repository dependencies.

### Final comparison in this cloud environment

The final code at `7e5d9b43` and baseline `ec4143e` were each tested once with the
full suite, then once with the opt-in tmux suite. Later commits only record these
results. Both development builds pass. The exact failure/skip sets, added tests,
configuration and evidence paths are saved in [review-validation.json](review-validation.json).

| Run | Passed | Failed | Skipped | Total |
| --- | ---: | ---: | ---: | ---: |
| Reviewed branch, full suite | 165 | 46 | 31 | 242 |
| Local ec4143e, full suite | 154 | 46 | 31 | 231 |
| Reviewed branch, tmux enabled | 0 | 23 | 0 | 23 |
| Local ec4143e, tmux enabled | 0 | 23 | 0 | 23 |

Full-suite failure and skip sets match exactly. All 11 added CTests pass: the
loop/request suites plus nine isolated Session cases. There are no new failing
CTest names in this comparison. That is evidence against an added failure, not
proof that an existing failure cannot mask a regression.

The full runners exited 8. Of their failed entries, 31 report missing procfs
child-enumeration files; the isolated-environment check explicitly reports
missing `librhash.so.1`. Other failures include helper startup, readiness and
assertion failures. Matching the baseline does not prove a single cause for all
46 failures, so they remain failures in this report.

Both tmux runs fail before a usable UI. Suspend/resume captures explicitly show
missing `libdw.so.1`; the remaining cases report early pane exit. No socket-path
length error occurred. CTest recorded 23 failures on each build, and each runner
then exited 2 because an unreaped zombie client kept its worker process group
visible. Both stale locks were removed only after the runner PIDs were absent,
the remaining group members were zombies, and no live test/build workers
remained. No runner policy or test was changed.

The reviewer's seven environment failures and 23 passing tmux scenarios were
not reproduced in this cloud environment. These local tmux results cannot
validate terminal behavior; a runtime-compatible environment is still needed
for that evidence. No environment failures were repaired as part of this review.

Commands, after activation and UTF-8 locale setup, were:

```sh
# In /workspace/AVA:
scripts/run-tests.sh --output-junit /workspace/ava-tools/review-final-full.xml
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --jobs 4 -R '^ava_tui\.tmux_smoke_' \
  --output-junit /workspace/ava-tools/review-final-tmux.xml

# In /workspace/ava-base:
scripts/run-tests.sh --output-junit /workspace/ava-tools/review-baseline-full.xml
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --jobs 4 -R '^ava_tui\.tmux_smoke_' \
  --output-junit /workspace/ava-tools/review-baseline-tmux.xml
```

Current/baseline full runs took
71.96/67.33 seconds; tmux took 8.14/8.18 seconds. No full-suite or tmux rerun was
used to replace these outcomes.

## Historical validation before this review

The original goal-11 run configured 233 tests: 114 passed, 88 failed and 31
skipped. Subsequent setup corrections and focused reruns cleared the documentation
link failure and 40 terminal failures, leaving 47 without a passing follow-up
at ab5189c. Those are historical observations, not the result of this review.
The ledger retains their commands and causes; the complete original per-test
table is preserved in the ab5189c version of this handoff.

The reviewer separately observed the same seven environment failures on ec4143e
and ab5189c, with all 23 tmux scenarios passing on each. Because this cloud
environment differs, the final comparison must use a local ec4143e build too.

## Remaining limits and newcomer questions

No maintainer decision is blocking. Three places still require careful reading:

1. Receipt authority differs from event arrival. A command's local tool output
   can be visible without entering the conversation transcript. Read
   ActiveRunEvents with the completion sequence and its tests before changing it.
2. A prompt result and release of its UI claim are separate events. A worker may
   remain blocked after a result exists because a callback still borrows its
   request. PendingRequest and coordinator comments explain that ordering.
3. Screen, cached layout and drawn selection are distinct views. Detached
   scrolling deliberately preserves old drawn authority while updates accumulate.
   Read TranscriptView with renderer preparation before changing selection logic.

Preserved limits found while documenting code: automatic reuse of a session
permission grant retains the baseline render-result handling; it has no visible
prompt claim. The pre-existing noexcept Mermaid bridge drain can terminate on
allocation failure, and identity wrap skips zero without a collision proof.
Those are outside the requested findings and were not silently changed. The
100 ms control cadence and finite final backlog also do not bound wall time.
