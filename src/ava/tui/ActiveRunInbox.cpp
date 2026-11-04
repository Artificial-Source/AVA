// Serializes worker writes and TUI event reads without sharing screen state.
#include "sys.h"
#include "ActiveRunInbox.h"

#include <utility>

namespace ava::tui {

ActiveRunInbox::ActiveRunInbox(TuiWakeup wakeup) : wakeup_(std::move(wakeup))
{
}

ava::event::RuntimeEventSink ActiveRunInbox::event_sink()
{
  return std::bind_front(&ActiveRunInbox::enqueue_event, shared_from_this());
}

ava::event::EventEnvelopeSink ActiveRunInbox::envelope_sink()
{
  return std::bind_front(&ActiveRunInbox::enqueue_envelope, shared_from_this());
}

ava::agent::SubagentLaunchSink ActiveRunInbox::subagent_launch_sink()
{
  return std::bind_front(&ActiveRunInbox::enqueue_subagent_launch, shared_from_this());
}

ActiveRunInbox::FollowUpStarted ActiveRunInbox::wrap_follow_up_started(FollowUpStarted original)
{
  return std::bind_front(&ActiveRunInbox::follow_up_started, shared_from_this(), std::move(original));
}

void ActiveRunInbox::set_request_id(std::string request_id)
{
  std::lock_guard lock(mutex_);
  if (!accepting_)
    return;
  context_.request_id = request_id;
  context_.correlation_id = std::move(request_id);
}

ava::core::VoidResult ActiveRunInbox::follow_up_started(FollowUpStarted const& original, TuiQueuedFollowUp const& follow_up)
{
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
      return {};
    auto request_id = follow_up.request_id;
    context_.request_id = request_id;
    context_.correlation_id = std::move(request_id);
  }
  // Invoke outside mutex_: the backend callback can enqueue events in this inbox.
  // The context update above must precede all events emitted by that callback.
  return original ? original(follow_up) : ava::core::VoidResult{};
}

ava::core::VoidResult ActiveRunInbox::enqueue_event(ava::event::RuntimeEvent const& event)
{
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
      return {};
    if (auto queued = events_.enqueue(event, context_); !queued)
      return queued;
  }
  // The event is visible before the wake; waking while still constructing it could
  // let the TUI find no work and return to sleep.
  wakeup_.notify();
  return {};
}

ava::core::VoidResult ActiveRunInbox::enqueue_envelope(ava::event::EventEnvelope const& envelope)
{
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
      return {};
    if (auto queued = events_.enqueue_envelope(envelope); !queued)
      return queued;
  }
  wakeup_.notify();
  return {};
}

void ActiveRunInbox::enqueue_subagent_launch(ava::agent::SubagentLaunchNotification const& notification)
{
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
      return;
    events_.enqueue_subagent_launch(notification);
  }
  wakeup_.notify();
}

bool ActiveRunInbox::request_stop()
{
  auto const already_requested = stop_requested_.exchange(true, std::memory_order_acq_rel);
  if (!already_requested)
    wakeup_.notify();
  return !already_requested;
}

bool ActiveRunInbox::stop_requested() const noexcept
{
  return stop_requested_.load(std::memory_order_acquire);
}

std::vector<QueuedTuiEvent> ActiveRunInbox::drain(std::size_t maximum)
{
  std::lock_guard lock(mutex_);
  return events_.drain(maximum);
}

bool ActiveRunInbox::has_pending() const
{
  std::lock_guard lock(mutex_);
  return std::holds_alternative<ActiveRunResult>(result_) || events_.has_pending();
}

bool ActiveRunInbox::result_ready() const
{
  std::lock_guard lock(mutex_);
  return std::holds_alternative<ActiveRunResult>(result_);
}

bool ActiveRunInbox::received_any() const
{
  std::lock_guard lock(mutex_);
  return events_.received_any();
}

void ActiveRunInbox::discard()
{
  std::lock_guard lock(mutex_);
  events_.discard();
}

void ActiveRunInbox::publish_result(ActiveRunResult result)
{
  {
    std::lock_guard lock(mutex_);
    if (!accepting_ || !std::holds_alternative<std::monostate>(result_))
      return;
    result_.emplace<ActiveRunResult>(std::move(result));
  }
  // Store the final result under lock before notifying. No remaining backend
  // callback may need the TUI, which can take this result and immediately join.
  wakeup_.notify();
}

std::optional<ActiveRunResult> ActiveRunInbox::take_result()
{
  std::lock_guard lock(mutex_);
  auto* ready = std::get_if<ActiveRunResult>(&result_);
  if (!ready)
    return std::nullopt;
  auto result = std::move(*ready);
  // Remember that the TUI took the result. An empty variant would incorrectly
  // allow publish_result() to store a second result.
  result_.emplace<ResultHistory>(ResultHistory::Consumed);
  return result;
}

void ActiveRunInbox::retire()
{
  std::lock_guard lock(mutex_);
  accepting_ = false;
}

} // namespace ava::tui
