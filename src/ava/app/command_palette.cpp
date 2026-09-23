#include "sys.h"
#include "ava/app/command_format.h"
#include "ava/app/command_models.h"
#include "ava/app/command_palette.h"
#include "ava/app/command_permissions.h"
#include "ava/app/display_settings.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/app/session_user_turns.h"
#include "ava/tui/keybindings.h"
#include "ava/plugin/diagnostics.h"
#include "ava/mcp/config.h"
#include "ava/config/model_config.h"
#include "ava/config/provider_profiles.h"
#include "ava/session/session_metadata.h"
#include "ava/session/session_store.h"
#include "ava/session/session_tree.h"
#include "ava/permissions/permission_rules.h"
#include "ava/provider/catalog.h"
#include "ava/provider/registry.h"
#include "ava/core/error.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

constexpr std::size_t kMaxPathCompletionVisited = 20000;
constexpr std::size_t kMaxPathCompletions = 2000;
constexpr std::size_t kMaxPathCompletionDepth = 8;

std::string hotkeys_for_action(std::vector<CommandHotkey> const& hotkeys, std::string_view action)
{
  for (auto const& hotkey : hotkeys)
  {
    if (hotkey.action == action)
      return hotkey.keys;
  }
  return "";
}

// CommandHotkey.description carries the concise human action label for palette rows.
std::string keybinding_action_display_label(CommandHotkey const& hotkey)
{
  if (!hotkey.description.empty())
    return hotkey.description;
  if (auto const action = ava::tui::key_binding_action_from_name(hotkey.action))
  {
    auto label = ava::tui::action_label(*action);
    if (!label.empty())
      return label;
  }
  return hotkey.action;
}

// Completion secondary text prioritizes effective keys, then the canonical machine id.
std::string keybinding_action_completion_description(CommandHotkey const& hotkey)
{
  if (!hotkey.keys.empty() && !hotkey.action.empty() && hotkey.keys != hotkey.action)
    return hotkey.keys + " · " + hotkey.action;
  if (!hotkey.keys.empty())
    return hotkey.keys;
  return hotkey.action;
}

std::string_view human_job_execution_label(ava::agent::SubagentExecutionState state) noexcept
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
  return "Unknown";
}

std::string_view human_job_mode_label(ava::agent::SubagentJobMode mode) noexcept
{
  switch (mode)
  {
    case ava::agent::SubagentJobMode::Foreground:
      return "Foreground";
    case ava::agent::SubagentJobMode::Background:
      return "Background";
  }
  return "Unknown";
}

// Process-local title first; never surface raw job ids or ordinals as the primary completion label.
std::string job_completion_display_label(ava::agent::SubagentJobSnapshot const& job)
{
  if (!job.display_title.empty())
    return sanitize_inline_text(job.display_title);
  if (job.mode == ava::agent::SubagentJobMode::Background)
    return "Background job";
  return "Foreground job";
}

// Concise human status/mode/type plus a short unique ref — no prompts, summaries, or full authority ids.
std::string job_completion_description(ava::agent::SubagentJobSnapshot const& job, std::string_view short_ref)
{
  std::string description;
  description += human_job_execution_label(job.execution);
  description += " · ";
  description += human_job_mode_label(job.mode);
  if (!job.display_subagent_type.empty())
  {
    description += " · ";
    description += sanitize_inline_text(job.display_subagent_type);
  }
  if (!short_ref.empty())
  {
    description += " · ref ";
    description += short_ref;
  }
  return description;
}

bool completion_exists(std::vector<tui::SlashCommandArgumentCompletion> const& completions, std::size_t argument_index,
                       std::vector<std::string> const& previous_args, std::string_view value)
{
  return std::ranges::any_of(completions, [&](auto const& completion) {
    return completion.argument_index == argument_index && completion.required_previous_args == previous_args && completion.value == value;
  });
}

void add_completion(tui::SlashCommandItem& item, std::size_t argument_index, std::string value, std::string description = {}, std::string category = {},
                    std::vector<std::string> previous_args = {}, bool append_space = true, bool enabled = true, std::string disabled_reason = {},
                    std::string display_label = {})
{
  if (value.empty() || completion_exists(item.argument_completions, argument_index, previous_args, value))
    return;
  item.argument_completions.push_back(tui::SlashCommandArgumentCompletion{.value = std::move(value),
                                                                          .display_label = std::move(display_label),
                                                                          .description = std::move(description),
                                                                          .category = std::move(category),
                                                                          .required_previous_args = std::move(previous_args),
                                                                          .argument_index = argument_index,
                                                                          .append_space = append_space,
                                                                          .enabled = enabled,
                                                                          .disabled_reason = std::move(disabled_reason)});
}

std::optional<std::size_t> find_item_index(std::vector<tui::SlashCommandItem> const& items, std::string_view command)
{
  for (std::size_t index = 0; index < items.size(); ++index)
  {
    if (items[index].command == command)
      return index;
  }
  return std::nullopt;
}

bool has_ascii_space(std::string_view text)
{
  return text.find_first_of(" \t\r\n") != std::string_view::npos;
}

bool has_unquotable_file_reference_char(std::string_view text)
{
  return text.find_first_of("\"\t\r\n") != std::string_view::npos;
}

bool path_is_under(std::filesystem::path const& path, std::filesystem::path const& root)
{
  auto const relative = path.lexically_normal().lexically_relative(root.lexically_normal());
  return !relative.empty() && relative.native().find("..") != 0 && !relative.is_absolute();
}

bool is_reference_code_path(runtime::session_ts const& unlocked_session, std::filesystem::path const& path)
{
  auto const workspace_dir = runtime::session_ts::crat(unlocked_session)->workspace_dir();
  return path_is_under(path, workspace_dir / "docs" / "reference-code") ||
         path.lexically_normal() == (workspace_dir / "docs" / "reference-code").lexically_normal();
}

bool should_skip_path_completion_entry(runtime::session_ts const& unlocked_session, std::filesystem::directory_entry const& entry)
{
  auto const name = entry.path().filename().generic_string();
  if (name == ".git" || name == "node_modules" || name == ".cache" || name == ".ccache")
    return true;
  if (name == "build" || name.starts_with("build-"))
    return true;
  return is_reference_code_path(unlocked_session, entry.path());
}

std::optional<std::string> completion_relative_path(std::filesystem::path const& base, std::filesystem::path const& path, bool directory, bool allow_spaces)
{
  std::error_code error;
  auto relative = std::filesystem::relative(path, base, error);
  if (error || relative.empty() || relative == ".")
    return std::nullopt;
  auto value = relative.generic_string();
  if (value.empty() || value.starts_with("../") || value == ".." || value.starts_with('/'))
    return std::nullopt;
  if (directory && !value.ends_with('/'))
    value.push_back('/');
  if (has_unquotable_file_reference_char(value))
    return std::nullopt;
  if (!allow_spaces && has_ascii_space(value))
    return std::nullopt;
  return value;
}

std::string path_completion_description(std::filesystem::directory_entry const& entry, bool directory)
{
  if (directory)
    return "directory";

  std::error_code error;
  auto const size = entry.file_size(error);
  if (error)
    return "file";
  return "file " + std::to_string(size) + " bytes";
}

std::vector<WorkspacePathCandidate> walk_workspace_path_candidates(runtime::session_ts const& unlocked_session)
{
  std::vector<WorkspacePathCandidate> candidates;
  auto const current_dir = runtime::session_ts::crat(unlocked_session)->current_dir();
  std::error_code error;
  if (!std::filesystem::is_directory(current_dir, error) || error)
    return candidates;

  std::size_t visited = 0;
  for (std::filesystem::recursive_directory_iterator it(current_dir, error), end; it != end && visited < kMaxPathCompletionVisited; it.increment(error))
  {
    if (error)
    {
      error.clear();
      continue;
    }

    auto const entry = *it;
    ++visited;
    if (should_skip_path_completion_entry(unlocked_session, entry))
    {
      if (entry.is_directory(error))
        it.disable_recursion_pending();
      error.clear();
      continue;
    }

    auto const directory = entry.is_directory(error);
    if (error)
    {
      error.clear();
      continue;
    }
    if (static_cast<std::size_t>(it.depth()) >= kMaxPathCompletionDepth)
      it.disable_recursion_pending();

    auto value = completion_relative_path(current_dir, entry.path(), directory, true);
    if (!value)
      continue;
    candidates.push_back(
        WorkspacePathCandidate{.value = std::move(*value), .description = path_completion_description(entry, directory), .directory = directory});
  }
  return candidates;
}

std::vector<WorkspacePathCandidate> prepare_workspace_path_candidates(std::vector<WorkspacePathCandidate> candidates, bool allow_spaces)
{
  if (!allow_spaces)
  {
    std::erase_if(candidates, [](WorkspacePathCandidate const& candidate) { return has_ascii_space(candidate.value); });
  }
  std::ranges::sort(candidates, [](WorkspacePathCandidate const& left, WorkspacePathCandidate const& right) {
    if (left.directory != right.directory)
      return left.directory > right.directory;
    return left.value < right.value;
  });
  if (candidates.size() > kMaxPathCompletions)
    candidates.resize(kMaxPathCompletions);
  return candidates;
}

std::vector<tui::FileReferenceItem> file_reference_items_from_candidates(std::vector<WorkspacePathCandidate> candidates)
{
  candidates = prepare_workspace_path_candidates(std::move(candidates), true);
  std::vector<tui::FileReferenceItem> items;
  items.reserve(candidates.size());
  for (auto const& candidate : candidates)
  {
    items.push_back(tui::FileReferenceItem{.value = candidate.value,
                                           .description = candidate.description,
                                           .category = "Files",
                                           .directory = candidate.directory,
                                           .enabled = true,
                                           .disabled_reason = {}});
  }
  return items;
}

std::string glob_completion_value(WorkspacePathCandidate const& candidate)
{
  if (!candidate.directory)
    return candidate.value;
  auto value = candidate.value;
  if (!value.empty() && value.ends_with('/'))
    value.pop_back();
  value += "/**";
  return value;
}

void add_path_completions(tui::SlashCommandItem& item, std::vector<WorkspacePathCandidate> const& candidates, std::size_t argument_index,
                          bool file_append_space)
{
  for (auto const& candidate : candidates)
  {
    add_completion(item, argument_index, candidate.value, candidate.description, "Files", {}, candidate.directory ? false : file_append_space);
  }
}

void add_glob_completions(tui::SlashCommandItem& item, std::vector<WorkspacePathCandidate> const& candidates, std::size_t argument_index)
{
  for (auto const& candidate : candidates)
  {
    auto description = candidate.directory ? std::string("directory glob") : candidate.description;
    add_completion(item, argument_index, glob_completion_value(candidate), std::move(description), "Files", {}, false);
  }
}

std::string model_completion_description(ava::config::ModelInfo const& model, bool registered)
{
  auto description = model.provider_id + "/" + model.model_id;
  auto const diagnostics = model_configuration_diagnostics(model, registered);
  if (!registered)
  {
    if (!description.empty())
      description += " - ";
    description += "provider unavailable";
  }
  else if (!diagnostics.empty())
  {
    description += " - diagnostics " + std::to_string(diagnostics.size());
  }
  else if (model.supports_reasoning.value_or(false) && !model.reasoning_levels.empty())
  {
    description += " - reasoning levels: ";
    for (std::size_t index = 0; index < model.reasoning_levels.size(); ++index)
    {
      if (index > 0)
        description += ", ";
      description += model.reasoning_levels[index];
    }
  }
  return description;
}

std::string provider_display_name(std::string_view provider_id)
{
  if (provider_id == "openai")
    return "OpenAI";
  if (provider_id == "anthropic")
    return "Anthropic";
  if (provider_id == "google")
    return "Google";
  if (provider_id == "azure")
    return "Azure";
  auto display = std::string(provider_id);
  if (!display.empty())
    display.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(display.front())));
  return display;
}

tui::SelectListItemView model_selector_item(ava::config::ModelInfo const& model, ava::config::ModelInfo const& current_model, bool registered)
{
  auto const current = model.provider_id == current_model.provider_id && model.model_id == current_model.model_id;
  auto label = model.display_name.empty() ? model.model_id : model.display_name;
  return tui::SelectListItemView{.value = model.provider_id + "/" + model.model_id,
                                 .label = std::move(label),
                                 .description = {},
                                 .group = provider_display_name(model.provider_id),
                                 .detail = {},
                                 .badge = {},
                                 .current = current,
                                 .enabled = registered,
                                 .disabled_reason = registered ? std::string{} : std::string("provider unavailable")};
}

std::string model_selector_value(ava::config::ModelInfo const& model)
{
  return model.provider_id + "/" + model.model_id;
}

bool scoped_model_enabled(std::optional<std::vector<std::string>> const& scoped_model_cycle, std::string_view value)
{
  if (!scoped_model_cycle)
    return true;
  return std::ranges::find_if(*scoped_model_cycle, [&](auto const& existing) { return existing == value; }) != scoped_model_cycle->end();
}

std::vector<ava::config::ModelInfo> scoped_model_selector_models(std::vector<ava::config::ModelInfo> models,
                                                                 std::optional<std::vector<std::string>> const& scoped_model_cycle)
{
  if (!scoped_model_cycle)
    return models;

  std::vector<ava::config::ModelInfo> sorted;
  sorted.reserve(models.size());
  for (auto const& id : *scoped_model_cycle)
  {
    auto const found = std::ranges::find_if(models, [&](auto const& model) { return model_selector_value(model) == id; });
    if (found != models.end())
      sorted.push_back(*found);
  }
  for (auto const& model : models)
  {
    auto const value = model_selector_value(model);
    auto const already_added =
        std::ranges::find_if(sorted, [&](auto const& existing) { return existing.provider_id == model.provider_id && existing.model_id == model.model_id; });
    if (already_added == sorted.end())
      sorted.push_back(model);
  }
  return sorted;
}

tui::SelectListItemView scoped_model_selector_item(ava::config::ModelInfo const& model, ava::config::ModelInfo const& current_model,
                                                   std::optional<std::vector<std::string>> const& scoped_model_cycle, bool registered)
{
  auto item = model_selector_item(model, current_model, registered);
  auto const value = model_selector_value(model);
  bool const enabled_for_cycle = scoped_model_enabled(scoped_model_cycle, value);
  item.value = value;
  item.description.clear();
  item.badge = enabled_for_cycle ? std::string("enabled") : std::string("disabled");
  item.detail.clear();
  item.enabled = registered;
  item.disabled_reason = registered ? std::string{} : std::string("provider unavailable");
  return item;
}

std::string session_sort_label(SessionSelectorSort sort)
{
  return session_selector_sort_label(sort);
}

std::string labels_text(std::vector<std::string> const& labels)
{
  std::string text;
  for (std::size_t index = 0; index < labels.size(); ++index)
  {
    if (index > 0)
      text += ", ";
    text += labels[index];
  }
  return text;
}

std::string session_completion_description(ava::session::SessionTreeNode const& node)
{
  std::string description = node.summary.session_id + " · entries=" + std::to_string(node.summary.entry_count);
  if (!node.summary.last_updated.empty())
    description += " updated=" + node.summary.last_updated;
  if (!node.metadata.branch_origin.empty())
    description += " origin=" + node.metadata.branch_origin;
  if (node.metadata.archived)
    description += " archived";
  if (!node.metadata.labels.empty())
    description += " labels=" + labels_text(node.metadata.labels);
  return description;
}

std::string session_node_label(ava::session::SessionTreeNode const& node, std::size_t depth)
{
  auto label = node.metadata.effective_title().empty() ? std::string("Untitled session") : node.metadata.effective_title();
  if (depth == 0)
    return label;
  return std::string(depth * 2, ' ') + "↳ " + label;
}

std::string session_node_description(ava::session::SessionTreeNode const& node, bool show_paths, bool show_label_time)
{
  std::vector<std::string> parts;
  if (!node.metadata.labels.empty())
    parts.push_back(labels_text(node.metadata.labels));
  if (show_label_time && !node.metadata.labels_updated.empty())
    parts.push_back("labels updated " + node.metadata.labels_updated);
  if (show_paths)
    parts.push_back(node.summary.path.empty() ? std::string("path unavailable") : node.summary.path.generic_string());

  std::string description;
  for (auto const& part : parts)
  {
    if (!description.empty())
      description += " · ";
    description += part;
  }
  return description;
}

std::string session_node_badge(ava::session::SessionTreeNode const& node)
{
  return node.metadata.archived ? std::string("archived") : std::string{};
}

tui::SelectListItemView session_selector_item(ava::session::SessionSummary const& summary, std::string_view current_session_id, bool show_paths)
{
  auto const current = !current_session_id.empty() && summary.session_id == current_session_id;
  auto path = summary.path.empty() ? std::string("path unavailable") : summary.path.generic_string();
  return tui::SelectListItemView{.value = summary.session_id,
                                 .label = summary.title.empty() ? std::string("Untitled session") : summary.title,
                                 .description = show_paths ? std::move(path) : std::string{},
                                 .group = {},
                                 .detail = {},
                                 .badge = {},
                                 .current = current,
                                 .enabled = true,
                                 .disabled_reason = {}};
}

bool node_less(ava::session::SessionTreeNode const& left, ava::session::SessionTreeNode const& right, SessionSelectorSort sort)
{
  switch (sort)
  {
    case SessionSelectorSort::Recent:
      if (left.summary.last_updated != right.summary.last_updated)
        return left.summary.last_updated > right.summary.last_updated;
      return left.summary.session_id > right.summary.session_id;
    case SessionSelectorSort::Name: {
      auto const left_name = left.metadata.effective_title().empty() ? left.summary.session_id : left.metadata.effective_title();
      auto const right_name = right.metadata.effective_title().empty() ? right.summary.session_id : right.metadata.effective_title();
      if (left_name != right_name)
        return left_name < right_name;
      return left.summary.session_id < right.summary.session_id;
    }
    case SessionSelectorSort::Path:
      if (left.summary.path.generic_string() != right.summary.path.generic_string())
      {
        return left.summary.path.generic_string() < right.summary.path.generic_string();
      }
      return left.summary.session_id < right.summary.session_id;
  }
  return left.summary.session_id < right.summary.session_id;
}

void sort_session_summaries(std::vector<ava::session::SessionSummary>& summaries, SessionSelectorSort sort)
{
  std::ranges::sort(summaries, [&](ava::session::SessionSummary const& left, ava::session::SessionSummary const& right) {
    switch (sort)
    {
      case SessionSelectorSort::Recent:
        if (left.last_updated != right.last_updated)
          return left.last_updated > right.last_updated;
        return left.session_id > right.session_id;
      case SessionSelectorSort::Name: {
        auto const& left_title = left.title.empty() ? left.session_id : left.title;
        auto const& right_title = right.title.empty() ? right.session_id : right.title;
        if (left_title != right_title)
          return left_title < right_title;
        return left.session_id < right.session_id;
      }
      case SessionSelectorSort::Path:
        if (left.path.generic_string() != right.path.generic_string())
        {
          return left.path.generic_string() < right.path.generic_string();
        }
        return left.session_id < right.session_id;
    }
    return left.session_id < right.session_id;
  });
}

std::unordered_map<std::string, std::size_t> session_tree_index_by_id(std::vector<ava::session::SessionTreeNode> const& nodes)
{
  std::unordered_map<std::string, std::size_t> index;
  for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index)
  {
    index.emplace(nodes[node_index].summary.session_id, node_index);
  }
  return index;
}

std::vector<std::string> sorted_tree_ids(std::vector<std::string> ids, std::vector<ava::session::SessionTreeNode> const& nodes,
                                         std::unordered_map<std::string, std::size_t> const& index_by_id, SessionSelectorSort sort)
{
  std::erase_if(ids, [&](std::string const& id) { return index_by_id.find(id) == index_by_id.end(); });
  std::ranges::sort(
      ids, [&](std::string const& left, std::string const& right) { return node_less(nodes[index_by_id.at(left)], nodes[index_by_id.at(right)], sort); });
  return ids;
}

void append_session_tree_items(tui::SelectListView& view, std::vector<ava::session::SessionTreeNode> const& nodes,
                               std::unordered_map<std::string, std::size_t> const& index_by_id, std::vector<std::string> ids, SessionSelectorSort sort,
                               std::size_t depth, bool named_only, bool show_paths, bool show_archived, bool show_label_time)
{
  ids = sorted_tree_ids(std::move(ids), nodes, index_by_id, sort);
  for (auto const& id : ids)
  {
    auto const found = index_by_id.find(id);
    if (found == index_by_id.end())
      continue;
    auto const& node = nodes[found->second];
    auto const visible = show_archived || !node.metadata.archived;
    if (visible && (!named_only || !node.metadata.effective_title().empty()))
    {
      if (node.current)
        view.selected_item_index = view.items.size();
      view.items.push_back(tui::SelectListItemView{.value = node.summary.session_id,
                                                   .label = session_node_label(node, depth),
                                                   .description = session_node_description(node, show_paths, show_label_time),
                                                   .group = {},
                                                   .detail = {},
                                                   .badge = session_node_badge(node),
                                                   .current = node.current,
                                                   .enabled = true,
                                                   .disabled_reason = {}});
    }
    append_session_tree_items(view, nodes, index_by_id, node.children, sort, depth + (visible ? 1 : 0), named_only, show_paths, show_archived, show_label_time);
  }
}

void add_parent_summary_hint(tui::SelectListView& view, ava::session::SessionTreeIndex const& tree, std::string summarize_parent_keys)
{
  auto const current =
      std::ranges::find_if(tree.sessions, [&](ava::session::SessionTreeNode const& node) { return node.summary.session_id == tree.current_session_id; });
  if (current == tree.sessions.end() || current->metadata.parent_session_id.empty())
    return;
  auto const parent = std::ranges::find_if(view.items, [&](tui::SelectListItemView const& item) { return item.value == current->metadata.parent_session_id; });
  if (parent == view.items.end())
    return;
  if (summarize_parent_keys.size() > 64)
    summarize_parent_keys.resize(64);
  auto hint = summarize_parent_keys.empty() ? std::string("bind app.sessions.summarizeParent") : summarize_parent_keys + " summarize abandoned parent";
  if (parent->detail.empty())
    parent->detail = std::move(hint);
  else
    parent->detail += " · " + hint;
}

void add_backend_argument_completions(std::vector<tui::SlashCommandItem>& items, runtime::session_ts const& unlocked_session,
                                      std::vector<CommandHotkey> const& hotkeys, std::vector<WorkspacePathCandidate> const& path_completions,
                                      ava::session::SessionTreeIndex const* session_tree)
{
  auto [paths, workspace_dir, include_project_resources, subagent_coordinator, session_id, anchor_set, context_sources] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->paths(),
                      session_r->workspace_dir(),
                      project_resources_trusted(session_r->project_trust()),
                      session_r->subagent_coordinator(),
                      session_r->store.session_id(),
                      session_r->anchor_set(),
                      session_r->context_sources()};
  }();
  auto const resource_policy = runtime::make_extension_resource_policy(paths, workspace_dir, include_project_resources);
  if (!path_completions.empty())
  {
    if (auto index = find_item_index(items, "/read"))
      add_path_completions(items[*index], path_completions, 0, false);
    if (auto index = find_item_index(items, "/attach"))
      add_path_completions(items[*index], path_completions, 0, false);
    if (auto index = find_item_index(items, "/write"))
      add_path_completions(items[*index], path_completions, 0, true);
    if (auto index = find_item_index(items, "/glob"))
      add_glob_completions(items[*index], path_completions, 0);
    if (auto index = find_item_index(items, "/find"))
      add_glob_completions(items[*index], path_completions, 0);
    if (auto index = find_item_index(items, "/ls"))
      add_path_completions(items[*index], path_completions, 0, false);
    if (auto index = find_item_index(items, "/grep"))
      add_glob_completions(items[*index], path_completions, 1);
    if (auto index = find_item_index(items, "/import"))
      add_path_completions(items[*index], path_completions, 0, false);
  }

  if (auto index = find_item_index(items, "/models"))
  {
    auto& item = items[*index];
    auto catalog = runtime::session_ts::crat(unlocked_session)->ensure_provider_catalog();
    if (auto registry = ava::config::load_model_registry(paths))
    {
      for (auto const& model : ava::config::effective_models(*registry))
      {
        auto const registered = catalog->contains(model.provider_id);
        add_completion(item, 0, model.provider_id + "/" + model.model_id, model_completion_description(model, registered), "Models", {}, false, registered,
                       registered ? "" : "provider is not registered", model.display_name.empty() ? model.model_id : model.display_name);
      }
    }
  }

  if (auto index = find_item_index(items, "/hotkeys"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "init", "Create $XDG_CONFIG_HOME/ava/keybinds.json from AVA defaults", "General");
    add_completion(item, 0, "import", "Import a validated keybindings JSON file", "General", {}, false);
    add_completion(item, 0, "set", "Set one keybinding action in keybinds.json", "General");
    add_completion(item, 0, "reset", "Remove one action override from keybinds.json", "General");
    add_completion(item, 0, "unset", "Alias for reset", "General");
    add_completion(item, 0, "validate", "Validate $XDG_CONFIG_HOME/ava/keybinds.json without reloading", "General", {}, false);
    for (auto const& hotkey : hotkeys)
    {
      auto const label = keybinding_action_display_label(hotkey);
      auto const description = keybinding_action_completion_description(hotkey);
      add_completion(item, 1, hotkey.action, description, "Keybindings", {"set"}, true, true, {}, label);
      add_completion(item, 1, hotkey.action, description, "Keybindings", {"reset"}, true, true, {}, label);
      add_completion(item, 1, hotkey.action, description, "Keybindings", {"unset"}, true, true, {}, label);
    }
    add_completion(item, 1, "--force", "Replace an existing keybinds.json starter file", "General", {"init"}, false);
    add_completion(item, 2, "--force", "Replace the existing keybinds.json with the imported file", "General", {"import"}, false);
  }

  if (auto index = find_item_index(items, "/details"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "compact", "Use one fitted summary row per tool", "General", {}, false);
    add_completion(item, 0, "rich", "Use human calls with bounded useful output", "General", {}, false);
    add_completion(item, 0, "expanded", "Show all retained output within the defensive display cap", "General", {}, false);
  }

  if (auto index = find_item_index(items, "/cursor"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "default", "Do not override the shell cursor", "General", {}, false);
    add_completion(item, 0, "block", "Use a block cursor", "General", {}, false);
    add_completion(item, 0, "underline", "Use an underline cursor", "General", {}, false);
    add_completion(item, 0, "bar", "Use a bar cursor", "General", {}, false);
    for (auto const& style : {std::string("default"), std::string("block"), std::string("underline"), std::string("bar")})
    {
      add_completion(item, 1, "blink", "Use the blinking variant", "General", {style}, false);
      add_completion(item, 1, "steady", "Use the steady variant", "General", {style}, false);
    }
  }

  if (auto index = find_item_index(items, "/theme"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "dark", "Persist the built-in dark palette", "General", {}, false);
    add_completion(item, 0, "light", "Persist the built-in light palette", "General", {}, false);
    add_completion(item, 0, "plain", "Persist no-ANSI output", "General", {}, false);
    add_completion(item, 0, "reset", "Use the built-in default unless an environment override is set", "General", {}, false);
    for (auto const& theme : available_tui_custom_themes(paths))
    {
      add_completion(item, 0, theme.name, "Persist custom theme from " + theme.path.string(), "Themes", {}, false);
    }
  }

  if (auto index = find_item_index(items, "/reload"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "keybindings", "Reload $XDG_CONFIG_HOME/ava/keybinds.json inside the TUI", "General", {}, false);
    add_completion(item, 0, "theme", "Reload $XDG_CONFIG_HOME/ava/display.json and repaint the TUI", "General", {}, false);
  }

  if (auto index = find_item_index(items, "/export"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "markdown", "Return or write a Markdown session export", "Sessions", {}, false);
    add_completion(item, 0, "html", "Return or write a self-contained HTML session export", "Sessions", {}, false);
    add_completion(item, 0, "jsonl", "Return or write a sanitized portable AVA JSONL archive", "Sessions", {}, false);
  }

  if (auto index = find_item_index(items, "/import"))
  {
    auto& item = items[*index];
    add_completion(item, 1, "--confirm", "Create a new local session from the validated JSONL archive", "Sessions", {}, false);
  }

  auto add_session_completions_for = [&](std::string_view command) {
    auto index = find_item_index(items, command);
    if (!index)
      return;
    auto& item = items[*index];
    if (command == "/sessions")
    {
      add_completion(item, 0, "rename", "Rename a session without switching to it", "Sessions");
      add_completion(item, 0, "labels", "Set or clear labels on a session without switching to it", "Sessions");
      add_completion(item, 0, "archive", "Archive a session after confirmation", "Sessions");
      add_completion(item, 0, "unarchive", "Restore an archived session to default views", "Sessions");
      add_completion(item, 0, "--archived", "Include archived sessions in the list", "Sessions");
    }
    if (session_tree)
    {
      for (auto const& node : session_tree->sessions)
      {
        add_completion(item, 0, node.summary.session_id, session_completion_description(node), "Sessions", {}, false, true, {},
                       node.metadata.effective_title().empty() ? node.summary.session_id : node.metadata.effective_title());
        if (command == "/sessions")
        {
          add_completion(item, 1, node.summary.session_id, session_completion_description(node), "Sessions", {"rename"}, false, true, {},
                         node.metadata.effective_title().empty() ? node.summary.session_id : node.metadata.effective_title());
          add_completion(item, 1, node.summary.session_id, session_completion_description(node), "Sessions", {"labels"}, false, true, {},
                         node.metadata.effective_title().empty() ? node.summary.session_id : node.metadata.effective_title());
          add_completion(item, 1, node.summary.session_id, session_completion_description(node), "Sessions", {"archive"}, false, true, {},
                         node.metadata.effective_title().empty() ? node.summary.session_id : node.metadata.effective_title());
          add_completion(item, 1, node.summary.session_id, session_completion_description(node), "Sessions", {"unarchive"}, false, node.metadata.archived,
                         node.metadata.archived ? "" : "session is not archived",
                         node.metadata.effective_title().empty() ? node.summary.session_id : node.metadata.effective_title());
          add_completion(item, 2, "--clear", "Clear labels without switching sessions", "Sessions", {"labels", node.summary.session_id}, false);
          add_completion(item, 2, "--confirm", "Confirm archive without deleting session files", "Sessions", {"archive", node.summary.session_id}, false);
        }
      }
    }
  };

  add_session_completions_for("/sessions");
  add_session_completions_for("/resume");

  if (auto index = find_item_index(items, "/jobs"))
  {
    auto& item = items[*index];
    for (auto const& action : {"list", "history", "show", "wait", "result", "cancel", "promote"})
      add_completion(item, 0, action, "Subagent job control", "Sessions");
    if (subagent_coordinator)
    {
      auto const jobs = subagent_coordinator->list(session_id);
      std::vector<std::string> job_ids;
      job_ids.reserve(jobs.size());
      for (auto const& snapshot : jobs)
        job_ids.push_back(snapshot.job.identity.job_id);
      auto const job_refs = unique_short_id_refs(job_ids);
      for (std::size_t job_index = 0; job_index < jobs.size(); ++job_index)
      {
        auto const& snapshot = jobs[job_index];
        auto const label = job_completion_display_label(snapshot.job);
        auto const description = job_completion_description(snapshot.job, job_refs[job_index]);
        for (auto const& action : {"show", "wait", "result", "cancel", "promote"})
          add_completion(item, 1, snapshot.job.identity.job_id, description, "Jobs", {action}, false, true, {}, label);
      }
    }
  }

  if (auto index = find_item_index(items, "/permissions"))
  {
    auto& item = items[*index];
    for (auto const& subcommand : {"list", "audit", "diagnose", "explain", "add", "remove"})
    {
      add_completion(item, 0, subcommand, "Permission rule command", "Safety");
    }
    add_completion(item, 1, "export", "Render matching permission decisions as a Markdown table", "Safety", {"audit"}, false);
    add_completion(item, 1, "summary", "Summarize matching permission audit decisions", "Safety", {"audit"}, false);
    add_completion(item, 1, "show", "Inspect one permission audit entry or request", "Safety", {"audit"}, false);
    for (auto const& value : {"action=allow", "action=deny"})
    {
      add_completion(item, 1, value, "Persistent rule action", "Safety", {"add"});
    }
    for (auto const& value : {"operation=read", "operation=search", "operation=edit", "operation=bash", "operation=network.fetch", "operation=network.search",
                              "operation=skill", "operation=mcp.tool.call"})
    {
      add_completion(item, 1, value, "Persistent rule operation", "Safety", {"add"});
    }
    for (auto const& value : {"scope=workspace", "scope=global", "mode=any", "mode=build", "mode=plan"})
    {
      add_completion(item, 1, value, "Persistent rule field", "Safety", {"add"});
    }
    for (auto const& value : {"path=", "command=", "tool=", "reason="})
    {
      add_completion(item, 1, value, "Persistent rule field", "Safety", {"add"}, false);
    }
    auto const store = ava::permissions::PermissionRuleStore{
        .global_rules_file = paths.ava_config_dir / "permission-rules.json",
        .workspace_rules_file = workspace_dir / ".ava" / "permission-rules.json",
        .workspace_dir = workspace_dir,
        .anchor_set = anchor_set,
    };
    if (auto rules = ava::permissions::load_persistent_permission_rules(store))
    {
      std::vector<std::string> rule_ids;
      rule_ids.reserve(rules->size());
      for (auto const& rule : *rules)
        rule_ids.push_back(rule.rule_id);
      auto const rule_refs = unique_short_id_refs(rule_ids);
      for (std::size_t rule_index = 0; rule_index < rules->size(); ++rule_index)
      {
        auto const& rule = (*rules)[rule_index];
        // Keep .value as the exact permrule id for authority; surface human summary + short unique ref only.
        auto const summary = format_permission_rule_summary(rule, workspace_dir);
        auto const ref_suffix = std::string(" · ref ") + rule_refs[rule_index];
        add_completion(item, 1, rule.rule_id, "Explain rule" + ref_suffix, "Rules", {"explain"}, false, true, {}, summary);
        add_completion(item, 1, rule.rule_id, "Remove rule" + ref_suffix, "Rules", {"remove"}, false, true, {}, summary);
      }
    }
  }

  if (auto index = find_item_index(items, "/context"))
  {
    auto& item = items[*index];
    for (auto const& source : context_sources)
    {
      add_completion(item, 0, source.path.generic_string(), std::to_string(source.byte_count) + " bytes", "Context", {}, false);
    }
  }

  if (auto index = find_item_index(items, "/mcp"))
  {
    auto& item = items[*index];
    for (auto const& subcommand : {"list", "inspect", "tools", "restart"})
    {
      add_completion(item, 0, subcommand, "MCP command", "MCP");
    }
    if (auto config = ava::mcp::load_mcp_config(resource_policy.mcp_config))
    {
      for (auto const& server : config->servers)
      {
        auto const description = server.name.empty() ? std::string("configured MCP server") : server.name;
        for (auto const& subcommand : {"inspect", "tools", "restart"})
        {
          add_completion(item, 1, server.id, description, "MCP", {subcommand});
        }
      }
    }
  }

  if (auto index = find_item_index(items, "/trust"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "status", "Show current project trust decision", "Trust");
    add_completion(item, 0, "project", "Trust project resources for this workspace", "Trust");
    add_completion(item, 0, "deny", "Skip project resources for this workspace", "Trust");
    add_completion(item, 0, "clear", "Remove this workspace trust decision", "Trust");
  }

  auto const diagnostics = ava::plugin::collect_plugin_diagnostics(resource_policy.plugin_discovery, resource_policy.plugin_enablement_file, workspace_dir);
  if (auto index = find_item_index(items, "/plugins"))
  {
    auto& item = items[*index];
    for (auto const& subcommand : {"list", "inspect", "enable", "disable", "validate", "failures", "prompts", "prompt", "skills", "skill"})
    {
      add_completion(item, 0, subcommand, "Plugin command", "Plugins");
    }
    for (auto const& status : diagnostics.plugins)
    {
      auto const& manifest = status.plugin.manifest;
      auto description = manifest.name;
      if (!status.enabled)
        description += " (disabled)";
      for (auto const& subcommand : {"inspect", "enable", "disable", "prompts", "prompt", "skills", "skill"})
      {
        add_completion(item, 1, manifest.id, description, "Plugins", {subcommand});
      }
      for (auto const& prompt : manifest.contributes.prompts)
      {
        add_completion(item, 2, prompt.name, prompt.description, "Prompts", {"prompt", manifest.id});
      }
      for (auto const& skill : manifest.contributes.skills)
      {
        add_completion(item, 2, skill.name, skill.description, "Skills", {"skill", manifest.id});
      }
    }
  }

  if (auto index = find_item_index(items, "/plugin"))
  {
    auto& item = items[*index];
    add_completion(item, 0, "run", "Run an enabled plugin command", "Plugins");
    for (auto const& status : diagnostics.plugins)
    {
      auto const& manifest = status.plugin.manifest;
      add_completion(item, 1, manifest.id, status.enabled ? manifest.name : manifest.name + " (disabled)", "Plugins", {"run"}, true, status.enabled,
                     status.enabled ? "" : "plugin is disabled");
      for (auto const& command : manifest.contributes.commands)
      {
        add_completion(item, 2, command.name, command.description, "Plugin commands", {"run", manifest.id});
      }
    }
  }
}

}  // namespace

SessionSelectorSort next_session_selector_sort(SessionSelectorSort sort) noexcept
{
  switch (sort)
  {
    case SessionSelectorSort::Recent:
      return SessionSelectorSort::Name;
    case SessionSelectorSort::Name:
      return SessionSelectorSort::Path;
    case SessionSelectorSort::Path:
      return SessionSelectorSort::Recent;
  }
  return SessionSelectorSort::Recent;
}

std::string session_selector_sort_label(SessionSelectorSort sort)
{
  switch (sort)
  {
    case SessionSelectorSort::Recent:
      return "recent";
    case SessionSelectorSort::Name:
      return "name";
    case SessionSelectorSort::Path:
      return "path";
  }
  return "recent";
}

std::vector<tui::SlashCommandItem> command_catalog_slash_items(std::vector<CommandHotkey> const& hotkeys)
{
  std::vector<tui::SlashCommandItem> items;
  items.reserve(command_catalog().size());
  for (auto const& entry : command_catalog())
  {
    std::string key_display;
    if (entry.command == "/mode")
      key_display = hotkeys_for_action(hotkeys, "mode_toggle");
    if (entry.command == "/details")
      key_display = hotkeys_for_action(hotkeys, "details_toggle");
    if (entry.command == "/quit")
      key_display = hotkeys_for_action(hotkeys, "exit");
    items.push_back(tui::SlashCommandItem{.command = entry.command,
                                          .description = entry.description,
                                          .hint = entry.hint,
                                          .category = entry.category,
                                          .aliases = entry.aliases,
                                          .key_display = std::move(key_display),
                                          .enabled = entry.enabled,
                                          .disabled_reason = entry.disabled_reason});
  }
  return items;
}

namespace {

// Build the current session tree with session_tree_builder or from a short snapshot of unlocked_session.
//
// The optional builder receives the unlocked wrapper and is invoked without an active session guard.
ava::core::Result<ava::session::SessionTreeIndex> build_current_session_tree(runtime::session_ts const& unlocked_session,
                                                                             SessionTreeIndexBuilder const& session_tree_builder)
{
  if (session_tree_builder)
    return session_tree_builder(unlocked_session);
  auto [workspace_dir, sessions_dir, session_id] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->workspace_dir(), session_r->paths().sessions_dir, session_r->store.session_id()};
  }();
  return ava::session::build_session_tree(workspace_dir, sessions_dir, session_id);
}

}  // namespace

void refresh_application_catalog_values(ApplicationCatalogCache& cache, runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys)
{
  auto items = command_catalog_slash_items(hotkeys);
  add_backend_argument_completions(items, unlocked_session, hotkeys, cache.workspace_path_candidates, cache.session_tree ? &*cache.session_tree : nullptr);
  cache.slash_commands = std::move(items);
  ++cache.slash_catalog_generation;
  ++cache.operations.value_refreshes;
}

void refresh_application_workspace_catalog(ApplicationCatalogCache& cache, runtime::session_ts const& unlocked_session,
                                           std::vector<CommandHotkey> const& hotkeys, WorkspaceCatalogWalker workspace_walker)
{
  auto candidates = workspace_walker ? workspace_walker(unlocked_session) : walk_workspace_path_candidates(unlocked_session);
  ++cache.operations.workspace_walks;
  ++cache.workspace_catalog_generation;
  cache.file_references = file_reference_items_from_candidates(candidates);
  cache.workspace_path_candidates = prepare_workspace_path_candidates(std::move(candidates), false);
  refresh_application_catalog_values(cache, unlocked_session, hotkeys);
}

void refresh_application_session_tree(ApplicationCatalogCache& cache, runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys,
                                      SessionTreeIndexBuilder session_tree_builder)
{
  auto tree = build_current_session_tree(unlocked_session, session_tree_builder);
  ++cache.operations.session_tree_builds;
  if (tree)
  {
    cache.session_tree = std::move(*tree);
    cache.session_tree_error.clear();
  }
  else
  {
    cache.session_tree.reset();
    cache.session_tree_error = tree.error().format();
  }
  refresh_application_catalog_values(cache, unlocked_session, hotkeys);
}

void retarget_application_session(ApplicationCatalogCache& cache, std::string_view current_session_id)
{
  if (cache.session_tree)
    ava::session::retarget_session_tree(*cache.session_tree, current_session_id);
}

ApplicationCatalogCache build_application_catalog_cache(runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys,
                                                        WorkspaceCatalogWalker workspace_walker, SessionTreeIndexBuilder session_tree_builder)
{
  ApplicationCatalogCache cache;
  auto candidates = workspace_walker ? workspace_walker(unlocked_session) : walk_workspace_path_candidates(unlocked_session);
  ++cache.operations.workspace_walks;
  ++cache.workspace_catalog_generation;
  cache.file_references = file_reference_items_from_candidates(candidates);
  cache.workspace_path_candidates = prepare_workspace_path_candidates(std::move(candidates), false);

  auto tree = build_current_session_tree(unlocked_session, session_tree_builder);
  ++cache.operations.session_tree_builds;
  if (tree)
  {
    cache.session_tree = std::move(*tree);
  }
  else
  {
    cache.session_tree_error = tree.error().format();
  }
  refresh_application_catalog_values(cache, unlocked_session, hotkeys);
  return cache;
}

ApplicationCatalogCoordinator::ApplicationCatalogCoordinator(ApplicationCatalogCache cache, std::size_t title_catalog_cursor)
    : cache_(std::move(cache)), title_catalog_cursor_(title_catalog_cursor)
{
}

ApplicationCatalogCache ApplicationCatalogCoordinator::snapshot() const
{
  std::lock_guard lock(mutex_);
  return cache_;
}

ApplicationCatalogDelivery ApplicationCatalogCoordinator::delivery_snapshot()
{
  std::lock_guard lock(mutex_);
  ApplicationCatalogDelivery delivery;
  delivery.slash_catalog_generation = cache_.slash_catalog_generation;
  delivery.workspace_catalog_generation = cache_.workspace_catalog_generation;
  if (cache_.slash_catalog_generation != delivered_slash_catalog_generation_)
  {
    delivery.slash_commands = cache_.slash_commands;
    delivered_slash_catalog_generation_ = cache_.slash_catalog_generation;
  }
  if (cache_.workspace_catalog_generation != delivered_workspace_catalog_generation_)
  {
    delivery.file_references = cache_.file_references;
    delivered_workspace_catalog_generation_ = cache_.workspace_catalog_generation;
  }
  return delivery;
}

void ApplicationCatalogCoordinator::refresh_values_during_operation(runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys)
{
  refresh_application_catalog_values(cache_, unlocked_session, hotkeys);
}

void ApplicationCatalogCoordinator::refresh_values(runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys)
{
  std::lock_guard lock(mutex_);
  refresh_values_during_operation(unlocked_session, hotkeys);
}

void ApplicationCatalogCoordinator::refresh_workspace(runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys,
                                                      WorkspaceCatalogWalker workspace_walker)
{
  std::lock_guard lock(mutex_);
  refresh_application_workspace_catalog(cache_, unlocked_session, hotkeys, std::move(workspace_walker));
}

ava::core::Result<bool> ApplicationCatalogCoordinator::refresh_session_tree_during_operation(runtime::session_ts const& unlocked_session,
                                                                                             std::vector<CommandHotkey> const& hotkeys,
                                                                                             SessionTreeIndexBuilder session_tree_builder)
{
  auto tree = build_current_session_tree(unlocked_session, session_tree_builder);
  ++cache_.operations.session_tree_builds;
  if (!tree)
  {
    auto error = std::move(tree.error());
    cache_.session_tree.reset();
    cache_.session_tree_error = error.format();
    refresh_values_during_operation(unlocked_session, hotkeys);
    return std::unexpected(std::move(error));
  }
  cache_.session_tree = std::move(*tree);
  cache_.session_tree_error.clear();
  refresh_values_during_operation(unlocked_session, hotkeys);
  return true;
}

ava::core::Result<bool> ApplicationCatalogCoordinator::refresh_session_tree_and_consume_title_changes(runtime::session_ts const& unlocked_session,
                                                                                                      SessionTitleCatalogChanges const& captured_changes,
                                                                                                      std::vector<CommandHotkey> const& hotkeys,
                                                                                                      SessionTreeIndexBuilder session_tree_builder)
{
  std::lock_guard lock(mutex_);
  auto refreshed = refresh_session_tree_during_operation(unlocked_session, hotkeys, std::move(session_tree_builder));
  if (!refreshed)
    return refreshed;
  title_catalog_cursor_ = std::max(title_catalog_cursor_, captured_changes.cursor);
  return refreshed;
}

ava::core::Result<bool> ApplicationCatalogCoordinator::refresh_current_session_during_operation(runtime::session_ts const& unlocked_session,
                                                                                                std::vector<CommandHotkey> const& hotkeys)
{
  auto [authority, session_id, session_path] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->read_authority_1(), session_r->store.session_id(), session_r->store.session_path()};
  }();
  if (!authority)
    return std::unexpected(std::move(authority.error()));
  auto entries = authority->load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  auto metadata = ava::session::session_metadata_from_entries(session_id, *entries);
  if (!metadata)
    return std::unexpected(std::move(metadata.error()));

  ava::session::SessionSummary summary{
      .session_id = metadata->session_id,
      .path = std::move(session_path), // This is the store’s stable logical pathname; it does not change during the store’s lifetime.
      .last_updated = entries->empty() ? std::string{} : entries->back().timestamp,
      .entry_count = entries->size(),
      .original_cwd = metadata->original_cwd,
      .title = metadata->effective_title()};
  bool refreshed = false;
  if (cache_.session_tree)
    refreshed = ava::session::refresh_session_tree_node(*cache_.session_tree, std::move(summary), std::move(*metadata));
  if (refreshed)
    ++cache_.operations.session_node_refreshes;
  if (refreshed)
    refresh_values_during_operation(unlocked_session, hotkeys);
  return refreshed;
}

ava::core::Result<bool> ApplicationCatalogCoordinator::refresh_current_session(runtime::session_ts const& unlocked_session,
                                                                               std::vector<CommandHotkey> const& hotkeys)
{
  std::lock_guard lock(mutex_);
  return refresh_current_session_during_operation(unlocked_session, hotkeys);
}

ava::core::Result<bool> ApplicationCatalogCoordinator::refresh_title_changes(runtime::session_ts const& unlocked_session,
                                                                             SessionTitleCatalogChanges const& changes,
                                                                             std::vector<CommandHotkey> const& hotkeys,
                                                                             SessionTreeIndexBuilder session_tree_builder)
{
  std::lock_guard lock(mutex_);
  if (changes.cursor <= title_catalog_cursor_)
    return false;
  if (changes.dirty_session_ids.empty())
    return false;

  auto const current_session_id = runtime::session_ts::crat(unlocked_session)->store.session_id();
  auto const needs_full_rebuild =
      std::ranges::any_of(changes.dirty_session_ids, [&](std::string const& session_id) { return session_id != current_session_id; });
  if (needs_full_rebuild)
  {
    auto tree = build_current_session_tree(unlocked_session, session_tree_builder);
    if (!tree)
      return std::unexpected(std::move(tree.error()));
    cache_.session_tree = std::move(*tree);
    cache_.session_tree_error.clear();
    ++cache_.operations.session_tree_builds;
    refresh_values_during_operation(unlocked_session, hotkeys);
  }
  else
  {
    auto refreshed = refresh_current_session_during_operation(unlocked_session, hotkeys);
    if (!refreshed)
      return refreshed;
    if (!*refreshed)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "current session is missing from the application catalog"));
  }

  title_catalog_cursor_ = changes.cursor;
  return true;
}

void ApplicationCatalogCoordinator::retarget_session(std::string_view current_session_id)
{
  std::lock_guard lock(mutex_);
  if (cache_.session_tree)
    ava::session::retarget_session_tree(*cache_.session_tree, current_session_id);
}

std::size_t ApplicationCatalogCoordinator::title_catalog_cursor() const
{
  std::lock_guard lock(mutex_);
  return title_catalog_cursor_;
}

tui::SelectListView ApplicationCatalogCoordinator::session_view(SessionSelectorSort sort, std::string footer_hint, bool named_only, bool show_paths,
                                                                bool show_archived, bool show_label_time, std::string summarize_parent_keys) const
{
  std::lock_guard lock(mutex_);
  auto view = ava::app::session_selector_view(cache_, sort, std::move(footer_hint), named_only, show_paths, show_archived, show_label_time);
  if (cache_.session_tree)
    add_parent_summary_hint(view, *cache_.session_tree, std::move(summarize_parent_keys));
  return view;
}

ava::core::Result<std::optional<ava::session::SessionSummary>> ApplicationCatalogCoordinator::session_summary(std::string_view session_id) const
{
  std::lock_guard lock(mutex_);
  if (!cache_.session_tree)
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Session, cache_.session_tree_error.empty() ? "unable to load session tree" : cache_.session_tree_error));
  auto const found =
      std::ranges::find_if(cache_.session_tree->sessions, [&](ava::session::SessionTreeNode const& node) { return node.summary.session_id == session_id; });
  if (found == cache_.session_tree->sessions.end())
    return std::optional<ava::session::SessionSummary>{};
  return std::optional<ava::session::SessionSummary>{found->summary};
}

ava::core::Result<std::optional<std::string>> ApplicationCatalogCoordinator::parent_target(std::string_view session_id) const
{
  std::lock_guard lock(mutex_);
  if (!cache_.session_tree)
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Session, cache_.session_tree_error.empty() ? "unable to load session tree" : cache_.session_tree_error));
  return session_selector_parent_target(*cache_.session_tree, session_id);
}

ava::core::Result<std::optional<std::string>> ApplicationCatalogCoordinator::child_target(std::string_view session_id, SessionSelectorSort sort,
                                                                                          bool include_archived) const
{
  std::lock_guard lock(mutex_);
  if (!cache_.session_tree)
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Session, cache_.session_tree_error.empty() ? "unable to load session tree" : cache_.session_tree_error));
  return session_selector_child_target(*cache_.session_tree, session_id, sort, include_archived);
}

std::vector<tui::SlashCommandItem> command_catalog_slash_items_1(runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys)
{
  auto cache = build_application_catalog_cache(unlocked_session, hotkeys);
  return std::move(cache.slash_commands);
}

std::vector<tui::FileReferenceItem> file_reference_items(runtime::session_ts const& unlocked_session)
{
  return file_reference_items_from_candidates(walk_workspace_path_candidates(unlocked_session));
}

tui::SelectListView model_selector_view(ava::config::ModelRegistry const& registry, ava::config::ModelInfo const& current_model,
                                        std::shared_ptr<ava::provider::ProviderCatalog const> ensured_provider_catalog, std::string footer_hint)
{
  // The caller passed a null provider catalog; pass Session::ensure_provider_catalog() or
  // ava::provider::ProviderCatalog::build_builtins_only() so the palette always has a catalog.
  ASSERT(ensured_provider_catalog);

  auto models = ava::config::effective_models(registry);
  auto const current_in_catalog = std::ranges::any_of(
      models, [&](auto const& model) { return model.provider_id == current_model.provider_id && model.model_id == current_model.model_id; });
  if (!current_in_catalog && !current_model.provider_id.empty() && !current_model.model_id.empty())
    models.push_back(current_model);
  std::ranges::stable_sort(models, [](auto const& left, auto const& right) {
    auto const left_provider = provider_display_name(left.provider_id);
    auto const right_provider = provider_display_name(right.provider_id);
    if (left_provider != right_provider)
      return left_provider < right_provider;
    return left.provider_id < right.provider_id;
  });

  tui::SelectListView view{.title = "Select model",
                           .subtitle = {},
                           .items = {},
                           .selected_item_index = 0,
                           .query = {},
                           .placeholder = "Search models",
                           .empty_text = "No configured models match",
                           .footer_hint = std::move(footer_hint)};
  view.items.reserve(models.size() + 1);

  for (auto const& model : models)
  {
    auto const current = model.provider_id == current_model.provider_id && model.model_id == current_model.model_id;
    if (current)
      view.selected_item_index = view.items.size();
    view.items.push_back(model_selector_item(model, current_model, ensured_provider_catalog->contains(model.provider_id)));
  }

  return view;
}

tui::SelectListView model_selector_view_1(runtime::session_ts const& unlocked_session, std::string footer_hint)
{
  auto [paths, model, ensured_provider_catalog] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->paths(), session_r->model(), session_r->ensure_provider_catalog()};
  }();
  auto registry = ava::config::load_model_registry(paths);
  if (registry)
    return model_selector_view(*registry, model, ensured_provider_catalog, std::move(footer_hint));

  return tui::SelectListView{.title = "Select model",
                             .subtitle = {},
                             .items = {tui::SelectListItemView{.value = {},
                                                               .label = "Model registry unavailable",
                                                               .description = {},
                                                               .group = "Models",
                                                               .detail = {},
                                                               .badge = {},
                                                               .current = false,
                                                               .enabled = false,
                                                               .disabled_reason = "model registry failed to load"}},
                             .selected_item_index = 0,
                             .query = {},
                             .placeholder = "Search models",
                             .empty_text = "No configured models match",
                             .footer_hint = std::move(footer_hint)};
}

tui::SelectListView scoped_model_selector_view(ava::config::ModelRegistry const& registry, ava::config::ModelInfo const& current_model,
                                               std::optional<std::vector<std::string>> const& scoped_model_cycle,
                                               std::shared_ptr<ava::provider::ProviderCatalog const> ensured_provider_catalog, std::string footer_hint)
{
  // The caller passed a null provider catalog; pass Session::ensure_provider_catalog() or
  // ava::provider::ProviderCatalog::build_builtins_only() so the palette always has a catalog.
  ASSERT(ensured_provider_catalog);

  auto models = scoped_model_selector_models(ava::config::effective_models(registry), scoped_model_cycle);
  auto const enabled_count = scoped_model_cycle ? scoped_model_cycle->size() : models.size();

  tui::SelectListView view{.title = "Scoped model cycle",
                           .subtitle = scoped_model_cycle ? std::to_string(enabled_count) + " of " + std::to_string(models.size()) + " enabled"
                                                          : std::string("All registered models enabled"),
                           .items = {},
                           .selected_item_index = 0,
                           .query = {},
                           .placeholder = "Search models",
                           .empty_text = "No configured models match",
                           .footer_hint = std::move(footer_hint)};
  view.items.reserve(models.size());

  bool current_in_catalog = false;
  for (auto const& model : models)
  {
    auto const current = model.provider_id == current_model.provider_id && model.model_id == current_model.model_id;
    current_in_catalog = current_in_catalog || current;
    if (current)
      view.selected_item_index = view.items.size();
    view.items.push_back(scoped_model_selector_item(model, current_model, scoped_model_cycle, ensured_provider_catalog->contains(model.provider_id)));
  }

  if (!current_in_catalog && !current_model.provider_id.empty() && !current_model.model_id.empty())
  {
    view.selected_item_index = view.items.size();
    view.items.push_back(
        scoped_model_selector_item(current_model, current_model, scoped_model_cycle, ensured_provider_catalog->contains(current_model.provider_id)));
  }

  return view;
}

tui::SelectListView scoped_model_selector_view_1(runtime::session_ts const& unlocked_session, std::string footer_hint)
{
  auto [paths, model, scoped_model_cycle, ensured_provider_catalog] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->paths(), session_r->model(), session_r->scoped_model_cycle(), session_r->ensure_provider_catalog()};
  }();
  auto registry = ava::config::load_model_registry(paths);
  if (registry)
    return scoped_model_selector_view(*registry, model, scoped_model_cycle, ensured_provider_catalog, std::move(footer_hint));

  return tui::SelectListView{.title = "Scoped model cycle",
                             .subtitle = {},
                             .items = {tui::SelectListItemView{.value = {},
                                                               .label = "Model registry unavailable",
                                                               .description = {},
                                                               .group = "Models",
                                                               .detail = {},
                                                               .badge = {},
                                                               .current = false,
                                                               .enabled = false,
                                                               .disabled_reason = "model registry failed to load"}},
                             .selected_item_index = 0,
                             .query = {},
                             .placeholder = "Search models",
                             .empty_text = "No configured models match",
                             .footer_hint = std::move(footer_hint)};
}

tui::SelectListView session_selector_view(std::vector<ava::session::SessionSummary> summaries, std::string current_session_id, SessionSelectorSort sort,
                                          std::string footer_hint, bool show_paths)
{
  sort_session_summaries(summaries, sort);

  tui::SelectListView view{
      .title = "Select session",
      .subtitle = "sort " + session_sort_label(sort) + (show_paths ? std::string(" · paths") : std::string{}),
      .items = {},
      .selected_item_index = 0,
      .query = {},
      .placeholder = "Search sessions",
      .empty_text = "No sessions match",
      .footer_hint = footer_hint.empty() ? std::string("Enter choose · PgUp/PgDn page · type to filter · Esc cancel") : std::move(footer_hint)};
  view.items.reserve(summaries.size() + 1);

  bool current_found = false;
  for (auto const& summary : summaries)
  {
    if (!current_session_id.empty() && summary.session_id == current_session_id)
    {
      view.selected_item_index = view.items.size();
      current_found = true;
    }
    view.items.push_back(session_selector_item(summary, current_session_id, show_paths));
  }

  if (!current_found && !current_session_id.empty())
  {
    view.selected_item_index = view.items.size();
    view.items.push_back(tui::SelectListItemView{.value = current_session_id,
                                                 .label = "Current session",
                                                 .description = {},
                                                 .group = {},
                                                 .detail = {},
                                                 .badge = {},
                                                 .current = true,
                                                 .enabled = true,
                                                 .disabled_reason = {}});
  }

  if (view.items.empty())
  {
    view.items.push_back(tui::SelectListItemView{.value = {},
                                                 .label = "No sessions found",
                                                 .description = "Start a conversation to create a session",
                                                 .group = "Sessions",
                                                 .detail = {},
                                                 .badge = {},
                                                 .current = false,
                                                 .enabled = false,
                                                 .disabled_reason = "session list is empty"});
  }

  return view;
}

tui::SelectListView session_selector_view(ava::session::SessionTreeIndex const& tree, SessionSelectorSort sort, std::string footer_hint, bool named_only,
                                          bool show_paths, bool show_archived, bool show_label_time)
{
  auto const index_by_id = session_tree_index_by_id(tree.sessions);
  tui::SelectListView view{
      .title = "Select session",
      .subtitle = "sort " + session_sort_label(sort) + (named_only ? std::string(" · named") : std::string{}) +
                  (show_paths ? std::string(" · paths") : std::string{}) + (show_archived ? std::string(" · archived") : std::string{}) +
                  (show_label_time ? std::string(" · label times") : std::string{}),
      .items = {},
      .selected_item_index = 0,
      .query = {},
      .placeholder = "Search sessions, labels, branches",
      .empty_text = named_only ? std::string("No named sessions match") : std::string("No sessions match"),
      .footer_hint = footer_hint.empty() ? std::string("Enter choose · PgUp/PgDn page · type to filter · Esc cancel") : std::move(footer_hint)};
  view.items.reserve(tree.sessions.size() + 1);

  append_session_tree_items(view, tree.sessions, index_by_id, tree.roots, sort, 0, named_only, show_paths, show_archived, show_label_time);

  if (!named_only && view.items.empty() && !tree.current_session_id.empty())
  {
    view.items.push_back(tui::SelectListItemView{.value = tree.current_session_id,
                                                 .label = "Current session",
                                                 .description = {},
                                                 .group = {},
                                                 .detail = {},
                                                 .badge = {},
                                                 .current = true,
                                                 .enabled = true,
                                                 .disabled_reason = {}});
  }

  if (view.items.empty())
  {
    view.items.push_back(tui::SelectListItemView{.value = {},
                                                 .label = named_only ? std::string("No named sessions found") : std::string("No sessions found"),
                                                 .description = named_only ? std::string("Use /name <name> to make a session appear in this filter")
                                                                           : std::string("Start a conversation to create a session"),
                                                 .group = "Sessions",
                                                 .detail = {},
                                                 .badge = {},
                                                 .current = false,
                                                 .enabled = false,
                                                 .disabled_reason = named_only ? std::string("no sessions have names") : std::string("session tree is empty")});
  }

  return view;
}

tui::SelectListView session_selector_view(ApplicationCatalogCache const& cache, SessionSelectorSort sort, std::string footer_hint, bool named_only,
                                          bool show_paths, bool show_archived, bool show_label_time)
{
  if (cache.session_tree)
    return session_selector_view(*cache.session_tree, sort, std::move(footer_hint), named_only, show_paths, show_archived, show_label_time);

  return tui::SelectListView{.title = "Select session",
                             .subtitle = "Unable to load session list",
                             .items = {tui::SelectListItemView{.value = {},
                                                               .label = "Session list unavailable",
                                                               .description = cache.session_tree_error,
                                                               .group = "Sessions",
                                                               .detail = {},
                                                               .badge = {},
                                                               .current = false,
                                                               .enabled = false,
                                                               .disabled_reason = "session list failed to load"}},
                             .selected_item_index = 0,
                             .query = {},
                             .placeholder = "Search sessions",
                             .empty_text = "No sessions match",
                             .footer_hint = std::move(footer_hint)};
}

#if 0 // Nothing is calling this function.
tui::SelectListView session_selector_view(runtime::session_ts const& unlocked_session, SessionSelectorSort sort, std::string footer_hint, bool named_only, bool show_paths,
                                           bool show_archived, bool show_label_time)
{
  auto tree = build_current_session_tree(unlocked_session, {});
  if (tree)
    return session_selector_view(*tree, sort, std::move(footer_hint), named_only, show_paths, show_archived, show_label_time);

  return tui::SelectListView{.title = "Select session",
                             .subtitle = "Unable to load session list",
                             .items = {tui::SelectListItemView{.value = {},
                                                               .label = "Session list unavailable",
                                                               .description = tree.error().format(),
                                                               .group = "Sessions",
                                                               .detail = {},
                                                               .badge = {},
                                                               .current = false,
                                                               .enabled = false,
                                                               .disabled_reason = "session list failed to load"}},
                             .selected_item_index = 0,
                             .query = {},
                             .placeholder = "Search sessions",
                             .empty_text = "No sessions match",
                             .footer_hint = std::move(footer_hint)};
}
#endif

std::optional<std::string> session_selector_parent_target(ava::session::SessionTreeIndex const& tree, std::string_view session_id)
{
  auto const index_by_id = session_tree_index_by_id(tree.sessions);
  auto const found = index_by_id.find(std::string(session_id));
  if (found == index_by_id.end())
    return std::nullopt;
  auto const& parent_id = tree.sessions[found->second].metadata.parent_session_id;
  if (parent_id.empty() || index_by_id.find(parent_id) == index_by_id.end())
    return std::nullopt;
  return parent_id;
}

std::optional<std::string> session_selector_child_target(ava::session::SessionTreeIndex const& tree, std::string_view session_id, SessionSelectorSort sort,
                                                         bool include_archived)
{
  auto const index_by_id = session_tree_index_by_id(tree.sessions);
  auto const found = index_by_id.find(std::string(session_id));
  if (found == index_by_id.end())
    return std::nullopt;

  auto children = sorted_tree_ids(tree.sessions[found->second].children, tree.sessions, index_by_id, sort);
  for (auto const& child_id : children)
  {
    auto const child = index_by_id.find(child_id);
    if (child == index_by_id.end())
      continue;
    if (include_archived || !tree.sessions[child->second].metadata.archived)
      return child_id;
  }
  return std::nullopt;
}

tui::SelectListView user_turn_selector_view(std::vector<SessionUserTurn> turns, std::string title, std::string footer_hint, std::string initial_query,
                                            bool truncated_before)
{
  // Newest first so Enter on the initial selection forks/copies the latest public user turn.
  std::ranges::reverse(turns);

  tui::SelectListView view{
      .title = std::move(title),
      .subtitle = truncated_before ? std::string("newest retained turns · older history omitted") : std::string{},
      .items = {},
      .selected_item_index = 0,
      .query = std::move(initial_query),
      .placeholder = "Search user turns",
      .empty_text = "No user turns match",
      .footer_hint = std::move(footer_hint),
  };
  view.items.reserve(turns.size());
  for (auto& turn : turns)
  {
    auto label = turn.preview.empty() ? std::string("(empty user turn)") : std::move(turn.preview);
    view.items.push_back(tui::SelectListItemView{
        .value = std::move(turn.entry_id),
        .label = std::move(label),
        .description = {},
        .group = {},
        .detail = std::move(turn.timestamp),
        .badge = {},
        .current = false,
        .enabled = true,
        .disabled_reason = {},
    });
  }
  if (!view.query.empty())
    view.selected_item_index = tui::clamp_select_list_selection(view, 0);
  return view;
}

ava::core::Result<tui::SelectListView> user_turn_selector_view(runtime::session_ts const& unlocked_session, std::string title, std::string footer_hint,
                                                               std::string initial_query)
{
  auto listed = list_session_user_turns(unlocked_session);
  if (!listed)
    return std::unexpected(std::move(listed.error()));
  if (listed->turns.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "no public user turns available");
    error.with_context("operation", "user_turn_selector_view");
    return std::unexpected(std::move(error));
  }
  return user_turn_selector_view(std::move(listed->turns), std::move(title), std::move(footer_hint), std::move(initial_query), listed->truncated_before);
}

}  // namespace ava::app
