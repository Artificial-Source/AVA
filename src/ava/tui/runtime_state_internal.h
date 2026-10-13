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
  explicit RuntimePresentationState(TuiRuntimeOptions& options);

  void refresh_token_status(TuiRuntimeOptions const& options);
  void refresh_active_context_status(TuiRuntimeOptions const& options);
  void apply_runtime_state_snapshot(TuiRuntimeOptions const& options, TuiRuntimeStateSnapshot state);

  ComposerSnapshot snapshot;

 private:
  std::optional<std::size_t> applied_slash_catalog_generation_;
  std::optional<std::size_t> applied_workspace_catalog_generation_;

 public:
  SidebarSnapshot sidebar;
  std::vector<ava::session::ImageAttachmentRef> pending_image_attachments;
  TuiSessionGrantRegistry command_session_grants;

 private:
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Shared overview presentation helpers. Normal and active-run snapshot application
// both route through these so an open overview rebuilds from the new DTO while
// preserving local query/selection identity, and competing authority closes it.
void close_startup_overview_presentation(ComposerSnapshot& snapshot, ActiveSelectList& active_select_list);
void synchronize_startup_overview_presentation(ComposerSnapshot& snapshot, ActiveSelectList& active_select_list);

// Applies an authoritative runtime snapshot and synchronizes any open overview list.
void apply_runtime_state_snapshot_with_overview_sync(TuiRuntimeOptions const& options, RuntimePresentationState& presentation_state,
                                                     ActiveSelectList& active_select_list, TuiRuntimeStateSnapshot state);

// Applies every authoritative runtime snapshot, but clears session-scoped TUI
// presentation first only when the authoritative session identity changed.
// Session transitions always close an open overview select-list.
[[nodiscard]] bool apply_runtime_state_snapshot_with_presentation_transition(TuiRuntimeOptions const& options, RuntimePresentationState& presentation_state,
                                                                             RuntimeDraftState& draft_state, RuntimeRenderer& renderer,
                                                                             TranscriptSearchController& transcript_search,
                                                                             RuntimeSubagentWorkspaceController& subagent_workspace,
                                                                             ActiveSelectList& active_select_list, TuiRuntimeStateSnapshot state);

}  // namespace ava::tui
