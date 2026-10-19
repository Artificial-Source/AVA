#include "sys.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_output.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/runtime_views_internal.h"

#include <utility>

namespace ava::tui {
namespace {

ComposerSnapshot initial_snapshot(TuiFrontEnd* frontend, TuiRuntimeOptions& options)
{
  auto const mode = frontend->prompt_metadata().mode_refresh();
  ComposerSnapshot snapshot{.mode = mode.value,
                            .session_id = options.session_id,
                            .input = "",
                            .status = options.initial_status,
                            .transcript = std::move(options.initial_transcript),
                            .slash_commands = std::move(options.slash_commands),
                            .file_references = std::move(options.file_references),
                            .custom_themes = options.custom_themes,
                            .project_trust = options.project_trust,
                            .show_images = options.show_images,
                            .image_width_cells = options.image_width_cells,
                            .cursor = options.cursor,
                            .startup_overview = options.startup_overview};
  seed_tui_tool_index(snapshot);
  snapshot.mermaid_config_epoch = options.mermaid_render.config_epoch;
  snapshot.mermaid_enabled = options.mermaid_render.enabled;
  snapshot.active_run_hint = runtime_views::active_run_hint_for(options.key_bindings);
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

RuntimePresentationState::RuntimePresentationState(TuiFrontEnd* frontend, TuiRuntimeOptions& options)
    : snapshot(initial_snapshot(frontend, options)),
      frontend_(frontend),
      applied_slash_catalog_generation_(options.slash_catalog_generation),
      applied_workspace_catalog_generation_(options.workspace_catalog_generation),
      sidebar{.todos = std::move(options.initial_todos),
              .session_id = options.session_id,
              .mode = snapshot.mode,
              .workspace = options.workspace,
              .version = options.app_version,
              .session_path = options.session_path}
{
}

void RuntimePresentationState::refresh_token_status(TuiRuntimeOptions const& options)
{
  DoutEntering(dc::notice, "RuntimePresentationState::refresh_token_status(" << options << ")");

  if (options.refresh_session_statistics)
    options.refresh_session_statistics();
}

void RuntimePresentationState::refresh_prompt_metadata()
{
  auto const mode = frontend_->prompt_metadata().mode_refresh();
  snapshot.mode = mode.value;
  sidebar.mode = mode.value;
}

void RuntimePresentationState::apply_runtime_state_snapshot(TuiRuntimeOptions const& options, TuiRuntimeStateSnapshot state)
{
  DoutEntering(dc::notice, "RuntimePresentationState::apply_runtime_state_snapshot(" << options << ", " << state << ")");

  auto const session_changed = command_session_grants.clear_for_session_transition(snapshot.session_id, state.session_id);
  if (session_changed)
  {
    pending_image_attachments.clear();
    snapshot.pending_attachments.clear();
  }
  refresh_prompt_metadata();
  snapshot.session_id = std::move(state.session_id);
  snapshot.status = std::move(state.status);
  bool completion_sources_changed = false;
  if (!state.slash_catalog_generation || !applied_slash_catalog_generation_ || *state.slash_catalog_generation != *applied_slash_catalog_generation_)
  {
    snapshot.slash_commands = std::move(state.slash_commands);
    applied_slash_catalog_generation_ = state.slash_catalog_generation;
    completion_sources_changed = true;
  }
  if (!state.workspace_catalog_generation || !applied_workspace_catalog_generation_ ||
      *state.workspace_catalog_generation != *applied_workspace_catalog_generation_)
  {
    snapshot.file_references = std::move(state.file_references);
    applied_workspace_catalog_generation_ = state.workspace_catalog_generation;
    completion_sources_changed = true;
  }
  if (completion_sources_changed)
    ++snapshot.file_references_generation;
  snapshot.custom_themes = std::move(state.custom_themes);
  snapshot.project_trust = std::move(state.project_trust);
  snapshot.show_images = state.show_images;
  snapshot.image_width_cells = state.image_width_cells;
  snapshot.cursor = state.cursor;
  if (snapshot.mermaid_config_epoch != state.mermaid_config_epoch || snapshot.mermaid_enabled != state.mermaid_enabled)
  {
    snapshot.mermaid_config_epoch = state.mermaid_config_epoch;
    snapshot.mermaid_enabled = state.mermaid_enabled;
    snapshot.mermaid_projection.reset();
    ++snapshot.transcript_generation;
  }
  snapshot.startup_overview = std::move(state.startup_overview);
  if (!snapshot.show_images)
  {
    for (auto& attachment : snapshot.pending_attachments)
      attachment.preview.reset();
  }

  sidebar.session_id = snapshot.session_id;
  sidebar.session_path = std::move(state.session_path);
  sidebar.workspace = std::move(state.workspace);
  sidebar.todos = std::move(state.todos);
  refresh_token_status(options);
}

void close_startup_overview_presentation(ComposerSnapshot& snapshot, ActiveSelectList& active_select_list)
{
  if (active_select_list != ActiveSelectList::Overview)
    return;
  snapshot.select_list.reset();
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
  // Rebuild from the new DTO while preserving safe local filter/selection identity.
  auto query = snapshot.select_list ? snapshot.select_list->query : std::string{};
  auto selected = snapshot.select_list ? snapshot.select_list->selected_item_index : std::size_t{0};
  auto view = overview_select_list_view(*snapshot.startup_overview);
  view.query = std::move(query);
  view.selected_item_index = clamp_select_list_selection(view, selected);
  snapshot.select_list = std::move(view);
}

void apply_runtime_state_snapshot_with_overview_sync(TuiRuntimeOptions const& options, RuntimePresentationState& presentation_state,
                                                     ActiveSelectList& active_select_list, TuiRuntimeStateSnapshot state)
{
  presentation_state.apply_runtime_state_snapshot(options, std::move(state));
  synchronize_startup_overview_presentation(presentation_state.snapshot, active_select_list);
}

bool apply_runtime_state_snapshot_with_presentation_transition(TuiRuntimeOptions const& options, RuntimePresentationState& presentation_state,
                                                               RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                                                               TranscriptSearchController& transcript_search,
                                                               RuntimeSubagentWorkspaceController& subagent_workspace, ActiveSelectList& active_select_list,
                                                               TuiRuntimeStateSnapshot state)
{
  auto& snapshot = presentation_state.snapshot;
  auto& sidebar = presentation_state.sidebar;
  auto const session_changed = snapshot.session_id != state.session_id;
  if (session_changed)
  {
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
  }

  apply_runtime_state_snapshot_with_overview_sync(options, presentation_state, active_select_list, std::move(state));
  return session_changed;
}

}  // namespace ava::tui
