#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_completions.h"
#include "ava/tui/command_output.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/theme.h"

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
                            .cursor = initial_display_settings.cursor
                           };
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

ava::event::EventEnvelopeSink RuntimeEventQueue::envelope_sink()
{
  return [this](ava::event::EventEnvelope const& event) -> ava::core::VoidResult {
    std::lock_guard<std::mutex> lock(mutex);
    events.emplace_back(std::in_place_type<ava::event::EventEnvelope>, event);
    received = true;
    return {};
  };
}

ava::agent::SubagentLaunchSink RuntimeEventQueue::subagent_launch_sink()
{
  return [this](ava::agent::SubagentLaunchNotification const& notification) {
    std::lock_guard<std::mutex> lock(mutex);
    events.emplace_back(std::in_place_type<ava::agent::SubagentLaunchNotification>, notification);
    received = true;
  };
}

ava::core::VoidResult RuntimeEventQueue::enqueue(ava::event::RuntimeEvent const& event, ava::event::EventEnvelopeContext context)
{
  std::lock_guard<std::mutex> lock(mutex);
  events.emplace_back(std::in_place_type<QueuedRuntimeEvent>, event, std::move(context));
  received = true;
  return {};
}

std::vector<QueuedTuiEvent> RuntimeEventQueue::drain()
{
  std::lock_guard<std::mutex> lock(mutex);
  auto drained = std::vector<QueuedTuiEvent>{};
  drained.swap(events);
  return drained;
}

void RuntimeEventQueue::discard()
{
  std::lock_guard<std::mutex> lock(mutex);
  events.clear();
  received = false;
}

bool RuntimeEventQueue::received_any()
{
  std::lock_guard<std::mutex> lock(mutex);
  return received;
}

// Startup hydration reads backend state before the UI lock exists, so it cannot
// invert the backend-to-UI lock order. RuntimePresentationState is created only
// after the concrete frontend has finished construction, making virtual backend
// reads safe here.
RuntimePresentationState::RuntimePresentationState(TuiFrontEnd* frontend, std::string const& initial_keybind_status)
    : snapshot(initial_snapshot(frontend, initial_keybind_status)),
      frontend_(frontend),
      applied_slash_catalog_generation_(frontend->options().slash_catalog_generation),
      applied_workspace_catalog_generation_(frontend->options().workspace_catalog_generation),
      sidebar{.todos = frontend->read_committed_todos(), .mode = snapshot.mode, .version = frontend->options().app_version}
{
  snapshot.session_id = frontend->read_committed_session_id();
}

void RuntimePresentationState::refresh_prompt_metadata()
{
  auto const mode = frontend_->prompt_metadata().mode_refresh();
  snapshot.mode = mode.value;
  sidebar.mode = mode.value;
}

void RuntimePresentationState::apply_catalog_delivery(TuiFrontEnd const& frontend, TuiCatalogDelivery delivery)
{
  bool completion_sources_changed = false;
  if (!delivery.slash_catalog_generation || !applied_slash_catalog_generation_ || *delivery.slash_catalog_generation != *applied_slash_catalog_generation_)
  {
    snapshot.slash_commands = std::move(delivery.slash_commands);
    applied_slash_catalog_generation_ = delivery.slash_catalog_generation;
    completion_sources_changed = true;
  }
  if (!delivery.workspace_catalog_generation || !applied_workspace_catalog_generation_ ||
      *delivery.workspace_catalog_generation != *applied_workspace_catalog_generation_)
  {
    snapshot.file_references = std::move(delivery.file_references);
    applied_workspace_catalog_generation_ = delivery.workspace_catalog_generation;
    completion_sources_changed = true;
  }
  if (completion_sources_changed)
  {
    refresh_local_palette_hints(snapshot, frontend.key_bindings());
    ++snapshot.file_references_generation;
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
    sidebar.todos = std::move(*reads.todos);
}

void RuntimePresentationState::refresh_after_backend_action(TuiFrontEnd& frontend)
{
  apply_boundary_data(frontend, read_boundary_data(frontend));
}

void RuntimePresentationState::apply_local_key_bindings(TuiFrontEnd& frontend, TuiKeyBindings bindings)
{
  frontend.key_bindings() = std::move(bindings);
  snapshot.active_run_hint = runtime_views::active_run_hint_for(frontend.key_bindings());
  refresh_local_palette_hints(snapshot, frontend.key_bindings());
  refresh_local_overview_hints(snapshot, frontend);
}

void RuntimePresentationState::apply_local_display_settings(TuiFrontEnd const& frontend, TuiLocalDisplaySettings settings)
{
  snapshot.custom_themes = std::move(settings.custom_themes);
  snapshot.show_images = settings.show_images;
  snapshot.image_width_cells = settings.image_width_cells;
  snapshot.cursor = settings.cursor;
  if (snapshot.mermaid_config_epoch != settings.mermaid_config_epoch || snapshot.mermaid_enabled != settings.mermaid_enabled)
  {
    snapshot.mermaid_config_epoch = settings.mermaid_config_epoch;
    snapshot.mermaid_enabled = settings.mermaid_enabled;
    snapshot.mermaid_projection.reset();
    ++snapshot.transcript_generation;
  }
  if (!snapshot.show_images)
  {
    for (auto& attachment : snapshot.pending_attachments)
      attachment.preview.reset();
  }
  refresh_local_palette_hints(snapshot, frontend.key_bindings());
  refresh_local_overview_hints(snapshot, frontend);
}

void RuntimePresentationState::initialize_startup_overview(TuiFrontEnd const& frontend, StartupOverviewSnapshot overview)
{
  snapshot.startup_overview = std::move(overview);
  refresh_local_overview_hints(snapshot, frontend);
}

void close_startup_overview_presentation(ComposerSnapshot& snapshot, ActiveSelectList& active_select_list)
{
  if (active_select_list != ActiveSelectList::Overview)
    return;
  snapshot.select_list.reset();
  snapshot.startup_overview.reset();
  active_select_list = ActiveSelectList::None;
}

void synchronize_startup_overview_presentation(ComposerSnapshot& snapshot, ActiveSelectList& active_select_list)
{
  if (active_select_list != ActiveSelectList::Overview)
    return;
  if (!snapshot.startup_overview)
  {
    close_startup_overview_presentation(snapshot, active_select_list);
    return;
  }
  // Rebuild from the popup-local DTO while preserving safe local filter/selection identity.
  auto query = snapshot.select_list ? snapshot.select_list->query : std::string{};
  auto selected = snapshot.select_list ? snapshot.select_list->selected_item_index : std::size_t{0};
  auto view = overview_select_list_view(*snapshot.startup_overview);
  view.query = std::move(query);
  view.selected_item_index = clamp_select_list_selection(view, selected);
  snapshot.select_list = std::move(view);
}

bool apply_committed_session_transition(RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                                        TranscriptSearchController& transcript_search, RuntimeSubagentWorkspaceController& subagent_workspace,
                                        ActiveSelectList& active_select_list, std::string committed_session_id)
{
  auto& snapshot = presentation_state.snapshot;
  auto& sidebar = presentation_state.sidebar;
  auto const session_changed = snapshot.session_id != committed_session_id;
  if (session_changed)
  {
    static_cast<void>(presentation_state.command_session_grants.clear_for_session_transition(snapshot.session_id, committed_session_id));
    presentation_state.pending_image_attachments.clear();
    snapshot.pending_attachments.clear();
    transcript_search.reset_for_session_transition();
    subagent_workspace.reset_for_session_transition();
    draft_state.reset_for_session_transition();
    renderer.reset_for_session_transition();

    snapshot.transcript.clear();
    ++snapshot.transcript_generation;
    snapshot.command_output.reset();
    snapshot.tool_index.clear();
    snapshot.evicted_tool_index_identities.clear();
    snapshot.next_tool_index_sequence = 0;
    snapshot.queued_messages.clear();
    snapshot.permission_prompt.reset();
    snapshot.question_prompt.reset();
    snapshot.sidebar.reset();
    snapshot.sidebar_drawer_visible = false;
    snapshot.sidebar_drawer_scroll_offset = 0;
    snapshot.input.clear();
    snapshot.input_cursor = 0;
    snapshot.input_selection_start = std::string::npos;
    snapshot.input_selection_end = std::string::npos;
    snapshot.selected_slash_command_index = 0;
    snapshot.slash_palette_suppressed = false;
    snapshot.path_completion_force_active = false;
    snapshot.draft_scroll_offset = 0;
    snapshot.reasoning_feedback.reset();
    snapshot.local_command_feedback.reset();
    // Expanded overview is process-local presentation; close on session switch.
    close_startup_overview_presentation(snapshot, active_select_list);
    if (snapshot.select_list)
      snapshot.select_list.reset();
    if (active_select_list != ActiveSelectList::None)
      active_select_list = ActiveSelectList::None;

    sidebar.activity.clear();
    sidebar.modified_files.clear();
    sidebar.todos.clear();
    sidebar.session_entry_count.reset();
    snapshot.session_id = std::move(committed_session_id);
  }

  return session_changed;
}

bool apply_committed_session_transition_and_refresh(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state,
                                                    RuntimeRenderer& renderer, TranscriptSearchController& transcript_search,
                                                    RuntimeSubagentWorkspaceController& subagent_workspace, ActiveSelectList& active_select_list,
                                                    std::string committed_session_id)
{
  auto const session_changed = apply_committed_session_transition(presentation_state, draft_state, renderer, transcript_search, subagent_workspace,
                                                                  active_select_list, std::move(committed_session_id));
  presentation_state.refresh_after_backend_action(frontend);
  return session_changed;
}

}  // namespace ava::tui
