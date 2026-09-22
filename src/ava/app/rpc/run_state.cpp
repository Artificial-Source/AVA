#include "sys.h"
#include "input.h"
#include "protocol.h"
#include "run_state.h"

#include <utility>

namespace ava::app::rpc {
namespace {

ava::core::Error requires_active_prompt_error(std::string_view command_type)
{
  auto error = invalid_rpc("RPC command requires an active prompt");
  error.with_context("type", std::string(command_type));
  return error;
}

ava::core::Error input_closed_error(std::string_view command_type)
{
  auto error = invalid_rpc("RPC input is closed");
  error.with_context("type", std::string(command_type));
  return error;
}

ava::core::Error queue_limit_error(std::string_view command_type)
{
  auto error = invalid_rpc("RPC queued message limit exceeded");
  error.with_context("type", std::string(command_type));
  error.with_context("max_entries", std::to_string(kMaxRpcQueuedMessages));
  error.with_context("max_message_bytes", std::to_string(kMaxRpcQueuedMessageBytes));
  return error;
}

std::size_t queued_message_bytes(std::deque<QueuedRpcMessage> const& queue)
{
  std::size_t bytes = 0;
  for (auto const& queued : queue) bytes += queued.message.size();
  return bytes;
}

ClearedRpcQueues clear_queued_messages_locked(RpcRunState& state)
{
  ClearedRpcQueues cleared;
  cleared.steering_messages.reserve(state.steering_messages.size());
  while (!state.steering_messages.empty())
  {
    cleared.steering_messages.push_back(std::move(state.steering_messages.front()));
    state.steering_messages.pop_front();
  }
  cleared.follow_up_messages.reserve(state.follow_up_messages.size());
  while (!state.follow_up_messages.empty())
  {
    cleared.follow_up_messages.push_back(std::move(state.follow_up_messages.front()));
    state.follow_up_messages.pop_front();
  }
  return cleared;
}

void transition_input_terminal_locked(RpcRunState& state, RpcInputTerminalOutcome outcome)
{
  state.input_terminal_observed = true;
  switch (outcome)
  {
    case RpcInputTerminalOutcome::EofWithFinalRecord:
      return;
    case RpcInputTerminalOutcome::Eof:
    case RpcInputTerminalOutcome::Canceled:
    case RpcInputTerminalOutcome::Error:
      state.input_closed = true;
      state.cancel_requested.store(true, std::memory_order_relaxed);
      return;
  }
}

}  // namespace

ava::core::Error canceled_error()
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled", ava::core::ErrorCode::Canceled);
  error.with_context("rpc_error_code", "canceled");
  return error;
}

ava::core::Error skipped_follow_up_error(std::string_view reason)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "queued follow_up skipped");
  error.with_context("rpc_error_code", "follow_up_skipped");
  error.with_context("reason", std::string(reason));
  return error;
}

ava::core::Error active_run_reject_error(std::string_view command_type)
{
  auto error = invalid_rpc("RPC command is unavailable while a prompt is active");
  error.with_context("rpc_error_code", "active_run");
  error.with_context("type", std::string(command_type));
  return error;
}

ava::core::Error duplicate_request_id_error(std::string_view request_id)
{
  auto error = invalid_rpc("RPC request id is already outstanding");
  error.with_context("request_id", std::string(request_id));
  return error;
}

bool cancel_requested(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  return state.cancel_requested.load(std::memory_order_relaxed);
}

bool active_run(RpcRunState& state)
{
  std::unique_lock lock(state.mutex);
  state.publication_cv.wait(lock, [&] { return !state.terminal_publication_in_progress || state.input_closed; });
  return state.active_run_kind != RpcRunKind::None;
}

bool active_prompt_run(RpcRunState& state)
{
  std::unique_lock lock(state.mutex);
  state.publication_cv.wait(lock, [&] { return !state.terminal_publication_in_progress || state.input_closed; });
  return state.active_run_kind == RpcRunKind::Prompt;
}

bool async_worker_reap_ready(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  return !state.terminal_publication_in_progress && state.active_run_kind == RpcRunKind::None;
}

bool input_closed(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  return state.input_closed;
}

void observe_input_terminal(RpcRunState& state, RpcInputTerminalOutcome outcome)
{
  {
    std::lock_guard lock(state.mutex);
    transition_input_terminal_locked(state, outcome);
  }
  if (outcome != RpcInputTerminalOutcome::EofWithFinalRecord)
    state.publication_cv.notify_all();
}

RpcCommandAdmission await_command_admission(RpcRunState& state, std::string_view request_id, bool wait_for_terminal_publication)
{
  auto const id = std::string(request_id);
  std::unique_lock lock(state.mutex);
  if (wait_for_terminal_publication)
  {
    while (state.terminal_publication_in_progress && !state.input_closed) state.publication_cv.wait(lock);
  }
  if (state.input_closed)
    return RpcCommandAdmission::InputClosed;
  if (state.outstanding_request_ids.contains(id))
    return RpcCommandAdmission::DuplicateRequestId;
  return RpcCommandAdmission::Admitted;
}

bool outstanding_request_id(RpcRunState& state, std::string_view request_id)
{
  return await_command_admission(state, request_id) == RpcCommandAdmission::DuplicateRequestId;
}

void set_active_run(RpcRunState& state, RpcRunKind kind, std::string request_id)
{
  std::unique_lock lock(state.mutex);
  state.publication_cv.wait(lock, [&] { return !state.terminal_publication_in_progress || state.input_closed; });
  if (state.active_run_kind != RpcRunKind::None && !state.active_request_id.empty())
    state.outstanding_request_ids.erase(state.active_request_id);
  state.active_run_kind = kind;
  if (kind != RpcRunKind::None)
  {
    state.cancel_requested.store(false, std::memory_order_relaxed);
    state.active_request_id = std::move(request_id);
    if (!state.active_request_id.empty())
      state.outstanding_request_ids.insert(state.active_request_id);
  }
  else
  {
    state.active_request_id.clear();
  }
}

void set_active_request_id(RpcRunState& state, std::string request_id)
{
  std::lock_guard lock(state.mutex);
  state.active_run_kind = RpcRunKind::Prompt;
  state.active_request_id = std::move(request_id);
  if (!state.active_request_id.empty())
    state.outstanding_request_ids.insert(state.active_request_id);
}

void complete_outstanding_request(RpcRunState& state, std::string_view request_id)
{
  std::lock_guard lock(state.mutex);
  state.outstanding_request_ids.erase(std::string(request_id));
}

ava::core::Result<QueuedRpcMessage> queue_rpc_message(std::deque<QueuedRpcMessage>& queue, RpcRunState& state, std::string command_type, std::string request_id,
                                                      std::string message)
{
  std::unique_lock lock(state.mutex);
  state.publication_cv.wait(lock, [&] { return !state.terminal_publication_in_progress || state.input_closed; });
  if (state.input_closed)
    return std::unexpected(input_closed_error(command_type));
  if (state.cancel_requested.load(std::memory_order_relaxed))
    return std::unexpected(canceled_error());
  if (state.active_run_kind != RpcRunKind::Prompt)
  {
    if (state.active_run_kind != RpcRunKind::None)
      return std::unexpected(active_run_reject_error(command_type));
    return std::unexpected(requires_active_prompt_error(command_type));
  }
  if (state.active_request_id.empty())
    return std::unexpected(requires_active_prompt_error(command_type));
  if (state.outstanding_request_ids.contains(request_id))
    return std::unexpected(duplicate_request_id_error(request_id));
  if (queue.size() >= kMaxRpcQueuedMessages || message.size() > kMaxRpcQueuedMessageBytes ||
      queued_message_bytes(queue) + message.size() > kMaxRpcQueuedMessageBytes)
  {
    return std::unexpected(queue_limit_error(command_type));
  }
  QueuedRpcMessage queued{.request_id = std::move(request_id), .correlation_id = state.active_request_id, .message = std::move(message)};
  if (command_type == "follow_up")
    state.outstanding_request_ids.insert(queued.request_id);
  queue.push_back(queued);
  return queued;
}

std::vector<QueuedRpcMessage> take_queued_steering_messages(RpcRunState& state, std::string_view correlation_id)
{
  std::lock_guard lock(state.mutex);
  std::vector<QueuedRpcMessage> queued;
  std::deque<QueuedRpcMessage> remaining;
  while (!state.steering_messages.empty())
  {
    if (state.steering_messages.front().correlation_id == correlation_id)
    {
      queued.push_back(std::move(state.steering_messages.front()));
    }
    else
    {
      remaining.push_back(std::move(state.steering_messages.front()));
    }
    state.steering_messages.pop_front();
  }
  state.steering_messages = std::move(remaining);
  return queued;
}

void begin_prompt_terminal_publication(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  state.terminal_publication_in_progress = true;
}

RpcFollowUpTransition transition_after_prompt_terminal_response(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  if (state.cancel_requested.load(std::memory_order_relaxed) || state.input_closed || state.input_terminal_observed)
  {
    state.active_run_kind = RpcRunKind::None;
    state.active_request_id.clear();
    return RpcFollowUpTransition{.kind = RpcFollowUpTransitionKind::Skipped, .follow_up = std::nullopt, .cleared = clear_queued_messages_locked(state)};
  }

  if (!state.follow_up_messages.empty())
  {
    auto queued = std::move(state.follow_up_messages.front());
    state.follow_up_messages.pop_front();
    state.active_run_kind = RpcRunKind::Prompt;
    state.active_request_id = queued.request_id;
    return RpcFollowUpTransition{.kind = RpcFollowUpTransitionKind::Activated, .follow_up = std::move(queued), .cleared = {}};
  }

  state.active_run_kind = RpcRunKind::None;
  state.active_request_id.clear();
  return RpcFollowUpTransition{.kind = RpcFollowUpTransitionKind::Deactivated, .follow_up = std::nullopt, .cleared = clear_queued_messages_locked(state)};
}

ClearedRpcQueues begin_terminal_publication(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  state.terminal_publication_in_progress = true;
  state.active_run_kind = RpcRunKind::None;
  state.active_request_id.clear();
  return clear_queued_messages_locked(state);
}

void complete_terminal_publication(RpcRunState& state, std::string_view request_id)
{
  {
    std::lock_guard lock(state.mutex);
    state.outstanding_request_ids.erase(std::string(request_id));
    state.terminal_publication_in_progress = false;
  }
  state.publication_cv.notify_all();
}

RpcCancellation begin_cancellation(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  state.cancel_requested.store(true, std::memory_order_relaxed);
  RpcCancellation cancellation;
  cancellation.active_run = state.active_run_kind != RpcRunKind::None;
  cancellation.active_request_id = state.active_request_id;
  cancellation.deferred_to_terminal_publication = state.terminal_publication_in_progress;
  if (cancellation.deferred_to_terminal_publication)
  {
    cancellation.deferred_steering_count = state.steering_messages.size();
    cancellation.deferred_follow_up_count = state.follow_up_messages.size();
  }
  else
  {
    cancellation.cleared = clear_queued_messages_locked(state);
  }
  return cancellation;
}

void wait_for_terminal_publication(RpcRunState& state)
{
  std::unique_lock lock(state.mutex);
  state.publication_cv.wait(lock, [&] { return !state.terminal_publication_in_progress; });
}

std::vector<QueuedRpcMessage> clear_queued_steering_messages(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  std::vector<QueuedRpcMessage> cleared;
  cleared.reserve(state.steering_messages.size());
  while (!state.steering_messages.empty())
  {
    cleared.push_back(std::move(state.steering_messages.front()));
    state.steering_messages.pop_front();
  }
  return cleared;
}

ClearedRpcQueues deactivate_and_clear_queued_messages(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  if (!state.active_request_id.empty())
    state.outstanding_request_ids.erase(state.active_request_id);
  state.active_run_kind = RpcRunKind::None;
  state.active_request_id.clear();
  return clear_queued_messages_locked(state);
}

ClearedRpcQueues close_input_and_cancel(RpcRunState& state)
{
  ClearedRpcQueues cleared;
  {
    std::lock_guard lock(state.mutex);
    transition_input_terminal_locked(state, RpcInputTerminalOutcome::Eof);
    if (!state.terminal_publication_in_progress)
      cleared = clear_queued_messages_locked(state);
  }
  state.publication_cv.notify_all();
  return cleared;
}

void record_async_error(RpcRunState& state, ava::core::Error error)
{
  std::lock_guard lock(state.mutex);
  if (!state.async_error)
    state.async_error = std::move(error);
}

std::optional<ava::core::Error> take_async_error(RpcRunState& state)
{
  std::lock_guard lock(state.mutex);
  auto error = std::move(state.async_error);
  state.async_error.reset();
  return error;
}

}  // namespace ava::app::rpc
