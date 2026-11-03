#pragma once

#include "ava/event/EventEnvelope.h"
#include "ava/event/events.h"
#include "ava/agent/subagent_launch.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "debug.h"

namespace ava::tui {

class Composer;
struct ComposerInputResult;
class ActiveRunEvents;
class RuntimeTranscriptSelectionState;
struct CappedTranscriptSnapshotUpdate;
namespace runtime_views {
struct DisplayPreviewTransaction;
}
class RuntimeRenderer;
class SelectorHost;
class RuntimeSubagentWorkspaceController;
class TranscriptSearchController;

struct QueuedRuntimeEvent
{
  QueuedRuntimeEvent(ava::event::RuntimeEvent event_in, ava::event::EventEnvelopeContext context_in);

  ava::event::RuntimeEvent event;
  ava::event::EventEnvelopeContext context;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using QueuedTuiEvent = std::variant<QueuedRuntimeEvent, ava::event::EventEnvelope, ava::agent::SubagentLaunchNotification>;

struct RuntimeEventQueue
{
  [[nodiscard]] ava::core::VoidResult enqueue(ava::event::RuntimeEvent const& event, ava::event::EventEnvelopeContext context = {});
  [[nodiscard]] ava::core::VoidResult enqueue_envelope(ava::event::EventEnvelope const& envelope);
  void enqueue_subagent_launch(ava::agent::SubagentLaunchNotification const& notification);
  [[nodiscard]] std::vector<QueuedTuiEvent> drain(std::size_t maximum = std::numeric_limits<std::size_t>::max());
  [[nodiscard]] bool has_pending() const;
  void discard();
  [[nodiscard]] bool received_any() const;

  // Queued runtime events contain arbitrary messages and authority aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  mutable std::mutex mutex;
  std::deque<QueuedTuiEvent> events;
  bool received = false;
};

enum class RuntimeEventDrainResult
{
  NoEvents,
  UpdatedNoRender,
  Rendered,
  RenderFailed
};

enum class ActiveSelectList
{
  None,
  Hotkeys,
  Settings,
  Model,
  Reasoning,
  ScopedModels,
  Session,
  BranchSummary,
  TranscriptSearch,
  ForkUserTurn,
  CopyUserTurn,
  PromptStash,
  Overview,
  SubagentWorkspace
};

enum class ComposerJumpMode
{
  None,
  Forward,
  Backward
};

struct PendingSessionArchiveAction
{
  ava::core::NoLogString session_id;
  bool archive = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class RuntimePresentationState final
{
 public:
  RuntimePresentationState(TuiFrontEnd* frontend, std::string const& initial_keybind_status);

  void refresh_prompt_metadata();
  void apply_catalog_delivery(TuiFrontEnd const& frontend, TuiCatalogDelivery delivery);
  // The retained catalog/todo transports read at one serialized boundary.
  struct BoundaryReads
  {
    std::optional<TuiCatalogDelivery> catalog;
    std::optional<std::vector<agent::TodoItem>> todos;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };
  // Reads the retained transports through the frontend's application boundary. Like
  // read_committed_session_id, call this without holding the UI lock.
  static BoundaryReads read_boundary_data(TuiFrontEnd& frontend);
  // Applies previously read transports without reading or replacing popup-only facts.
  void apply_boundary_data(TuiFrontEnd const& frontend, BoundaryReads reads);
  // Reads and applies in one step for callers that hold no lock.
  void refresh_after_backend_action(TuiFrontEnd& frontend);
  // Applies TUI-owned bindings and refreshes every local hint projection.
  // This never changes backend catalog generations or applies a backend snapshot.
  void apply_local_key_bindings(TuiFrontEnd& frontend, TuiKeyBindings bindings);
  // Applies TUI-owned display values without changing backend runtime state.
  void apply_local_display_settings(TuiFrontEnd const& frontend, TuiLocalDisplaySettings settings);
  // Starts one popup-local overview lifetime from fresh backend facts, then
  // composes current UI-owned theme and keybinding hints.
  void initialize_startup_overview(TuiFrontEnd const& frontend, StartupOverviewSnapshot overview);

  // TUI thread: publish feedback without giving feature owners writable screen storage.
  void publish_status(std::string status);
  // TUI thread: Composer alone selects and projects editor/completion fields.
  // The composer and this projection must outlive the synchronous call only.
  void publish_composer(Composer& composer);
  void publish_completion(Composer const& composer);
  // TUI thread: publish observed terminal geometry before preparing layouts.
  void publish_dimensions(std::size_t width, std::size_t height);
  // TUI thread: attachment ownership stays in PendingAttachments. Badge changes
  // accompany its successful stage/take/clear operations, without exposing paths.
  void publish_attachment(PendingAttachmentItem badge);
  void clear_attachment_badges() noexcept;
  // TUI thread: SelectorHost owns opening/closing policy. These operations keep
  // one row vector in the projection; readers borrow it until the next mutation.
  void publish_selector(std::optional<SelectListView> view);
  void publish_selector_selection(std::size_t index, std::string query);
  void publish_selector_subtitle(std::string subtitle);
  void publish_selector_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query, std::string subtitle);

  // TUI thread: prompt owners publish one modal or retire it before completing
  // the waiting request. Input updates preserve the existing modal identity.
  void publish_permission_prompt(std::optional<PermissionPromptView> prompt);
  void publish_permission_input(PermissionPromptChoice choice, bool guidance, std::string text);
  void publish_question_prompt(std::optional<QuestionPromptView> prompt);
  void publish_question_input(std::size_t selected, std::vector<QuestionPromptOptionView> options, std::string text);
  // TUI thread: plugin admission stays in its synchronized coordinator. These
  // methods only publish acknowledged surfaces or remove retired surfaces.
  void publish_plugin_dock(std::optional<TuiPluginUiDockView> dock);
  void publish_plugin_modal(std::optional<TuiPluginUiModalView> modal);
  void publish_plugin_selection(std::size_t selected);
  // TUI thread: publish the workspace's bounded view without backend job authority.
  void publish_workspace(std::optional<SubagentWorkspaceView> workspace);
  // TUI thread: accepted Mermaid output changes transcript geometry, so publish
  // its immutable shared projection and generation together.
  void publish_mermaid(std::shared_ptr<detail::MermaidTranscriptProjection const> projection);

  // TUI thread: viewport owners publish chrome without exposing transcript storage.
  void publish_transcript_scroll(std::size_t offset);
  void publish_transcript_chrome(std::size_t offset, std::size_t new_output_count);
  void publish_visible_sidebar(SidebarSnapshot sidebar);
  void publish_position_indicator(bool visible);
  void publish_selection(RuntimeTranscriptSelectionState const& selection);
  // TUI thread: drawer and output scrolling are clamped by their layout owner.
  void publish_sidebar_drawer(bool visible, std::size_t offset);
  void publish_command_output(std::optional<CommandOutputView> output);
  void publish_command_output_scroll(std::size_t offset);
  // TUI thread: preserve feedback precedence when an action replaces local output.
  void publish_local_status(std::string status);
  void publish_local_completion(std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools = {},
                                bool record_tools = true);
  void clear_input_feedback();
  void clear_local_feedback();
  void publish_composer_feedback(ComposerInputResult const& result);
  void publish_reasoning_feedback(std::string feedback);
  void publish_local_output(std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools = {});
  void publish_local_error(std::string_view submitted, std::string error);
  // TUI thread: explicitly invalidate layouts when a display toggle requires it.
  void invalidate_transcript_layout();
  // TUI thread: preview changes only local display fields; backend settings remain authoritative.
  void publish_display_preview(runtime_views::DisplayPreviewTransaction const& preview);
  void reapply_display_preview(runtime_views::DisplayPreviewTransaction& preview);
  void publish_project_trust(std::optional<ProjectTrustSnapshot> trust);
  void publish_tool_presentation(ToolPresentation presentation);
  void publish_thinking_visibility(bool visible);
  // TUI thread: scope search layout preparation by moving, then restoring, the single selector projection.
  [[nodiscard]] std::optional<SelectListView> withdraw_selector_projection();
  void clear_startup_overview();
  // TUI thread: transcript changes always invalidate the layout generation. Cap results
  // must be passed to viewport/search owners before they prepare another layout.
  void publish_transcript(std::vector<TranscriptItem> transcript);
  [[nodiscard]] std::ptrdiff_t append_transcript(TranscriptItem item);
  [[nodiscard]] std::ptrdiff_t append_fallback_outputs(std::vector<std::string> const& output, std::string const& meta);
  [[nodiscard]] CappedTranscriptSnapshotUpdate publish_run_transcript(ActiveRunEvents& events, std::vector<TranscriptItem> turn);
  void publish_tool(ToolTimelineItem tool, TuiToolIndexOrigin origin);
  void publish_tool_update(ToolTimelineItem const& tool);
  [[nodiscard]] bool toggle_tool_details(std::size_t item_index);
  [[nodiscard]] std::optional<std::size_t> toggle_matching_tool_details(std::string_view query);
  [[nodiscard]] bool toggle_thinking_details(std::size_t item_index, std::size_t width);
  [[nodiscard]] std::optional<std::size_t> toggle_latest_thinking_details(std::size_t width);
  // TUI thread: run reduction replaces current activity/todos; file history is bounded
  // and merged by path so a later update does not duplicate an earlier edit.
  void publish_processing(bool processing);
  void publish_spinner(std::size_t frame);
  void publish_run_activity(std::vector<SidebarActivityItem> activity);
  void publish_run_todos(std::vector<agent::TodoItem> todos);
  void publish_queued_messages(std::vector<QueuedMessageItem> messages);
  void publish_modified_files(std::vector<SidebarModifiedFile> const& files);
  void publish_activity_item(SidebarActivityItem item);
  void settle_responding_activity(ToolTimelineStatus status, std::string detail);

  // These existing render DTOs are shared only on the TUI thread. Feature owners
  // publish their own screen fields here; renderer reads/prepares the projection.
  // Selector, composer, viewport and attachment authority stays in its owner.
  // References remain valid for this object's lifetime; nested views expire on
  // the corresponding feature's next mutation. No backend authority is exposed.
  [[nodiscard]] ComposerSnapshot const& screen() const noexcept { return snapshot_; }
  // Live sidebar facts are published by backend hydration and ActiveRun; the
  // renderer/TranscriptView consume them through const references or copies.
  [[nodiscard]] SidebarSnapshot const& sidebar() const noexcept { return sidebar_; }

  // TUI thread: the transition coordinator clears the old session before applying
  // committed identity. Call reset before closing features, then finish afterward.
  void reset_session_projection(bool close_overview);
  void finish_session_transition(std::string committed_session_id);

  // The screen contains prompt/message bodies and must never be debug streamed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ComposerSnapshot snapshot_;
  TuiFrontEnd* frontend_;
  std::optional<std::size_t> applied_slash_catalog_generation_;
  std::optional<std::size_t> applied_workspace_catalog_generation_;

  SidebarSnapshot sidebar_;
};

// TUI-thread projection transform used when committed session identity changes.
// Clear only session-local facts; mode/version remain process or backend display values.
void reset_session_sidebar(SidebarSnapshot& sidebar);

// Shared overview presentation helpers for popup-local display/keybinding changes
// and competing presentation authority.
void close_startup_overview_presentation(RuntimePresentationState& presentation, SelectorHost& selectors);
void synchronize_startup_overview_presentation(RuntimePresentationState& presentation, SelectorHost& selectors);

}  // namespace ava::tui
