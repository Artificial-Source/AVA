#include "sys.h"
#include "ActiveRunEvents.h"
#include "Composer.h"
#include "SelectorHost.h"
#include "runtime_internal.h"
#include "runtime_transcript_selection_internal.h"
#include "tool_cards.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_completions.h"
#include "ava/tui/command_output.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/theme.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace ava::tui {
namespace {

// Recompose current key displays and all TUI-local argument choices from the
// retained display/keybinding facts; no filesystem discovery occurs here.
void refresh_local_palette_hints(ComposerSnapshot& snapshot, TuiKeyBindings const& bindings)
{
  compose_tui_slash_command_items(snapshot.slash_commands, snapshot.custom_themes, key_binding_help_items(bindings));
}

void refresh_local_overview_hints(ComposerSnapshot& snapshot, TuiFrontEnd const& frontend)
{
  if (!snapshot.startup_overview)
    return;
  auto& overview = *snapshot.startup_overview;
  auto strip_exact_suffix = [](std::string& line, std::string_view suffix) {
    if (suffix.empty())
      return false;
    if (line == suffix)
    {
      line.clear();
      return true;
    }
    auto const separated_suffix = std::string(" · ") + std::string(suffix);
    if (!line.ends_with(separated_suffix))
      return false;
    line.erase(line.size() - separated_suffix.size());
    return true;
  };
  // Remove only local suffixes that the prior bounded composition actually retained.
  // Either suffix may have been omitted independently when the 160-byte budget was full.
  auto const old_open_hint = overview.overview_toggle_keys.empty() ? std::string("/overview") : overview.overview_toggle_keys + " · /overview";
  static_cast<void>(strip_exact_suffix(overview.compact_line, old_open_hint));
  static_cast<void>(strip_exact_suffix(overview.compact_line, overview.theme_name));

  auto const theme = active_tui_theme();
  overview.theme_name = theme.name;
  overview.theme_badge = theme.badge;
  overview.overview_toggle_keys = keys_display(frontend.key_bindings(), TuiAction::OverviewToggle);
  auto append_bounded = [&overview](std::string_view segment) {
    auto candidate = overview.compact_line;
    if (!candidate.empty())
      candidate += " · ";
    candidate += segment;
    if (candidate.size() <= 160)
      overview.compact_line = std::move(candidate);
  };
  append_bounded(overview.theme_name);
  auto open_hint = overview.overview_toggle_keys.empty() ? std::string("/overview") : overview.overview_toggle_keys + " · /overview";
  append_bounded(open_hint);
}

ComposerSnapshot initial_snapshot(TuiFrontEnd* frontend, std::string const& initial_keybind_status)
{
  TuiRuntimeOptions const& options = frontend->options();
  TuiLocalDisplaySettings initial_display_settings = frontend->display_settings_snapshot();
  auto const mode = frontend->prompt_metadata().mode_refresh();
  ComposerSnapshot snapshot{.mode = mode.value,
                            .session_id = {},
                            .input = "",
                            .status = initial_keybind_status,
                            .transcript = options.initial_transcript,
                            .slash_commands = options.slash_commands,
                            .file_references = options.file_references,
                            .custom_themes = initial_display_settings.custom_themes, // presentation
                            .show_images = initial_display_settings.show_images,
                            .image_width_cells = initial_display_settings.image_width_cells,
                            .cursor = initial_display_settings.cursor};
  seed_tui_tool_index(snapshot);
  snapshot.mermaid_config_epoch = initial_display_settings.mermaid_config_epoch;
  snapshot.mermaid_enabled = initial_display_settings.mermaid_enabled;
  snapshot.active_run_hint = runtime_views::active_run_hint_for(frontend->key_bindings());
  refresh_local_palette_hints(snapshot, frontend->key_bindings());
  return snapshot;
}

}  // namespace

QueuedRuntimeEvent::QueuedRuntimeEvent(ava::event::RuntimeEvent event_in, ava::event::EventEnvelopeContext context_in)
    : event(std::move(event_in)), context(std::move(context_in))
{
}

ava::core::VoidResult RuntimeEventQueue::enqueue_envelope(ava::event::EventEnvelope const& envelope)
{
  std::lock_guard lock(mutex);
  events.emplace_back(std::in_place_type<ava::event::EventEnvelope>, envelope);
  received = true;
  return {};
}

void RuntimeEventQueue::enqueue_subagent_launch(ava::agent::SubagentLaunchNotification const& notification)
{
  std::lock_guard lock(mutex);
  events.emplace_back(std::in_place_type<ava::agent::SubagentLaunchNotification>, notification);
  received = true;
}

ava::core::VoidResult RuntimeEventQueue::enqueue(ava::event::RuntimeEvent const& event, ava::event::EventEnvelopeContext context)
{
  std::lock_guard<std::mutex> lock(mutex);
  events.emplace_back(std::in_place_type<QueuedRuntimeEvent>, event, std::move(context));
  received = true;
  return {};
}

std::vector<QueuedTuiEvent> RuntimeEventQueue::drain(std::size_t maximum)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto drained = std::vector<QueuedTuiEvent>{};
  auto const count = std::min(maximum, events.size());
  drained.reserve(count);
  for (std::size_t index = 0; index < count; ++index)
  {
    drained.push_back(std::move(events.front()));
    events.pop_front();
  }
  return drained;
}

bool RuntimeEventQueue::has_pending() const
{
  std::lock_guard lock(mutex);
  return !events.empty();
}

void RuntimeEventQueue::discard()
{
  std::lock_guard<std::mutex> lock(mutex);
  events.clear();
  received = false;
}

bool RuntimeEventQueue::received_any() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return received;
}

// Startup hydration reads backend state before the UI lock exists, so it cannot
// invert the backend-to-UI lock order. RuntimePresentationState is created only
// after the concrete frontend has finished construction, making virtual backend
// reads safe here.
RuntimePresentationState::RuntimePresentationState(TuiFrontEnd* frontend, std::string const& initial_keybind_status)
    : snapshot_(initial_snapshot(frontend, initial_keybind_status)),
      frontend_(frontend),
      applied_slash_catalog_generation_(frontend->options().slash_catalog_generation),
      applied_workspace_catalog_generation_(frontend->options().workspace_catalog_generation),
      sidebar_{.todos = frontend->read_committed_todos(), .mode = snapshot_.mode, .version = frontend->options().app_version}
{
  snapshot_.session_id = frontend->read_committed_session_id();
}

void RuntimePresentationState::refresh_prompt_metadata()
{
  auto const mode = frontend_->prompt_metadata().mode_refresh();
  snapshot_.mode = mode.value;
  sidebar_.mode = mode.value;
}

void RuntimePresentationState::apply_catalog_delivery(TuiFrontEnd const& frontend, TuiCatalogDelivery delivery)
{
  bool completion_sources_changed = false;
  if (!delivery.slash_catalog_generation || !applied_slash_catalog_generation_ || *delivery.slash_catalog_generation != *applied_slash_catalog_generation_)
  {
    snapshot_.slash_commands = std::move(delivery.slash_commands);
    applied_slash_catalog_generation_ = delivery.slash_catalog_generation;
    completion_sources_changed = true;
  }
  if (!delivery.workspace_catalog_generation || !applied_workspace_catalog_generation_ ||
      *delivery.workspace_catalog_generation != *applied_workspace_catalog_generation_)
  {
    snapshot_.file_references = std::move(delivery.file_references);
    applied_workspace_catalog_generation_ = delivery.workspace_catalog_generation;
    completion_sources_changed = true;
  }
  if (completion_sources_changed)
  {
    refresh_local_palette_hints(snapshot_, frontend.key_bindings());
    ++snapshot_.file_references_generation;
  }
}

//static
RuntimePresentationState::BoundaryReads RuntimePresentationState::read_boundary_data(TuiFrontEnd& frontend)
{
  BoundaryReads reads;
  reads.catalog = frontend.read_catalog_delivery();
  reads.todos = frontend.read_committed_todos();
  return reads;
}

void RuntimePresentationState::apply_boundary_data(TuiFrontEnd const& frontend, BoundaryReads reads)
{
  refresh_prompt_metadata();
  if (reads.catalog)
    apply_catalog_delivery(frontend, std::move(*reads.catalog));
  if (reads.todos)
    sidebar_.todos = std::move(*reads.todos);
}

void RuntimePresentationState::refresh_after_backend_action(TuiFrontEnd& frontend)
{
  apply_boundary_data(frontend, read_boundary_data(frontend));
}

void RuntimePresentationState::apply_local_key_bindings(TuiFrontEnd& frontend, TuiKeyBindings bindings)
{
  frontend.key_bindings() = std::move(bindings);
  snapshot_.active_run_hint = runtime_views::active_run_hint_for(frontend.key_bindings());
  refresh_local_palette_hints(snapshot_, frontend.key_bindings());
  refresh_local_overview_hints(snapshot_, frontend);
}

void RuntimePresentationState::apply_local_display_settings(TuiFrontEnd const& frontend, TuiLocalDisplaySettings settings)
{
  snapshot_.custom_themes = std::move(settings.custom_themes);
  snapshot_.show_images = settings.show_images;
  snapshot_.image_width_cells = settings.image_width_cells;
  snapshot_.cursor = settings.cursor;
  if (snapshot_.mermaid_config_epoch != settings.mermaid_config_epoch || snapshot_.mermaid_enabled != settings.mermaid_enabled)
  {
    snapshot_.mermaid_config_epoch = settings.mermaid_config_epoch;
    snapshot_.mermaid_enabled = settings.mermaid_enabled;
    snapshot_.mermaid_projection.reset();
    ++snapshot_.transcript_generation;
  }
  if (!snapshot_.show_images)
  {
    for (auto& attachment : snapshot_.pending_attachments)
      attachment.preview.reset();
  }
  refresh_local_palette_hints(snapshot_, frontend.key_bindings());
  refresh_local_overview_hints(snapshot_, frontend);
}

void RuntimePresentationState::initialize_startup_overview(TuiFrontEnd const& frontend, StartupOverviewSnapshot overview)
{
  snapshot_.startup_overview = std::move(overview);
  refresh_local_overview_hints(snapshot_, frontend);
}

void close_startup_overview_presentation(RuntimePresentationState& presentation, SelectorHost& selectors)
{
  if (selectors.kind() != ActiveSelectList::Overview)
    return;
  selectors.close();
  presentation.clear_startup_overview();
}

void synchronize_startup_overview_presentation(RuntimePresentationState& presentation, SelectorHost& selectors)
{
  if (selectors.kind() != ActiveSelectList::Overview)
    return;
  if (!presentation.screen().startup_overview)
  {
    close_startup_overview_presentation(presentation, selectors);
    return;
  }
  // Rebuild from the popup-local DTO while preserving safe local filter/selection identity.
  auto query = selectors.view() ? selectors.view()->query : std::string{};
  auto selected = selectors.view() ? selectors.view()->selected_item_index : std::size_t{0};
  auto view = overview_select_list_view(*presentation.screen().startup_overview);
  view.query = std::move(query);
  view.selected_item_index = clamp_select_list_selection(view, selected);
  selectors.replace(std::move(view));
}

void RuntimePresentationState::reset_session_projection(bool close_overview)
{
  snapshot_.transcript.clear();
  ++snapshot_.transcript_generation;
  snapshot_.command_output.reset();
  snapshot_.tool_index.clear();
  snapshot_.evicted_tool_index_identities.clear();
  snapshot_.next_tool_index_sequence = 0;
  snapshot_.queued_messages.clear();
  snapshot_.permission_prompt.reset();
  snapshot_.question_prompt.reset();
  snapshot_.sidebar.reset();
  snapshot_.sidebar_drawer_visible = false;
  snapshot_.sidebar_drawer_scroll_offset = 0;
  snapshot_.input.clear();
  snapshot_.input_cursor = 0;
  snapshot_.input_selection_start = std::string::npos;
  snapshot_.input_selection_end = std::string::npos;
  snapshot_.selected_slash_command_index = 0;
  snapshot_.slash_palette_suppressed = false;
  snapshot_.path_completion_force_active = false;
  snapshot_.draft_scroll_offset = 0;
  snapshot_.reasoning_feedback.reset();
  snapshot_.local_command_feedback.reset();
  // Expanded overview is process-local presentation; close on session switch.
  if (close_overview)
    snapshot_.startup_overview.reset();
}

void reset_session_sidebar(SidebarSnapshot& sidebar)
{
  sidebar.activity.clear();
  sidebar.modified_files.clear();
  sidebar.todos.clear();
  sidebar.session_entry_count.reset();
}

void RuntimePresentationState::finish_session_transition(std::string committed_session_id)
{
  reset_session_sidebar(sidebar_);
  snapshot_.session_id = std::move(committed_session_id);
}

// Publication runs synchronously on the TUI thread. Readers never receive a
// writable alias, and feature owners keep policy and backend work outside here.
void RuntimePresentationState::publish_status(std::string status)
{
  snapshot_.status = std::move(status);
}
void RuntimePresentationState::publish_composer(Composer& composer)
{
  composer.project(snapshot_);
}
void RuntimePresentationState::publish_completion(Composer const& composer)
{
  composer.project_completion(snapshot_);
}
void RuntimePresentationState::publish_dimensions(std::size_t width, std::size_t height)
{
  snapshot_.width = width;
  snapshot_.height = height;
}
void RuntimePresentationState::publish_attachment(PendingAttachmentItem badge)
{
  snapshot_.pending_attachments.push_back(std::move(badge));
}
void RuntimePresentationState::clear_attachment_badges() noexcept
{
  snapshot_.pending_attachments.clear();
}
void RuntimePresentationState::publish_selector(std::optional<SelectListView> view)
{
  snapshot_.select_list = std::move(view);
}
void RuntimePresentationState::publish_selector_selection(std::size_t index, std::string query)
{
  // Open a selector before applying input; callers must not publish a selection without its rows.
  ASSERT(snapshot_.select_list);
  snapshot_.select_list->selected_item_index = index;
  snapshot_.select_list->query = std::move(query);
}
void RuntimePresentationState::publish_selector_subtitle(std::string subtitle)
{
  if (snapshot_.select_list)
    snapshot_.select_list->subtitle = std::move(subtitle);
}
void RuntimePresentationState::publish_selector_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query,
                                                     std::string subtitle)
{
  // Open the selector before replacing rows so its title and layout remain defined.
  ASSERT(snapshot_.select_list);
  auto& view = *snapshot_.select_list;
  view.items.resize(std::min(first_changed, view.items.size()));
  view.items.insert(view.items.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
  view.selected_item_index = selected;
  view.query = std::move(query);
  view.subtitle = std::move(subtitle);
}

void RuntimePresentationState::publish_permission_prompt(std::optional<PermissionPromptView> prompt)
{
  snapshot_.permission_prompt = std::move(prompt);
}
void RuntimePresentationState::publish_permission_input(PermissionPromptChoice choice, bool guidance, std::string text)
{
  // Start a permission prompt before applying its decoded input result.
  ASSERT(snapshot_.permission_prompt);
  snapshot_.permission_prompt->selected_choice = choice;
  snapshot_.permission_prompt->guidance_mode = guidance;
  snapshot_.permission_prompt->guidance_text = std::move(text);
}
void RuntimePresentationState::publish_question_prompt(std::optional<QuestionPromptView> prompt)
{
  snapshot_.question_prompt = std::move(prompt);
}
void RuntimePresentationState::publish_question_input(std::size_t selected, std::vector<QuestionPromptOptionView> options, std::string text)
{
  // Start a question prompt before applying its decoded input result.
  ASSERT(snapshot_.question_prompt);
  snapshot_.question_prompt->selected_option_index = selected;
  snapshot_.question_prompt->options = std::move(options);
  snapshot_.question_prompt->custom_text = std::move(text);
}
void RuntimePresentationState::publish_plugin_dock(std::optional<TuiPluginUiDockView> dock)
{
  snapshot_.plugin_ui_dock = std::move(dock);
}
void RuntimePresentationState::publish_plugin_modal(std::optional<TuiPluginUiModalView> modal)
{
  snapshot_.plugin_ui_modal = std::move(modal);
}
void RuntimePresentationState::publish_plugin_selection(std::size_t selected)
{
  // Publish a modal before selecting one of its validated options.
  ASSERT(snapshot_.plugin_ui_modal);
  snapshot_.plugin_ui_modal->selected_option = selected;
}
void RuntimePresentationState::publish_workspace(std::optional<SubagentWorkspaceView> workspace)
{
  snapshot_.subagent_workspace = std::move(workspace);
}
void RuntimePresentationState::publish_mermaid(std::shared_ptr<detail::MermaidTranscriptProjection const> projection)
{
  snapshot_.mermaid_projection = std::move(projection);
  ++snapshot_.transcript_generation;
}

// Viewport updates deliberately separate the scroll offset from painted chrome:
// output received while detached must not move the last painted new-output badge.
void RuntimePresentationState::publish_transcript_scroll(std::size_t offset)
{
  snapshot_.transcript_scroll_offset = offset;
}
void RuntimePresentationState::publish_transcript_chrome(std::size_t offset, std::size_t count)
{
  snapshot_.transcript_scroll_offset = offset;
  snapshot_.transcript_new_output_count = count;
}
void RuntimePresentationState::publish_visible_sidebar(SidebarSnapshot sidebar)
{
  snapshot_.sidebar = std::move(sidebar);
}
void RuntimePresentationState::publish_position_indicator(bool visible)
{
  snapshot_.transcript_position_indicator_visible = visible;
}
void RuntimePresentationState::publish_selection(RuntimeTranscriptSelectionState const& selection)
{
  selection.publish(snapshot_);
}
void RuntimePresentationState::publish_sidebar_drawer(bool visible, std::size_t offset)
{
  snapshot_.sidebar_drawer_visible = visible;
  snapshot_.sidebar_drawer_scroll_offset = offset;
}
void RuntimePresentationState::publish_command_output(std::optional<CommandOutputView> output)
{
  snapshot_.command_output = std::move(output);
}
void RuntimePresentationState::publish_command_output_scroll(std::size_t offset)
{
  // Open output before applying its layout or input scroll result.
  ASSERT(snapshot_.command_output);
  snapshot_.command_output->scroll_offset = offset;
}
void RuntimePresentationState::publish_local_status(std::string status)
{
  settle_local_command_status(snapshot_, std::move(status));
}
void RuntimePresentationState::publish_local_completion(std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools,
                                                        bool record_tools)
{
  settle_local_command_completion(snapshot_, submitted, std::move(output), std::move(tools), record_tools);
}
void RuntimePresentationState::clear_input_feedback()
{
  clear_reasoning_feedback_for_user_input(snapshot_);
}
void RuntimePresentationState::publish_display_preview(runtime_views::DisplayPreviewTransaction const& preview)
{
  preview.apply_image_overlay(snapshot_);
}
void RuntimePresentationState::reapply_display_preview(runtime_views::DisplayPreviewTransaction& preview)
{
  runtime_views::reapply_settings_preview_after_display_reload(preview, snapshot_);
}
void RuntimePresentationState::publish_project_trust(std::optional<ProjectTrustSnapshot> trust)
{
  snapshot_.project_trust = std::move(trust);
}
void RuntimePresentationState::publish_tool_presentation(ToolPresentation presentation)
{
  snapshot_.tool_presentation = presentation;
}
void RuntimePresentationState::publish_thinking_visibility(bool visible)
{
  snapshot_.thinking_visible = visible;
}
std::optional<SelectListView> RuntimePresentationState::withdraw_selector_projection()
{
  return std::exchange(snapshot_.select_list, std::nullopt);
}
void RuntimePresentationState::clear_startup_overview()
{
  snapshot_.startup_overview.reset();
}
void RuntimePresentationState::publish_transcript(std::vector<TranscriptItem> transcript)
{
  snapshot_.transcript = std::move(transcript);
  ++snapshot_.transcript_generation;
}
std::ptrdiff_t RuntimePresentationState::append_transcript(TranscriptItem item)
{
  return runtime_transcript::push_transcript(snapshot_, std::move(item));
}
std::ptrdiff_t RuntimePresentationState::append_fallback_outputs(std::vector<std::string> const& output, std::string const& meta)
{
  return runtime_transcript::push_fallback_assistant_outputs(snapshot_, output, meta);
}
CappedTranscriptSnapshotUpdate RuntimePresentationState::publish_run_transcript(ActiveRunEvents& events, std::vector<TranscriptItem> turn)
{
  // Provider replacement must carry the user's current expansion choices through
  // leading eviction before readers observe the new generation.
  auto const tools = runtime_transcript::capture_tool_detail_visibility(snapshot_.transcript);
  auto const thinking = runtime_transcript::capture_thinking_expansion(snapshot_.transcript);
  auto const capped = events.apply_transcript_projection(snapshot_.transcript, std::move(turn));
  runtime_transcript::carry_tool_detail_visibility(tools, snapshot_.transcript);
  runtime_transcript::carry_thinking_expansion(thinking, snapshot_.transcript, capped.item_index_shift);
  ++snapshot_.transcript_generation;
  return capped;
}
void RuntimePresentationState::publish_tool(ToolTimelineItem tool, TuiToolIndexOrigin origin)
{
  record_tui_tool(snapshot_, std::move(tool), origin);
}
void RuntimePresentationState::publish_tool_update(ToolTimelineItem const& tool)
{
  update_indexed_tui_tool(snapshot_, tool);
}
bool RuntimePresentationState::toggle_tool_details(std::size_t index)
{
  if (index >= snapshot_.transcript.size() || !snapshot_.transcript[index].tool)
    return false;
  auto& tool = *snapshot_.transcript[index].tool;
  tool.details_visible = detail::tool_card_presentation(tool, snapshot_.tool_presentation) != ToolPresentation::Expanded;
  ++snapshot_.transcript_generation;
  return true;
}
std::optional<std::size_t> RuntimePresentationState::toggle_matching_tool_details(std::string_view query)
{
  auto const index = toggle_latest_matching_tool_details(snapshot_.transcript, query, snapshot_.tool_presentation);
  if (index)
    ++snapshot_.transcript_generation;
  return index;
}
bool RuntimePresentationState::toggle_thinking_details(std::size_t index, std::size_t width)
{
  if (!toggle_thinking_expansion_at(snapshot_.transcript, index, width, snapshot_.thinking_visible))
    return false;
  ++snapshot_.transcript_generation;
  return true;
}
std::optional<std::size_t> RuntimePresentationState::toggle_latest_thinking_details(std::size_t width)
{
  auto const index = toggle_latest_boundable_thinking(snapshot_.transcript, width, snapshot_.thinking_visible);
  if (index)
    ++snapshot_.transcript_generation;
  return index;
}
void RuntimePresentationState::publish_processing(bool processing)
{
  snapshot_.processing = processing;
}
void RuntimePresentationState::publish_spinner(std::size_t frame)
{
  snapshot_.spinner_frame = frame;
}
void RuntimePresentationState::publish_run_activity(std::vector<SidebarActivityItem> activity)
{
  sidebar_.activity = std::move(activity);
}
void RuntimePresentationState::publish_run_todos(std::vector<agent::TodoItem> todos)
{
  sidebar_.todos = std::move(todos);
}
void RuntimePresentationState::publish_queued_messages(std::vector<QueuedMessageItem> messages)
{
  snapshot_.queued_messages = std::move(messages);
}
void RuntimePresentationState::publish_modified_files(std::vector<SidebarModifiedFile> const& files)
{
  for (auto const& file : files)
    if (!std::ranges::any_of(sidebar_.modified_files, [&](auto const& existing) { return existing.path == file.path; }))
      sidebar_.modified_files.push_back(file);
  constexpr std::size_t maximum = 50;
  if (sidebar_.modified_files.size() > maximum)
    sidebar_.modified_files.erase(sidebar_.modified_files.begin(), sidebar_.modified_files.end() - maximum);
}
void RuntimePresentationState::publish_activity_item(SidebarActivityItem item)
{
  auto existing = std::ranges::find_if(sidebar_.activity, [&](auto const& activity) { return activity.id == item.id; });
  if (existing == sidebar_.activity.end())
    sidebar_.activity.push_back(std::move(item));
  else
    *existing = std::move(item);
}
void RuntimePresentationState::settle_responding_activity(ToolTimelineStatus status, std::string detail)
{
  auto item = std::ranges::find_if(sidebar_.activity, [](auto const& activity) { return activity.id == "responding"; });
  if (item == sidebar_.activity.end() || item->status != ToolTimelineStatus::Running)
    return;
  item->status = status;
  item->detail = std::move(detail);
}

void RuntimePresentationState::publish_reasoning_feedback(std::string feedback)
{
  apply_reasoning_cycle_success(snapshot_, std::move(feedback));
}
void RuntimePresentationState::publish_local_output(std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools)
{
  open_command_output(snapshot_, submitted, std::move(output), std::move(tools));
}
void RuntimePresentationState::publish_local_error(std::string_view submitted, std::string error)
{
  open_command_error(snapshot_, submitted, std::move(error));
}
void RuntimePresentationState::invalidate_transcript_layout()
{
  ++snapshot_.transcript_generation;
}

void RuntimePresentationState::clear_local_feedback()
{
  snapshot_.local_command_feedback.reset();
}
void RuntimePresentationState::publish_composer_feedback(ComposerInputResult const& result)
{
  if (result.status)
    snapshot_.status = *result.status;
}

}  // namespace ava::tui
