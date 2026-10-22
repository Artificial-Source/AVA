#pragma once

#include "ava/event/EventEnvelope.h"
#include "ava/event/events.h"
#include "ava/agent/subagent_launch.h"
#include "ava/tui/composer.h"
#include "ava/tui/session_grants.h"
#include "ava/session/attachments.h"
#include "ava/core/result.h"

#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "debug.h"

namespace ava::tui {

struct RuntimeDraftState;
struct TuiRuntimeOptions;
struct TuiRuntimeStateSnapshot;
struct TuiLocalDisplaySettings;
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
  std::string session_id;
  bool archive = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class RuntimePresentationState final
{
 public:
  RuntimePresentationState(TuiFrontEnd* frontend, TuiRuntimeOptions& options);

  void refresh_token_status(TuiRuntimeOptions const& options);
  void refresh_prompt_metadata();
  void apply_runtime_state_snapshot(TuiRuntimeOptions const& options, TuiRuntimeStateSnapshot state);
  // Applies TUI-owned bindings and refreshes every local hint projection.
  // This never changes backend catalog generations or applies a backend snapshot.
  void apply_local_key_bindings(TuiRuntimeOptions& options, TuiKeyBindings bindings);
  // Applies TUI-owned display values without changing backend runtime state.
  void apply_local_display_settings(TuiRuntimeOptions const& options, TuiLocalDisplaySettings settings);
  // Starts one popup-local overview lifetime from fresh backend facts, then
  // composes current UI-owned theme and keybinding hints.
  void initialize_startup_overview(TuiRuntimeOptions const& options, StartupOverviewSnapshot overview);

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

// Applies an authoritative runtime snapshot without reading or replacing popup-only facts.
void apply_runtime_state_snapshot_preserving_popups(TuiRuntimeOptions const& options, RuntimePresentationState& presentation_state,
                                                    TuiRuntimeStateSnapshot state);

// Applies the local effects of a committed backend session transition. The
// committed_session_id parameter must be read through
// TuiRuntimeOptions::read_committed_session_id before taking the TUI UI lock.
// Returns true only when the committed identity differs from local control
// tracking; same-session refreshes preserve session-scoped presentation.
[[nodiscard]] bool apply_committed_session_transition(RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                                                      TranscriptSearchController& transcript_search, RuntimeSubagentWorkspaceController& subagent_workspace,
                                                      ActiveSelectList& active_select_list, std::string committed_session_id);

// Applies every authoritative runtime snapshot after synchronizing local state
// to committed_session_id. The identity parameter is backend-read control data,
// never an identity copied from the incoming presentation DTO.
[[nodiscard]] bool apply_runtime_state_snapshot_with_presentation_transition(TuiRuntimeOptions const& options, RuntimePresentationState& presentation_state,
                                                                             RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                                                                             TranscriptSearchController& transcript_search,
                                                                             RuntimeSubagentWorkspaceController& subagent_workspace,
                                                                             ActiveSelectList& active_select_list, std::string committed_session_id,
                                                                             TuiRuntimeStateSnapshot state);

}  // namespace ava::tui
