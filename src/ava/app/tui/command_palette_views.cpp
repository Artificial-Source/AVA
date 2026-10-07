#include "sys.h"
#include "ava/app/command_palette_projection_internal.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/session_user_turns.h"
#include "ava/app/tui/command_palette_views.h"
#include "ava/config/model_config.h"
#include "ava/provider/catalog.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <tuple>
#include <utility>

namespace ava::app {

std::vector<tui::SlashCommandItem> project_slash_command_items(std::vector<SlashCommandCatalogItem> items)
{
  std::vector<tui::SlashCommandItem> projected;
  projected.reserve(items.size());
  for (auto& item : items)
  {
    std::vector<tui::SlashCommandArgumentCompletion> completions;
    completions.reserve(item.argument_completions.size());
    for (auto& completion : item.argument_completions)
    {
      completions.push_back(tui::SlashCommandArgumentCompletion{.value = std::move(completion.value),
                                                                .display_label = std::move(completion.display_label),
                                                                .description = std::move(completion.description),
                                                                .category = std::move(completion.category),
                                                                .required_previous_args = std::move(completion.required_previous_args),
                                                                .argument_index = completion.argument_index,
                                                                .append_space = completion.append_space,
                                                                .enabled = completion.enabled,
                                                                .disabled_reason = std::move(completion.disabled_reason)});
    }
    projected.push_back(tui::SlashCommandItem{.command = std::move(item.command),
                                              .display_label = std::move(item.display_label),
                                              .description = std::move(item.description),
                                              .hint = std::move(item.hint),
                                              .category = std::move(item.category),
                                              .aliases = std::move(item.aliases),
                                              .key_display = std::move(item.key_display),
                                              .enabled = item.enabled,
                                              .disabled_reason = std::move(item.disabled_reason),
                                              .argument_completions = std::move(completions),
                                              .argument_completion = item.argument_completion,
                                              .completion_insert_text = std::move(item.completion_insert_text)});
  }
  return projected;
}

std::vector<tui::FileReferenceItem> project_file_reference_items(std::vector<FileReferenceCatalogItem> items)
{
  std::vector<tui::FileReferenceItem> projected;
  projected.reserve(items.size());
  for (auto& item : items)
  {
    projected.push_back(tui::FileReferenceItem{.value = std::move(item.value),
                                               .description = std::move(item.description),
                                               .category = std::move(item.category),
                                               .directory = item.directory,
                                               .enabled = item.enabled,
                                               .disabled_reason = std::move(item.disabled_reason)});
  }
  return projected;
}

std::vector<tui::SlashCommandItem> command_catalog_slash_items(std::vector<CommandHotkey> const& hotkeys)
{
  return project_slash_command_items(command_catalog_records(hotkeys));
}

std::vector<tui::SlashCommandItem> command_catalog_slash_items_1(runtime::session_ts const& unlocked_session, std::vector<CommandHotkey> const& hotkeys)
{
  return project_slash_command_items(command_catalog_records_1(unlocked_session, hotkeys));
}

std::vector<tui::FileReferenceItem> file_reference_items(runtime::session_ts const& unlocked_session)
{
  return project_file_reference_items(file_reference_records(unlocked_session));
}

namespace {

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
  return tui::SelectListItemView{.non_searchable_suffix = {},
                                 .priority_suffix = {},
                                 .value = model.provider_id + "/" + model.model_id,
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
  auto const enabled = !scoped_model_cycle || std::ranges::find(*scoped_model_cycle, item.value) != scoped_model_cycle->end();
  item.badge = enabled ? "enabled" : "disabled";
  return item;
}

std::string session_node_description(ava::session::SessionTreeNode const& node, bool show_paths, bool show_label_time)
{
  std::vector<std::string> parts;
  if (!node.metadata.labels.empty())
    parts.push_back(command_palette_detail::format_session_labels(node.metadata.labels));
  if (show_label_time && !node.metadata.labels_updated.empty())
    parts.push_back("labels updated " + node.metadata.labels_updated);
  if (show_paths)
    parts.push_back(node.summary.path.empty() ? "path unavailable" : node.summary.path.generic_string());
  std::string description;
  for (auto const& part : parts)
  {
    if (!description.empty())
      description += " · ";
    description += part;
  }
  return description;
}

void append_session_tree_items(tui::SelectListView& view, std::vector<ava::session::SessionTreeNode> const& nodes, std::vector<std::string> ids,
                               SessionSelectorSort sort, std::size_t depth, bool named_only, bool show_paths, bool show_archived, bool show_label_time)
{
  for (auto const* node : command_palette_detail::sorted_session_tree_nodes(nodes, std::move(ids), sort))
  {
    auto const visible = show_archived || !node->metadata.archived;
    if (visible && (!named_only || !node->metadata.effective_title().empty()))
    {
      if (node->current)
        view.selected_item_index = view.items.size();
      auto label = node->metadata.effective_title().empty() ? std::string("Untitled session") : node->metadata.effective_title();
      if (depth > 0)
        label = std::string(depth * 2, ' ') + "↳ " + label;
      view.items.push_back(tui::SelectListItemView{.non_searchable_suffix = {},
                                                   .priority_suffix = {},
                                                   .value = node->summary.session_id,
                                                   .label = std::move(label),
                                                   .description = session_node_description(*node, show_paths, show_label_time),
                                                   .group = {},
                                                   .detail = {},
                                                   .badge = node->metadata.archived ? "archived" : "",
                                                   .current = node->current,
                                                   .enabled = true,
                                                   .disabled_reason = {}});
    }
    append_session_tree_items(view, nodes, node->children, sort, depth + (visible ? 1 : 0), named_only, show_paths, show_archived, show_label_time);
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
  parent->detail = parent->detail.empty() ? std::move(hint) : parent->detail + " · " + hint;
}

}  // namespace

tui::SelectListView model_selector_view(ava::config::ModelRegistry const& registry, ava::config::ModelInfo const& current_model,
                                        std::shared_ptr<ava::provider::ProviderCatalog const> ensured_provider_catalog, std::string footer_hint)
{
  // The caller passed a null provider catalog; pass Session::ensure_provider_catalog() or a builtins-only catalog.
  ASSERT(ensured_provider_catalog);
  auto models = ava::config::effective_models(registry);
  auto const current_in_catalog = std::ranges::any_of(
      models, [&](auto const& model) { return model.provider_id == current_model.provider_id && model.model_id == current_model.model_id; });
  if (!current_in_catalog && !current_model.provider_id.empty() && !current_model.model_id.empty())
    models.push_back(current_model);
  std::ranges::stable_sort(models, [](auto const& left, auto const& right) {
    auto const left_provider = provider_display_name(left.provider_id);
    auto const right_provider = provider_display_name(right.provider_id);
    return left_provider == right_provider ? left.provider_id < right.provider_id : left_provider < right_provider;
  });
  tui::SelectListView view{.title = "Select model",
                           .subtitle = {},
                           .items = {},
                           .selected_item_index = 0,
                           .query = {},
                           .placeholder = "Search models",
                           .empty_text = "No configured models match",
                           .footer_hint = std::move(footer_hint),
                           .compact_settings_chrome = false,
                           .freeze_underlying_transcript_layout = false};
  for (auto const& model : models)
  {
    if (model.provider_id == current_model.provider_id && model.model_id == current_model.model_id)
      view.selected_item_index = view.items.size();
    view.items.push_back(model_selector_item(model, current_model, ensured_provider_catalog->contains(model.provider_id)));
  }
  return view;
}

tui::SelectListView model_selector_view_1(runtime::session_ts const& unlocked_session, std::string footer_hint)
{
  auto [paths, model, catalog] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->paths(), session_r->model(), session_r->ensure_provider_catalog()};
  }();
  if (auto registry = ava::config::load_model_registry(paths))
    return model_selector_view(*registry, model, std::move(catalog), std::move(footer_hint));
  return tui::SelectListView{.title = "Select model",
                             .subtitle = {},
                             .items = {{.non_searchable_suffix = {},
                                        .priority_suffix = {},
                                        .value = {},
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
                             .footer_hint = std::move(footer_hint),
                             .compact_settings_chrome = false,
                             .freeze_underlying_transcript_layout = false};
}

tui::SelectListView scoped_model_selector_view(ava::config::ModelRegistry const& registry, ava::config::ModelInfo const& current_model,
                                               std::optional<std::vector<std::string>> const& scoped_model_cycle,
                                               std::shared_ptr<ava::provider::ProviderCatalog const> ensured_provider_catalog, std::string footer_hint)
{
  // The caller passed a null provider catalog; pass Session::ensure_provider_catalog() or a builtins-only catalog.
  ASSERT(ensured_provider_catalog);
  auto models = scoped_model_selector_models(ava::config::effective_models(registry), scoped_model_cycle);
  tui::SelectListView view{.title = "Scoped model cycle",
                           .subtitle = scoped_model_cycle ? std::to_string(scoped_model_cycle->size()) + " of " + std::to_string(models.size()) + " enabled"
                                                          : "All registered models enabled",
                           .items = {},
                           .selected_item_index = 0,
                           .query = {},
                           .placeholder = "Search models",
                           .empty_text = "No configured models match",
                           .footer_hint = std::move(footer_hint),
                           .compact_settings_chrome = false,
                           .freeze_underlying_transcript_layout = false};
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
  auto [paths, model, cycle, catalog] = [&] {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    return std::tuple{session_r->paths(), session_r->model(), session_r->scoped_model_cycle(), session_r->ensure_provider_catalog()};
  }();
  if (auto registry = ava::config::load_model_registry(paths))
    return scoped_model_selector_view(*registry, model, cycle, std::move(catalog), std::move(footer_hint));
  return tui::SelectListView{.title = "Scoped model cycle",
                             .subtitle = {},
                             .items = {{.non_searchable_suffix = {},
                                        .priority_suffix = {},
                                        .value = {},
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
                             .footer_hint = std::move(footer_hint),
                             .compact_settings_chrome = false,
                             .freeze_underlying_transcript_layout = false};
}

tui::SelectListView session_selector_view(std::vector<ava::session::SessionSummary> summaries, std::string current_session_id, SessionSelectorSort sort,
                                          std::string footer_hint, bool show_paths)
{
  std::ranges::sort(summaries, [&](auto const& left, auto const& right) {
    if (sort == SessionSelectorSort::Recent)
      return left.last_updated == right.last_updated ? left.session_id > right.session_id : left.last_updated > right.last_updated;
    if (sort == SessionSelectorSort::Name)
    {
      auto const& left_name = left.title.empty() ? left.session_id : left.title;
      auto const& right_name = right.title.empty() ? right.session_id : right.title;
      return left_name == right_name ? left.session_id < right.session_id : left_name < right_name;
    }
    return left.path == right.path ? left.session_id < right.session_id : left.path.generic_string() < right.path.generic_string();
  });
  tui::SelectListView view{.title = "Select session",
                           .subtitle = "sort " + session_selector_sort_label(sort) + (show_paths ? " · paths" : ""),
                           .items = {},
                           .selected_item_index = 0,
                           .query = {},
                           .placeholder = "Search sessions",
                           .empty_text = "No sessions match",
                           .footer_hint = footer_hint.empty() ? "Enter choose · PgUp/PgDn page · type to filter · Esc cancel" : std::move(footer_hint),
                           .compact_settings_chrome = false,
                           .freeze_underlying_transcript_layout = false};
  bool current_found = false;
  for (auto const& summary : summaries)
  {
    auto const current = !current_session_id.empty() && summary.session_id == current_session_id;
    if (current)
    {
      current_found = true;
      view.selected_item_index = view.items.size();
    }
    view.items.push_back({.non_searchable_suffix = {},
                          .priority_suffix = {},
                          .value = summary.session_id,
                          .label = summary.title.empty() ? "Untitled session" : summary.title,
                          .description = show_paths ? (summary.path.empty() ? "path unavailable" : summary.path.generic_string()) : "",
                          .group = {},
                          .detail = {},
                          .badge = {},
                          .current = current,
                          .enabled = true,
                          .disabled_reason = {}});
  }
  if (!current_found && !current_session_id.empty())
  {
    view.selected_item_index = view.items.size();
    view.items.push_back({.non_searchable_suffix = {},
                          .priority_suffix = {},
                          .value = std::move(current_session_id),
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
    view.items.push_back({.non_searchable_suffix = {},
                          .priority_suffix = {},
                          .value = {},
                          .label = "No sessions found",
                          .description = "Start a conversation to create a session",
                          .group = "Sessions",
                          .detail = {},
                          .badge = {},
                          .current = false,
                          .enabled = false,
                          .disabled_reason = "session list is empty"});
  return view;
}

tui::SelectListView session_selector_view(ava::session::SessionTreeIndex const& tree, SessionSelectorSort sort, std::string footer_hint, bool named_only,
                                          bool show_paths, bool show_archived, bool show_label_time)
{
  tui::SelectListView view{.title = "Select session",
                           .subtitle = "sort " + session_selector_sort_label(sort) + (named_only ? " · named" : "") + (show_paths ? " · paths" : "") +
                                       (show_archived ? " · archived" : "") + (show_label_time ? " · label times" : ""),
                           .items = {},
                           .selected_item_index = 0,
                           .query = {},
                           .placeholder = "Search sessions, labels, branches",
                           .empty_text = named_only ? "No named sessions match" : "No sessions match",
                           .footer_hint = footer_hint.empty() ? "Enter choose · PgUp/PgDn page · type to filter · Esc cancel" : std::move(footer_hint),
                           .compact_settings_chrome = false,
                           .freeze_underlying_transcript_layout = false};
  append_session_tree_items(view, tree.sessions, tree.roots, sort, 0, named_only, show_paths, show_archived, show_label_time);
  if (!named_only && view.items.empty() && !tree.current_session_id.empty())
    view.items.push_back({.non_searchable_suffix = {},
                          .priority_suffix = {},
                          .value = tree.current_session_id,
                          .label = "Current session",
                          .description = {},
                          .group = {},
                          .detail = {},
                          .badge = {},
                          .current = true,
                          .enabled = true,
                          .disabled_reason = {}});
  if (view.items.empty())
    view.items.push_back({.non_searchable_suffix = {},
                          .priority_suffix = {},
                          .value = {},
                          .label = named_only ? "No named sessions found" : "No sessions found",
                          .description = named_only ? "Use /name <name> to make a session appear in this filter" : "Start a conversation to create a session",
                          .group = "Sessions",
                          .detail = {},
                          .badge = {},
                          .current = false,
                          .enabled = false,
                          .disabled_reason = named_only ? "no sessions have names" : "session tree is empty"});
  return view;
}

tui::SelectListView session_selector_view(ApplicationCatalogCache const& cache, SessionSelectorSort sort, std::string footer_hint, bool named_only,
                                          bool show_paths, bool show_archived, bool show_label_time)
{
  if (cache.session_tree)
    return session_selector_view(*cache.session_tree, sort, std::move(footer_hint), named_only, show_paths, show_archived, show_label_time);
  return tui::SelectListView{.title = "Select session",
                             .subtitle = "Unable to load session list",
                             .items = {{.non_searchable_suffix = {},
                                        .priority_suffix = {},
                                        .value = {},
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
                             .footer_hint = std::move(footer_hint),
                             .compact_settings_chrome = false,
                             .freeze_underlying_transcript_layout = false};
}

#if 0 // Nothing is calling this function.
tui::SelectListView session_selector_view(runtime::session_ts const& unlocked_session, SessionSelectorSort sort, std::string footer_hint, bool named_only,
                                           bool show_paths, bool show_archived, bool show_label_time)
{
  auto tree = build_current_session_tree(unlocked_session, {});
  if (tree)
    return session_selector_view(*tree, sort, std::move(footer_hint), named_only, show_paths, show_archived, show_label_time);

  return tui::SelectListView{.title = "Select session",
                             .subtitle = "Unable to load session list",
                             .items = {{.non_searchable_suffix = {},
                                        .priority_suffix = {},
                                        .value = {},
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
                             .footer_hint = std::move(footer_hint),
                             .compact_settings_chrome = false,
                             .freeze_underlying_transcript_layout = false};
}
#endif

tui::SelectListView user_turn_selector_view(std::vector<SessionUserTurn> turns, std::string title, std::string footer_hint, std::string initial_query,
                                            bool truncated_before)
{
  std::ranges::reverse(turns);
  tui::SelectListView view{.title = std::move(title),
                           .subtitle = truncated_before ? "newest retained turns · older history omitted" : "",
                           .items = {},
                           .selected_item_index = 0,
                           .query = std::move(initial_query),
                           .placeholder = "Search user turns",
                           .empty_text = "No user turns match",
                           .footer_hint = std::move(footer_hint),
                           .compact_settings_chrome = false,
                           .freeze_underlying_transcript_layout = false};
  for (auto& turn : turns)
    view.items.push_back({.non_searchable_suffix = {},
                          .priority_suffix = {},
                          .value = std::move(turn.entry_id),
                          .label = turn.preview.empty() ? "(empty user turn)" : std::move(turn.preview),
                          .description = {},
                          .group = {},
                          .detail = std::move(turn.timestamp),
                          .badge = {},
                          .current = false,
                          .enabled = true,
                          .disabled_reason = {}});
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

tui::SelectListView application_catalog_session_view(ApplicationCatalogCoordinator const& coordinator, SessionSelectorSort sort, std::string footer_hint,
                                                     bool named_only, bool show_paths, bool show_archived, bool show_label_time,
                                                     std::string summarize_parent_keys)
{
  auto cache = coordinator.snapshot();
  auto view = session_selector_view(cache, sort, std::move(footer_hint), named_only, show_paths, show_archived, show_label_time);
  if (cache.session_tree)
    add_parent_summary_hint(view, *cache.session_tree, std::move(summarize_parent_keys));
  return view;
}

}  // namespace ava::app
