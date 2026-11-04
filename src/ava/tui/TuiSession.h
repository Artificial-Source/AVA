// Composes the interactive TUI; start with docs/engineering/tui-architecture.md.
#pragma once

#include "ActiveRun.h"
#include "ActiveRunInput.h"
#include "BranchSummaryOperation.h"
#include "CommandOutput.h"
#include "Composer.h"
#include "PendingAttachments.h"
#include "SelectorHost.h"
#include "SelectorInputController.h"
#include "SessionSelector.h"
#include "SessionTransitionCoordinator.h"
#include "SettingsDialog.h"
#include "TuiEventLoop.h"
#include "TuiInputDispatcher.h"
#include "prompt_stash_internal.h"
#include "runtime_actions_internal.h"
#include "runtime_input_internal.h"
#include "runtime_mermaid_internal.h"
#include "runtime_navigation_internal.h"
#include "runtime_plugin_ui_internal.h"
#include "runtime_prompts_internal.h"
#include "runtime_render_internal.h"
#include "runtime_state_internal.h"
#include "runtime_subagent_workspace_internal.h"
#include "runtime_submit_internal.h"
#include "runtime_transcript_search_internal.h"
#include "runtime_views_internal.h"
#include "session_grants.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

// TuiFrontEnd::run owns this object for one interactive session on the TUI thread.
// The application-owned frontend and initialized terminal Context outlive it.
// Feature owners keep their own state; this object chooses loop order and performs
// transitions between owners. Workers receive synchronized endpoints, never this
// object. Member order preserves borrows through shutdown and reverse destruction.
// Read docs/engineering/tui-architecture.md before adding a feature or timer.
class TuiSession final
{
 public:
  // TUI thread: construct once after terminal initialization. Members may register
  // timers while being built, but run() alone dispatches them after construction.
  // All borrowed arguments must outlive the session.
  TuiSession(TuiFrontEnd& frontend, terminal::Context& terminal_context, std::string const& initial_status_line);
  // TUI thread: stop new worker requests and join active work while all feature
  // owners are still alive. This also covers exceptions and early run returns.
  ~TuiSession();
  // Not copyable or movable: timers and input callbacks hold member addresses.
  TuiSession(TuiSession const&) = delete;
  TuiSession& operator=(TuiSession const&) = delete;
  TuiSession(TuiSession&&) = delete;
  TuiSession& operator=(TuiSession&&) = delete;
  // TUI thread, once per constructed session: run control/timers/updates/input/
  // paint/wait until exit, then retire work before invoking before_shutdown.
  // The callback is borrowed only for this call; the outer frontend guard covers
  // early returns/exceptions. Return 130 for a signal, 1 for I/O or cleanup failure,
  // otherwise 0.
  [[nodiscard]] int run(std::function<bool()> const& before_shutdown);

  // Owned Composer, ActiveRun and presentation members deliberately opt out of
  // printing because they retain user content; this aggregate cannot print them.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Resolve configured bindings on the TUI thread without repeating lookup policy.
  [[nodiscard]] bool is_action(TuiAction action, InputEvent const& event) const;
  // TUI thread: consume editor feedback at its dispatch stage before rendering.
  // Executes only the cross-owner request after Composer resolves key precedence.
  TuiInputOutcome handle_composer_input(ComposerInputPhase phase, runtime_input::RuntimeInput const& input);
  // TUI thread: translate summary completion/cancellation into session exit policy
  // without losing a terminal failure reported by the feature.
  bool stop_for_branch_summary(BranchSummaryOutcome outcome);
  // TUI thread, presentation stage: rebase an open settings preview after a due
  // backend reload, then invalidate the final overlay once. False means I/O failure.
  bool maybe_reload_display_settings();
  // TUI thread: close the settings preview before opening the competing overview
  // so its snapshot reflects restored display settings.
  bool toggle_startup_overview();
  // TUI thread: cross from editor/selector intent into submission policy. Retain
  // the returned run outcome before interpreting whether dispatch continues.
  RuntimeSubmitDisposition handle_submit(std::optional<std::string> forced_submission = std::nullopt);
  // TUI thread after a successful backend open: read committed identity, reset
  // dependent UI owners and publish optional feedback for the new session.
  void settle_opened_session(std::string status);
  // TUI thread after a backend action: reset UI state only if committed identity
  // changed. The coordinator owns the ordered cross-feature reset.
  bool synchronize_committed_session();
  // TUI thread, before due timers. Handle signals and observe terminal failures;
  // cancellation may release workers, so it must run outside timer callbacks.
  void service_control();
  // Plain timer wake: keeps the 100 ms observation cadence without handling
  // signals or running application work from inside service_due().
  void wake_for_control(TuiEventLoop::Clock::time_point now);
  // TUI thread after due timers: consume at most 256 events, then attempt final
  // settlement. Return whether another turn can process accepted application work.
  [[nodiscard]] bool service_application_updates();
  // TUI thread after run updates: consume feature failure/outcome flags, diagram
  // readiness and eligible display reloads before decoding input. Never waits.
  void service_presentations();
  // TUI thread after updates: poll at most 32 input results, including discarded
  // replies. True keeps the next wait nonblocking; closing input is drained only.
  [[nodiscard]] bool service_input_batch();
  // TUI thread: centralize loop-stop and I/O-failure latches after a stage returns.
  // Pass/Handled leave the next input or loop phase available.
  void apply_input_outcome(TuiInputOutcome outcome);
  // TUI thread before timers and again after decode. Automatic question resolution
  // precedes signal claiming; a claimed signal consumes any retained input key.
  TuiInputOutcome handle_signals();
  // TUI input stage: retire wheel cadence and refresh search geometry before
  // requesting the resize frame. Pass leaves ordinary input to later stages.
  TuiInputOutcome handle_resize_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: filter burst wheel reports once before modal/view dispatch
  // so rejected reports cannot reach a second scroll owner.
  TuiInputOutcome handle_wheel_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: honor output-modal capture or typing pass-through before
  // workspace/editor policy. Request a frame only for a published modal change.
  TuiInputOutcome handle_command_output_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: a visible workspace owns the key and its feedback. Leave
  // backend subagent ownership unchanged while requesting any changed frame.
  TuiInputOutcome handle_workspace_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: let the summary owner interpret its modal, then preserve
  // any eventual-exit or rendering-failure decision in the session outcome.
  TuiInputOutcome handle_branch_summary_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: clear transient input feedback before offering the key to
  // transcript search; an absent result preserves dispatch to ordinary selectors.
  TuiInputOutcome handle_search_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: ask the visible selector owner for a typed result, then
  // perform cross-owner navigation/submission/session transitions before painting.
  TuiInputOutcome handle_selector_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: preserve copy/stash priority, then primary editor input,
  // before terminal handoffs such as external editor, suspend or image paste.
  TuiInputOutcome handle_primary_input(runtime_input::RuntimeInput const& input);
  // TUI input stage after editor clearing: resolve session/mode/exit bindings
  // before transcript navigation, delegating backend actions through the frontend.
  TuiInputOutcome handle_session_input(runtime_input::RuntimeInput const& input);
  // TUI input stage: transcript bindings precede composer cursor/history keys.
  // Navigation owns scrolling and pointer hit-testing; this stage requests paint.
  TuiInputOutcome handle_transcript_input(runtime_input::RuntimeInput const& input);
  // TUI input stage after navigation: let Composer decide Cancel/Submit/Space
  // precedence, then perform any returned cross-owner request.
  TuiInputOutcome handle_tail_input(runtime_input::RuntimeInput const& input);
  // TUI thread after an input mutation: publish current completion fields before
  // scheduling the next frame. This does not synchronously paint.
  TuiInputOutcome render_input();

  // Declaration order is dependency order for owned collaborators. Reverse
  // destruction keeps each borrowed owner alive until its borrowers have retired.
  // Application frontend borrowed until destruction; backend validation stays in its implementation.
  TuiFrontEnd& frontend_;
  // External initialized terminal, borrowed for TUI-thread input/output until destruction.
  terminal::Context& terminal_context_;
  // TUI-thread waiter and timer registry, constructed before every registered feature.
  // Timer handles below die first; copied weak wakeups may outlive the loop.
  TuiEventLoop loop_;
  // TUI-owned display data; readers borrow const views and features update them through named operations.
  RuntimePresentationState presentation_state_;
  // TUI-thread output-modal policy shared by idle and active dispatch. Its current
  // view lives in presentation_state_, which outlives this owner.
  CommandOutput command_output_;
  // TUI-thread staging owner; submission takes attachments and session transitions clear them.
  PendingAttachments attachments_;
  // Session-local command-prompt grants, read by the prompt coordinator and cleared on session transition.
  TuiSessionGrantRegistry command_session_grants_;
  // Synchronized plugin request queue with TUI-only views; stop new requests before destroying the screen.
  RuntimePluginUiCoordinator plugin_ui_;
  // TUI-thread failure latch set by rendering, descriptor, notification or shutdown failure; retained until run returns.
  bool terminal_write_failed_ = false;
  // TUI-thread latch for a claimed signal that requests exit, preserving status 130 through cleanup.
  bool terminal_signal_received_ = false;
  // TUI-thread cleanup failure copied from completed or stopped runs; survives their reset.
  bool cleanup_failed_ = false;
  // TUI-thread terminal-closure latch; once set, only drain buffered input and retire work.
  bool terminal_closing_ = false;
  // The session's one TUI-thread editor; declared before every owner that borrows draft state.
  Composer draft_state_;
  // TUI-thread caches, viewport and frame scheduler; its timer handles retire before the loop.
  RuntimeRenderer renderer_;
  // TUI-thread transcript/sidebar navigation using the editor and renderer declared above.
  RuntimeNavigationController navigation_;
  // TUI-thread owner of visible selector kind/query/rows; feature policy objects borrow this host.
  SelectorHost selectors_;
  // TUI-thread generic selector policy; returns values for cross-owner actions rather than mutating their internals.
  SelectorInputController selector_input_;
  // TUI-thread staged settings/preview lifetime; close restores or commits before a competing modal opens.
  SettingsDialog settings_;
  // TUI-thread session-browser policy; the application still validates and opens selected sessions.
  SessionSelector session_selector_;
  // TUI-thread presentation/discovery for application-owned summary work, including cancel/exit settlement.
  BranchSummaryOperation branch_summary_;
  // TUI-thread stash editing and selector policy, sharing the one editor and attachment owner.
  RuntimePromptStashController prompt_stash_;

  // TUI-thread search view and saved scroll position; close search before opening a worker prompt.
  TranscriptSearchController transcript_search_;
  // Synchronized worker request queues and TUI-only active prompt; remove borrowed
  // callbacks and release its pending request before joining the worker.
  RuntimePromptCoordinator prompt_coordinator_;
  // TUI-thread diagram views and retry timers; copied worker callbacks retain only
  // shared readiness state and a weak wake handle, so they can outlive this owner.
  RuntimeMermaidPresentationController mermaid_presentation_;
  // TUI-thread frontend action policy and display-discovery timer; backend decisions remain in frontend callbacks.
  RuntimeActionController action_controller_;
  // TUI-thread child-workspace view/discovery; closing it never destroys the backend job.
  RuntimeSubagentWorkspaceController subagent_workspace_;
  // TUI-thread ordered reset across editor, viewport, search, workspace, selectors, attachments and grants.
  SessionTransitionCoordinator session_transitions_;
  // TUI-thread submission lifetime; shutdown joins its worker before any borrowed feature is destroyed.
  ActiveRun active_run_controller_;
  // TUI-thread ordinary active keys; borrows ActiveRun without exposing its private
  // operation or queues. ActiveRun remains alive and joins its worker before teardown.
  ActiveRunInput active_run_input_;
  // TUI-thread entry from draft intent to local commands or ActiveRun; records history before launch.
  RuntimeSubmitController submit_controller_;
  // TUI-thread idle handlers in precedence order. Their targets above outlive this
  // dispatcher and stay at stable addresses for the session.
  TuiInputDispatcher input_dispatcher_;
  // TUI-thread plain wake every 100 ms, targeting this session. Cancel on exit
  // before any feature owner is destroyed.
  TuiTimer control_timer_;
};

} // namespace ava::tui
