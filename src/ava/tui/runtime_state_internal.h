// Defines serialized event storage and the TUI-owned display data updated through feature operations.
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

// Owned FIFO entry pairing a legacy event with its captured envelope context. Producers construct it under
// ActiveRunInbox serialization; the TUI later moves it out for reduction. No borrowed event payload survives
// enqueue.
struct QueuedRuntimeEvent
{
  // Serialized caller: copy/move event and context together so later reduction does not consult mutable
  // producer context.
  QueuedRuntimeEvent(ava::event::RuntimeEvent event_in, ava::event::EventEnvelopeContext context_in);

  // Owned event payload moved through the queue; only the externally serialized queue owner or drained consumer
  // accesses it.
  ava::event::RuntimeEvent event;
  // Context captured with this event under the inbox mutex, preserving event/session provenance across worker-
  // to-TUI transfer.
  ava::event::EventEnvelopeContext context;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned queue alternatives preserve one FIFO order across legacy events, envelopes, and subagent-launch
// notifications.
using QueuedTuiEvent = std::variant<QueuedRuntimeEvent, ava::event::EventEnvelope, ava::agent::SubagentLaunchNotification>;

// Pure FIFO value; ActiveRunInbox is its sole production owner and holds
// its one producer mutex across queue and request-context operations. This value
// has no independent synchronization and must never be shared without that lock.
struct RuntimeEventQueue
{
  // Externally serialized caller: append an event with its captured context and remember that events arrived; production
  // ActiveRunInbox holds its sole producer mutex.
  [[nodiscard]] ava::core::VoidResult enqueue(ava::event::RuntimeEvent const& event, ava::event::EventEnvelopeContext context = {});
  // Externally serialized caller: retain the already formed envelope without changing provenance or FIFO order.
  [[nodiscard]] ava::core::VoidResult enqueue_envelope(ava::event::EventEnvelope const& envelope);
  // Externally serialized caller: append path-free launch notification in the same FIFO order as runtime
  // events.
  void enqueue_subagent_launch(ava::agent::SubagentLaunchNotification const& notification);
  // Externally serialized TUI consumer: move at most maximum oldest entries out; remaining entries retain FIFO
  // order and the record of earlier arrivals survives.
  [[nodiscard]] std::vector<QueuedTuiEvent> drain(std::size_t maximum = std::numeric_limits<std::size_t>::max());
  // Externally serialized reader: test backlog for loop readiness; this value performs no locking itself.
  [[nodiscard]] bool has_pending() const;
  // Externally serialized owner: drop all entries and clear the record of earlier arrivals when retiring the run/session queue.
  void discard();
  // Externally serialized reader: distinguish a run that emitted events from an empty one even after earlier
  // drains.
  [[nodiscard]] bool received_any() const;

  // Queued runtime events contain arbitrary message bodies and backend data.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // FIFO payload ownership, accessed only under the owning inbox's mutex in production; drained values become
  // TUI-owned.
  std::deque<QueuedTuiEvent> events;
  // Records that at least one event arrived; enqueue sets it and discard clears it. Draining does not clear it.
  bool received = false;
};

// TUI-thread reducer result preserves whether consumed updates require painting and whether a render request
// failed.
enum class RuntimeEventDrainResult
{
  // No queued events were consumed by this drain.
  NoEvents,
  // Events were consumed without painting here; a frame may be requested for the paint stage.
  UpdatedNoRender,
  // Reduction requested or performed its applicable render successfully.
  Rendered,
  // Reduction encountered renderer failure and session control must handle it.
  RenderFailed
};

// TUI-thread selector owner identity; SelectorHost uses one active kind to route input and retire the matching
// feature.
enum class ActiveSelectList
{
  // TUI thread: No selector feature owns input; ordinary session routing continues.
  None,
  // TUI thread: Keybinding help surface owns list input.
  Hotkeys,
  // TUI thread: Settings feature owns preview and list resolution.
  Settings,
  // TUI thread: Model selection owns resolution against the retained catalog.
  Model,
  // TUI thread: Reasoning selection owns the chosen effort value.
  Reasoning,
  // TUI thread: Scoped model configuration owns list resolution.
  ScopedModels,
  // TUI thread: Session selector owns switch/archive actions.
  Session,
  // TUI thread: Branch-summary feature owns its option surface.
  BranchSummary,
  // TUI thread: Search controller owns query updates and viewport restoration.
  TranscriptSearch,
  // TUI thread: Fork workflow owns the selected retained user turn.
  ForkUserTurn,
  // TUI thread: Copy workflow owns the selected retained user turn.
  CopyUserTurn,
  // TUI thread: Draft stash workflow owns selection of stored input.
  PromptStash,
  // TUI thread: Startup overview owns its popup-local facts.
  Overview,
  // TUI thread: Workspace controller owns chooser/inspector transitions.
  SubagentWorkspace
};

// TUI-thread two-key editor jump direction; None means the next character is ordinary input.
enum class ComposerJumpMode
{
  // TUI thread: no pending jump consumes the next typed character.
  None,
  // The next jump character seeks a later occurrence in the current draft.
  Forward,
  // The next jump character seeks an earlier occurrence in the current draft.
  Backward
};

// Owned TUI-thread deferred session action; the session selector retains identity until its confirmed archive
// operation runs.
struct PendingSessionArchiveAction
{
  // Owned target session identity avoids borrowing a selector row that may be rebuilt before the action
  // resolves.
  ava::core::NoLogString session_id;
  // TUI-thread selected operation: archive when true, unarchive when false; retained with the target identity.
  bool archive = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Session-owned display storage used only by the TUI thread. Features update it through named
// operations; renderers borrow const views. Features still own their behavior, and the backend
// still validates actions. The fully constructed frontend outlives this object. There is no
// internal lock: workers must never read or write these views.
class RuntimePresentationState final
{
 public:
  // TUI thread after concrete frontend construction: hydrate initial display/catalog/session facts through safe
  // application boundaries; frontend must be nonnull and outlive this object.
  RuntimePresentationState(TuiFrontEnd* frontend, std::string const& initial_keybind_status);

  // TUI thread at serialized application boundaries: refresh mode in both screen and live sidebar from frontend
  // prompt metadata.
  void refresh_prompt_metadata();
  // TUI thread: apply only changed or unversioned catalog deliveries, then rebuild local completion hints and
  // invalidate completion cache generation.
  void apply_catalog_delivery(TuiFrontEnd const& frontend, TuiCatalogDelivery delivery);
  // Temporary owned catalog/todo values read together on the TUI thread, then
  // moved into apply_boundary_data. This does not include popup-only state.
  struct BoundaryReads
  {
    // Owned optional catalog delivery; absence preserves existing command/file-reference projections.
    std::optional<TuiCatalogDelivery> catalog;
    // Owned optional committed todo read; presence replaces live sidebar todos at the same application
    // boundary.
    std::optional<std::vector<agent::TodoItem>> todos;
    // Catalog rows and todo text may contain private paths or user content.
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };
  // Reads the retained transports through the frontend's application boundary. Like
  // read_committed_session_id, this runs on the TUI thread before publishing changes.
  static BoundaryReads read_boundary_data(TuiFrontEnd& frontend);
  // Applies previously read transports without reading or replacing popup-only facts.
  void apply_boundary_data(TuiFrontEnd const& frontend, BoundaryReads reads);
  // TUI thread: reads and applies at one serialized application boundary.
  void refresh_after_backend_action(TuiFrontEnd& frontend);
  // Applies TUI-owned bindings and refreshes every local hint.
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
  // The composer and this display storage must outlive the synchronous call only.
  void publish_composer(Composer& composer);
  // TUI thread: let Composer project only its selected completion/suppression facts after editor actions.
  void publish_completion(Composer const& composer);
  // TUI thread: publish observed terminal geometry before preparing layouts.
  void publish_dimensions(std::size_t width, std::size_t height);
  // TUI thread: attachment ownership stays in PendingAttachments. Badge changes
  // accompany its successful stage/take/clear operations, without exposing paths.
  void publish_attachment(PendingAttachmentItem badge);
  // TUI thread after PendingAttachments take/clear: remove display badges while attachment resource ownership
  // remains outside this DTO.
  void clear_attachment_badges() noexcept;
  // TUI thread: SelectorHost owns opening/closing policy. These operations keep
  // one row vector in display storage; readers borrow it until the next mutation.
  void publish_selector(std::optional<SelectListView> view);
  // TUI thread with a published selector: replace selected index/query from SelectorHost's validated input
  // result.
  void publish_selector_selection(std::size_t index, std::string query);
  // TUI thread: update explanatory selector feedback if a selector is still visible; absence is harmless.
  void publish_selector_subtitle(std::string subtitle);
  // TUI thread with a published selector: retain the unchanged prefix, replace rows from first_changed, and
  // publish matching query/selection/subtitle together.
  void publish_selector_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query, std::string subtitle);

  // TUI thread: prompt owners publish one modal or retire it before completing
  // the waiting request. Input updates preserve the existing modal identity.
  void publish_permission_prompt(std::optional<PermissionPromptView> prompt);
  // TUI thread with a live permission prompt: publish decoded choice/guidance while preserving the prompt
  // request identity.
  void publish_permission_input(PermissionPromptChoice choice, bool guidance, std::string text);
  // TUI thread: publish or retire the question surface; the prompt owner retires it before resolving the
  // waiting request.
  void publish_question_prompt(std::optional<QuestionPromptView> prompt);
  // TUI thread with a live question prompt: publish selection/options/custom text without replacing its request
  // identity.
  void publish_question_input(std::size_t selected, std::vector<QuestionPromptOptionView> options, std::string text);
  // TUI thread: the synchronized plugin coordinator accepts or rejects requests. These
  // methods only publish acknowledged surfaces or remove retired surfaces.
  void publish_plugin_dock(std::optional<TuiPluginUiDockView> dock);
  // TUI thread: publish acknowledged plugin modal data or retire it; the synchronized coordinator still owns the request lifetime.
  void publish_plugin_modal(std::optional<TuiPluginUiModalView> modal);
  // TUI thread with a published modal: apply its validated option index while keeping the request identity
  // intact.
  void publish_plugin_selection(std::size_t selected);
  // TUI thread: publish the workspace's bounded view; the backend alone controls its jobs.
  void publish_workspace(std::optional<SubagentWorkspaceView> workspace);
  // TUI thread: accepted Mermaid output changes transcript geometry, so publish
  // its immutable shared display data and layout generation together.
  void publish_mermaid(std::shared_ptr<detail::MermaidTranscriptProjection const> projection);

  // TUI thread: viewport owners update scroll position without exposing transcript storage.
  void publish_transcript_scroll(std::size_t offset);
  // TUI thread at paint/synchronization: publish scroll and detached-output count together so reserved-row
  // geometry matches the visible position and new-output indicator.
  void publish_transcript_chrome(std::size_t offset, std::size_t new_output_count);
  // TUI thread at frame preparation: copy either live sidebar or detached snapshot into the visible frame.
  void publish_visible_sidebar(SidebarSnapshot sidebar);
  // TUI thread: publish the viewport owner's timed visibility decision; renderer registration owns actual
  // expiry scheduling.
  void publish_position_indicator(bool visible);
  // TUI thread: ask the selection owner to project validated endpoints; never expose writable selection state.
  void publish_selection(RuntimeTranscriptSelectionState const& selection);
  // TUI thread: drawer and output scrolling are clamped by their layout owner.
  void publish_sidebar_drawer(bool visible, std::size_t offset);
  // TUI thread: replace or retire local command output after its owner has chosen modal precedence.
  void publish_command_output(std::optional<CommandOutputView> output);
  // TUI thread with open output: publish scroll already clamped by renderer/input geometry.
  void publish_command_output_scroll(std::size_t offset);
  // TUI thread: preserve feedback precedence when an action replaces local output.
  void publish_local_status(std::string status);
  // TUI thread: apply local command feedback/output/tool recording in its existing precedence order; submitted
  // text is borrowed only during this call.
  void publish_local_completion(std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools = {},
                                bool record_tools = true);
  // TUI thread before user input feedback: clear reasoning-cycle feedback so a later key does not display a
  // stale success.
  void clear_input_feedback();
  // TUI thread: retire local-command feedback when the owning command/input transition has consumed it.
  void clear_local_feedback();
  // TUI thread after Composer dispatch: publish optional status; the caller remains responsible for beep and
  // frame routing.
  void publish_composer_feedback(ComposerInputResult const& result);
  // TUI thread after a successful cycle: apply reasoning feedback through the shared feedback-precedence
  // helper.
  void publish_reasoning_feedback(std::string feedback);
  // TUI thread: open command output using an owned copy of submitted text/results; command execution and
  // permissions remain outside presentation.
  void publish_local_output(std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools = {});
  // TUI thread: open local command error output; submitted text/error are not retained as borrowed views.
  void publish_local_error(std::string_view submitted, std::string error);
  // TUI thread: explicitly invalidate layouts when a display toggle requires it.
  void invalidate_transcript_layout();
  // TUI thread: preview changes only local display fields; backend settings remain authoritative.
  void publish_display_preview(runtime_views::DisplayPreviewTransaction const& preview);
  // TUI thread after display reload: refresh transaction baseline and reapply active local preview without
  // persisting backend settings.
  void reapply_display_preview(runtime_views::DisplayPreviewTransaction& preview);
  // TUI thread: replace displayed trust facts from the backend; displaying them grants no permissions.
  void publish_project_trust(std::optional<ProjectTrustSnapshot> trust);
  // TUI thread: choose the display style; caller coordinates any explicit transcript invalidation or viewport
  // restoration.
  void publish_tool_presentation(ToolPresentation presentation);
  // TUI thread: choose whether thinking is drawn; caller coordinates corresponding layout/viewport transitions.
  void publish_thinking_visibility(bool visible);
  // TUI thread: scope search layout preparation by moving, then restoring, the single selector view.
  [[nodiscard]] std::optional<SelectListView> withdraw_selector_projection();
  // TUI thread after popup retirement: release overview-local facts so later openings hydrate a fresh view.
  void clear_startup_overview();
  // TUI thread: transcript changes always invalidate the layout generation. Cap results
  // must be passed to viewport/search owners before they prepare another layout.
  void publish_transcript(std::vector<TranscriptItem> transcript);
  // TUI thread: append/cap via transcript helpers and return the leading-eviction shift for search/viewport
  // remapping.
  [[nodiscard]] std::ptrdiff_t append_transcript(TranscriptItem item);
  // TUI thread: append fallback assistant text when no runtime events supplied it; return cap shift before any
  // next layout preparation.
  [[nodiscard]] std::ptrdiff_t append_fallback_outputs(std::vector<std::string> const& output, std::string const& meta);
  // TUI thread after run reduction: preserve expansion choices while replacing/capping current turn, invalidate
  // layout, and return remapping details.
  [[nodiscard]] CappedTranscriptSnapshotUpdate publish_run_transcript(ActiveRunEvents& events, std::vector<TranscriptItem> turn);
  // TUI thread: record a bounded indexed tool fact with its origin; backend tool execution stays outside this
  // owner.
  void publish_tool(ToolTimelineItem tool, TuiToolIndexOrigin origin);
  // TUI thread: merge a later tool fact into the existing index without exposing writable indexed storage.
  void publish_tool_update(ToolTimelineItem const& tool);
  // TUI thread: validate live index/tool, change expansion, and invalidate transcript generation only on
  // success.
  [[nodiscard]] bool toggle_tool_details(std::size_t item_index);
  // TUI thread: toggle newest matching indexed transcript tool and return its item index for
  // viewport/navigation handling.
  [[nodiscard]] std::optional<std::size_t> toggle_matching_tool_details(std::string_view query);
  // TUI thread: use supplied content width to toggle an eligible thinking block and invalidate changed
  // geometry.
  [[nodiscard]] bool toggle_thinking_details(std::size_t item_index, std::size_t width);
  // TUI thread: find newest width-boundable thinking block, toggle it, and return its item index if geometry
  // changed.
  [[nodiscard]] std::optional<std::size_t> toggle_latest_thinking_details(std::size_t width);
  // TUI thread: show whether a run is processing. ActiveRun changes this at start
  // and completion; this operation does not start, stop or join its worker.
  void publish_processing(bool processing);
  // TUI thread: publish Spinner's current frame; scheduling and active-run lifetime remain with its owner.
  void publish_spinner(std::size_t frame);
  // TUI thread after reduction: replace live sidebar activity; detached visible sidebar remains under
  // TranscriptView control.
  void publish_run_activity(std::vector<SidebarActivityItem> activity);
  // TUI thread after reduction: replace live todo facts; no backend todo mutation occurs here.
  void publish_run_todos(std::vector<agent::TodoItem> todos);
  // TUI thread: replace the displayed queued follow-ups; the backend run still accepts and stores
  // those messages.
  void publish_queued_messages(std::vector<QueuedMessageItem> messages);
  // TUI thread: merge unseen paths into live sidebar history and retain at most the most recent 50 entries.
  void publish_modified_files(std::vector<SidebarModifiedFile> const& files);
  // TUI thread: upsert one live sidebar activity by identity, preserving other current activities.
  void publish_activity_item(SidebarActivityItem item);
  // TUI thread at run settlement: change only a still-running responding item so later completion cannot
  // overwrite a settled state.
  void settle_responding_activity(ToolTimelineStatus status, std::string detail);

  // These existing render DTOs are shared only on the TUI thread. Feature owners
  // publish their own screen fields here; the renderer reads/prepares display values.
  // Selector, composer, viewport and attachment behavior stays with those feature owners.
  // References remain valid for this object's lifetime; nested views expire on
  // the corresponding feature's next mutation. These references cannot perform backend actions.
  [[nodiscard]] ComposerSnapshot const& screen() const noexcept { return snapshot_; }
  // Live sidebar facts are published by backend hydration and ActiveRun; the
  // renderer/TranscriptView consume them through const references or copies.
  [[nodiscard]] SidebarSnapshot const& sidebar() const noexcept { return sidebar_; }

  // TUI thread: the transition coordinator clears the old session before applying
  // committed identity. Call reset before closing features, then finish afterward.
  void reset_session_projection(bool close_overview);
  // TUI thread after old feature cleanup: clear session-local sidebar facts and publish the committed session
  // identity.
  void finish_session_transition(std::string committed_session_id);

  // The screen contains prompt/message bodies and must never be debug streamed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Owned TUI-thread render DTO; named feature publications are its only external mutation path and nested
  // borrows expire on those mutations.
  ComposerSnapshot snapshot_;
  // Non-owning nonnull application frontend outlives this display storage; only TUI-thread hydration reads it.
  TuiFrontEnd* frontend_;
  // TUI-thread generation last accepted for slash commands; absent generation forces reapplication of
  // unversioned delivery.
  std::optional<std::size_t> applied_slash_catalog_generation_;
  // TUI-thread generation last accepted for file references; changed/unversioned delivery invalidates derived
  // completion matches.
  std::optional<std::size_t> applied_workspace_catalog_generation_;

  // Owned live sidebar facts from startup refreshes and run updates; TranscriptView may hold a separate frozen copy
  // while detached.
  SidebarSnapshot sidebar_;
};

// TUI-thread display reset used when committed session identity changes.
// Clear only session-local facts; mode/version remain process or backend display values.
void reset_session_sidebar(SidebarSnapshot& sidebar);

// Shared overview presentation helpers for popup-local display/keybinding changes
// when a competing view opens.
void close_startup_overview_presentation(RuntimePresentationState& presentation, SelectorHost& selectors);
// TUI thread after local hint/display changes: rebuild active overview rows while preserving safe
// filter/selection; close when its popup facts have retired.
void synchronize_startup_overview_presentation(RuntimePresentationState& presentation, SelectorHost& selectors);

}  // namespace ava::tui
