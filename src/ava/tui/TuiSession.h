#pragma once

#include "ActiveRun.h"
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

// One run's composition. Frontend and application-owned Context outlive this owner.
// Feature owners maintain their own invariants; this owner coordinates lifecycle,
// decoded input precedence and the sole live terminal event loop.
class TuiSession final
{
 public:
  TuiSession(TuiFrontEnd& frontend, terminal::Context& terminal_context, std::string const& initial_status_line);
  ~TuiSession();
  TuiSession(TuiSession const&) = delete;
  TuiSession& operator=(TuiSession const&) = delete;
  TuiSession(TuiSession&&) = delete;
  TuiSession& operator=(TuiSession&&) = delete;
  // Called only on normal loop exit; never retained. The outer guard handles early exits.
  [[nodiscard]] int run(std::function<bool()> const& before_shutdown);

  // Owns arbitrary draft/message bodies and presentation authority aggregates.
  // Those contents must not become generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Resolve configured bindings on the TUI thread without repeating lookup policy.
  [[nodiscard]] bool is_action(TuiAction action, InputEvent const& event) const;
  // TUI thread: consume editor feedback at its dispatch stage before rendering.
  // Executes only the cross-owner request after Composer resolves key precedence.
  TuiInputOutcome handle_composer_input(ComposerInputPhase phase, runtime_input::RuntimeInput const& input);
  bool stop_for_branch_summary(BranchSummaryOutcome outcome);
  bool maybe_reload_display_settings();
  bool toggle_startup_overview();
  RuntimeSubmitDisposition handle_submit(std::optional<std::string> forced_submission = std::nullopt);
  void settle_opened_session(std::string status);
  bool synchronize_committed_session();
  // TUI thread, before due timers. Claim signals and observe terminal failures;
  // cancellation may release workers, so it must run outside timer callbacks.
  void service_control();
  // Plain timer wake: keeps the 100 ms observation cadence without handling
  // signals or running application work from inside service_due().
  void wake_for_control(TuiEventLoop::Clock::time_point now);
  [[nodiscard]] bool service_application_updates();
  void service_presentations();
  [[nodiscard]] bool service_input_batch();
  void apply_input_outcome(TuiInputOutcome outcome);
  TuiInputOutcome handle_signals();
  TuiInputOutcome handle_resize_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_wheel_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_command_output_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_workspace_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_branch_summary_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_search_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_selector_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_primary_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_session_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_transcript_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome handle_tail_input(runtime_input::RuntimeInput const& input);
  TuiInputOutcome render_input();

  // Declaration order is dependency order; all borrowed controller references
  // refer to earlier members and remain valid through their destruction.
  TuiFrontEnd& frontend_;
  terminal::Context& terminal_context_;
  // Every timer/notification client below retires before the loop resources.
  TuiEventLoop loop_;
  RuntimePresentationState presentation_state_;
  // Modal input policy; presentation storage above outlives this owner.
  CommandOutput command_output_;
  PendingAttachments attachments_;
  TuiSessionGrantRegistry command_session_grants_;
  RuntimePluginUiCoordinator plugin_ui_;
  bool terminal_write_failed_ = false;
  bool terminal_signal_received_ = false;
  bool terminal_closing_ = false;
  Composer draft_state_;
  RuntimeRenderer renderer_;
  RuntimeNavigationController navigation_;
  SelectorHost selectors_;
  SelectorInputController selector_input_;
  SettingsDialog settings_;
  SessionSelector session_selector_;
  BranchSummaryOperation branch_summary_;
  RuntimePromptStashController prompt_stash_;

  TranscriptSearchController transcript_search_;
  RuntimePromptCoordinator prompt_coordinator_;
  RuntimeMermaidPresentationController mermaid_presentation_;
  RuntimeActionController action_controller_;
  RuntimeSubagentWorkspaceController subagent_workspace_;
  SessionTransitionCoordinator session_transitions_;
  ActiveRun active_run_controller_;
  RuntimeSubmitController submit_controller_;
  // Registered targets above outlive the dispatcher; the session cannot move.
  TuiInputDispatcher input_dispatcher_;
  // This callback target is the session itself; cancel before collaborators retire.
  TuiTimer control_timer_;
};

} // namespace ava::tui
