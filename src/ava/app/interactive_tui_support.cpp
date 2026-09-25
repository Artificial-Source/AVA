#include "sys.h"
#include "ava/app/command_palette.h"
#include "ava/app/interactive.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/project_trust.h"
#include "ava/agent/todo.h"
#include "ava/tui/composer.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/theme.h"
#include "ava/config/model_config.h"
#include "ava/session/compaction.h"
#include "ava/session/stats.h"
#include "ava/permissions/permission.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/mode.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ava::app::interactive_internal {

void append_status_line(std::string& target, std::string line)
{
  if (line.empty())
    return;
  if (!target.empty())
    target += '\n';
  target += std::move(line);
}

std::string git_branch_for_sidebar(std::filesystem::path const& workspace)
{
  auto const head_path = workspace / ".git" / "HEAD";
  std::ifstream input(head_path);
  if (!input)
    return {};
  std::string head;
  std::getline(input, head);
  constexpr std::string_view ref_prefix = "ref: refs/heads/";
  if (head.rfind(ref_prefix, 0) == 0)
    return head.substr(ref_prefix.size());
  return head.size() > 12 ? head.substr(0, 12) : head;
}

std::vector<ava::app::CommandHotkey> command_hotkeys_from_key_bindings(ava::tui::TuiKeyBindings const& key_bindings)
{
  std::vector<ava::app::CommandHotkey> hotkeys;
  for (auto const& item : ava::tui::key_binding_help_items(key_bindings))
  {
    // description carries the concise human action label for palette completions.
    hotkeys.push_back(ava::app::CommandHotkey{.action = item.action, .description = item.label.empty() ? item.action : item.label, .keys = item.keys});
  }
  return hotkeys;
}

std::string display_theme_status(std::string_view prefix)
{
  auto const active = ava::tui::active_tui_theme();
  return std::string(prefix) + ": " + active.name + " (" + active.badge + ")";
}

ava::tui::ProjectTrustSnapshot project_trust_snapshot(ava::app::ProjectTrustState const& state)
{
  return ava::tui::ProjectTrustSnapshot{.decision = std::string(ava::app::to_string(state.decision)),
                                        .project_resources = ava::app::project_resources_trusted(state) ? std::string("enabled") : std::string("skipped"),
                                        .workspace = state.workspace_dir.string(),
                                        .matched_path = state.matched_path.string(),
                                        .trust_file = state.trust_file.string(),
                                        .protected_resource_count = state.protected_resources.size(),
                                        .diagnostic = state.diagnostic};
}

ava::tui::ToolTimelineStatus tui_tool_status(ava::agent::ToolTimelineStatus status)
{
  switch (status)
  {
    case ava::agent::ToolTimelineStatus::Running:
      return ava::tui::ToolTimelineStatus::Running;
    case ava::agent::ToolTimelineStatus::Success:
      return ava::tui::ToolTimelineStatus::Success;
    case ava::agent::ToolTimelineStatus::Canceled:
      return ava::tui::ToolTimelineStatus::Canceled;
    case ava::agent::ToolTimelineStatus::Error:
      return ava::tui::ToolTimelineStatus::Error;
  }
  return ava::tui::ToolTimelineStatus::Error;
}

std::string permission_summary_field(std::string_view summary, std::string_view label)
{
  for (auto line : ava::tui::split_lines(std::string(summary)))
  {
    std::string_view view(line);
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t'))
      view.remove_prefix(1);
    if (!view.starts_with(label))
      continue;
    view.remove_prefix(label.size());
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t'))
      view.remove_prefix(1);
    return std::string(view);
  }
  return {};
}

std::vector<ava::tui::ToolPermissionAuditItem> tui_permission_audits(ava::agent::ToolTimelineEntry const& entry)
{
  std::vector<ava::tui::ToolPermissionAuditItem> audits;
  audits.reserve(entry.permission_request_ids.size());
  auto const resolution = permission_summary_field(entry.result_summary, "resolution:");
  auto const decision = resolution == "deny" || entry.result_summary.find("permission_denied:") != std::string::npos ? std::string("deny") : std::string{};
  auto const reason = permission_summary_field(entry.result_summary, "reason:");
  auto const risk = permission_summary_field(entry.result_summary, "risk:");
  auto const command = permission_summary_field(entry.result_summary, "command:");
  for (auto const& id : entry.permission_request_ids)
  {
    audits.push_back(ava::tui::ToolPermissionAuditItem{
        .permission_request_id = id, .decision = decision, .risk = risk, .reason = reason, .command = command, .resolution_reason = resolution});
  }
  return audits;
}

std::vector<ava::tui::ToolTimelineItem> tui_tool_timeline(std::vector<ava::agent::ToolTimelineEntry> const& entries)
{
  struct StartedArguments
  {
    std::string summary;
    std::string json;
  };
  std::unordered_map<std::string, StartedArguments> started_arguments;
  std::unordered_map<std::string, std::size_t> item_indexes;
  std::vector<ava::tui::ToolTimelineItem> items;
  items.reserve(entries.size());
  for (auto const& entry : entries)
  {
    if (entry.status == ava::agent::ToolTimelineStatus::Running && !entry.call_id.empty())
    {
      auto& started = started_arguments[entry.call_id];
      if (started.summary.empty() && !entry.argument_summary.empty())
        started.summary = entry.argument_summary;
      if (started.json.empty() && !entry.arguments_json.empty())
        started.json = entry.arguments_json;
    }
    auto const started = entry.call_id.empty() ? started_arguments.end() : started_arguments.find(entry.call_id);
    auto const argument_summary = started != started_arguments.end() && !started->second.summary.empty() ? started->second.summary : entry.argument_summary;
    auto const arguments_json = started != started_arguments.end() && !started->second.json.empty() ? started->second.json : entry.arguments_json;
    auto item = ava::tui::ToolTimelineItem{.status = tui_tool_status(entry.status),
                                           .name = entry.name,
                                           .argument_summary = argument_summary,
                                           .result_summary = entry.result_summary,
                                           .arguments_json = arguments_json,
                                           .result_json = entry.result_json,
                                           .call_id = entry.call_id,
                                           .lifecycle = entry.status == ava::agent::ToolTimelineStatus::Running ? ava::tui::ToolLifecycleState::ExecutionStarted
                                                                                                                : ava::tui::ToolLifecycleState::Complete,
                                           .permission_request_ids = entry.permission_request_ids,
                                           .permissions = tui_permission_audits(entry),
                                           .diff = entry.diff,
                                           .diff_truncated = entry.diff_truncated,
                                           .changed_paths = entry.changed_paths,
                                           .truncated = entry.truncated,
                                           .byte_limited = entry.byte_limited,
                                           .line_limited = entry.line_limited,
                                           .output_bytes = entry.output_bytes,
                                           .total_bytes = entry.total_bytes,
                                           .output_lines = entry.output_lines,
                                           .total_lines = entry.total_lines,
                                           .start_line = entry.start_line,
                                           .end_line = entry.end_line,
                                           .next_offset_line = entry.next_offset_line,
                                           .omitted_bytes = entry.omitted_bytes,
                                           .omitted_lines = entry.omitted_lines,
                                           .visible_matches = entry.visible_matches,
                                           .total_matches = entry.total_matches,
                                           .spill_path = entry.spill_path,
                                           .spill_truncated = entry.spill_truncated};
    if (entry.status == ava::agent::ToolTimelineStatus::Error)
      item.lifecycle = ava::tui::ToolLifecycleState::Error;
    if (entry.status == ava::agent::ToolTimelineStatus::Canceled)
      item.lifecycle = ava::tui::ToolLifecycleState::Canceled;
    if (entry.call_id.empty())
    {
      items.push_back(std::move(item));
      continue;
    }
    auto const [existing, inserted] = item_indexes.try_emplace(entry.call_id, items.size());
    if (inserted)
      items.push_back(std::move(item));
    else
      items[existing->second] = std::move(item);
  }
  return items;
}

void add_token_component(std::optional<long long>& total, std::optional<long long> value)
{
  if (!value || *value < 0)
    return;
  if (!total)
    total = 0;
  constexpr auto maximum = std::numeric_limits<long long>::max();
  *total = *total > maximum - *value ? maximum : *total + *value;
}

std::optional<long long> compact_token_total(ava::session::SessionStats const& stats)
{
  if (stats.total_tokens)
    return stats.total_tokens;

  std::optional<long long> total;
  add_token_component(total, stats.input_tokens);
  add_token_component(total, stats.output_tokens);
  add_token_component(total, stats.reasoning_tokens);
  add_token_component(total, stats.cache_read_tokens);
  add_token_component(total, stats.cache_write_tokens);
  return total;
}

std::string format_compact_token_count(long long value)
{
  if (value < 1000)
    return std::to_string(value);

  auto const format_scaled = [](long long tenths, std::string_view suffix) {
    std::ostringstream output;
    output << (tenths / 10);
    if (tenths % 10 != 0)
      output << '.' << (tenths % 10);
    output << suffix;
    return output.str();
  };

  if (value < 1'000'000)
    return format_scaled(value / 100, "k");
  return format_scaled(value / 100'000, "m");
}

std::optional<std::string> format_context_window_percent(long long tokens, std::optional<long long> context_window_tokens)
{
  if (!context_window_tokens || *context_window_tokens <= 0)
    return std::nullopt;
  if (tokens <= 0)
    return std::string("0.0%");

  auto const percent = (static_cast<long double>(tokens) * 100.0L) / static_cast<long double>(*context_window_tokens);
  if (percent > 0.0L && percent < 0.1L)
    return std::string("<0.1%");

  std::ostringstream output;
  output << std::fixed << std::setprecision(1) << percent << '%';
  return output.str();
}

std::string format_active_context_status_value(long long tokens, std::optional<long long> context_window_tokens)
{
  auto const count = format_compact_token_count(tokens);
  if (auto const percent = format_context_window_percent(tokens, context_window_tokens))
    return count + " (" + *percent + ')';
  return "~" + count;
}

std::optional<std::string> compact_token_status(ava::session::SessionStats const& stats, std::optional<long long> context_window_tokens)
{
  auto const tokens = compact_token_total(stats);
  if (!tokens)
    return std::nullopt;

  std::ostringstream output;
  output << format_compact_token_count(*tokens);
  if (auto const percent = format_context_window_percent(*tokens, context_window_tokens))
  {
    output << " (" << *percent << ')';
  }
  return output.str();
}

std::optional<std::string> token_status_for_session(ava::app::runtime::session_ts const& unlocked_session)
{
  auto snapshot = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::pair{session_r->read_authority_1(), session_r->model().context_window_tokens};
  }();
  auto& [read_authority, context_window_tokens] = snapshot;
  if (!read_authority)
    return std::nullopt;
  auto entries = read_authority->load();
  if (!entries)
    return std::nullopt;
  auto stats = ava::session::compute_session_stats(*entries);
  if (!stats)
    return std::nullopt;
  return compact_token_status(*stats, context_window_tokens);
}

ava::tui::TodoStatus to_tui_todo_status(ava::agent::TodoStatus status)
{
  switch (status)
  {
    case ava::agent::TodoStatus::Pending:
      return ava::tui::TodoStatus::Pending;
    case ava::agent::TodoStatus::InProgress:
      return ava::tui::TodoStatus::InProgress;
    case ava::agent::TodoStatus::Completed:
      return ava::tui::TodoStatus::Completed;
  }
  return ava::tui::TodoStatus::Pending;
}

std::vector<ava::tui::TodoItem> todos_for_session(runtime::session_ts const& unlocked_session)
{
  auto read_authority = runtime::session_ts::crat(unlocked_session)->read_authority_1();
  if (!read_authority)
    return {};
  auto entries = read_authority->load();
  if (!entries)
    return {};
  auto snapshot = ava::agent::latest_committed_todowrite_snapshot(*entries);
  if (!snapshot)
    return {};
  std::vector<ava::tui::TodoItem> todos;
  todos.reserve(snapshot->todos.size());
  for (auto const& item : snapshot->todos)
  {
    todos.push_back(ava::tui::TodoItem{.id = item.id, .content = item.content, .status = to_tui_todo_status(item.status)});
  }
  return todos;
}

ava::core::Result<std::string> formatted_active_context_status(ava::app::runtime::session_ts const& unlocked_session)
{
  auto snapshot = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->read_authority_1(), session_r->system_prompt(), session_r->model().context_window_tokens};
  }();
  auto& [read_authority, system_prompt, context_window_tokens] = snapshot;
  if (!read_authority)
    return std::unexpected(std::move(read_authority.error()));
  auto entries = read_authority->load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  auto active_tokens = ava::session::estimate_active_context_tokens(*entries);
  if (!active_tokens)
    return std::unexpected(std::move(active_tokens.error()));

  auto const system_prompt_tokens = ava::session::estimate_tokens(system_prompt);
  auto const maximum = std::numeric_limits<std::size_t>::max();
  auto const total_tokens = *active_tokens > maximum - system_prompt_tokens ? maximum : *active_tokens + system_prompt_tokens;
  auto const display_tokens = total_tokens > static_cast<std::size_t>(std::numeric_limits<long long>::max()) ? std::numeric_limits<long long>::max()
                                                                                                             : static_cast<long long>(total_tokens);
  return format_active_context_status_value(display_tokens, context_window_tokens);
}

std::optional<std::string> active_context_status_for_session(ava::app::runtime::session_ts const& unlocked_session)
{
  auto status = formatted_active_context_status(unlocked_session);
  if (!status)
    return std::nullopt;
  return std::move(*status);
}

std::string session_selector_footer_hint(ava::app::SessionSelectorSort sort, bool named_only, bool show_paths, bool show_archived, bool show_label_time)
{
  static_cast<void>(sort);
  static_cast<void>(named_only);
  static_cast<void>(show_paths);
  static_cast<void>(show_archived);
  static_cast<void>(show_label_time);
  return "↑↓ navigate · Enter open · type filter · Esc close · Ctrl+D archive";
}

ava::tui::TuiBranchSummarySnapshot tui_branch_summary_snapshot(BranchSummarySnapshot const& snapshot)
{
  auto map_phase = [](BranchSummaryPhase phase) {
    switch (phase)
    {
      case BranchSummaryPhase::Idle:
        return ava::tui::TuiBranchSummaryPhase::Idle;
      case BranchSummaryPhase::Preparing:
        return ava::tui::TuiBranchSummaryPhase::Preparing;
      case BranchSummaryPhase::AwaitingConfirmation:
        return ava::tui::TuiBranchSummaryPhase::AwaitingConfirmation;
      case BranchSummaryPhase::Generating:
        return ava::tui::TuiBranchSummaryPhase::Generating;
      case BranchSummaryPhase::Revalidating:
        return ava::tui::TuiBranchSummaryPhase::Revalidating;
      case BranchSummaryPhase::Appending:
        return ava::tui::TuiBranchSummaryPhase::Appending;
      case BranchSummaryPhase::Succeeded:
        return ava::tui::TuiBranchSummaryPhase::Succeeded;
      case BranchSummaryPhase::Existing:
        return ava::tui::TuiBranchSummaryPhase::Existing;
      case BranchSummaryPhase::Ineligible:
        return ava::tui::TuiBranchSummaryPhase::Ineligible;
      case BranchSummaryPhase::Canceled:
        return ava::tui::TuiBranchSummaryPhase::Canceled;
      case BranchSummaryPhase::Failed:
        return ava::tui::TuiBranchSummaryPhase::Failed;
    }
    return ava::tui::TuiBranchSummaryPhase::Failed;
  };
  auto map_eligibility = [](BranchSummaryEligibilityCode code) {
    switch (code)
    {
      case BranchSummaryEligibilityCode::CurrentSessionEphemeral:
        return ava::tui::TuiBranchSummaryEligibilityCode::CurrentSessionEphemeral;
      case BranchSummaryEligibilityCode::CurrentSessionUnavailable:
        return ava::tui::TuiBranchSummaryEligibilityCode::CurrentSessionUnavailable;
      case BranchSummaryEligibilityCode::ActiveRun:
        return ava::tui::TuiBranchSummaryEligibilityCode::ActiveRun;
      case BranchSummaryEligibilityCode::InvalidSourceSelection:
        return ava::tui::TuiBranchSummaryEligibilityCode::InvalidSourceSelection;
      case BranchSummaryEligibilityCode::NotDirectSource:
        return ava::tui::TuiBranchSummaryEligibilityCode::NotDirectSource;
      case BranchSummaryEligibilityCode::InvalidFork:
        return ava::tui::TuiBranchSummaryEligibilityCode::InvalidFork;
      case BranchSummaryEligibilityCode::SourceUnavailable:
        return ava::tui::TuiBranchSummaryEligibilityCode::SourceUnavailable;
      case BranchSummaryEligibilityCode::SourceLeaseBusy:
        return ava::tui::TuiBranchSummaryEligibilityCode::SourceLeaseBusy;
      case BranchSummaryEligibilityCode::SourceCorrupt:
        return ava::tui::TuiBranchSummaryEligibilityCode::SourceCorrupt;
      case BranchSummaryEligibilityCode::ForkEntryNotFound:
        return ava::tui::TuiBranchSummaryEligibilityCode::ForkEntryNotFound;
      case BranchSummaryEligibilityCode::EmptySuffix:
        return ava::tui::TuiBranchSummaryEligibilityCode::EmptySuffix;
    }
    return ava::tui::TuiBranchSummaryEligibilityCode::InvalidSourceSelection;
  };
  auto map_failure = [](BranchSummaryFailureCode code) {
    switch (code)
    {
      case BranchSummaryFailureCode::Deadline:
        return ava::tui::TuiBranchSummaryFailureCode::Deadline;
      case BranchSummaryFailureCode::RecoveryFailed:
        return ava::tui::TuiBranchSummaryFailureCode::RecoveryFailed;
      case BranchSummaryFailureCode::ProjectionRecordLimit:
        return ava::tui::TuiBranchSummaryFailureCode::ProjectionRecordLimit;
      case BranchSummaryFailureCode::ProjectionTextLimit:
        return ava::tui::TuiBranchSummaryFailureCode::ProjectionTextLimit;
      case BranchSummaryFailureCode::ProjectionByteLimit:
        return ava::tui::TuiBranchSummaryFailureCode::ProjectionByteLimit;
      case BranchSummaryFailureCode::ProjectionInvalidText:
        return ava::tui::TuiBranchSummaryFailureCode::ProjectionInvalidText;
      case BranchSummaryFailureCode::ProjectionEmpty:
        return ava::tui::TuiBranchSummaryFailureCode::ProjectionEmpty;
      case BranchSummaryFailureCode::ModelUnavailable:
        return ava::tui::TuiBranchSummaryFailureCode::ModelUnavailable;
      case BranchSummaryFailureCode::AuthenticationUnavailable:
        return ava::tui::TuiBranchSummaryFailureCode::AuthenticationUnavailable;
      case BranchSummaryFailureCode::ProviderFailed:
        return ava::tui::TuiBranchSummaryFailureCode::ProviderFailed;
      case BranchSummaryFailureCode::InvalidGeneratedSummary:
        return ava::tui::TuiBranchSummaryFailureCode::InvalidGeneratedSummary;
      case BranchSummaryFailureCode::StaleSource:
        return ava::tui::TuiBranchSummaryFailureCode::StaleSource;
      case BranchSummaryFailureCode::AppendFailed:
        return ava::tui::TuiBranchSummaryFailureCode::AppendFailed;
      case BranchSummaryFailureCode::Internal:
        return ava::tui::TuiBranchSummaryFailureCode::Internal;
    }
    return ava::tui::TuiBranchSummaryFailureCode::Internal;
  };

  std::optional<ava::tui::TuiBranchSummaryAppendCommitState> append_commit_state;
  if (snapshot.append_commit_state == "not_started")
    append_commit_state = ava::tui::TuiBranchSummaryAppendCommitState::NotStarted;
  else if (snapshot.append_commit_state == "partial_or_unknown")
    append_commit_state = ava::tui::TuiBranchSummaryAppendCommitState::PartialOrUnknown;
  else if (snapshot.append_commit_state == "committed_to_leased_inode")
    append_commit_state = ava::tui::TuiBranchSummaryAppendCommitState::CommittedToLeasedInode;

  return ava::tui::TuiBranchSummarySnapshot{
      .generation = snapshot.generation,
      .phase = map_phase(snapshot.phase),
      .source_label = snapshot.source_label,
      .model_label = snapshot.model_label,
      .eligibility_code = snapshot.eligibility_code ? std::optional(map_eligibility(*snapshot.eligibility_code)) : std::nullopt,
      .failure_code = snapshot.failure_code ? std::optional(map_failure(*snapshot.failure_code)) : std::nullopt,
      .append_commit_state = append_commit_state,
      .refresh_required = snapshot.refresh_required};
}

std::string scoped_model_selector_footer_hint()
{
  return "Enter toggle · Ctrl+A enable visible · Ctrl+X clear visible · Ctrl+P provider · Alt+Up/Down reorder · Ctrl+S save · type to filter · Esc cancel";
}

bool contains_value(std::vector<std::string> const& values, std::string_view value)
{
  return std::ranges::find_if(values, [&](auto const& existing) { return existing == value; }) != values.end();
}

std::vector<std::string> registered_model_cycle_values(ava::app::runtime::session_ts const& unlocked_session)
{
  auto view = ava::app::scoped_model_selector_view_1(unlocked_session, {});
  std::vector<std::string> values;
  for (auto const& item : view.items)
  {
    if (item.enabled && !item.value.empty() && !contains_value(values, item.value))
      values.push_back(item.value);
  }
  return values;
}

std::vector<std::string> normalized_model_scope(std::vector<std::string> const& candidate, std::vector<std::string> const& all_values)
{
  std::vector<std::string> normalized;
  normalized.reserve(candidate.size());
  for (auto const& value : candidate)
  {
    if (contains_value(all_values, value) && !contains_value(normalized, value))
      normalized.push_back(value);
  }
  return normalized;
}

void store_model_scope(ava::app::runtime::session_ts& unlocked_session, std::vector<std::string> candidate, std::vector<std::string> const& all_values,
                       bool reset_full_scope_to_all)
{
  auto normalized = normalized_model_scope(candidate, all_values);
  SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
  if (reset_full_scope_to_all && normalized.size() == all_values.size())
  {
    session_w->model_selection().scoped_model_cycle = std::nullopt;
    return;
  }
  session_w->model_selection().scoped_model_cycle = std::move(normalized);
}

std::vector<std::string> active_model_scope_or_all(ava::app::runtime::session_ts const& unlocked_session, std::vector<std::string> const& all_values)
{
  auto const scoped_cycle = runtime::session_ts::crat(unlocked_session)->scoped_model_cycle();
  if (scoped_cycle)
    return normalized_model_scope(*scoped_cycle, all_values);
  return all_values;
}

std::string provider_from_model_value(std::string_view value)
{
  auto const slash = value.find('/');
  if (slash == std::string_view::npos)
    return {};
  return std::string(value.substr(0, slash));
}

ava::tui::SelectListView preserve_scoped_model_selector_state(ava::tui::SelectListView view, ava::tui::SelectListView const& previous)
{
  view.query = previous.query;
  std::string selected_value;
  if (previous.selected_item_index < previous.items.size())
    selected_value = previous.items[previous.selected_item_index].value;
  if (!selected_value.empty())
  {
    for (std::size_t index = 0; index < view.items.size(); ++index)
    {
      if (view.items[index].value == selected_value)
      {
        view.selected_item_index = index;
        break;
      }
    }
  }
  view.selected_item_index = ava::tui::clamp_select_list_selection(view, view.selected_item_index);
  return view;
}

ava::tui::SelectListView refreshed_scoped_model_selector(ava::app::runtime::session_ts const& unlocked_session, ava::tui::SelectListView const& previous)
{
  return preserve_scoped_model_selector_state(ava::app::scoped_model_selector_view_1(unlocked_session, scoped_model_selector_footer_hint()), previous);
}

ava::core::Result<ava::tui::SelectListView> toggle_scoped_model(ava::app::runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                std::string_view value)
{
  auto const all_values = registered_model_cycle_values(unlocked_session);
  if (!contains_value(all_values, value))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "model is not available for scoped cycling"));
  auto const scoped_cycle = runtime::session_ts::rat(unlocked_session)->scoped_model_cycle();
  if (!scoped_cycle)
  {
    runtime::session_ts::wat(unlocked_session)->model_selection().scoped_model_cycle = std::vector<std::string>{std::string(value)};
    return refreshed_scoped_model_selector(unlocked_session, previous);
  }
  auto next = normalized_model_scope(*scoped_cycle, all_values);
  if (contains_value(next, value))
    std::erase(next, std::string(value));
  else
    next.push_back(std::string(value));
  store_model_scope(unlocked_session, std::move(next), all_values, true);
  return refreshed_scoped_model_selector(unlocked_session, previous);
}

ava::core::Result<ava::tui::SelectListView> enable_scoped_models(ava::app::runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                 std::vector<std::string> targets)
{
  auto const all_values = registered_model_cycle_values(unlocked_session);
  auto next = active_model_scope_or_all(unlocked_session, all_values);
  for (auto const& target : targets)
  {
    if (contains_value(all_values, target) && !contains_value(next, target))
      next.push_back(target);
  }
  store_model_scope(unlocked_session, std::move(next), all_values, true);
  return refreshed_scoped_model_selector(unlocked_session, previous);
}

ava::core::Result<ava::tui::SelectListView> clear_scoped_models(ava::app::runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                std::vector<std::string> targets)
{
  auto const all_values = registered_model_cycle_values(unlocked_session);
  auto next = active_model_scope_or_all(unlocked_session, all_values);
  for (auto const& target : targets)
  {
    std::erase(next, target);
  }
  store_model_scope(unlocked_session, std::move(next), all_values, true);
  return refreshed_scoped_model_selector(unlocked_session, previous);
}

ava::core::Result<ava::tui::SelectListView> toggle_scoped_model_provider(ava::app::runtime::session_ts& unlocked_session,
                                                                         ava::tui::SelectListView const& previous, std::string_view selected_value)
{
  auto const provider = provider_from_model_value(selected_value);
  if (provider.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "model selection is missing provider"));
  auto const all_values = registered_model_cycle_values(unlocked_session);
  std::vector<std::string> provider_values;
  for (auto const& value : all_values)
  {
    if (provider_from_model_value(value) == provider)
      provider_values.push_back(value);
  }
  auto next = active_model_scope_or_all(unlocked_session, all_values);
  bool const provider_enabled =
      !provider_values.empty() && std::ranges::all_of(provider_values, [&](auto const& value) { return contains_value(next, value); });
  if (provider_enabled)
  {
    for (auto const& value : provider_values)
      std::erase(next, value);
  }
  else
  {
    for (auto const& value : provider_values)
    {
      if (!contains_value(next, value))
        next.push_back(value);
    }
  }
  store_model_scope(unlocked_session, std::move(next), all_values, true);
  return refreshed_scoped_model_selector(unlocked_session, previous);
}

ava::core::Result<ava::tui::SelectListView> reorder_scoped_model(ava::app::runtime::session_ts& unlocked_session, ava::tui::SelectListView const& previous,
                                                                 std::string_view selected_value, bool up)
{
  auto const all_values = registered_model_cycle_values(unlocked_session);
  auto next = active_model_scope_or_all(unlocked_session, all_values);
  auto const selected = std::string(selected_value);
  auto const found = std::ranges::find(next, selected);
  if (found == next.end())
    return refreshed_scoped_model_selector(unlocked_session, previous);
  auto const index = static_cast<std::size_t>(found - next.begin());
  if ((up && index == 0) || (!up && index + 1 >= next.size()))
    return refreshed_scoped_model_selector(unlocked_session, previous);
  auto const other = up ? index - 1 : index + 1;
  std::swap(next[index], next[other]);
  store_model_scope(unlocked_session, std::move(next), all_values, false);
  return refreshed_scoped_model_selector(unlocked_session, previous);
}

ava::core::Result<std::string> save_scoped_model_cycle(ava::app::runtime::session_ts& unlocked_session)
{
  auto scope_to_save = runtime::session_ts::rat(unlocked_session)->scoped_model_cycle();
  if (scope_to_save)
  {
    auto const all_values = registered_model_cycle_values(unlocked_session);
    scope_to_save = normalized_model_scope(*scope_to_save, all_values);
    runtime::session_ts::wat(unlocked_session)->model_selection().scoped_model_cycle = scope_to_save;
  }

  auto const paths = runtime::session_ts::rat(unlocked_session)->paths();
  auto saved = ava::config::store_scoped_model_cycle(paths, scope_to_save);
  if (!saved)
    return std::unexpected(std::move(saved.error()));

  if (!scope_to_save)
    return std::string("scoped model cycle saved: all registered models enabled");
  if (scope_to_save->size() == 1)
    return std::string("scoped model cycle saved: 1 model enabled");
  return std::string("scoped model cycle saved: ") + std::to_string(scope_to_save->size()) + " models enabled";
}

ava::permissions::PermissionRuleMode permission_rule_mode_for_agent_mode(ava::core::Mode mode)
{
  switch (mode)
  {
    case ava::core::Mode::Build:
      return ava::permissions::PermissionRuleMode::Build;
    case ava::core::Mode::Plan:
      return ava::permissions::PermissionRuleMode::Plan;
  }
  return ava::permissions::PermissionRuleMode::Any;
}

ava::core::Result<ava::tui::TuiRememberedPermissionRule> remember_permission_rule_for_prompt(ava::app::runtime::session_ts const& unlocked_session,
                                                                                             ava::permissions::PermissionPrompt const& prompt,
                                                                                             ava::permissions::PermissionAction action, std::string actor)
{
  auto reason = prompt.reason.empty() ? std::string("remembered from TUI permission prompt") : prompt.reason;
  std::string recipe_key;
  std::string recipe_display;
  if (prompt.operation == ava::permissions::Operation::RunCommand)
  {
    auto const reusable = prompt.command_metadata && ava::permissions::command_permission_allows_reusable_grant(*prompt.command_metadata);
    if (action == ava::permissions::PermissionAction::Allow && !ava::permissions::command_prompt_allows_persistent_allow(prompt))
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied,
                                              "this command cannot be remembered because no reusable sealed workspace recipe is available"));
    }
    if (reusable)
    {
      recipe_key = prompt.command_metadata->workspace_recipe_key;
      recipe_display = prompt.command_metadata->recipe_display;
    }
  }
  auto const rule_store = runtime::session_ts::crat(unlocked_session)->permission_rule_store();
  auto added = ava::permissions::add_persistent_permission_rule(
      rule_store, ava::permissions::PermissionRuleDraft{
                      .scope = ava::permissions::PermissionRuleScope::Workspace,
                      .action = action,
                      .operation = prompt.operation,
                      .mode = permission_rule_mode_for_agent_mode(prompt.mode),
                      .tool_name = prompt.tool_name,
                      .target_path = prompt.target_path,
                      .command = prompt.operation == ava::permissions::Operation::RunCommand && !recipe_key.empty() ? std::string{} : prompt.command,
                      .command_recipe_key = std::move(recipe_key),
                      .recipe_display = std::move(recipe_display),
                      .reason = std::move(reason),
                      .actor = std::move(actor)});
  if (!added)
    return std::unexpected(std::move(added.error()));
  return ava::tui::TuiRememberedPermissionRule{.rule_id = added->rule_id};
}

bool workspace_catalog_changed(InteractiveResult const& result)
{
  return std::ranges::any_of(result.tool_timeline, [](ava::agent::ToolTimelineEntry const& entry) {
    return entry.status == ava::agent::ToolTimelineStatus::Success && !entry.changed_paths.empty();
  });
}

void capture_tui_request_presentation(TuiRequestPresentation& presentation, bool initial_is_local_command, std::string_view request_line,
                                      std::string const& request_id, InteractiveResult const& request_result)
{
  if (request_result.ordinary_turn_committed)
  {
    presentation.ordinary_turn_request_ids.push_back(request_id);
    presentation.conversation.output.insert(presentation.conversation.output.end(), request_result.output.begin(), request_result.output.end());
    presentation.conversation.tool_timeline.insert(presentation.conversation.tool_timeline.end(), request_result.tool_timeline.begin(),
                                                   request_result.tool_timeline.end());
    return;
  }
  if (!initial_is_local_command && !request_line.starts_with('/') && !request_line.starts_with('!'))
    return;
  presentation.has_local_command = true;
  presentation.local_command.output.insert(presentation.local_command.output.end(), request_result.output.begin(), request_result.output.end());
  presentation.local_command.tool_timeline.insert(presentation.local_command.tool_timeline.end(), request_result.tool_timeline.begin(),
                                                  request_result.tool_timeline.end());
}

bool workspace_catalog_reload_requested(std::string_view submitted)
{
  auto const separator = submitted.find_first_of(" \t\r\n");
  auto const command = submitted.substr(0, separator);
  if (command != "/reload")
    return false;
  if (separator == std::string_view::npos)
    return true;
  auto arguments = submitted.substr(separator + 1);
  auto const first = arguments.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return true;
  auto const end = arguments.find_first_of(" \t\r\n", first);
  return arguments.substr(first, end - first) == "all";
}

bool run_queued_follow_ups_until_session_transition(InteractiveResult& result, bool& workspace_catalog_reload, std::string_view initial_session_id,
                                                    ava::tui::TuiSubmitContext const& context, std::function<std::string()> const& current_session_id,
                                                    std::function<InteractiveResult(ava::tui::TuiQueuedFollowUp const&)> const& run_follow_up)
{
  if (current_session_id() != initial_session_id)
    return true;

  while (!result.quit && (!context.cancel_requested || !context.cancel_requested()))
  {
    if (context.skip_active_steering)
    {
      if (auto skipped = context.skip_active_steering("run_completed_before_safe_point"); !skipped)
      {
        add_output(result, skipped.error().format());
        break;
      }
    }
    if (!context.take_next_follow_up)
      break;
    auto follow_up = context.take_next_follow_up();
    if (!follow_up)
      break;
    if (context.mark_follow_up_started)
    {
      if (auto started = context.mark_follow_up_started(*follow_up); !started)
      {
        add_output(result, started.error().format());
        break;
      }
    }

    workspace_catalog_reload = workspace_catalog_reload || workspace_catalog_reload_requested(follow_up->message);
    auto next = run_follow_up(*follow_up);
    auto const session_changed = current_session_id() != initial_session_id;
    if (session_changed)
    {
      next.quit = next.quit || result.quit;
      next.session_tree_changed = next.session_tree_changed || result.session_tree_changed;
      next.ordinary_turn_committed = next.ordinary_turn_committed || result.ordinary_turn_committed;
      result = std::move(next);
      return true;
    }

    result.quit = result.quit || next.quit;
    result.session_tree_changed = result.session_tree_changed || next.session_tree_changed;
    result.ordinary_turn_committed = result.ordinary_turn_committed || next.ordinary_turn_committed;
    result.output.insert(result.output.end(), std::make_move_iterator(next.output.begin()), std::make_move_iterator(next.output.end()));
    result.tool_timeline.insert(result.tool_timeline.end(), std::make_move_iterator(next.tool_timeline.begin()),
                                std::make_move_iterator(next.tool_timeline.end()));
  }
  return false;
}

}  // namespace ava::app::interactive_internal

namespace ava::app {

std::vector<ava::tui::ToolTimelineItem> tool_timeline_for_tui(std::vector<ava::agent::ToolTimelineEntry> const& entries)
{
  return interactive_internal::tui_tool_timeline(entries);
}

}  // namespace ava::app
