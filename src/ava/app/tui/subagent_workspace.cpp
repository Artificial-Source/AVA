#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/tui/subagent_workspace.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

namespace ava::app {
namespace {

std::string execution_label(ava::agent::SubagentExecutionState state)
{
  switch (state)
  {
    case ava::agent::SubagentExecutionState::Starting:
      return "Starting";
    case ava::agent::SubagentExecutionState::Running:
      return "Running";
    case ava::agent::SubagentExecutionState::Completed:
      return "Completed";
    case ava::agent::SubagentExecutionState::Failed:
      return "Failed";
    case ava::agent::SubagentExecutionState::Canceled:
      return "Canceled";
    case ava::agent::SubagentExecutionState::Interrupted:
      return "Interrupted";
  }
  return "Unavailable";
}

std::string mode_label(ava::agent::SubagentJobMode mode)
{
  return mode == ava::agent::SubagentJobMode::Background ? "Background" : "Foreground";
}

std::string launch_display_text(ava::agent::SubagentLaunchDisplay const& display)
{
  std::string text = display.model_display_name();
  auto const& reasoning = display.reasoning_label();
  if (!reasoning.empty())
  {
    if (!text.empty())
      text += " · ";
    text += "thinking " + reasoning;
  }
  return text;
}

bool safe_display_metadata(std::string_view text)
{
  return !text.empty() && text.find_first_not_of(' ') != std::string_view::npos && text.size() <= 120 && text.find('<') == std::string_view::npos &&
         text.find('>') == std::string_view::npos && text.find("job_") == std::string_view::npos && text.find("session_") == std::string_view::npos &&
         !text.starts_with('/') &&
         !(text.size() >= 3 && ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')) && text[1] == ':' &&
           (text[2] == '/' || text[2] == '\\'));
}

std::string display_title(ava::agent::SubagentJobSnapshot const& job)
{
  auto title = sanitize_inline_text(job.display_title);
  if (safe_display_metadata(title))
    return title;
  if (job.display_subagent_type == "explore" || job.display_subagent_type == "Explore")
    return "Explore subagent";
  return "Subagent";
}

void append_detail(std::string& detail, std::string text)
{
  if (text.empty())
    return;
  if (!detail.empty())
    detail += " · ";
  detail += std::move(text);
}

std::string short_hidden_ref(std::string_view id)
{
  std::uint64_t hash = 1469598103934665603ULL;
  for (unsigned char const byte : id)
  {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string ref = "@";
  for (int shift = 28; shift >= 0; shift -= 4)
    ref.push_back(kHex[(hash >> shift) & 0xfU]);
  return ref;
}

std::vector<std::string> short_hidden_refs(std::vector<std::string> const& ids)
{
  std::vector<std::string> refs;
  refs.reserve(ids.size());
  for (auto const& id : ids)
    refs.push_back(short_hidden_ref(id));
  for (std::size_t index = 0; index < refs.size(); ++index)
  {
    std::size_t duplicate = 0;
    for (std::size_t other = 0; other < index; ++other)
    {
      if (ids[other] != ids[index] && refs[other].substr(0, 9) == refs[index].substr(0, 9))
        ++duplicate;
    }
    if (duplicate > 0)
      refs[index] += "-" + std::to_string(duplicate + 1);
  }
  return refs;
}

bool terminal_execution(ava::agent::SubagentExecutionState state) noexcept
{
  return state == ava::agent::SubagentExecutionState::Completed || state == ava::agent::SubagentExecutionState::Failed ||
         state == ava::agent::SubagentExecutionState::Canceled || state == ava::agent::SubagentExecutionState::Interrupted;
}

bool background_or_promoted(ava::agent::SubagentJobSnapshot const& job) noexcept
{
  return job.mode == ava::agent::SubagentJobMode::Background || job.was_promoted;
}

std::optional<ava::tui::SubagentWorkspacePromoteOutcome> classify_promote_snapshot(ava::agent::SubagentCoordinatorJobSnapshot const& snapshot)
{
  if (terminal_execution(snapshot.job.execution))
    return ava::tui::SubagentWorkspacePromoteOutcome::AlreadyFinished;
  if (background_or_promoted(snapshot.job))
    return ava::tui::SubagentWorkspacePromoteOutcome::CurrentlyBackground;
  return std::nullopt;
}

}  // namespace

ava::tui::SelectListView subagent_selector_view(std::vector<ava::agent::SubagentCoordinatorJobSnapshot> const& snapshots)
{
  ava::tui::SelectListView view;
  view.title = "Subagents";
  view.placeholder = "Search subagents";
  view.empty_text = "No matching subagents";
  view.footer_hint = "Enter open · type filter · Esc close";
  view.freeze_underlying_transcript_layout = true;

  if (snapshots.empty())
  {
    view.items.push_back(ava::tui::SelectListItemView{.value = {},
                                                      .label = "No subagents yet",
                                                      .description = "Background subagents will appear here",
                                                      .group = {},
                                                      .detail = {},
                                                      .badge = {},
                                                      .current = false,
                                                      .enabled = false,
                                                      .disabled_reason = {}});
    return view;
  }

  std::vector<std::string> ids;
  ids.reserve(snapshots.size());
  for (auto const& snapshot : snapshots)
    ids.push_back(snapshot.job.identity.job_id);
  auto const refs = short_hidden_refs(ids);

  view.items.reserve(snapshots.size());
  for (std::size_t reverse_index = snapshots.size(); reverse_index > 0; --reverse_index)
  {
    auto const index = reverse_index - 1;
    auto const& job = snapshots[index].job;
    std::string detail;
    auto const subagent_type = sanitize_inline_text(job.display_subagent_type);
    if (safe_display_metadata(subagent_type))
      append_detail(detail, "type " + subagent_type);
    if (job.tool_calls > 0)
      append_detail(detail, "tools " + std::to_string(job.tool_calls));
    auto const status = execution_label(job.execution) + " · " + mode_label(job.mode);
    auto priority_suffix = status;
    if (!refs[index].empty())
      append_detail(priority_suffix, "ref " + refs[index]);
    view.items.push_back(ava::tui::SelectListItemView{.non_searchable_suffix = launch_display_text(job.launch_display),
                                                      .priority_suffix = std::move(priority_suffix),
                                                      .value = job.identity.job_id,
                                                      .label = display_title(job),
                                                      .description = std::move(detail),
                                                      .group = {},
                                                      .detail = {},
                                                      .badge = status,
                                                      .current = false,
                                                      .enabled = true,
                                                      .disabled_reason = {}});
  }
  return view;
}

ava::core::Result<ava::tui::SelectListView> subagent_selector_view(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                   std::string_view parent_session_id)
{
  if (!coordinator)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "subagent workspace is unavailable"));
  return subagent_selector_view(coordinator->list(parent_session_id));
}

ava::tui::SubagentWorkspaceCancelOutcome map_subagent_cancel_outcome(ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> const& result)
{
  if (!result)
    return ava::tui::SubagentWorkspaceCancelOutcome::CancelUnavailable;
  if (terminal_execution(result->job.execution))
    return ava::tui::SubagentWorkspaceCancelOutcome::AlreadyFinished;
  if (result->job.cancel_requested)
    return ava::tui::SubagentWorkspaceCancelOutcome::CancellationRequested;
  return ava::tui::SubagentWorkspaceCancelOutcome::CancelUnavailable;
}

ava::tui::SubagentWorkspacePromoteOutcome map_subagent_promote_outcome(ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> const& promote_result,
                                                                       std::optional<ava::agent::SubagentCoordinatorJobSnapshot> const& owner_bound_status)
{
  if (promote_result)
  {
    if (auto classified = classify_promote_snapshot(*promote_result))
      return *classified;
    return ava::tui::SubagentWorkspacePromoteOutcome::PromotionUnavailable;
  }
  if (owner_bound_status)
  {
    if (auto classified = classify_promote_snapshot(*owner_bound_status))
      return *classified;
  }
  return ava::tui::SubagentWorkspacePromoteOutcome::PromotionUnavailable;
}

ava::tui::SubagentWorkspaceCancelOutcome cancel_subagent_for_workspace(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                       std::string_view parent_session_id, std::string_view job_id)
{
  if (!coordinator)
    return ava::tui::SubagentWorkspaceCancelOutcome::CancelUnavailable;
  return map_subagent_cancel_outcome(coordinator->cancel(parent_session_id, job_id));
}

ava::tui::SubagentWorkspacePromoteOutcome promote_subagent_for_workspace(std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator,
                                                                         std::string_view parent_session_id, std::string_view job_id)
{
  if (!coordinator)
    return ava::tui::SubagentWorkspacePromoteOutcome::PromotionUnavailable;
  auto promoted = coordinator->promote(parent_session_id, job_id);
  std::optional<ava::agent::SubagentCoordinatorJobSnapshot> owner_bound_status;
  if (!promoted)
  {
    if (auto status = coordinator->snapshot(parent_session_id, job_id))
      owner_bound_status = std::move(*status);
  }
  return map_subagent_promote_outcome(promoted, owner_bound_status);
}

}  // namespace ava::app
