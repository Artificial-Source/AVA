#pragma once

#include "ava/event/EventEnvelope.h"
#include "ava/event/events.h"
#include "ava/agent/subagent_launch.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime.h"
#include "ava/tui/session_grants.h"
#include "ava/session/attachments.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "debug.h"

namespace ava::tui {

struct RuntimeDraftState;
class RuntimeRenderer;
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
  [[nodiscard]] ava::event::EventEnvelopeSink envelope_sink();
  [[nodiscard]] ava::agent::SubagentLaunchSink subagent_launch_sink();
  [[nodiscard]] ava::core::VoidResult enqueue(ava::event::RuntimeEvent const& event, ava::event::EventEnvelopeContext context = {});
  [[nodiscard]] std::vector<QueuedTuiEvent> drain();
  void discard();
  [[nodiscard]] bool received_any();

 private:
  std::mutex mutex;
  std::vector<QueuedTuiEvent> events;
  bool received = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
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
  Overview
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
  RuntimePresentationState(TuiFrontEnd* frontend);

  void refresh_prompt_metadata();
  void apply_catalog_delivery(TuiFrontEnd const& frontend, TuiCatalogDelivery delivery);
  // The retained catalog/todo transports read at one serialized boundary.
  struct BoundaryReads
  {
    std::optional<TuiCatalogDelivery> catalog;
    std::optional<std::vector<agent::TodoItem>> todos;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };
  // Reads the retained transports through the options callbacks. Like
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

  ComposerSnapshot snapshot;

 private:
  TuiFrontEnd* frontend_;
  std::optional<std::size_t> applied_slash_catalog_generation_;
  std::optional<std::size_t> applied_workspace_catalog_generation_;

 public:
  SidebarSnapshot sidebar;
  std::vector<ava::session::ImageAttachmentRef> pending_image_attachments;
  TuiSessionGrantRegistry command_session_grants;

 private:
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Shared overview presentation helpers for popup-local display/keybinding changes
// and competing presentation authority.
void close_startup_overview_presentation(ComposerSnapshot& snapshot, ActiveSelectList& active_select_list);
void synchronize_startup_overview_presentation(ComposerSnapshot& snapshot, ActiveSelectList& active_select_list);

// Applies the local effects of a committed backend session transition. The
// committed_session_id parameter must be read through
// TuiFrontEnd::read_committed_session_id before taking the TUI UI lock.
// Returns true only when the committed identity differs from local control
// tracking; same-session refreshes preserve session-scoped presentation.
[[nodiscard]] bool apply_committed_session_transition(RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                                                      TranscriptSearchController& transcript_search, RuntimeSubagentWorkspaceController& subagent_workspace,
                                                      ActiveSelectList& active_select_list, std::string committed_session_id);

// Synchronizes session-scoped presentation before pulling retained backend data.
[[nodiscard]] bool apply_committed_session_transition_and_refresh(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state,
                                                                  RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                                                                  TranscriptSearchController& transcript_search,
                                                                  RuntimeSubagentWorkspaceController& subagent_workspace, ActiveSelectList& active_select_list,
                                                                  std::string committed_session_id);

}  // namespace ava::tui
