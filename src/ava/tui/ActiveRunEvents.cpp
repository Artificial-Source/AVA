#include "sys.h"
#include "ActiveRunEvents.h"
#include "ava/tui/command_output.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace ava::tui {
namespace detail {

bool command_event_request_has_conversation_authority(std::optional<std::string> const& event_request_id,
                                                      std::span<std::string const> ordinary_turn_request_ids) noexcept
{
  return event_request_id && !event_request_id->empty() && std::ranges::find(ordinary_turn_request_ids, *event_request_id) != ordinary_turn_request_ids.end();
}

} // namespace detail
namespace {

bool envelope_has_prompt_only_authority(ava::event::EventEnvelope const& envelope)
{
  return envelope.name == "permission_requested" || envelope.name == "permission_replied" || envelope.name == "question_requested" ||
         envelope.name == "question_replied";
}

bool has_transcript_events(TuiEventState const& state)
{
  return !state.transcript.empty() || !state.pending_assistant_text.empty() || !state.pending_reasoning_text.empty() || !state.pending_tools.empty();
}

void append_transcript(std::vector<TranscriptItem>& destination, TuiEventState const& state)
{
  auto items = event_state_transcript_snapshot(state, PendingTextProjection::Unparsed);
  destination.insert(destination.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
}

std::string tool_identity(ToolTimelineItem const& tool)
{
  if (!tool.call_id.empty())
    return "call\n" + tool.call_id;
  if (!tool.request_id.empty())
    return "request\n" + tool.request_id;
  if (!tool.correlation_id.empty())
    return "correlation\n" + tool.correlation_id;
  return {};
}

bool settled_tool(ToolTimelineItem const& tool)
{
  return tool.status != ToolTimelineStatus::Running &&
         (tool.lifecycle == ToolLifecycleState::Complete || tool.lifecycle == ToolLifecycleState::Canceled || tool.lifecycle == ToolLifecycleState::Error);
}

void merge_tool(std::vector<ToolTimelineItem>& tools, ToolTimelineItem const& tool)
{
  auto const key = tool_identity(tool);
  if (!key.empty())
  {
    auto existing = std::ranges::find_if(tools, [&](ToolTimelineItem const& candidate) { return tool_identity(candidate) == key; });
    if (existing != tools.end())
    {
      *existing = tool;
      return;
    }
  }
  tools.push_back(tool);
}

void append_settled_tools(std::vector<ToolTimelineItem>& tools, TuiEventState const& state)
{
  for (auto const& item : state.transcript)
  {
    if (item.tool && settled_tool(*item.tool))
      merge_tool(tools, *item.tool);
  }
  for (auto const& pending : state.pending_tools)
  {
    if (settled_tool(pending.item))
      merge_tool(tools, pending.item);
  }
}

} // namespace

ActiveRunEvents::ActiveRunEvents(bool command, std::string submitted, std::string initial_request_id, std::vector<TranscriptItem> baseline,
                                 std::vector<ava::agent::TodoItem> todos)
    : command_(command), submitted_(std::move(submitted)), initial_request_id_(std::move(initial_request_id)), baseline_(std::move(baseline))
{
  aggregate_.todos = std::move(todos);
}

TuiEventState& ActiveRunEvents::command_state(std::optional<std::string> const& request_id)
{
  if (!request_id || request_id->empty())
    return unattributed_;
  if (*request_id == initial_request_id_)
    return initial_command_;
  auto existing = std::ranges::find_if(request_states_, [&](auto const& state) { return state.first == *request_id; });
  if (existing != request_states_.end())
    return existing->second;
  request_states_.emplace_back(*request_id, TuiEventState{});
  return request_states_.back().second;
}

void ActiveRunEvents::apply_runtime(QueuedRuntimeEvent const& event, RuntimePresentationState& presentation)
{
  apply_runtime_event(aggregate_, event.event, event.context);
  if (command_)
  {
    apply_runtime_event(command_state(event.context.request_id), event.event, event.context);
  }
  else
  {
    if (auto tool = runtime_event_tool_projection(aggregate_, event.event, event.context))
      presentation.publish_tool(std::move(*tool), TuiToolIndexOrigin::Provider);
    for (auto const& pending : aggregate_.pending_tools)
      presentation.publish_tool_update(pending.item);
  }
}

void ActiveRunEvents::apply_envelope(ava::event::EventEnvelope const& envelope, RuntimePresentationState& presentation)
{
  apply_control_event_envelope(aggregate_, envelope);
  if (command_ && !envelope_has_prompt_only_authority(envelope))
    apply_control_event_envelope(command_state(envelope.request_id), envelope);
  if (!command_)
  {
    for (auto const& pending : aggregate_.pending_tools)
      presentation.publish_tool_update(pending.item);
  }
}

void ActiveRunEvents::apply_subagent_launch(ava::agent::SubagentLaunchNotification const& notification, RuntimePresentationState& presentation)
{
  apply_subagent_launch_notification(aggregate_, notification);
  if (command_)
  {
    auto const request_id = notification.request_id.empty() ? std::optional<std::string>{} : std::optional<std::string>{notification.request_id};
    apply_subagent_launch_notification(command_state(request_id), notification);
  }
  else
  {
    for (auto const& pending : aggregate_.pending_tools)
      presentation.publish_tool_update(pending.item);
  }
}

void ActiveRunEvents::apply_batch(std::span<QueuedTuiEvent const> events, RuntimePresentationState& presentation)
{
  for (auto const& event : events)
  {
    if (auto runtime = std::get_if<QueuedRuntimeEvent>(&event))
      apply_runtime(*runtime, presentation);
    else if (auto envelope = std::get_if<ava::event::EventEnvelope>(&event))
      apply_envelope(*envelope, presentation);
    else
      apply_subagent_launch(std::get<ava::agent::SubagentLaunchNotification>(event), presentation);
  }
  if (!events.empty())
  {
    projection_pending_ = true;
  }
}

void ActiveRunEvents::authorize_receipts(std::vector<std::string> request_ids)
{
  ordinary_turn_request_ids_ = std::move(request_ids);
  command_events_released_ = !command_ || !ordinary_turn_request_ids_.empty();
  projection_pending_ = true;
}

std::vector<TranscriptItem> ActiveRunEvents::authorized_transcript() const
{
  std::vector<TranscriptItem> transcript;
  if (detail::command_event_request_has_conversation_authority(std::optional<std::string>{initial_request_id_}, ordinary_turn_request_ids_))
    append_transcript(transcript, initial_command_);
  for (auto const& [request_id, state] : request_states_)
  {
    if (detail::command_event_request_has_conversation_authority(std::optional<std::string>{request_id}, ordinary_turn_request_ids_))
      append_transcript(transcript, state);
  }
  return transcript;
}

std::optional<std::vector<TranscriptItem>> ActiveRunEvents::take_transcript_projection()
{
  if (!projection_pending_ || (command_ && !command_events_released_))
    return std::nullopt;
  projection_pending_ = false;
  auto transcript = command_ ? authorized_transcript() : event_state_transcript_snapshot(aggregate_, PendingTextProjection::Unparsed);
  if (command_)
    remove_literal_command_invocation(transcript, submitted_);
  return transcript;
}

CappedTranscriptSnapshotUpdate ActiveRunEvents::apply_transcript_projection(std::vector<TranscriptItem>& destination, std::vector<TranscriptItem> turn)
{
  auto const update = apply_capped_transcript_snapshot(destination, baseline_, std::move(turn), leading_evictions_);
  leading_evictions_ = update.leading_evictions;
  return update;
}

TuiEventState ActiveRunEvents::settlement_state() const
{
  auto settled = aggregate_;
  if (command_)
  {
    settled.transcript = authorized_transcript();
    settled.pending_assistant_text.clear();
    settled.pending_reasoning_text.clear();
    settled.pending_tools.clear();
  }
  return settled;
}

bool ActiveRunEvents::conversation_committed() const noexcept
{
  return !command_ || !ordinary_turn_request_ids_.empty();
}

bool ActiveRunEvents::authorized_events_received() const
{
  // Only command submissions need receipt-filtered evidence; ordinary runs use
  // the inbox history, which includes events already drained into this reducer.
  ASSERT(command_);
  if (detail::command_event_request_has_conversation_authority(std::optional<std::string>{initial_request_id_}, ordinary_turn_request_ids_) &&
      has_transcript_events(initial_command_))
    return true;
  return std::ranges::any_of(request_states_, [&](auto const& request) {
    return detail::command_event_request_has_conversation_authority(std::optional<std::string>{request.first}, ordinary_turn_request_ids_) &&
           has_transcript_events(request.second);
  });
}

std::vector<ToolTimelineItem> ActiveRunEvents::merge_unauthorized_tools(std::vector<ToolTimelineItem> tools) const
{
  if (!detail::command_event_request_has_conversation_authority(std::optional<std::string>{initial_request_id_}, ordinary_turn_request_ids_))
    append_settled_tools(tools, initial_command_);
  append_settled_tools(tools, unattributed_);
  for (auto const& [request_id, state] : request_states_)
  {
    if (!detail::command_event_request_has_conversation_authority(std::optional<std::string>{request_id}, ordinary_turn_request_ids_))
      append_settled_tools(tools, state);
  }
  return tools;
}

void ActiveRunEvents::discard()
{
  baseline_.clear();
  leading_evictions_ = 0;
  aggregate_ = {};
  initial_command_ = {};
  unattributed_ = {};
  request_states_.clear();
  ordinary_turn_request_ids_.clear();
  command_events_released_ = false;
  projection_pending_ = false;
}

} // namespace ava::tui
