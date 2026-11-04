# TUI refactor handoff

## Merge notes

Implementation and final self-review are complete through `0d6173b0` on
`astra/06-TuiFrontEnd_run_refactor`. All five review angles pass. Every requested
validation command was run again after those reviews, with no new failures
against the separately built local baseline and no sanitizer diagnostics.

This is not an all-green merge certification. S4 certification remains deferred:
the full instrumented suites retain environment failures, and all 23 tmux
scenarios exit before usable screens because a child cannot load `libdw.so.1`.
R5 was not assigned in the supplied review. No implementation finding remains
open. The branch merges cleanly with the freshly fetched `origin/develop`.

Start with [the TUI architecture guide](../../docs/engineering/tui-architecture.md).
The preserved [second review](../review-astra-d48292c.md) defines R1–R20;
[the first review](../review-astra-ab5189c.md) is background. The original
[maintainer requirements](../handoff.md#maintainer-design-requirements-apply-throughout-this-objective) remain
subject to the explicit final-round decisions recorded below.

### Finding status

“Fixed” means the requested implementation and review are complete. Validation
limits are separate from code correctness; the S4 row explicitly records the
remaining certification gap.

| Finding | Status and commits | Result |
| --- | --- | --- |
| R1 | Fixed — `0d162cfe` | Close-after-submit decodes and discards bytes while its worker finishes; a held-worker session test bounds loop turns. |
| R2 | Fixed — `7104bd58`, `457b9c79`, `ed206ec7`, `0d6173b0` | Correct return-value, timing, ownership and shutdown comments; distinguish scheduling from successful drawing. |
| R3 | Fixed — `f610d6ce`, `f866a139`, `ed206ec7` | Glossary, concrete extension steps, ordinary language, consolidated comments, and final-result names replace ambiguous terminal-result names. |
| R4 | Fixed — `4b2b9914` | Completing a host prompt wakes the loop after the worker can resume, so queued plugin work is reconsidered. |
| R5 | Deferred — no finding supplied | The second review skips R5. There is no identified change to implement. |
| R6 | Fixed — `d8e0d872` | The 100 ms control and 500 ms display timers share one origin and keep their phase when skipping missed ticks. |
| R7 | Fixed — `2903c8dc`, `d866ea6f` | Branch-summary and workspace timers record due work; application servicing performs frontend reads. Workspace retains its 150 ms cadence and prompt eligibility. |
| R8 | Fixed — `f6758348` | Every latched terminal-render failure reaches active-prompt failure before shutdown. |
| R9 | Fixed — `659b20cc`, `e93ac4d8`, `1aec9ceb` | Release failures are logged and latched, then terminate before a potentially blocked join; post-join cleanup errors preserve a nonzero outcome. Returned queue errors are retained too. |
| R10 | Fixed — `86ff8a55` | Queued interruption rejects misuse on a claimed request; claim completion/retirement retains borrowed callback lifetime. |
| R11 | Fixed — `a3c00acb`, `b20acb78` | Hold Mermaid completions and display reloads during host prompts, retain due work, and require an attached completion notifier. |
| R12 | Fixed — `b573ff9a` | Group run/frame/progress publications, publish draft mutations at their owner, and make projection const. |
| R13 | Fixed — `6c71f0e0` | Composer owns phase methods and an ordered edit-action table; active input reuses that policy with its existing precedence. |
| R14 | Fixed — `5544a51c`, `8c4cdf0b` | Five narrow interfaces expose feature actions; concrete backend methods stay protected; keybindings use `apply_key_bindings`. |
| R15 | Fixed — `6617a980`, `b78d9235`, `d93b9b36`, `6dc63489` | Raw pointers print addresses only; safe metadata remains printable, sensitive owned strings are omitted, and `MemberReference` has its own header. |
| R16 | Fixed — `d93b9b36`, `6dc63489`, `be8e3aaa`, `e3ec4ea7`, `9091c2d1`, `52c1339b`, `12108a7b` | Private decoder transitions, typed selector variants, matching result headers, real loop ownership, removal of test-only readers/modes and internal tool merging. Case-only filenames stay by maintainer decision. |
| R17 | Fixed — `5af19561` | Archive travels through `SelectorHost::dispatch` with a nested Settings preview and restores it. |
| R18 | Fixed — `155a018b`, `02f2559d`, `0d162cfe` | Active signal exit 130, retained-key rejection, permission HUP interrupted audit and close-after-submit tests. |
| R19 | Fixed — `5f26e69f` | Bounded waits, predicate gates and fixture timer postponement replace fragile spins or unbounded test waits. |
| R20 | Fixed — `640c4b49` | Seed the live count, make unchanged-workspace timing independent of machine speed, and observe the real control timeout and callback. |
| S1 | Fixed — `155a018b`, `02f2559d`, `f6758348`, `42d8d6c3` | Typeahead quarantine, paste rejection, real prompt/result paints, fail-closed cancellation and unlocked callable copying/retirement. |
| S2 | Fixed — `70bac50d` | Sanitize plugin binding identities in width and visible-text paths; adversarial controls are covered by regression tests. |
| S3 | Fixed — `5ee43632` | Preserve baseline queue limits, invoke Mermaid callbacks outside locks, and reserve result storage before removing completions. Local unbounded queues are reported below. |
| S4 | Deferred — all-green sanitizer-job certification; validation work `a431d68f`, warning fix `c90bb262` | Both instrumented builds and all required runs completed without sanitizer diagnostics. ASan/UBSan retains the baseline 46 failures; the TSan preset retains its known ACP helper failure. Fixing environment policy is outside the allowed scope. |
| S5 | Fixed — `6617a980` | Raw C-string, object, incomplete, array and function pointers cannot print pointed-to content. |

The maintainer explicitly retained the 100 ms control timer, Runtime names,
per-feature screen publication and the Composer/composer filenames. Signals,
terminal-thread assertions, ncurses threading, utils, RPC and backend decisions
were not changed. No production dependency was added. No unresolved instruction
conflict required a new user-visible behavior choice.

### Security checks and limits

The following paths were checked against `ec4143e`; paths without a prefix
below are under `src/ava/tui/`.

| Check | Paths traced |
| --- | --- |
| S1 | `TuiSession.cpp`, `ActiveRun.cpp`, `ActiveRunInbox.cpp`, `PendingRequest.h`, `runtime_prompts_internal.cpp`, `runtime_input.cpp`, `terminal/Context.cxx`, `session_grants.cpp`, `SessionTransitionCoordinator.cpp`; real session, prompt, paste and shutdown regressions. |
| S2 | `runtime_state.cpp`, `composer.cpp`, `composer_transcript.cpp`, `composer_permission.cpp`, `composer_select_list.cpp`, `composer_palette.cpp`, `composer_plugin_ui.cpp`, `tool_cards.cpp`, `command_output.cpp`, `runtime_plugin_ui_internal.cpp`, plus `src/ava/app/terminal_text.cpp`. |
| S3 | `ActiveRunInbox.cpp`, `runtime_state.cpp`, `runtime_prompts_internal.cpp`, `runtime_plugin_ui_internal.cpp`, `runtime_mermaid_internal.cpp`, `runtime_input.cpp`, plus `src/ava/app/mermaid_render_coordinator.cpp` and `src/ava/app/tui/mermaid_tui_bridge.cpp`. |
| Debug output | `src/ava/debug/MemberPointer.h`, `MemberReference.h`, `print_reference.h`, `generate_print_members.py`, generated member selection and `ActiveRun::Operation`'s restricted printer. |

Permission and question paths were traced from `ActiveRunInbox` and resolver
queues through `PendingRequest`, `RuntimePromptCoordinator`, `ActiveRun`,
`TuiSession` and terminal decoding. The checks cover queued and claimed requests,
typeahead before presentation, retained input during acquisition, pasted shortcuts,
automatic session grants, normal answers, Escape, signals, HUP, notification and
render failures, exceptions and shutdown. A visible prompt first drains stale
input; an allow/answer is released only after its required real paint succeeds.
Pending permissions deny or return an interruption/error on exit. A ready
automatic question answer retains baseline precedence during ordinary cancellation;
shutdown retires its callbacks without invoking that automatic answer.
An unrecoverable release failure
terminates the process and may deliver no result; it cannot justify allowing
the operation. Claim retirement precedes releasing a worker
whose stack may contain a callback. Audit callable copying/destruction and question
construction/retirement now happen outside their mutexes. Five regressions failed
on the prior code and pass after the fix, including cancellation racing construction
and last-owner retirement reentering the closed queue. SIGINT's existing draft-clearing policy stays.

Screen paths were traced through feature publications, `CommandOutput`, status
and provider errors, transcript/tool rendering, plugin messages/modals/docks,
selector titles and file/session/config labels. Publication stores owned display
values; the existing terminal-text/render helpers sanitize them at rendering.
The new bypass found was a plugin binding identity used directly for display and
width. Both paths now sanitize it. Tests include SGR, OSC 52 and BEL controls.
Debug tests verify that pointer addresses and safe metadata remain available
without printing strings or invoking pointed-to content printers.

| Queue or buffer | Bound and baseline comparison |
| --- | --- |
| ActiveRunInbox event FIFO | No local item/byte admission cap, unchanged. Drain 256 is a fairness budget, not capacity. One separate final-result slot. |
| Permission and question queues | No local queue cap, unchanged. One active claimed prompt; blocking producers do not establish a local capacity guarantee. |
| Plugin UI | 63 ordinary queued records plus up to 63 in a detached poll batch, with one interactive modal reservation. Validated text is 8 KiB/request, widget lines 32 and displayed widgets four. Baseline limits preserved. |
| Mermaid coordinator | 32 undrained identities across queued/active/completed work; source 64 KiB, accepted result 256 KiB. Cache: 128 sources and 4 MiB accepted output; retained sources have their separate size budget. Detached drain storage is additional. |
| Mermaid TUI | 128 retained records, 4 MiB accepted output, at most 32 pending backend requests. Scanning can temporarily inspect the full transcript. Baseline limits preserved. |
| Startup replay | 64 decoded events. Paste text is at most 1 MiB and stored twice per event, so the event-count cap alone is not a small byte bound. Internal enqueue has no independent byte cap; production decoding supplies it. |
| Decoder | Paste storage 1 MiB per copy, escape sequence 16 KiB. A truncated-paste tail keeps only a discard flag and closing-marker position. Native read 4096 bytes. |
| Keyboard negotiation | Two 50 ms collection phases; no total byte cap, unchanged. |
| Input turn | 32 decoded/discarded results. A result can contain multiple continuation reads; this is not a wall-time or retained-memory bound. |

These are local limits, not a global bound on memory. Adding hard event/prompt
queue limits requires an overflow policy; silently dropping tool results or
permission requests would change behavior beyond this task.

### Validation

Evidence date: 2026-11-04. Environment: GCC 16, `BetaTest`, `EnableDebug=ON`,
UTF-8 locale. No live-provider tests or credentials were enabled. All groups built
through `scripts/build.sh`; header changes first generated print members.
Changed C++ was formatted and diffs checked before commits. clang-tidy and
clang-tidy-22 are not installed, so that optional analysis could not run.

| Check | Final result after self-review |
| --- | --- |
| Dev build at `0d6173b0` | Passed, BetaTest and EnableDebug=ON, no new warnings. |
| Full dev suite | 194 passed, 46 failed, 31 skipped; 271 total, 79.52 seconds. Failure names exactly match local `ec4143e`; no new failed test. |
| Local baseline `ec4143e` | 154 passed, the same 46 failed, 31 skipped; 231 total. |
| Local pre-final-round `d48292c` | 165 passed, the same 46 failed, 31 skipped; 242 total. |
| Reviewer reference | Seven environment failures and 23/23 tmux passes on both baseline and pre-final branch. This cloud environment differs; comparisons use its separately built baseline. |
| tmux | 23/23 scenarios failed before usable screens. Suspend/resume captured missing `libdw.so.1`. Behavioral smoke verification is unavailable here; no child-environment workaround was made. |
| TUI/plugin repeat 20 | 59/59 tests passed every repetition: 1,180 successful executions, 270.16 seconds. |
| ASan/UBSan | Configure and full build passed. Full suite: 196 passed, the baseline 46 failures, 31 skipped; 273 total, 212.83 seconds. No ASan, LeakSanitizer or UBSan diagnostic. |
| TSan | Configure/build passed. Preset: 3 passed, 1 known ACP helper failure. Requested TUI tests: 40/40 passed; additional composer/notification tests: 2/2 passed. No TSan diagnostic. |
| Final self-review | All five angles PASS after correcting every finding. All commands below were rerun afterward. |
| Merge with origin/develop | `0d6173b0` merges cleanly with freshly fetched `d1b6db7b96a9783afe9893e6a6f7c8986dcdd1ee`; throwaway tree `fda5ed82d910571bd6042040e66fee1c158ff81f`. No merge or rebase of this branch. |

The failed subprocess paths are not proven safe by the passing tests. This
machine's existing child environment prevents some helpers from loading
`libdw.so.1`; other failures report unavailable procfs child-accounting paths.
No loader-policy workaround or sanitizer suppression was added. Failure-name
sets match baseline; individual downstream assertion messages can vary.
The tmux runner also retained a lock because PID 1 had not reaped a terminated
client. Manual recovery verified the runner was absent and its process group
contained only a zombie before removing that stale lock.

Required validation commands, with `LANG=C.UTF-8 LC_ALL=C.UTF-8`:

```sh
scripts/build.sh --target generate-print-members
scripts/build.sh
scripts/run-tests.sh
AVA_TUI_TMUX_SMOKE=1 scripts/run-tests.sh --jobs 4 -R '^ava_tui\.tmux_smoke_'
ctest --test-dir build -R '^ava_tests\.(tui_.*|plugin_ui)' --repeat until-fail:20 -j4
cmake --preset sanitize
scripts/build.sh --build-dir build-sanitize --jobs 2
scripts/run-tests.sh --build-dir build-sanitize --jobs 2
cmake --preset tsan
scripts/build.sh --build-dir build-tsan --jobs 2 --target ava_tests
ctest --preset tsan
ctest --test-dir build-tsan -R '^ava_tests\.(tui_event_loop|tui_pending_requests|tui_session\..*|plugin_ui|tui_permission|tui_composer_rendering.*)$'
ctest --test-dir build-tsan -R '^ava_tests\.(tui_composer|mermaid_notifications)$'
```

Both sanitizer runtimes worked. No sanitizer diagnostic was observed in the
runs above, so there are no third-party sanitizer stacks or suppressions to list. TSan compilation exposed
a new GCC 16 initializer-list warning in the yank regression; `c90bb262` removed
the temporary initializer list while preserving both values and all assertions.
The final TSan build emitted one inherited GCC warning:
`pending_attachment_graphic` reports `Iterm2ImageOptions::name` as possibly
uninitialized. Earlier TSan compilation also reported an optional shared pointer
as possibly uninitialized in `bound_read_authority`. The former function body and the latter file are unchanged
from `ec4143e`; both appeared in the first S4 build. They are compiler warnings,
not sanitizer runtime reports. No warning or sanitizer suppression was added.
The final logs were inspected separately. No new compiler warning was found;
compilation and absence of sanitizer reports do not prove unexecuted paths safe.

The following 46 test names failed in local baseline, final dev and final
ASan/UBSan runs. This comparison uses names because test numbers change when
regressions are added. All runs used UTF-8 locale. No failing test was disabled,
weakened or skipped to obtain these results.

```text
ava_benchmark.process_smoke
ava_benchmark.smoke
ava_build.parallel_test_runner
ava_cli.acp_subprocess
ava_cli.doctor_support
ava_cli.headless_e2e_model_smoke
ava_cli.headless_mode_json_alias
ava_cli.headless_parallel_read_search_tools
ava_cli.headless_performance_smoke
ava_cli.headless_print_file_args
ava_cli.headless_print_no_session
ava_cli.headless_print_positional_prompt
ava_cli.headless_print_prompt_overrides
ava_cli.headless_print_session_startup_options
ava_cli.headless_rpc_active_run_rejection
ava_cli.headless_rpc_bash_process_cleanup
ava_cli.headless_rpc_command_registry
ava_cli.headless_rpc_compact_success
ava_cli.headless_rpc_permission_grant
ava_cli.headless_rpc_permission_grant_lifecycle
ava_cli.headless_rpc_permission_reply
ava_cli.headless_rpc_provider_error
ava_cli.headless_rpc_question_reply
ava_cli.headless_rpc_question_reply_multi
ava_cli.headless_rpc_tool_failure
ava_cli.headless_tool_visibility
ava_cli.interactive_startup
ava_cli.isolated_env
ava_cli.runtime_diagnostics
ava_release.package_timeout_cleanup
ava_tests.acp
ava_tests.app_command_registry
ava_tests.app_interactive
ava_tests.app_rpc
ava_tests.app_runtime
ava_tests.benchmark_driver
ava_tests.clipboard_image_process
ava_tests.curl_transport_process
ava_tests.live_acp_dogfood_launcher
ava_tests.live_dogfood_launchers
ava_tests.lsp
ava_tests.mcp
ava_tests.mermaid_render
ava_tests.plugin
ava_tests.plugin_runner_process
ava_tests.tools
```

### Final self-review

Five fresh read-only reviewers inspected the complete `ec4143e..6a2b7d1c`
diff. Each then reviewed the corrections relevant to its angle. Correctness,
security, design and tests passed at `ed206ec7`; readability passed at
`0d6173b0`, whose only additional change corrected one timer comment. Each
correction group also received a separate independent review.

| Angle | Result | What it caught |
| --- | --- | --- |
| Correctness, threads and lifetimes | PASS | Workspace timer performed frontend reads during timer dispatch; `d866ea6f` moves them to application servicing. |
| Security, bounds and debug output | PASS | No new security defect. Verified host-prompt boundaries, sanitized rendering and baseline queue limits. |
| Design and repository rules | PASS | Persistent InputReplay exposed mutable fields and lacked a layout exception; `12108a7b` fixes both. |
| Comments and newcomer readability | PASS | Stale printer/thread/lifetime/attachment descriptions, repeated contracts and per-operator filler; `ed206ec7` corrects them. `0d6173b0` attributes waking to the timer deadline rather than its empty callback. |
| Tests and timing | PASS | Workspace test expected frontend calls during timers; the new regression separates stages, preserves visible assertions and checks coalescing plus open/close reset. |

Earlier group reviews caught unsafe default termination diagnostics, an
explicit-only death test accidentally registered in an aggregate, missing
marker inclusion, uninitialized diagnostic metadata, stale attachment comments
and public test-helper state. Each was corrected and reviewed again.

### Size and language measurements

Measurements use `d48292c` before the final round and current source after it.
Term counts are case-insensitive whole words across `src/ava/tui` .h/.cpp/.cxx
files, including identifiers when they match a whole word. Setter counts are
`publish_*` bodies containing one assignment statement. Public method counts
include overloads, exclude macros and use the concrete `TuiFrontEnd` declaration.
They do not count inherited interface methods as newly declared public methods.
For reproduction, run the same measurement in checkouts of both revisions.
Use `rg -i -o --no-filename '\bauthority\b' src/ava/tui -g '*.h' -g '*.cpp' -g '*.cxx' | wc -l`
and substitute the other two terms. For setters, identify each
`RuntimePresentationState::publish_*` definition, remove comments, and count
bodies consisting of one assignment and one semicolon. For public methods, use
Universal Ctags 5.9.0 JSON with `--fields=+a --kinds-C++=+p --language-force=C++`;
count function/prototype tags whose scope is `ava::tui::TuiFrontEnd` and access
is public. Preserve overloads as separate tags.

| Measure | Before | After |
| --- | ---: | ---: |
| authority | 164 | 6 |
| receipt | 30 | 2 |
| chrome | 35 | 8 |
| One-assignment publish methods | 19 | 14 |
| Public TuiFrontEnd methods | 62 | 34 |

Remaining one-assignment methods publish individual feature views or independent
display values. Correlated run/frame/progress updates are grouped. Interface
upcasts narrow what a collaborator receives; they are not a security boundary.

### C++ Core Guidelines review

The requested CP.2, CP.20, CP.22, CP.26, CP.42, R.1, F.52, F.53, C.2, C.9,
C.131, E.12 and I.11 sections were read with the repository safety rules taking
precedence. Changes and checks arising from that audit:

- CP.22: Mermaid notifier invocation and retirement, bridge callback/worker
  shutdown, and search dispatch could execute unknown code while locked.
  Final verification also found audit endpoint copy/destruction, question
  construction and queued last-owner destruction under prompt mutexes.
  Snapshot or decide under lock, then copy, invoke, retire or join unlocked.
  Recheck question queue closure after copying, and refuse claims once closed.
- E.12: fallible result-vector allocation in a noexcept Mermaid bridge drain
  could terminate unexpectedly. Reserve before removing completed results and
  return the explicit failure while preserving retryable work.
- C.2/C.9/C.131: public raw screen mutation, loosely typed selector results,
  public decoder fields, fixture-only getters/modes, duplicated edit policy and overly broad frontend
  actions were replaced with owned feature operations and narrow interfaces.
- R.1/CP.26/I.11: active shutdown previously could join after failing to release
  a waiting worker. Release steps are now explicit; failure logs, latches and
  terminates before that join, as the maintainer required. RAII still owns the
  thread and resources on ordinary paths.
- F.53/CP.2: the plugin close/poll race test used a UI owner across threads. The
  TUI now owns polling; the worker retains only its copied endpoint and scoped,
  synchronized gate. No terminal operation moves to a worker.
- CP.42: request waits retain the result-and-retired-claim predicate. New test
  gates use predicates and deadlines; they do not infer completion from sleeps.

The audit did not find a new raw owning-pointer transfer or detached production
thread in the changed paths. Existing local queue unboundedness is reported,
not disguised as compliance with a global bounded-memory requirement.

### User-visible changes and follow-ups

Users receive the listed bug fixes: stale keys and pasted shortcuts cannot
answer host prompts; prompt/result paint failures fail closed; close-after-submit
input cannot spin; signal/HUP and shutdown outcomes remain reliable; plugin
labels cannot inject terminal controls; prompt hold/resume and Settings Archive
behavior follow the required ordering. Other editor precedence, settings,
selectors, transcript behavior and backend decisions stay at baseline behavior.

| Follow-up | Reason and rough size |
| --- | --- |
| Composer.h/composer.h and .cpp case collision | Retained explicitly by the maintainer. A later coordinated rename for case-insensitive filesystems is small. |
| Hard limits for event/prompt queues | Baseline queues have no local cap. Designing fail-closed overflow/backpressure semantics is a separate medium-sized policy change. |
| S4 and tmux certification | All-green jobs need working child runtime libraries and procfs accounting. Changing protected execution/child-environment policy to make tests pass would risk behavior; use a suitable runner. Environment work plus suite reruns, small to medium. |
| R5 | No finding was supplied; no implementation estimate is possible. |

No additional maintainer design decision is required. An all-green CI and tmux
run is still needed before claiming the environment-blocked paths are verified. Three remaining
places may confuse a newcomer:

1. `Composer` edits the draft while lowercase `composer` renders screen values.
   Their names differ only by case, deliberately retained for this round.
2. A render request does not paint immediately. End-of-turn paint and synchronous
   prompt/ownership handoffs have different guarantees; read the renderer and
   prompt contracts together before changing ordering.
3. A host prompt can hold a worker's callback while withholding provider, plugin,
   Mermaid and display updates. Request completion and callback retirement are
   separate lifetime steps; a result alone does not permit destroying its owner.

### Member comment coverage

The table covers all 101 new named classes/structs compared with `ec4143e`, including
nested values and test fixtures. Counts include declared functions, data, nested
types and the values of directly nested enums. Nested classes have separate rows.
Counts exclude debug macros and duplicate out-of-line definitions,
and accept the requested single comment for a deleted-operator group. A class's
thread/lifetime contract supplies context for its members. Mechanical coverage
is checked separately from comment truth and usefulness in the readability review.

The inventory uses Universal Ctags 5.9.0. Run the following on existing changed
C++ files from `git diff --name-only ec4143e..HEAD` in the current tree and on
those paths that exist in an `ec4143e` checkout:

```sh
ctags --sort=no --output-format=json --fields=+nKeSa \
  --kinds-C++=csmfptge --language-force=C++ -f - <changed-files>
```

Compare defined named class/struct tags by qualified name after removing Ctags'
anonymous-namespace hash. Exclude forward declarations and anonymous compiler
names. Keep source-local definitions; exclude out-of-line definitions when a
header declaration already supplies the member. Count prototypes, inline member
functions, fields, nested types and directly nested enum values. The mechanical
comment check accepts a comment on the declaration line or immediately before
it, ignoring blank lines, template/attribute lines and grouped deleted operators.
This adjacency heuristic cannot prove comment quality; the separate review
checks the explanation and its accuracy. Existing owners listed below are
selected by substantial refactor changes, not merely by appearing in a changed
file.

| New class or struct | Commented / total | Location |
| --- | ---: | --- |
| `ava_utils::MemberPointer` | 3/3 | `src/ava/debug/MemberPointer.h` |
| `ava_utils::MemberReference` | 3/3 | `src/ava/debug/MemberReference.h` |
| `ava::tui::ActiveRun` | 54/54 | `src/ava/tui/ActiveRun.h` |
| `ava::tui::ActiveRun::Operation` | 13/13 | `src/ava/tui/ActiveRun.h` |
| `ava::tui::ActiveRunEvents` | 29/29 | `src/ava/tui/ActiveRunEvents.h` |
| `ava::tui::ActiveRunInbox` | 32/32 | `src/ava/tui/ActiveRunInbox.h` |
| `ava::tui::ActiveRunInput` | 35/35 | `src/ava/tui/ActiveRunInput.h` |
| `ava::tui::ActiveRunOutcome` | 14/14 | `src/ava/tui/ActiveRunOutcome.h` |
| `ava::tui::BranchSummaryActions` | 7/7 | `src/ava/tui/BranchSummaryActions.h` |
| `ava::tui::BranchSummaryOperation` | 30/30 | `src/ava/tui/BranchSummaryOperation.h` |
| `ava::tui::CommandOutput` | 3/3 | `src/ava/tui/CommandOutput.h` |
| `ava::tui::Composer` | 94/94 | `src/ava/tui/Composer.h` |
| `ava::tui::ComposerInputResult` | 4/4 | `src/ava/tui/ComposerInputResult.h` |
| `ava::tui::DraftCommand` | 2/2 | `src/ava/tui/DraftCommand.h` |
| `ava::tui::runtime_input::InputPollResult` | 2/2 | `src/ava/tui/InputPollResult.h` |
| `ava::tui::PendingAttachments` | 9/9 | `src/ava/tui/PendingAttachments.h` |
| `ava::tui::PendingRequest` | 16/16 | `src/ava/tui/PendingRequest.h` |
| `ava::tui::SelectorDraftCompletion` | 2/2 | `src/ava/tui/SelectorDispatchResult.h` |
| `ava::tui::SelectorDraftCommand` | 1/1 | `src/ava/tui/SelectorDispatchResult.h` |
| `ava::tui::SelectorOpenedSession` | 1/1 | `src/ava/tui/SelectorDispatchResult.h` |
| `ava::tui::SelectorSessionError` | 1/1 | `src/ava/tui/SelectorDispatchResult.h` |
| `ava::tui::SelectorParentSummary` | 4/4 | `src/ava/tui/SelectorDispatchResult.h` |
| `ava::tui::SelectorHost` | 17/17 | `src/ava/tui/SelectorHost.h` |
| `ava::tui::SelectorInputActions` | 12/12 | `src/ava/tui/SelectorInputActions.h` |
| `ava::tui::SelectorInputController` | 12/12 | `src/ava/tui/SelectorInputController.h` |
| `ava::tui::SessionSelector` | 17/17 | `src/ava/tui/SessionSelector.h` |
| `ava::tui::SessionSelectorActions` | 12/12 | `src/ava/tui/SessionSelectorActions.h` |
| `ava::tui::SessionTransitionCoordinator` | 13/13 | `src/ava/tui/SessionTransitionCoordinator.h` |
| `ava::tui::SettingsDialog` | 24/24 | `src/ava/tui/SettingsDialog.h` |
| `ava::tui::SettingsDialogActions` | 7/7 | `src/ava/tui/SettingsDialogActions.h` |
| `ava::tui::Spinner` | 12/12 | `src/ava/tui/Spinner.h` |
| `ava::tui::TranscriptUpdate` | 5/5 | `src/ava/tui/TranscriptUpdate.h` |
| `ava::tui::TranscriptView` | 47/47 | `src/ava/tui/TranscriptView.h` |
| `ava::tui::TuiEventLoop` | 29/29 | `src/ava/tui/TuiEventLoop.h` |
| `ava::tui::TuiInputDispatcher` | 6/6 | `src/ava/tui/TuiInputDispatcher.h` |
| `ava::tui::TuiSession` | 66/66 | `src/ava/tui/TuiSession.h` |
| `ava::tui::TuiSessionActions` | 5/5 | `src/ava/tui/TuiSessionActions.h` |
| `ava::tui::TuiTimer` | 16/16 | `src/ava/tui/TuiTimer.h` |
| `ava::tui::TuiTimerRegistration` | 15/15 | `src/ava/tui/TuiTimerRegistration.h` |
| `ava::tui::TuiWaitState` | 13/13 | `src/ava/tui/TuiWaitState.h` |
| `ava::tui::TuiWakeup` | 4/4 | `src/ava/tui/TuiWakeup.h` |
| `ava::tui::runtime_input::InputReplay` | 11/11 | `src/ava/tui/runtime_input.cpp` |
| `PrintablePointee` | 2/2 | `tests/debug_pointer_security_tests.cpp` |
| `NotificationWatchdog` | 6/6 | `tests/mermaid_notification_tests.cpp` |
| `NotificationGate` | 5/5 | `tests/mermaid_notification_tests.cpp` |
| `CompletionLog` | 5/5 | `tests/mermaid_notification_tests.cpp` |
| `NotificationBridge` | 6/6 | `tests/mermaid_notification_tests.cpp` |
| `AllocationFaultState` | 2/2 | `tests/mermaid_notification_tests.cpp` |
| `ScopedAllocationFault` | 5/5 | `tests/mermaid_notification_tests.cpp` |
| `tui_test_support::ScopedScreenDrawObserver` | 12/12 | `tests/support/ScopedScreenDrawObserver.h` |
| `tui_test_support::TuiEventLoopFixture` | 5/5 | `tests/support/TuiEventLoopFixture.h` |
| `SummaryBackendFixture` | 6/6 | `tests/tui_branch_summary_timer_tests.cpp` |
| `SummaryTimerFixture` | 17/17 | `tests/tui_branch_summary_timer_tests.cpp` |
| `ComposerDispatchFixture` | 9/9 | `tests/tui_composer_dispatch_tests.cpp` |
| `ComposerPolicyFixture` | 9/9 | `tests/tui_composer_policy_tests.cpp` |
| `EventLoopFixture` | 5/5 | `tests/tui_composer_rendering_tests.cpp` |
| `LoopWaitDeadline` | 2/2 | `tests/tui_composer_rendering_tests.cpp` |
| `test_display_reload_shares_control_deadlines::TickCounter` | 2/2 | `tests/tui_composer_rendering_tests.cpp` |
| `EventLoopPipe` | 9/9 | `tests/tui_event_loop_tests.cpp` |
| `TimerRecorder` | 3/3 | `tests/tui_event_loop_tests.cpp` |
| `SpinnerFrames` | 3/3 | `tests/tui_event_loop_tests.cpp` |
| `MutatingTimerPair` | 12/12 | `tests/tui_event_loop_tests.cpp` |
| `RearmingTimer` | 9/9 | `tests/tui_event_loop_tests.cpp` |
| `FeatureOwnerFixture` | 17/17 | `tests/tui_feature_owner_tests.cpp` |
| `InputPipe` | 7/7 | `tests/tui_mermaid_contract_tests.cpp` |
| `B5BridgeState` | 7/7 | `tests/tui_mermaid_tests.cpp` |
| `B5AttachedMermaidFixture` | 12/12 | `tests/tui_mermaid_tests.cpp` |
| `NativePasteInput` | 12/12 | `tests/tui_paste_security_tests.cpp` |
| `ResolverWorker` | 5/5 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `ReentryProbe` | 20/20 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `ReentryScope` | 5/5 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `AuditTarget` | 9/9 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `ConstructionGate` | 12/12 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `DestructionProbe` | 13/13 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `QuestionTarget` | 9/9 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `EnqueueDeadline` | 3/3 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `PromptFixture` | 10/10 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `QueuedReleaseProbe` | 7/7 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `QueuedReleaseScope` | 5/5 | `tests/tui_prompt_callback_lock_tests.cpp` |
| `PromptDeadlockWatchdog` | 6/6 | `tests/tui_prompt_security_tests.cpp` |
| `EventLoopFixture` | 5/5 | `tests/tui_prompt_security_tests.cpp` |
| `PublicationDeadline` | 2/2 | `tests/tui_prompt_security_tests.cpp` |
| `PromptWorker` | 7/7 | `tests/tui_prompt_security_tests.cpp` |
| `PromptSecurityFixture` | 13/13 | `tests/tui_prompt_security_tests.cpp` |
| `RuntimeOwnerPipe` | 6/6 | `tests/tui_runtime_owner_tests.cpp` |
| `WakeDeadline` | 3/3 | `tests/tui_runtime_owner_tests.cpp` |
| `Turn` | 11/11 | `tests/tui_session_tests.cpp` |
| `SessionProbe` | 31/31 | `tests/tui_session_tests.cpp` |
| `ProbeScope` | 5/5 | `tests/tui_session_tests.cpp` |
| `StreamRedirect` | 8/8 | `tests/tui_session_tests.cpp` |
| `SessionInput` | 7/7 | `tests/tui_session_tests.cpp` |
| `SessionSignals` | 3/3 | `tests/tui_session_tests.cpp` |
| `WorkerGate` | 10/10 | `tests/tui_session_tests.cpp` |
| `ReleaseWorker` | 3/3 | `tests/tui_session_tests.cpp` |
| `ShutdownProbe` | 14/14 | `tests/tui_shutdown_tests.cpp` |
| `ProbeScope` | 5/5 | `tests/tui_shutdown_tests.cpp` |
| `InputPipe` | 7/7 | `tests/tui_shutdown_tests.cpp` |
| `ReleaseWorker` | 5/5 | `tests/tui_shutdown_tests.cpp` |
| `InputReadDeadline` | 3/3 | `tests/tui_terminal_input_tests.cpp` |
| `TranscriptViewFixture` | 5/5 | `tests/tui_transcript_view_tests.cpp` |
| `WaitStatePipe` | 11/11 | `tests/tui_wait_state_tests.cpp` |

Substantially changed existing production owners are also covered. Their
member counts are listed separately so additions are not confused with existing
class declarations.

| Existing owner | Commented / total |
| --- | ---: |
| `ava::tui::TuiFrontEnd` | 70/70 |
| `ava::tui::RuntimePresentationState` | 77/77 |
| `ava::tui::RuntimeRenderer` | 61/61 |
| `ava::tui::RuntimePromptCoordinator` | 69/69 |
| `ava::tui::RuntimePluginUiCoordinator` | 18/18 |
| `ava::tui::RuntimePluginUiCoordinatorState` | 32/32 |
| `ava::tui::RuntimeNavigationController` | 30/30 |
| `ava::tui::RuntimeSubmitController` | 17/17 |
| `ava::tui::RuntimeActionController` | 31/31 |
| `ava::tui::RuntimeEventQueue` | 9/9 |
| `ava::tui::RuntimePromptStashController` | 17/17 |
| `ava::tui::RuntimeMermaidPresentationController` | 39/39 |
| `ava::tui::RuntimeMermaidPresentationController::Record` | 9/9 |
| `ava::tui::RuntimeSubagentWorkspaceController` | 42/42 |
| `ava::tui::RuntimeTranscriptSelectionState` | 73/73 |
| `ava::tui::RuntimeTranscriptSelectionState::ItemSourceAuthority` | 12/12 |
