#pragma once

#include "SelectorHost.h"
#include "SettingsDialog.h"
#include "prompt_stash_internal.h"
#include "runtime_actions_internal.h"
#include "runtime_active_run_internal.h"
#include "runtime_draft_internal.h"
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

namespace ava::tui {

// One run's composition. Frontend and application-owned Context outlive this owner.
// Feature algorithms are extracted from this intermediate owner by goals 04 and 05.
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
  void restore_branch_summary_selector(SelectListView view);
  void present_branch_summary_status(std::string status);
  bool settle_branch_summary(TuiBranchSummarySnapshot const& operation);
  bool apply_branch_summary_snapshot(TuiBranchSummarySnapshot const& operation);
  bool poll_branch_summary();
  bool begin_branch_summary(SelectListView const& session_view, std::size_t selected_index, std::string selected_value);
  bool cancel_branch_summary(bool exit_requested);
  bool confirm_branch_summary();
  bool service_mermaid_presentation();
  bool maybe_reload_display_settings();
  bool toggle_startup_overview();
  RuntimeSubmitDisposition handle_submit(std::optional<std::string> forced_submission = std::nullopt);
  void preserve_session_selector_state(SelectListInputResult const& input_result, SelectListView next_view, std::string status);
  void settle_opened_session(std::string status);
  bool synchronize_committed_session();
  void select_slash_command();
  void select_file_reference();
  void select_path_completion();
  bool force_path_completion();
  void insert_input_text(runtime_input::RuntimeInput const& input);
  SelectListInputResult classify_selector_input(runtime_input::RuntimeInput const& input);

  // Declaration order is dependency order; all borrowed controller references
  // refer to earlier members and remain valid through their destruction.
  TuiFrontEnd& frontend_;
  terminal::Context& terminal_context_;
  RuntimePresentationState presentation_state_;
  RuntimePluginUiCoordinator plugin_ui_;
  bool terminal_write_failed_ = false;
  bool terminal_signal_received_ = false;
  RuntimeDraftState draft_state_;
  RuntimeRenderer renderer_;
  RuntimeNavigationController navigation_;
  SelectorHost selectors_;
  SettingsDialog settings_;
  RuntimePromptStashController prompt_stash_;

  // Temporary branch-summary ownership, extracted in 04/04.
  bool branch_summary_active_ = false;
  bool branch_summary_exit_requested_ = false;
  std::uint64_t branch_summary_generation_ = 0;
  TuiBranchSummaryPhase branch_summary_phase_ = TuiBranchSummaryPhase::Idle;
  SelectListView branch_summary_prior_session_view_;
  std::size_t branch_summary_prior_selected_index_ = 0;
  std::string branch_summary_selected_source_value_;
  bool branch_summary_exit_ready_ = false;
  TranscriptSearchController transcript_search_;
  std::optional<PendingSessionArchiveAction> session_archive_confirmation_;
  RuntimePromptCoordinator prompt_coordinator_;
  RuntimeMermaidPresentationController mermaid_presentation_;
  RuntimeActionController action_controller_;
  RuntimeSubagentWorkspaceController subagent_workspace_;
  RuntimeActiveRunController active_run_controller_;
  RuntimeSubmitController submit_controller_;
};

} // namespace ava::tui
