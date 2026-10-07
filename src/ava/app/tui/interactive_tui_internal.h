#pragma once

#include "ava/app/branch_summary_coordinator.h"
#include "ava/app/command_palette.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/project_trust.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime.h"
#include "ava/permissions/permission_rules.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app::interactive_internal {

struct TuiRequestPresentation
{
 public:
  bool has_local_command = false;
  InteractiveResult local_command;
  InteractiveResult conversation;
  std::vector<std::string> ordinary_turn_request_ids;

  // TUI presentation can contain provider output and must not be debug-printed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

void append_status_line(std::string& target, std::string line);
[[nodiscard]] std::string git_branch_for_sidebar(std::filesystem::path const& workspace);
[[nodiscard]] std::vector<CommandHotkey> command_hotkeys_from_key_bindings(ava::tui::TuiKeyBindings const& key_bindings);
[[nodiscard]] std::string display_theme_status(std::string_view prefix);
[[nodiscard]] ava::tui::ProjectTrustSnapshot project_trust_snapshot(ProjectTrustState const& state);
[[nodiscard]] std::vector<ava::tui::ToolTimelineItem> tui_tool_timeline(std::vector<ava::agent::ToolTimelineEntry> const& entries);
[[nodiscard]] std::optional<std::string> token_status_for_session(runtime::session_ts const& unlocked_session);
// Fail-closed presentation hydration from the latest successful committed todowrite.
[[nodiscard]] std::vector<ava::tui::TodoItem> todos_for_session(runtime::session_ts const& unlocked_session);
[[nodiscard]] std::string format_active_context_status_value(long long tokens, std::optional<long long> context_window_tokens);
[[nodiscard]] std::optional<std::string> active_context_status_for_session(runtime::session_ts const& unlocked_session);
[[nodiscard]] std::string session_selector_footer_hint(SessionSelectorSort sort, bool named_only, bool show_paths, bool show_archived, bool show_label_time);
[[nodiscard]] ava::tui::TuiBranchSummarySnapshot tui_branch_summary_snapshot(BranchSummarySnapshot const& snapshot);
[[nodiscard]] std::string scoped_model_selector_footer_hint();
[[nodiscard]] ava::core::Result<ava::tui::SelectListView> toggle_scoped_model(runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                              std::string_view value);
[[nodiscard]] ava::core::Result<ava::tui::SelectListView> enable_scoped_models(runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                               std::vector<std::string> targets);
[[nodiscard]] ava::core::Result<ava::tui::SelectListView> clear_scoped_models(runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                              std::vector<std::string> targets);
[[nodiscard]] ava::core::Result<ava::tui::SelectListView> toggle_scoped_model_provider(runtime::session_ts& unlocked_session,
                                                                                       ava::tui::SelectListView const& previous,
                                                                                       std::string_view selected_value);
[[nodiscard]] ava::core::Result<ava::tui::SelectListView> reorder_scoped_model(runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                               std::string_view selected_value, bool up);
[[nodiscard]] ava::core::Result<std::string> save_scoped_model_cycle(runtime::session_ts& unlocked_session);
[[nodiscard]] ava::core::Result<ava::tui::TuiRememberedPermissionRule> remember_permission_rule_for_prompt(runtime::session_ts const& unlocked_session,
                                                                                                           ava::permissions::PermissionPrompt const& prompt,
                                                                                                           ava::permissions::PermissionAction action,
                                                                                                           std::string actor = "tui_prompt");
[[nodiscard]] bool workspace_catalog_changed(InteractiveResult const& result);
[[nodiscard]] bool workspace_catalog_reload_requested(std::string_view submitted);
// Request-segmented presentation is a TUI-only projection. An initial local
// command owns uncommitted queued results without granting conversation authority.
void capture_tui_request_presentation(TuiRequestPresentation& presentation, bool initial_is_local_command, std::string_view request_line,
                                      std::string const& request_id, InteractiveResult const& request_result);
// Runs queued follow-ups only while the submit worker still owns the same
// authoritative session. A transition keeps only that line's presentation
// output/tool data while preserving aggregate control flags.
[[nodiscard]] bool run_queued_follow_ups_until_session_transition(InteractiveResult& result, bool& workspace_catalog_reload,
                                                                  std::string_view initial_session_id, ava::tui::TuiSubmitContext const& context,
                                                                  std::function<std::string()> const& current_session_id,
                                                                  std::function<InteractiveResult(ava::tui::TuiQueuedFollowUp const&)> const& run_follow_up);
[[nodiscard]] int run_tui(InteractiveState state);

}  // namespace ava::app::interactive_internal
