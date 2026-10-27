#pragma once

#include "ava/event/EventEnvelope.h"
#include "ava/event/EventEnvelopeContext.h"
#include "ava/event/RuntimeEvent.h"
#include "ava/agent/subagent_launch.h"
#include "ava/tui/composer.h"
#include "ava/core/NoLogString.h"
#include "ava/core/mode.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ava::tui {

enum class TuiEventRunStatus
{
  Idle,
  Running,
  Completed,
  Canceled,
  Error,
  Done,
};

struct PendingToolItem
{
  std::string call_id;
  std::string backend_call_id;
  std::string request_id;
  std::string correlation_id;
  ToolTimelineItem item;
  bool append_only_stream = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TuiEventState
{
  // Completed transcript, pending assistant/reasoning/tool state, prompt audit activity, and
  // sidebar/status data are intentionally separate so live runs and replayed event streams settle
  // into the same visible story without mutating completed transcript history.
  std::vector<TranscriptItem> transcript;
  ava::core::NoLogString pending_assistant_text;
  ava::core::NoLogString pending_assistant_meta;
  ava::core::NoLogString pending_reasoning_text;
  bool pending_reasoning_redacted = false;
  std::vector<PendingToolItem> pending_tools;
  std::vector<ToolPermissionAuditItem> permission_audits;
  std::vector<QueuedMessageItem> queued_messages;
  std::vector<SidebarActivityItem> activity;
  std::vector<SidebarModifiedFile> modified_files;
  std::vector<TodoItem> todos;
  std::optional<std::string> active_run_id = std::nullopt;
  std::optional<std::string> active_turn_id = std::nullopt;
  std::optional<std::string> active_message_id = std::nullopt;
  std::optional<std::string> active_request_id = std::nullopt;
  std::optional<std::string> active_correlation_id = std::nullopt;
  ava::core::Mode current_mode = ava::core::Mode::Build;
  std::string current_provider_id;
  std::string current_model_id;
  TuiEventRunStatus run_status = TuiEventRunStatus::Idle;
  ava::core::NoLogString stop_reason;
  ava::core::NoLogString error_text;
  ava::core::NoLogString error_details;
  std::size_t provider_iterations = 0;
  std::size_t tool_calls = 0;

  std::optional<std::size_t> stream_assistant_transcript_index = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

void apply_runtime_event(TuiEventState& state, ava::event::RuntimeEvent const& event, ava::event::EventEnvelopeContext const& context = {});
// Returns the exact tool card touched by a lifecycle RuntimeEvent after that
// event has been applied. Non-tool/provider-audit events return no projection.
[[nodiscard]] std::optional<ToolTimelineItem> runtime_event_tool_projection(TuiEventState const& state, ava::event::RuntimeEvent const& event,
                                                                            ava::event::EventEnvelopeContext const& context = {});
void apply_control_event_envelope(TuiEventState& state, ava::event::EventEnvelope const& envelope);
void apply_subagent_launch_notification(TuiEventState& state, ava::agent::SubagentLaunchNotification const& notification);

enum class PendingTextProjection
{
  CompleteModels,
  Unparsed,
};

[[nodiscard]] std::vector<TranscriptItem> event_state_transcript_snapshot(
    TuiEventState const& state, PendingTextProjection pending_text_projection = PendingTextProjection::CompleteModels);

}  // namespace ava::tui
